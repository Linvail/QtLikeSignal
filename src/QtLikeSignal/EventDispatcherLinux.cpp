// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Linux event dispatcher implementation.

#include "QtLikeSignal/EventDispatcherLinux.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace QtLikeSignal
{
    //! Constructs the dispatcher and its wakeup descriptor.
    EventDispatcherLinux::EventDispatcherLinux()
    {
        // Non-blocking so drainWakeFd() can read it unconditionally without risking a stall, and
        // close-on-exec so a fork()/exec() from user code does not leak it into the child.
        mWakeFd = ::eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
        if( mWakeFd < 0 )
        {
            // Not fatal: waitForEvents() falls back to the inherited condition-variable wait, which
            // still delivers our own events correctly. Only registered platform descriptors stop
            // working, so say so rather than failing silently.
            std::fprintf( stderr,
                "EventDispatcherLinux: eventfd() failed (%d); falling back to the "
                "cross-platform wait, so platform event sources will not be polled\n", errno );
        }
    }

    //! Destroys the dispatcher, closing the wakeup descriptor.
    EventDispatcherLinux::~EventDispatcherLinux()
    {
        if( mWakeFd >= 0 )
        {
            ::close( mWakeFd );
            mWakeFd = -1;
        }
    }

    //! Registers a platform file descriptor to be polled alongside this thread's own events.
    //!
    //! @p aEvents is a poll(2) mask (POLLIN, POLLOUT, ...). @p aCallback runs on the dispatcher's
    //! own thread with no lock held, so it may freely post events, start timers, or register and
    //! unregister sources. Registering a descriptor that is already registered replaces its mask
    //! and callback, and counts as a new registration for the purposes of unregisterEventSource().
    //! Returns false if @p aFd is negative or @p aCallback is empty. Thread-safe.
    bool EventDispatcherLinux::registerEventSource
        (
        int aFd,                          //!< Descriptor to poll; must be >= 0.
        short aEvents,                    //!< poll(2) event mask to wait for.
        EventSourceCallback aCallback     //!< Invoked with the revents when the descriptor is ready.
        )
    {
        if( aFd < 0 || !aCallback )
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock( mMutex );

            // A fresh generation either way, so a poll() round already in flight against the old
            // registration cannot deliver to the new callback.
            const unsigned long long generation = mNextGeneration++;

            bool replaced = false;
            for( auto& source : mSources )
            {
                if( source.mFd == aFd )
                {
                    source.mEvents     = aEvents;
                    source.mCallback   = std::move( aCallback );
                    source.mGeneration = generation;
                    // Fall through to the wake below: a loop already blocked in poll() is waiting on
                    // the old mask and has to rebuild its descriptor set to honour the new one.
                    replaced = true;
                    break;
                }
            }
            if( !replaced )
            {
                mSources.push_back( { aFd, aEvents, std::move( aCallback ), generation } );
            }
        }

        // The descriptor set is snapshotted at the start of each wait, so a loop currently blocked
        // is polling a stale set. Wake it so it rebuilds and picks this source up immediately
        // instead of only after the next unrelated event.
        wakeWaiter();
        return true;
    }

    //! Stops polling a descriptor. Returns true if it was registered.
    //!
    //! **Called from the dispatcher's own thread -- including from inside a callback -- this is
    //! synchronous:** the callback will not run again, not even for a readiness this poll() round
    //! has already observed, because every invocation re-checks the registration first.
    //!
    //! Called from another thread it is not, and cannot be without blocking on a callback that may
    //! be running arbitrary code. The callback can still be in progress when this returns. A caller
    //! that is about to destroy what the callback captures must either unregister from the loop's
    //! own thread or arrange its own hand-off; closing the descriptor is safe either way, since a
    //! closed descriptor is re-checked and skipped rather than dispatched.
    //!
    //! Thread-safe.
    bool EventDispatcherLinux::unregisterEventSource
        (
        int aFd  //!< Descriptor previously passed to registerEventSource().
        )
    {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock( mMutex );
            for( auto it = mSources.begin(); it != mSources.end(); ++it )
            {
                if( it->mFd == aFd )
                {
                    mSources.erase( it );
                    removed = true;
                    break;
                }
            }
        }

        if( removed )
        {
            // Wake for the same reason as registerEventSource(): a blocked loop is still polling
            // this descriptor, and the caller is entitled to close it once we return.
            wakeWaiter();
        }
        return removed;
    }

    //! Blocks in poll() over the wakeup descriptor and every registered platform descriptor.
    void EventDispatcherLinux::waitForEvents
        (
        std::unique_lock<std::mutex>& aLock,  //!< Lock on mMutex, held on entry and on return.
        int aTimeoutMs                          //!< Milliseconds to wait, or -1 to wait indefinitely.
        )
    {
        if( mWakeFd < 0 )
        {
            // No eventfd, so there is nothing poll() could wait on that another thread can signal.
            // Use the inherited condition-variable wait instead of spinning.
            EventDispatcherDefault::waitForEvents( aLock, aTimeoutMs );
            return;
        }

        // Snapshot the descriptor set while we still hold the lock. Only each source's identity is
        // taken, not its callback: the callback is looked up again, under the lock, at the moment it is
        // about to be invoked. Copying them out here would pin a callback unregisterEventSource() has
        // since removed, and deliver to it anyway.
        std::vector<pollfd> pollSet;
        std::vector<unsigned long long> generations;
        pollSet.reserve( mSources.size() + 1 );
        generations.reserve( mSources.size() + 1 );

        pollSet.push_back( pollfd { mWakeFd, POLLIN, 0 } );
        generations.push_back( 0 );   // index 0 is the wakeup FD; it has no user callback
        for( const auto& source : mSources )
        {
            pollSet.push_back( pollfd { source.mFd, source.mEvents, 0 } );
            generations.push_back( source.mGeneration );
        }

        // Block with the lock released: poll() cannot hold a std::mutex, and holding it would stop
        // every other thread from posting -- including the post that is supposed to wake us.
        aLock.unlock();

        const int ready = ::poll( pollSet.data(), static_cast<nfds_t>( pollSet.size() ),
            aTimeoutMs );

        if( ready > 0 )
        {
            if( pollSet[0].revents != 0 )
            {
                drainWakeFd();
            }

            // Invoke platform callbacks unlocked, before re-acquiring. Index 0 is the wakeup FD and
            // is deliberately skipped -- it is ours, not a user source.
            // Resolved immediately before it is called, so a source unregistered by an earlier callback in
            // this same round is not then called itself. That is what makes unregisterEventSource()
            // synchronous on this thread, and it is the rule the dispatch batches follow too: re-check
            // under the lock, act outside it.
            for( size_t i = 1; i < pollSet.size(); ++i )
            {
                if( pollSet[i].revents == 0 )
                {
                    continue;
                }

                const EventSourceCallback callback
                    = callbackIfStillRegistered( pollSet[i].fd, generations[i] );
                if( callback )
                {
                    callback( pollSet[i].revents );
                }
            }
        }
        else if( ready < 0 && errno != EINTR )
        {
            // EINTR is routine (a signal arrived); treat the wait as finished and let the caller
            // re-evaluate. Anything else means the descriptor set is malformed, which would spin.
            std::fprintf( stderr, "EventDispatcherLinux: poll() failed (%d)\n", errno );
        }

        aLock.lock();
    }

    //! Copies the callback of the registration identified by @p aFd and @p aGeneration.
    //!
    //! Empty if that registration is gone -- unregistered, or replaced by a later
    //! registerEventSource() on the same descriptor. Takes mMutex itself, since it is called from
    //! waitForEvents() with the lock released.
    EventDispatcherLinux::EventSourceCallback EventDispatcherLinux::callbackIfStillRegistered
        (
        int aFd,                        //!< The descriptor that poll() reported ready.
        unsigned long long aGeneration  //!< The registration the poll set was built from.
        )
    {
        std::lock_guard<std::mutex> lock( mMutex );
        for( const auto& source : mSources )
        {
            if( source.mFd == aFd && source.mGeneration == aGeneration )
            {
                return source.mCallback;
            }
        }
        return {};
    }

    //! Wakes a thread blocked in poll(). Thread-safe and non-blocking.
    void EventDispatcherLinux::wakeWaiter()
    {
        // Keep the base behaviour too: waitForEvents() falls back to the condition variable when
        // the eventfd could not be created, and a waiter there still has to be notified.
        EventDispatcherDefault::wakeWaiter();

        if( mWakeFd < 0 )
        {
            return;
        }

        // Only the first wake of a batch performs the write; drainWakeFd() clears the flag once the
        // loop has actually observed it. Runs on every post, so skipping a redundant syscall here
        // is worth the atomic.
        if( mWakePending.exchange( true ) )
        {
            return;
        }

        const uint64_t one = 1;
        if( ::write( mWakeFd, &one, sizeof( one ) ) != static_cast<ssize_t>( sizeof( one ) ) )
        {
            // The write failed, so no wakeup is coming and the flag would wedge the fast path shut
            // forever. Clear it so the next caller retries rather than assuming a wake is pending.
            mWakePending.store( false );
        }
    }

    //! Clears the wakeup descriptor so the next poll() blocks again.
    void EventDispatcherLinux::drainWakeFd()
    {
        uint64_t value = 0;
        // One read drains the counter however many writes were coalesced into it.
        const ssize_t bytes = ::read( mWakeFd, &value, sizeof( value ) );
        ( void )bytes;

        // Cleared only after the read, never before: a wakeWaiter() racing this point either wrote
        // before the read (and is therefore already accounted for) or finds the flag clear and
        // performs its own write, so no wakeup can be dropped.
        mWakePending.store( false );
    }
}
