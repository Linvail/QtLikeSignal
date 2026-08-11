//! @file
//!
//! Implementation of QtMimic::Object (thread affinity + connection lifetime
//! management).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Object.hpp"

#include <climits>
#include <cstdio>
#include <deque>

namespace QtMimic
{

    namespace
    {
        //! Process-wide pool of timer ids, handing out reusable ids rather than an ever-rising
        //! count. Qt does the same with a lock-free QFreeList capped at 2^24 simultaneous timers; a
        //! mutex and a deque is the proportionate equivalent here, since an id is taken once per
        //! startTimer() rather than on any hot path.
        //!
        //! Reuse is FIFO, deliberately. A freed id going straight back out (LIFO) would make the
        //! narrowest recycling hazard trivially reachable: a handler that kills one timer and
        //! starts another would get the same id back immediately, and any delivery for the old
        //! timer still in flight would then match the new one. Taking the oldest free id instead
        //! means an id is only reused after every other freed id has been.
        struct TimerIdPool
        {
            //! Takes an id, reusing the oldest freed one if there is any.
            //! @return the id, or -1 if the space of ids is exhausted.
            static int allocate()
            {
                std::lock_guard<std::mutex> locker( sMutex );
                if( !sFree.empty() )
                {
                    const int id = sFree.front();
                    sFree.pop_front();
                    return id;
                }
                // Never hand out 0 or a negative value: -1 is startTimer()'s failure sentinel and
                // the value Timer::stop() tests against, so an id colliding with it would be
                // indistinguishable from "no timer".
                if( sNextFresh <= 0 )
                {
                    return -1;
                }
                const int id = sNextFresh;
                sNextFresh = ( sNextFresh == INT_MAX ) ? -1 : sNextFresh + 1;
                return id;
            }

            //! Returns an id to the pool.
            static void release
                (
                int aTimerId
                )
            {
                if( aTimerId <= 0 )
                {
                    return;
                }
                std::lock_guard<std::mutex> locker( sMutex );
                sFree.push_back( aTimerId );
            }

            static std::mutex sMutex;      //!< Guards both members below.
            static std::deque<int> sFree;  //!< Freed ids, oldest first.
            static int sNextFresh;         //!< Next never-yet-issued id.
        };

        std::mutex TimerIdPool::sMutex;
        std::deque<int> TimerIdPool::sFree;
        int TimerIdPool::sNextFresh = 1;
    }  // namespace

    //================================================================
    // Object
    //================================================================

    //! @brief Constructor - creates an Object bound to a thread with thread affinity.
    //! @param aThread The Thread this object lives in. If null (the default), the object
    //!        lives in the current thread (Thread::currentThread()). If no thread is current,
    //!        connections to this object behave as direct connections.
    Object::Object
        (
        Thread* aThread
        )
    // Store the thread's ThreadData, not the Thread itself: it outlives the Thread, so this
    // object's affinity can never become a dangling pointer. currentData() auto-adopts the
    // calling thread exactly as current() does when no thread is given. Held in an Affinity
    // box read at emit time, so a later moveToThread() redirects existing connections too.
        : Object( aThread ? aThread->data() : Thread::currentData() )
    {
    }

    //! @brief Constructor for internal helpers that already hold stable ThreadData.
    Object::Object
        (
        std::shared_ptr<ThreadData> aThreadData
        )
        : mAffinity( std::make_shared<Affinity>( std::move( aThreadData ) ) )
        , mLife( std::make_shared<int>( 0 ) )
    {
    }

    //! @brief The thread this object currently lives in, or nullptr if it has none -- or if the
    //! Thread it lived in has since been destroyed. This never returns a dangling pointer: the
    //! affinity is stored as a ThreadData (which outlives its Thread), exactly as Qt stores a
    //! refcounted QThreadData rather than a QThread*. Re-read it rather than caching it.
    Thread* Object::thread() const
    {
        const std::shared_ptr<ThreadData> data = threadDataRef();
        return data ? data->thread() : nullptr;
    }

    //! @brief Re-assign this object's thread affinity, following Qt6's QObject::moveToThread rules.
    //! The move is "push-only": the caller must be on the object's current affinity thread, so an
    //! object can be pushed to another thread but never pulled from an arbitrary one. The single
    //! exception, exactly as in Qt, is that an object with no affinity may be pulled to the calling
    //! thread. Affinity is resolved at emit time, so events posted after a successful move --
    //! including through connections made BEFORE it -- are delivered to @p aThread. Passing nullptr
    //! dissociates the object, after which thread() returns nullptr.
    //! @return true if the object now lives in @p aThread (including when it already did, which is a
    //!         successful no-op); false if the move was refused because the caller is not on the
    //!         object's affinity thread. On a false return the affinity is left unchanged.
    bool Object::moveToThread
        (
        Thread* aThread
        )
    {
        Thread* const currentAffinity = thread();
        if( currentAffinity == aThread )
        {
            return true; // already there -- a successful no-op, exactly as Qt returns true
        }

        // Push-only, with Qt's one exception: a thread-less object may be pulled to the caller.
        Thread* const callingThread = Thread::currentThread();
        const bool pullNoAffinityToCaller = ( currentAffinity == nullptr )
            && ( aThread == callingThread );
        if( !pullNoAffinityToCaller && currentAffinity != callingThread )
        {
            return false;
        }

        // Take any running timers off the outgoing thread before the affinity changes. Qt documents
        // this behaviour ("all active timers for the object will be reset ... stopped in the current
        // thread and restarted, with the same interval, in the targetThread"); without it the timers
        // would keep firing on the thread the object just left, delivering timerEvent() somewhere it
        // must not run. The caller is on that outgoing thread (push-only, checked above), so this
        // cannot race its loop's own delivery pass.
        std::vector<ThreadData::TimerRegistration> timersToMove;
        if( const std::shared_ptr<ThreadData> oldData = threadDataRef() )
        {
            timersToMove = oldData->takeTimersForReceiver( this );
        }

        mAffinity->setData( aThread ? aThread->data() : std::shared_ptr<ThreadData>() );

        if( !timersToMove.empty() )
        {
            // Registered directly rather than posted to the destination: registerTimer() takes the
            // destination's mutex and wakes its loop, so it is already safe from here, and posting
            // would leave a window in which the timers exist on neither thread.
            const std::shared_ptr<ThreadData> newData = threadDataRef();
            for( const auto& timer : timersToMove )
            {
                if( newData )
                {
                    newData->registerTimer( timer.mTimerId, timer.mIntervalMs, this );
                }
                else
                {
                    // moveToThread(nullptr) leaves nothing to service the timers, so they are gone
                    // rather than merely paused, and their ids go back to the pool. The object's own
                    // record has to be cleared too, or a later killTimer() would double-release.
                    forgetTimerId( timer.mTimerId );
                    TimerIdPool::release( timer.mTimerId );
                }
            }
        }

        return true;
    }

    //! @brief Called when one of this object's timers comes due. Does nothing by default.
    void Object::timerEvent
        (
        TimerEvent* aEvent
        )
    {
        ( void )aEvent;
    }

    //! @brief Start a repeating timer delivering timerEvent() to this object every @p aIntervalMs.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** The timer is owned by
    //! that thread's mailbox and only that thread's event loop can deliver it, so registering from
    //! elsewhere would install a timer whose events the caller is not positioned to receive.
    //! Rejected with a warning on stderr instead, matching Qt, whose QObject::startTimer() likewise
    //! refuses ("Timers cannot be started from another thread"). To start a timer for an object
    //! living in another thread, get onto that thread first.
    //!
    //! An interval of 0 means "fire on every pass of the event loop", as in Qt.
    //! @return the new timer's id, or -1 if the timer could not be started.
    int Object::startTimer
        (
        int aIntervalMs  //!< Interval in milliseconds; 0 fires on every loop pass.
        )
    {
        if( aIntervalMs < 0 )
        {
            std::fprintf( stderr, "Object::startTimer: interval cannot be negative\n" );
            return -1;
        }

        if( thread() != Thread::currentThread() )
        {
            std::fprintf( stderr,
                "Object::startTimer: timers cannot be started from another thread\n" );
            return -1;
        }

        const std::shared_ptr<ThreadData> data = threadDataRef();
        if( !data )
        {
            // Detached by moveToThread(nullptr): there is no mailbox to schedule against, and no
            // loop that would ever drain one.
            std::fprintf( stderr,
                "Object::startTimer: object has no thread, so the timer cannot be started\n" );
            return -1;
        }

        // Only consume an id once the timer is actually going to be registered.
        const int timerId = TimerIdPool::allocate();
        if( timerId < 0 )
        {
            std::fprintf( stderr, "Object::startTimer: no timer ids left\n" );
            return -1;
        }

        data->registerTimer( timerId, aIntervalMs, this );
        {
            // Recorded so ~Object() can hand the id back even if the timer is never killed.
            std::lock_guard<std::mutex> locker( mRunningTimerIdsMutex );
            mRunningTimerIds.push_back( timerId );
        }
        return timerId;
    }

    //! @brief Stop the timer with id @p aTimerId.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, for the same reason as
    //! startTimer(). Calls from another thread are rejected with a warning and do nothing. An id
    //! this object does not own is ignored.
    void Object::killTimer
        (
        int aTimerId  //!< Id returned by startTimer().
        )
    {
        if( thread() != Thread::currentThread() )
        {
            std::fprintf( stderr,
                "Object::killTimer: timers cannot be stopped from another thread\n" );
            return;
        }

        const bool wasOurs = forgetTimerId( aTimerId );

        if( const std::shared_ptr<ThreadData> data = threadDataRef() )
        {
            data->unregisterTimer( aTimerId );
        }

        // Released only after unregisterTimer() has both dropped the timer and cancelled anything
        // it had already collected for delivery, so the id cannot be reissued while something still
        // naming it is in flight. Only ids this object actually owns go back, so a bogus or
        // repeated killTimer() cannot inject a duplicate into the pool.
        if( wasOurs )
        {
            TimerIdPool::release( aTimerId );
        }
    }

    //! @brief Drop @p aTimerId from this object's running-timer record.
    //! @return true if the id was there, i.e. this object owns it and the caller may release it.
    bool Object::forgetTimerId
        (
        int aTimerId
        )
    {
        std::lock_guard<std::mutex> locker( mRunningTimerIdsMutex );
        auto it = std::find( mRunningTimerIds.begin(), mRunningTimerIds.end(), aTimerId );
        if( it == mRunningTimerIds.end() )
        {
            return false;
        }
        mRunningTimerIds.erase( it );
        return true;
    }

    //! @brief This object's descriptive name, empty unless one was set.
    //!
    //! **Not thread-safe**, exactly as QObject::objectName() is not; see mObjectName.
    std::string Object::objectName() const
    {
        return mObjectName;
    }

    //! @brief Give this object a descriptive name, for logs and diagnostics. Nothing keys off it.
    //!
    //! **Not thread-safe**; see objectName().
    void Object::setObjectName
        (
        const std::string& aName  //!< The new object name.
        )
    {
        mObjectName = aName;
    }

    //! @brief Number of live connections where this object is the receiver (diagnostics/tests).
    std::size_t Object::incomingConnectionCount() const
    {
        std::lock_guard<std::mutex> locker( mIncomingMutex );
        return mIncoming.size();
    }

    //! @brief Queue the object's destruction to its affinity thread.
    //! Qt-like QObject::deleteLater(). If the object has no thread, or its thread has stopped/gone
    //! (post() refuses), deletion happens immediately (a thread-affinity violation is preferable to
    //! a deferred delete that would never run and leak the object).
    void Object::deleteLater()
    {
        // Qt-like behavior: multiple deleteLater() calls coalesce into one.
        if( mDeleteLaterPosted.exchange( true ) )
        {
            return;
        }

        // Post through the affinity ThreadData (never a raw Thread*): it outlives its Thread and its
        // post() returns false once the loop has stopped. A null holder means the object was never
        // given a thread, so fall back to the calling thread (like a parent-less QObject).
        std::shared_ptr<ThreadData> targetData = threadDataRef();
        if( !targetData )
        {
            targetData = Thread::currentData();
        }

        std::weak_ptr<int> life = mLife;
        Object* self = this;

        const bool queued = targetData && targetData->post( [life, self]()
            {
                // Skip if the object was already destroyed another way.
                if( !life.expired() )
                {
                    delete self;
                }
            } );

        if( !queued )
        {
            // The target thread's loop has already stopped (or gone away), so post() refused the
            // task rather than accepting one nothing will ever run. Doing nothing here would leak
            // self forever, which is strictly worse than the thread-affinity violation of deleting
            // it synchronously, right here, on whichever thread called deleteLater(). Not the normal
            // path: it only triggers for a thread that has already finished or gone away.
            delete self;
        }
    }

    //! @brief Destructor - disconnects all incoming connections and invalidates the life token.
    //! This prevents any queued (not-yet-run) slot invocations from running after the
    //! object is destroyed.
    Object::~Object()
    {
        // Invalidate first so queued slots that check the token will skip. This
        // also makes each connection's Cleanup destructor a no-op (it sees the life
        // token expired), so disconnect() below won't re-enter mIncomingMutex.
        mLife.reset();

        // Strip this object's timers before it goes away. Unlike a queued slot, a timer carries no
        // life token to check -- the mailbox holds a raw Object* and the loop calls timerEvent() on
        // it directly -- so this is the only thing standing between a still-running timer and a call
        // into freed memory. It cancels anything already collected for delivery, too.
        std::vector<int> outstandingTimerIds;
        {
            std::lock_guard<std::mutex> locker( mRunningTimerIdsMutex );
            outstandingTimerIds.swap( mRunningTimerIds );
        }
        if( const std::shared_ptr<ThreadData> data = threadDataRef() )
        {
            data->removeTimersForReceiver( this );
        }
        for( const int timerId : outstandingTimerIds )
        {
            TimerIdPool::release( timerId );
        }

        // Disconnect every connection where this object is the receiver so the
        // signal no longer references this (soon to be destroyed) object.
        std::lock_guard<std::mutex> locker( mIncomingMutex );
        for( auto& handle : mIncoming )
        {
            handle.disconnect();
        }
        mIncoming.clear();
    }

    //! @return a strong reference to this object's current affinity. Safe from any thread while
    //! moveToThread() may be reassigning it (Affinity locks internally).
    std::shared_ptr<ThreadData> Object::threadDataRef() const
    {
        return mAffinity->data();
    }

    //================================================================
    // Object::Cleanup
    //================================================================

    //! Constructor
    Object::Cleanup::Cleanup
        (
        Object* aOwner,
        std::weak_ptr<int> aLife
        )
        : mOwner( aOwner )
        , mLife( aLife )
    {
    }

    //! Destructor - prune this connection from the receiver's mIncoming.
    Object::Cleanup::~Cleanup()
    {
        if( mLife.expired() )
        {
            return; // receiver gone (or being destroyed) - it clears mIncoming.
        }
        std::lock_guard<std::mutex> locker( mOwner->mIncomingMutex );
        auto& v = mOwner->mIncoming;
        v.erase( std::remove( v.begin(), v.end(), mHandle ), v.end() );
    }

} // namespace QtMimic
