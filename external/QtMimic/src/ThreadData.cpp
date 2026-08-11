//! @file
//!
//! Per-thread state that outlives its Thread object.
//!
//! ThreadData is the key to safe thread affinity without dangling pointers.
//! Objects hold shared_ptr<ThreadData>, not raw Thread*. When the Thread
//! is destroyed, ~Thread() nulls the back-pointer, so any surviving holder
//! sees thread() == nullptr rather than a use-after-free. This is exactly
//! how Qt's QObject::thread() works internally.
//!
//! ThreadData also OWNS the event mailbox (the pending-task queue and its
//! synchronization), mirroring Qt's QThreadData::postEventList. Posting a task
//! therefore goes through the ThreadData -- which the poster keeps alive with a
//! shared_ptr -- and never dereferences a Thread* that a concurrent ~Thread()
//! could free.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "ThreadData.hpp"

#include "Object.hpp"
#include "TimerEvent.hpp"

#include <algorithm>

namespace QtMimic
{

    //! @return the Thread this data describes, or nullptr once that Thread has been destroyed.
    //! Safe to call at any time: the ThreadData itself is kept alive by whoever holds it.
    Thread* ThreadData::thread() const
    {
        return mThread.load( std::memory_order_acquire );
    }

    //! @brief Queue a task into this thread's event mailbox. Thread-safe and callable from any
    //! thread. Crucially, this NEVER dereferences the Thread: the queue lives here, in the
    //! ThreadData, which the caller keeps alive with its shared_ptr -- so it is safe even if the
    //! Thread is being destroyed concurrently. This mirrors Qt posting into
    //! QThreadData::postEventList rather than through a QThread*.
    //! @return true if the task was queued; false if the mailbox has stopped accepting (the
    //!         loop has finished or the Thread is gone) or @p aTask was empty. A false return
    //!         lets callers with a safe fallback (e.g. deleteLater()) avoid stranding the task.
    bool ThreadData::post
        (
        std::function<void()> aTask
        )
    {
        if( !aTask )
        {
            return false;
        }

        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> locker( mMutex );
            if( !mAccepting )
            {
                return false;
            }
            mTasks.push_back( std::move( aTask ) );
            wake = mWakeCb;     // copy under the lock; invoke unlocked to avoid re-entrancy deadlock
        }
        mWake.notify_one();
        if( wake )
        {
            wake();
        }
        return true;
    }

    //! @brief Set by Thread's constructor and cleared by ~Thread(). Only Thread may touch it.
    void ThreadData::setThread
        (
        Thread* aThread
        )
    {
        mThread.store( aThread, std::memory_order_release );
    }

    //! @brief Ask the loop to stop (drain, then exit) and wake any waiter. Called by Thread::quit().
    void ThreadData::requestStop()
    {
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> locker( mMutex );
            mRunning = false;
            wake = mWakeCb;
        }
        mWake.notify_all();
        if( wake )
        {
            wake();
        }
    }

    //! @brief Reset the mailbox for a (re)start: keep running and accept tasks again.
    void ThreadData::prepareForRun()
    {
        std::lock_guard<std::mutex> locker( mMutex );
        mRunning = true;
        mAccepting = true;
    }

    //! @brief Refuse further posts. Called once the Thread is gone so post() returns false rather than
    //! stranding tasks in a queue nothing will ever drain.
    void ThreadData::stopAccepting()
    {
        std::lock_guard<std::mutex> locker( mMutex );
        mAccepting = false;
    }

    //! @brief Accept posts again, for a thread that outlives the loop that just stopped.
    //!
    //! The counterpart to stopAccepting(). loop() refuses further posts as it commits to stopping,
    //! which is right for a worker whose thread is ending -- nothing would ever drain them. It is
    //! wrong for a thread that merely finished one exec() cycle and carries on living: its mailbox
    //! would refuse posts forever after, and deleteLater() would silently fall back to deleting on
    //! the spot instead of deferring.
    void ThreadData::resumeAccepting()
    {
        std::lock_guard<std::mutex> locker( mMutex );
        mAccepting = true;
    }

    //! @brief Queue an object's destruction, to run the next time this thread drains its deferred
    //! deletes. Kept out of the ordinary mailbox; see mDeferredDeletes for why.
    //! @return true if it was queued; false if the mailbox has stopped accepting, which leaves the
    //!         caller to fall back to deleting on the spot rather than leaking.
    bool ThreadData::postDeferredDelete
        (
        std::function<void()> aDelete
        )
    {
        if( !aDelete )
        {
            return false;
        }

        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> locker( mMutex );
            if( !mAccepting )
            {
                return false;
            }
            mDeferredDeletes.push_back( std::move( aDelete ) );
            wake = mWakeCb;
        }
        mWake.notify_one();
        if( wake )
        {
            wake();
        }
        return true;
    }

    //! @brief Run every queued destruction, and any queued by those destructors in turn.
    //!
    //! Drained in a loop rather than once: a destructor may deleteLater() something else -- an
    //! object owning another -- and stopping after one pass would leak whatever the last round
    //! queued. Qt drains repeatedly for the same reason.
    //!
    //! Each batch is taken under the lock and run without it, because ~Object() reaches back in
    //! here to strip its timers and would deadlock on a non-recursive mutex this thread already
    //! holds.
    void ThreadData::processDeferredDeletes()
    {
        for(;;)
        {
            std::deque<std::function<void()> > batch;
            {
                std::lock_guard<std::mutex> locker( mMutex );
                batch.swap( mDeferredDeletes );
            }

            if( batch.empty() )
            {
                return;
            }

            for( auto& deleteOne : batch )
            {
                if( deleteOne )
                {
                    deleteOne();
                }
            }
        }
    }

    //! @brief Install the external-loop wake callback (used by adopted threads with their own loop).
    void ThreadData::setWakeCallback
        (
        std::function<void()> aWake
        )
    {
        std::lock_guard<std::mutex> locker( mMutex );
        mWakeCb = std::move( aWake );
    }

    //! @brief Atomically take all currently-queued tasks (for processEvents()).
    std::deque<std::function<void()> > ThreadData::takeAll()
    {
        std::deque<std::function<void()> > batch;
        std::lock_guard<std::mutex> locker( mMutex );
        batch.swap( mTasks );
        return batch;
    }

    //! @brief Schedule a timer to deliver timerEvent() to @p aReceiver every @p aIntervalMs.
    //!
    //! Re-registering an id that is already scheduled replaces it, which is what makes restarting a
    //! running Timer a single call rather than kill-then-start. Thread-safe, though in practice it
    //! is called on the thread the timer will be serviced on -- Object::startTimer() is
    //! thread-confined -- with the one exception of Object::moveToThread(), which registers on the
    //! destination thread from the source one. That is why the wake below is not optional: it is
    //! what makes a loop already asleep on an older deadline notice the new timer, and registering
    //! is the only timer operation that can bring that deadline forward. See mTimersChanged.
    void ThreadData::registerTimer
        (
        int aTimerId,
        int aIntervalMs,
        Object* aReceiver
        )
    {
        if( !aReceiver || aIntervalMs < 0 )
        {
            return;
        }

        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> locker( mMutex );

            TimerData td;
            td.mTimerId    = aTimerId;
            td.mIntervalMs = aIntervalMs;
            td.mReceiver   = aReceiver;
            td.mNextFire   = std::chrono::steady_clock::now()
                + std::chrono::milliseconds( aIntervalMs );

            auto existing = std::find_if( mTimers.begin(), mTimers.end(),
                [aTimerId]( const TimerData& aTd )
                {
                    return aTd.mTimerId == aTimerId;
                } );
            if( existing != mTimers.end() )
            {
                *existing = td;
            }
            else
            {
                mTimers.push_back( td );
            }
            mTimersChanged = true;
            wake = mWakeCb;  // copy under the lock; invoke unlocked, exactly as post() does
        }
        mWake.notify_all();

        // ...and the same wake again for a loop that is not blocked on our condition variable at
        // all: one parked in an external waiter, or pumping us from its own.
        if( wake )
        {
            wake();
        }
    }

    //! @brief Cancel the timer with id @p aTimerId.
    //!
    //! Also cancels it out of the batch currently being delivered, if there is one. That is the
    //! case this has to get right: a handler that kills a sibling timer runs after that sibling's
    //! entry was already collected, so without this the kill would be ignored and the dead timer
    //! would fire one last time.
    //!
    //! Does not wake the loop, and deliberately: see the note on mTimersChanged for why only
    //! registerTimer() ever has to.
    //! @return true if a timer with that id was registered.
    bool ThreadData::unregisterTimer
        (
        int aTimerId
        )
    {
        std::lock_guard<std::mutex> locker( mMutex );

        if( mDispatchingTimerBatch )
        {
            for( auto& entry : *mDispatchingTimerBatch )
            {
                if( entry.mTimerId == aTimerId )
                {
                    entry.mReceiver = nullptr;
                }
            }
        }

        auto it = std::remove_if( mTimers.begin(), mTimers.end(),
            [aTimerId]( const TimerData& aTd )
            {
                return aTd.mTimerId == aTimerId;
            } );
        if( it == mTimers.end() )
        {
            return false;
        }
        mTimers.erase( it, mTimers.end() );
        return true;
    }

    //! @brief Drop every timer targeting @p aReceiver, including any already collected for
    //! delivery. Called by ~Object(): the timer list holds raw Object pointers, so this is what
    //! stops the loop calling timerEvent() on freed memory.
    void ThreadData::removeTimersForReceiver
        (
        Object* aReceiver
        )
    {
        if( !aReceiver )
        {
            return;
        }

        std::lock_guard<std::mutex> locker( mMutex );

        if( mDispatchingTimerBatch )
        {
            for( auto& entry : *mDispatchingTimerBatch )
            {
                if( entry.mReceiver == aReceiver )
                {
                    entry.mReceiver = nullptr;
                }
            }
        }

        auto it = std::remove_if( mTimers.begin(), mTimers.end(),
            [aReceiver]( const TimerData& aTd )
            {
                return aTd.mReceiver == aReceiver;
            } );
        mTimers.erase( it, mTimers.end() );
    }

    //! @brief Unregister @p aReceiver's timers and hand them back so they can be re-registered on
    //! another thread. Used by Object::moveToThread(), which Qt documents as resetting an object's
    //! timers onto the target thread. The ids come back rather than being released, so a Timer's
    //! cached id still matches the events it receives after the move.
    //! @return the removed registrations, empty if the receiver had none.
    std::vector<ThreadData::TimerRegistration> ThreadData::takeTimersForReceiver
        (
        Object* aReceiver
        )
    {
        std::vector<TimerRegistration> taken;
        if( !aReceiver )
        {
            return taken;
        }

        std::lock_guard<std::mutex> locker( mMutex );

        if( mDispatchingTimerBatch )
        {
            for( auto& entry : *mDispatchingTimerBatch )
            {
                if( entry.mReceiver == aReceiver )
                {
                    entry.mReceiver = nullptr;
                }
            }
        }

        auto it = std::remove_if( mTimers.begin(), mTimers.end(),
            [aReceiver, &taken]( const TimerData& aTd )
            {
                if( aTd.mReceiver != aReceiver )
                {
                    return false;
                }
                taken.push_back( { aTd.mTimerId, aTd.mIntervalMs } );
                return true;
            } );
        mTimers.erase( it, mTimers.end() );
        return taken;
    }

    //! @brief Deliver timerEvent() for every timer that has come due, and re-arm them.
    //!
    //! Runs on the thread that owns this data, from its loop (or from Thread::processEvents() when
    //! an adopted thread pumps us from its own loop). Handlers run with mMutex released so they may
    //! start timers, kill timers, post tasks, or destroy objects.
    void ThreadData::dispatchExpiredTimers()
    {
        std::vector<ExpiredTimer> batch;

        {
            std::lock_guard<std::mutex> locker( mMutex );

            const auto now = std::chrono::steady_clock::now();
            for( auto& t : mTimers )
            {
                if( now < t.mNextFire )
                {
                    continue;
                }
                batch.push_back( { t.mReceiver, t.mTimerId } );

                // Advance from the deadline that just elapsed, not from the moment we noticed it.
                // `now` is whenever this pass got around to looking, so re-arming from it would fold
                // that lateness into the cadence permanently -- one pass 20 ms late and every fire
                // thereafter is 20 ms off, compounding under load.
                t.mNextFire += std::chrono::milliseconds( t.mIntervalMs );

                if( t.mNextFire < now )
                {
                    // The loop was blocked for longer than a whole interval. Keeping the original
                    // cadence would mean firing repeatedly to work off a backlog nobody asked for,
                    // so give up on it and resynchronise. Qt makes the same two choices in the same
                    // order (calculateNextTimeout(), qtimerinfo_unix.cpp).
                    t.mNextFire = now + std::chrono::milliseconds( t.mIntervalMs );
                }
            }

            if( batch.empty() )
            {
                return;
            }

            // Publish before delivering anything, so a handler can cancel the siblings behind it.
            mDispatchingTimerBatch = &batch;
        }

        // Retracts the published batch however this function leaves, so a pointer to a dead local
        // can never outlive the pass.
        struct BatchRetractor
        {
            ~BatchRetractor()
            {
                std::lock_guard<std::mutex> locker( mOwner->mMutex );
                mOwner->mDispatchingTimerBatch = nullptr;
            }

            ThreadData* mOwner;
        } batchRetractor { this };

        for( std::size_t i = 0; i < batch.size(); ++i )
        {
            // Claim the entry under the lock, clearing the slot as we take it. That hands ownership
            // over in one step: either a canceller got here first and we see nullptr, or we did and
            // it finds nothing left to cancel.
            ExpiredTimer entry { nullptr, 0 };
            {
                std::lock_guard<std::mutex> locker( mMutex );
                entry = batch[i];
                batch[i].mReceiver = nullptr;
            }

            if( !entry.mReceiver )
            {
                continue;
            }

            TimerEvent event( entry.mTimerId );
            entry.mReceiver->timerEvent( &event );
        }
    }

    //! @brief Whether any timer is already due as of @p aNow. Call with mMutex held.
    bool ThreadData::hasExpiredTimers
        (
        std::chrono::steady_clock::time_point aNow
        ) const
    {
        return std::any_of( mTimers.begin(), mTimers.end(),
            [aNow]( const TimerData& aTd )
            {
                return aNow >= aTd.mNextFire;
            } );
    }

    //! @brief How long the loop may block before the next timer comes due. Call with mMutex held.
    //! @return milliseconds until the earliest deadline (0 if one has already passed), or -1 when
    //!         no timer is registered and the loop may therefore block indefinitely.
    int ThreadData::nextTimerTimeoutMs
        (
        std::chrono::steady_clock::time_point aNow
        ) const
    {
        if( mTimers.empty() )
        {
            return -1;
        }

        auto earliest = mTimers.front().mNextFire;
        for( const auto& t : mTimers )
        {
            if( t.mNextFire < earliest )
            {
                earliest = t.mNextFire;
            }
        }

        if( earliest <= aNow )
        {
            return 0;
        }

        // Round up: truncating would wake us a fraction of a millisecond early, find nothing due,
        // and go straight back to sleep for the remainder.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            earliest - aNow + std::chrono::microseconds( 999 ) );
        return static_cast<int>( remaining.count() );
    }

}  // namespace QtMimic
