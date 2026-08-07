//! @file
//!
//! Implementation of QtMimic::Thread (event loop).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Thread.hpp"

#include <cstdio>
#include <memory>

namespace QtMimic
{

    namespace
    {
        //! Per-thread pointer to the Thread whose event loop is running on this
        //! thread. Mirrors Qt's per-thread QThreadData, letting a parent-less object
        //! discover the thread it is constructed on. Set in run() (which executes on
        //! this thread) and cleared on loop exit. Lock-free O(1) lookup.
        thread_local Thread* tCurrentThread = nullptr;

        //! Owns the dummy Thread auto-created to represent a native thread that was
        //! not started via Thread. Lives only for the duration of that native thread
        //! (torn down at thread exit); mirrors Qt's "adopted" QThread. Has no running
        //! event loop, so it gives affinity (isCurrent) and queued tasks accumulate
        //! only if someone exec()s it.
        //!
        //! Sole owner: nothing else holds the dummy Thread. Objects constructed on
        //! this thread capture its ThreadData (a shared_ptr<ThreadData>), never the
        //! Thread itself, so this thread_local can safely be a unique_ptr. When the
        //! native thread exits and this dummy is destroyed, ~Thread() nulls the
        //! ThreadData back-pointer, so any surviving holder sees thread() == nullptr
        //! rather than a dangling Thread* -- the use-after-free that motivated the
        //! ThreadData model (heap-use-after-free in isCurrent(), confirmed under
        //! AddressSanitizer) can no longer happen.
        thread_local std::unique_ptr<Thread> tAdoptedThread;
    }         // namespace

    //! @brief Constructor - initialize a Thread with an optional name.
    //! The thread is not started until start() or exec() is called.
    Thread::Thread
        (
        const std::string& aName
        )
        : mData( std::make_shared<ThreadData>() )
        , mName( aName )
    {
        mData->setThread( this );
    }

    //! @brief Destructor - stops the event loop (draining any pending tasks) and joins the thread.
    //! Also nulls the ThreadData back-pointer so any surviving Object still holding this
    //! thread's data sees thread() == nullptr instead of a dangling pointer.
    Thread::~Thread()
    {
        quit();
        join();

        // Refuse any further posts now that this Thread is gone: nothing will ever drain the mailbox
        // again, so post() must return false (letting deleteLater() fall back to a synchronous
        // delete) rather than silently stranding tasks. For a started+joined worker the loop already
        // did this on exit; this also covers adopted/never-started Threads. Done BEFORE clearing the
        // back-pointer, so the invariant "thread() == nullptr implies not accepting" holds.
        mData->stopAccepting();

        // Clear the back-pointer LAST, once the loop is guaranteed stopped and joined. Anything
        // still holding this ThreadData (an Object living on this thread, a queued connection that
        // captured it) now sees thread() == nullptr instead of a dangling pointer -- the same thing
        // Qt does in ~QThread() with `d->data->thread.storeRelease(nullptr)`.
        mData->setThread( nullptr );
    }

    //! @brief Start the event loop on a new native OS thread, running at @p aPriority.
    //!
    //! If already running or previously started without join(), this is a no-op or joins the
    //! previous run first. Safe to call multiple times.
    //!
    //! The thread is already at aPriority before it executes its first instruction, as in Qt: on
    //! Windows it is created suspended, given the priority, then resumed; on UNIX the priority
    //! travels in the pthread attributes handed to pthread_create(). Nothing runs at the wrong
    //! priority, not even briefly.
    //!
    //! The one exception is a UNIX kernel that refuses the scheduling attributes outright, where
    //! the thread is created inheriting the caller's priority and applies the requested one to
    //! itself as its first action -- still before mStarted is emitted. Qt falls back the same way.
    //! @param aPriority Priority for the new thread. InheritPriority, the default, keeps the
    //!        creating thread's priority and preserves the behaviour of the no-argument call.
    void Thread::start
        (
        Priority aPriority
        )
    {
        if( mAdopted )
        {
            return; // an adopted thread already represents an already-running native thread
        }

        {
            std::lock_guard<std::mutex> locker( mData->mMutex );
            #if defined( _WIN32 )
                const bool haveThread = ( mHandle != nullptr );
            #else
                const bool haveThread = mJoinable;
            #endif
            if( haveThread && mData->mAccepting )
            {
                // The native handle stays live throughout an active run (only join() clears it),
                // so that alone can't tell "still running" apart from "finished, just never
                // joined". mAccepting is what makes that distinction: still true here means the
                // previous run's loop() has not committed to stopping yet, i.e. it's genuinely
                // still in progress (or this is a fresh Thread that was never started, in which
                // case there is no handle at all and this branch is skipped regardless).
                return;
            }
            mData->mRunning = true;
            mData->mAccepting = true;
        }

        // A previous run may have finished (loop() returned) without ever being join()ed: its
        // handle is then still outstanding, and overwriting it below would leak the OS thread
        // instead of reaping it.
        join();

        // Held across thread creation so a UNIX priority fix-up inside run() can never run
        // before the priority meant for THIS run has been decided, and so setPriority()/
        // priority() can never observe a handle published for a run whose priority is still
        // being set up.
        std::lock_guard<std::mutex> startLock( mPriorityMutex );
        mPriorityNeedsReset = false;
        // Each run starts from what start() was given, never from what the previous run ended
        // at: a priority set on an earlier run said nothing about this one.
        mPriority = aPriority;

        startPlatformSpecific();
    }

    //! @brief Adopt the calling thread as this Thread and register it for affinity.
    //! Does not run an event loop; use with processEvents() in an external loop.
    void Thread::adopt()
    {
        mId = std::this_thread::get_id();
        mAdopted = true; // native thread is already executing
        tCurrentThread = this;
    }

    //! @brief Adopt the calling thread as this Thread and run its event loop here (blocking) until
    //! quit()/exit() is called. Unlike start(), no new OS thread is created: the loop runs on the
    //! caller (e.g. the main thread) -- this is how CoreApplication turns the program's main thread
    //! into a Thread.
    //! @return The exit code passed to exit() (0 if quit() was used).
    int Thread::exec()
    {
        mId = std::this_thread::get_id();
        loop();
        return mExitCode;
    }

    //! @brief Install an external event dispatcher invoked once per loop iteration. Lets external
    //! sources (OS events, file descriptors, timers) be pumped alongside posted tasks. The
    //! dispatcher's argument is the maximum time (ms) it may block waiting for external events
    //! before returning; 0 means do not block.
    void Thread::setDispatcher
        (
        std::function<void( int aTimeoutMs )> aDispatcher
        )
    {
        mDispatcher = std::move( aDispatcher );
    }

    //! @brief Queue a task to run in this thread's event loop. Thread-safe; may be called from any
    //! thread. Delegates to the ThreadData mailbox, so it never dereferences a Thread* that a
    //! concurrent ~Thread() could free.
    //! @return true if the task was queued into a still-live loop; false if this Thread's loop has
    //!         already stopped (or aTask was empty), in which case the task was NOT queued and will
    //!         never run -- callers with a safe fallback for "the target is gone" (e.g.
    //!         deleteLater()) should check this rather than assume the task is handled.
    bool Thread::post
        (
        std::function<void()> aTask
        )
    {
        return mData->post( std::move( aTask ) );
    }

    //! @brief Drain and run all currently-queued tasks, then return immediately.
    //! For adopted threads that pump from their own external loop instead of exec().
    void Thread::processEvents()
    {
        // Register affinity so Objects/current() resolve to us even without exec().
        tCurrentThread = this;

        for( auto& task : mData->takeAll() )
        {
            if( task )
            {
                task();
            }
        }
    }

    //! @brief Set a callback invoked (from any thread) whenever a task is posted, so an
    //! adopted thread with an external loop can be woken to call processEvents()
    //! (e.g. by sending a TSK event). Optional; not needed when exec() runs.
    void Thread::setWakeCallback
        (
        std::function<void()> aWake
        )
    {
        mData->setWakeCallback( std::move( aWake ) );
    }

    //! @brief Install an external blocking wait. When set, the event loop blocks here
    //! (instead of on its internal condition variable) until work is posted or
    //! an external event arrives. It must return when woken via the wake
    //! callback (setWakeCallback); @p aTimeoutMs caps the block (<0 = block
    //! indefinitely). Used by platforms whose OS event source has its own
    //! waitable primitive (e.g. the Win32 message queue), so the loop is
    //! event-driven rather than polling.
    void Thread::setWaiter
        (
        std::function<void( int aTimeoutMs )> aWaiter,
        int aTimeoutMs
        )
    {
        mWaiterTimeoutMs = aTimeoutMs;
        mWaiter = std::move( aWaiter );
    }

    //! @brief Request the event loop to stop.
    //! Already-queued tasks are still drained before the loop exits. Thread-safe.
    void Thread::quit()
    {
        // requestStop() clears mRunning under the mailbox mutex, wakes the loop, and fires the
        // external wake callback (e.g. to unblock a Win32 message loop) so the quit is seen promptly.
        mData->requestStop();
    }

    //! @brief Stop the loop and set the exec() return code.
    //! @param aCode The exit code to return from exec().
    void Thread::exit
        (
        int aCode
        )
    {
        mExitCode = aCode;
        quit();
    }

    // join() is platform-specific: see ThreadWin.cpp / ThreadPosix.cpp.

    //! @brief Set the scheduling priority of the running OS thread. Thread-safe.
    //!
    //! Only meaningful while the thread is running: there is no OS thread to act on before
    //! start(), and the value is deliberately not remembered for a later start() either -- pass
    //! a priority to start() instead. A call made when there is no OS thread is rejected with a
    //! warning and changes nothing, so priority() will still report InheritPriority afterwards.
    //!
    //! What the OS does with the request varies, and a successful call does not promise the
    //! thread's scheduling actually changed. On Linux the default SCHED_OTHER policy reports a
    //! priority range of exactly one value, so every priority maps onto the same number and the
    //! call is accepted but has no effect; real prioritisation there needs a realtime policy and
    //! the privileges to select it. Qt behaves the same way. Windows applies all seven levels.
    //! @param aPriority The priority to apply. InheritPriority is not accepted; rejected with a
    //!        warning.
    void Thread::setPriority
        (
        Priority aPriority
        )
    {
        if( aPriority == InheritPriority )
        {
            std::fprintf( stderr,
                "Thread::setPriority: InheritPriority cannot be set, only reported\n" );
            return;
        }

        std::lock_guard<std::mutex> lock( mPriorityMutex );
        #if defined( _WIN32 )
            const bool haveThread = ( mHandle != nullptr );
        #else
            const bool haveThread = mJoinable;
        #endif
        if( !haveThread )
        {
            std::fprintf( stderr,
                "Thread::setPriority: cannot set priority, thread is not running\n" );
            return;
        }

        mPriority = aPriority;
        applyPriority( aPriority );
    }

    //! @brief Get the scheduling priority of the running OS thread. Thread-safe.
    //! @return The priority last set on the running thread, or InheritPriority if there is no OS
    //!         thread running or no priority has been set on this run.
    Thread::Priority Thread::priority() const
    {
        std::lock_guard<std::mutex> lock( mPriorityMutex );
        #if defined( _WIN32 )
            const bool haveThread = ( mHandle != nullptr );
        #else
            const bool haveThread = mJoinable;
        #endif
        if( !haveThread )
        {
            return InheritPriority;
        }
        return mPriority;
    }

    //! @brief Check if the calling thread is this Thread.
    //! @return true if the calling thread is this Thread, false otherwise.
    bool Thread::isCurrent() const
    {
        return std::this_thread::get_id() == mId;
    }

    //! @brief Get the Thread for the calling thread. If none is running/adopted, a dummy adopted
    //! Thread is created on demand so every thread has affinity, exactly like Qt auto-adopting a
    //! native thread; this therefore never returns null (the dummy has no event loop, so it only
    //! confers affinity).
    //! @note Do NOT store the returned raw pointer anywhere that may outlive the Thread -- for an
    //! auto-adopted dummy that is only until the adopting native thread exits. Store the ThreadData
    //! from currentData()/data() instead, which stays valid and reports nullptr once its Thread is
    //! gone.
    Thread* Thread::current()
    {
        if( tCurrentThread == nullptr )
        {
            tAdoptedThread = std::make_unique<Thread>( "adopted" );
            tAdoptedThread->adopt(); // sets tCurrentThread = this dummy
        }
        return tCurrentThread;
    }

    //! @brief Get the ThreadData for the calling thread.
    //! Like current() but returns the shared_ptr<ThreadData> (which outlives its Thread)
    //! instead of the Thread pointer itself, so it's safe to hold indefinitely.
    std::shared_ptr<ThreadData> Thread::currentData()
    {
        // Reuse current()'s auto-adopt logic, then hand back that Thread's data rather than the
        // Thread itself. The data survives the Thread, so callers may hold it indefinitely.
        return current()->data();
    }

    //! @brief Worker function that runs the event loop on a newly-created thread.
    //! @private
    void Thread::run()
    {
        mId = std::this_thread::get_id();

        {
            // Blocks here until start() releases mPriorityMutex, which is what guarantees the
            // native handle/id above and mPriority are published before anything below relies on
            // them. Normally nothing is left to do here -- Windows set the priority on the
            // suspended thread, UNIX passed it to pthread_create() -- so this only bites when the
            // UNIX scheduling attributes were refused, and even then it lands before mStarted is
            // emitted.
            std::lock_guard<std::mutex> priorityLock( mPriorityMutex );
            if( mPriorityNeedsReset )
            {
                mPriorityNeedsReset = false;
                applyPriority( mPriority );
            }
        }

        loop();
    }

    // threadEntry() and applyPriority() are platform-specific: see ThreadWin.cpp / ThreadPosix.cpp.

    //! @brief Shared event loop body.
    //! Drains posted tasks and pumps the dispatcher (if any) until quit()/exit() is
    //! requested. Runs on whichever thread invokes it (a worker via run(), or the
    //! adopted main thread via exec()).
    //! @private
    void Thread::loop()
    {
        bool stop = false;

        // Register this thread so objects constructed on it (via Thread::current())
        // and Objects default-bound to it resolve to this Thread. This runs on
        // this thread, so the thread_local naturally scopes to us.
        tCurrentThread = this;

        // Qt-like lifecycle signal: emitted when the event loop starts running.
        mStarted.emit();

        // When a dispatcher is present it polls external (OS) sources, so the loop
        // must not block indefinitely on the condition variable; give it a small
        // budget to wait for those events between task batches.
        const auto kPollSlice = std::chrono::milliseconds( 10 );

        while( !stop )
        {
            std::deque<std::function<void()> > batch;

            {
                std::unique_lock<std::mutex> locker( mData->mMutex );

                // Nothing to run yet: block until work is posted or a quit/OS event.
                if( mData->mTasks.empty() && mData->mRunning )
                {
                    if( mWaiter )
                    {
                        // Platform-provided blocking wait (e.g. the Win32 message
                        // queue). It returns when an OS event arrives or when woken
                        // via the wake callback on post()/quit(). Run it without the
                        // lock so other threads can post() while we wait.
                        locker.unlock();
                        mWaiter( mWaiterTimeoutMs );
                        locker.lock();
                    }
                    else if( mDispatcher )
                    {
                        mData->mWake.wait_for( locker, kPollSlice, [this]
                            {
                                return !mData->mTasks.empty() || !mData->mRunning;
                            } );
                    }
                    else
                    {
                        mData->mWake.wait( locker, [this]
                            {
                                return !mData->mTasks.empty() || !mData->mRunning;
                            } );
                    }
                }

                if( mData->mTasks.empty() && !mData->mRunning )
                {
                    // No work left and quit requested: exit the loop. mAccepting is cleared in this
                    // SAME locked block, atomically with that decision -- not later, after the loop,
                    // as a separate critical section -- so post() (also taking mData->mMutex) can
                    // never observe "still accepting" for a run that has already irrevocably
                    // committed to stopping. Either post()'s push happens-before this block (and is
                    // then seen by the check above, so the loop does NOT stop yet), or it
                    // happens-after (and post() then reliably observes !mAccepting and reports
                    // failure instead of stranding the task in a queue nothing will ever drain).
                    stop = true;
                    mData->mAccepting = false;
                }
                else
                {
                    // Take the whole pending batch and run it outside the lock so slots
                    // may safely post() more work (including back to this thread).
                    batch.swap( mData->mTasks );
                }
            }

            for( auto& task : batch )
            {
                if( task )
                {
                    task();
                }
            }

            // Pump OS-level events; don't block since Object tasks may be pending.
            if( mDispatcher && !stop )
            {
                mDispatcher( 0 );
            }
        }

        // The event loop has exited; clear our per-thread registration. mAccepting was already
        // cleared above, atomically with the decision to stop -- see that block's comment.
        tCurrentThread = nullptr;

        // Qt-like lifecycle signal: emitted when the event loop has finished.
        mFinished.emit();
    }

    //! @return the underlying std::thread id (valid after start()).
    std::thread::id Thread::id() const
    {
        return mId;
    }

    //! @return this thread's name.
    const std::string& Thread::name() const
    {
        return mName;
    }

    //! Connect a callback invoked when the thread event loop starts (Qt-like
    //! QThread::started()).
    Connection Thread::connectStarted
        (
        const LifecycleSignalType::Slot& aSlot
        )
    {
        return mStarted.connect( aSlot );
    }

    //! Connect a callback invoked when the thread event loop finishes (Qt-like
    //! QThread::finished()).
    Connection Thread::connectFinished
        (
        const LifecycleSignalType::Slot& aSlot
        )
    {
        return mFinished.connect( aSlot );
    }

    //! @return this Thread's ThreadData. Outlives this Thread; its thread() reports
    //! nullptr once this Thread has been destroyed.
    std::shared_ptr<ThreadData> Thread::data() const
    {
        return mData;
    }

} // namespace QtMimic
