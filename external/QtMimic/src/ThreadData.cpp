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

}  // namespace QtMimic
