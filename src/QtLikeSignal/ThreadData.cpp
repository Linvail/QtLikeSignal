// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

#include "QtLikeSignal/ThreadData.hpp"

#include "QtLikeSignal/Event.hpp"

#include <algorithm>

namespace QtLikeSignal
{
    //! Frees any events parked for a dispatcher that never arrived.
    ThreadData::~ThreadData()
    {
        for( const ParkedEvent& parked : mParkedEvents )
        {
            delete parked.mEvent;
        }
    }

    //! @return the Thread this data describes, or nullptr once that Thread has been destroyed.
    //! Safe to call at any time: the ThreadData itself is kept alive by whoever holds it.
    Thread* ThreadData::thread() const
    {
        return mThread.load( std::memory_order_acquire );
    }

    //! Set by Thread's constructor and cleared by ~Thread(). Only Thread may touch it.
    void ThreadData::setThread
        (
        Thread* aThread    //!< The owning thread, or nullptr once it is gone.
        )
    {
        mThread.store( aThread, std::memory_order_release );
    }

    //! @return true while the owning thread's body is executing.
    //!
    //! Safe to call from any thread at any time, including once that Thread has been destroyed --
    //! it reports false, because the run body clears this before the Thread can be torn down.
    //! Callers holding only this ThreadData can therefore ask "is that thread still running?"
    //! without dereferencing a Thread* that may already have been freed.
    bool ThreadData::isThreadRunning() const
    {
        return mThreadRunning.load( std::memory_order_acquire );
    }

    //! Records whether the owning thread's body is executing. Only Thread may touch it.
    void ThreadData::setThreadRunning
        (
        bool aRunning    //!< True on start, false once the run body has finished.
        )
    {
        mThreadRunning.store( aRunning, std::memory_order_release );
    }

    //! Gets a strong reference to this thread's dispatcher, or nullptr if none is installed.
    //! Thread-safe.
    std::shared_ptr<AbstractEventDispatcher> ThreadData::dispatcher() const
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        return mDispatcher;
    }

    //! Installs or clears this thread's dispatcher, and hands it anything parked. Thread-safe.
    void ThreadData::setDispatcher
        (
        std::shared_ptr<AbstractEventDispatcher> aDispatcher    //!< Dispatcher to install; nullptr clears it.
        )
    {
        std::vector<ParkedEvent> parked;
        std::shared_ptr<AbstractEventDispatcher> installed;

        {
            std::lock_guard<std::mutex> lock( mDispatcherMutex );
            mDispatcher = std::move( aDispatcher );
            installed   = mDispatcher;
            if( installed )
            {
                parked.swap( mParkedEvents );
            }
        }

        // Posted with mDispatcherMutex released. postEvent() ends in wakeWaiter(), which may run
        // the thread's wake callback -- user code, which is free to call back in here.
        for( const ParkedEvent& event : parked )
        {
            // postEvent() deletes the event itself when it refuses, so a dispatcher that is already
            // closing loses nothing and leaks nothing.
            installed->postEvent( event.mReceiver, event.mEvent );
        }
    }

    //! Hands back the dispatcher to post to, or takes ownership of @p aEvent until one exists.
    std::shared_ptr<AbstractEventDispatcher> ThreadData::dispatcherOrPark
        (
        Object* aReceiver,  //!< The event's receiver.
        Event* aEvent       //!< The event; ownership passes here only when it is parked.
        )
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        if( mDispatcher )
        {
            return mDispatcher;
        }
        mParkedEvents.push_back( { aReceiver, aEvent } );
        return nullptr;
    }

    //! Drops any parked events for @p aReceiver. Thread-safe.
    void ThreadData::removeParkedEventsFor
        (
        Object* aReceiver  //!< The receiver whose parked events should be dropped.
        )
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        auto it = std::remove_if( mParkedEvents.begin(),
            mParkedEvents.end(),
            [aReceiver]( const ParkedEvent& aParked )
            {
                if( aParked.mReceiver == aReceiver )
                {
                    delete aParked.mEvent;
                    return true;
                }
                return false;
            } );
        mParkedEvents.erase( it, mParkedEvents.end() );
    }
}
