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

        //! Combine two "milliseconds to wait, or -1 for no deadline at all" budgets into the
        //! earlier of the two. Used to fold a timer deadline into whatever the loop was already
        //! going to wait for, so adding a timer can only ever shorten a wait, never lengthen one.
        int earlierTimeout
            (
            int aFirst,
            int aSecond
            )
        {
            if( aFirst < 0 )
            {
                return aSecond;
            }
            if( aSecond < 0 )
            {
                return aFirst;
            }
            return aFirst < aSecond ? aFirst : aSecond;
        }
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
        wait();

        // Refuse any further posts now that this Thread is gone: nothing will ever drain the mailbox
        // again, so post() must return false (letting deleteLater() fall back to a synchronous
        // delete) rather than silently stranding tasks. For a started+joined worker the loop already
        // did this on exit; this also covers adopted/never-started Threads. Done BEFORE clearing the
        // back-pointer, so the invariant "thread() == nullptr implies not accepting" holds.
        mData->stopAccepting();

        // An adopted Thread is destroyed by the thread_local that owns it as the native thread
        // exits, and a Thread that ran exec() is destroyed by whoever created it; neither path
        // goes through threadBody(), which is what unregisters a started worker. Leaving the
        // registration set would hand out a pointer to freed memory -- currentThread() would
        // return it, and the first caller to follow it (deleteLater() reaching for
        // currentData(), say) reads destroyed storage.
        if( tCurrentThread == this )
        {
            tCurrentThread = nullptr;
        }

        // Clear the back-pointer LAST, once the loop is guaranteed stopped and joined. Anything
        // still holding this ThreadData (an Object living on this thread, a queued connection that
        // captured it) now sees thread() == nullptr instead of a dangling pointer -- the same thing
        // Qt does in ~QThread() with `d->data->thread.storeRelease(nullptr)`.
        mData->setThread( nullptr );
    }

    //! @brief Start the event loop on a new native OS thread, running at @p aPriority.
    //!
    //! If already running or previously started without wait(), this is a no-op or waits for the
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
                // The native handle stays live throughout an active run (only wait() clears it),
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

        // A previous run may have finished (loop() returned) without ever being waited for: its
        // handle is then still outstanding, and overwriting it below would leak the OS thread
        // instead of reaping it.
        wait();

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
        mAdopted.store( true ); // native thread is already executing
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

        // Reset the mailbox the way start() does. Without this a second exec() -- after a quit()
        // has already cleared mRunning -- returns instantly with no loop at all, so a program that
        // re-enters its own event loop silently stops processing events. start() has always done
        // this; exec() is the other way into loop() and was missing it.
        mData->prepareForRun();

        loop();

        // The loop stopped, but this thread has not. loop() clears mAccepting as it commits to
        // stopping -- right for a worker whose OS thread is about to end, wrong here, where the
        // caller carries on living and may well exec() again. Left cleared, its mailbox would
        // refuse every later post(), and deleteLater() would quietly delete on the spot rather
        // than defer. Same distinction as the registration above: the loop's lifetime is not the
        // thread's.
        mData->resumeAccepting();

        return mExitCode.load();
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
        // Confined to the thread this Thread represents. Draining another thread's queue on the
        // caller would run its handlers on the wrong thread -- every slot, every timer, every
        // deleteLater -- which is the exact confinement the whole affinity model exists to keep.
        //
        // Worse than merely running them in the wrong place: the old code registered `this` as the
        // caller's current thread on the way in and never put it back, so a single stray call left
        // the caller permanently believing it *was* the other thread. Everything it did afterwards
        // -- constructing an Object, resolving a queued connection, starting a timer -- picked up
        // the wrong affinity.
        //
        // Asked via isCurrent() rather than currentThread() for the reasons given in
        // CoreApplication::exec(): currentThread() would auto-adopt a Thread as a side effect of
        // answering, and reads a pointer that loop() clears on the way out.
        if( !isCurrent() )
        {
            std::fprintf( stderr,
                "Thread::processEvents: must be called from the thread it belongs to\n" );
            return;
        }

        for( auto& task : mData->takeAll() )
        {
            if( task )
            {
                task();
            }
        }

        // Timers get serviced on every pump too, otherwise an adopted thread driving us from its
        // own loop would be the one kind of thread where Timer silently never fires. Nothing here
        // controls how often the external loop calls us, so the resolution a timer actually gets is
        // that loop's pump rate; see setWakeCallback() for nudging it.
        mData->dispatchExpiredTimers();
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
        mExitCode.store( aCode );
        quit();
    }

    // wait() is platform-specific: see ThreadWin.cpp / ThreadPosix.cpp.

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
    Thread* Thread::currentThread()
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
        return currentThread()->data();
    }

    //! @brief Everything the new thread must do whether or not run() is overridden: publish the
    //! thread id, apply a priority the kernel refused at creation, then hand over to run().
    //! @private
    void Thread::threadBody()
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

        // Register this thread so objects constructed on it (via Thread::currentThread()) and
        // Objects default-bound to it resolve to this Thread, and announce the lifecycle -- both
        // around run() rather than inside loop(), so a subclass that overrides run() and never
        // enters the loop still gets them. Qt does the same: started() and finished() are emitted
        // by QThreadPrivate either side of run(), not by the event loop.
        tCurrentThread = this;

        mStarted.emit();

        run();

        mFinished.emit();

        tCurrentThread = nullptr;
    }

    //! @brief The body the new thread executes. The default runs the event loop until quit()/exit();
    //! override to do something else, as with QThread::run().
    void Thread::run()
    {
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

        // Registered here as well as in threadBody(), because exec() reaches this without going
        // through threadBody() at all -- that is how CoreApplication turns the caller into a
        // Thread. Setting it twice on the started-thread path is harmless; not setting it on the
        // exec() path would leave that thread unable to resolve its own affinity.
        tCurrentThread = this;

        // When a dispatcher is present it polls external (OS) sources, so the loop
        // must not block indefinitely on the condition variable; give it a small
        // budget to wait for those events between task batches.
        const auto kPollSlice = std::chrono::milliseconds( 10 );

        while( !stop )
        {
            std::deque<std::function<void()> > batch;

            {
                std::unique_lock<std::mutex> locker( mData->mMutex );

                // Nothing to run yet: block until work is posted, a timer comes due, or a quit/OS
                // event arrives.
                const auto now = std::chrono::steady_clock::now();
                if( mData->mTasks.empty() && !mData->hasExpiredTimers( now ) && mData->mRunning )
                {
                    // How long we may sleep before the earliest timer deadline, or -1 if no timer
                    // is registered and there is therefore no deadline to respect.
                    const int timerTimeoutMs = mData->nextTimerTimeoutMs( now );

                    // Cleared right before waiting, still holding mMutex, so a concurrent
                    // register/unregister either lands before this point (already reflected in the
                    // deadline above) or after it (blocked on mMutex until the wait releases it,
                    // then setting the flag and notifying). There is no window where a change to
                    // the deadline can be lost.
                    mData->mTimersChanged = false;

                    // A quit, a posted task or a timer change ends the wait early; a timer deadline
                    // ends it on time. Nothing else needs to be in the predicate, because a timer
                    // coming due is not a state change anyone signals -- it is just the clock.
                    auto wakeCondition = [this]
                        {
                            return !mData->mTasks.empty() || !mData->mRunning
                                   || mData->mTimersChanged;
                        };

                    if( mWaiter )
                    {
                        // Platform-provided blocking wait (e.g. the Win32 message
                        // queue). It returns when an OS event arrives or when woken
                        // via the wake callback on post()/quit(). Run it without the
                        // lock so other threads can post() while we wait.
                        locker.unlock();
                        mWaiter( earlierTimeout( mWaiterTimeoutMs, timerTimeoutMs ) );
                        locker.lock();
                    }
                    else if( mDispatcher )
                    {
                        const int sliceMs = static_cast<int>( kPollSlice.count() );
                        mData->mWake.wait_for( locker,
                            std::chrono::milliseconds( earlierTimeout( sliceMs, timerTimeoutMs ) ),
                            wakeCondition );
                    }
                    else if( timerTimeoutMs < 0 )
                    {
                        mData->mWake.wait( locker, wakeCondition );
                    }
                    else
                    {
                        mData->mWake.wait_for( locker,
                            std::chrono::milliseconds( timerTimeoutMs ), wakeCondition );
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

            // Deliver whatever came due, after the posted tasks and outside the lock. Skipped once
            // we have committed to stopping: quit() drains the tasks already queued, but a timer is
            // a standing schedule rather than queued work, so there is nothing owed to it -- and
            // firing one here would run user code after the loop has decided it is finished.
            if( !stop )
            {
                mData->dispatchExpiredTimers();
            }

            // Pump OS-level events; don't block since Object tasks may be pending.
            if( mDispatcher && !stop )
            {
                mDispatcher( 0 );
            }
        }

        // Deliberately does NOT clear the per-thread registration on the way out.
        //
        // It used to, and that was wrong for exec(). Registration belongs to the *thread*, not to
        // the loop: a started worker is registered by threadBody() and unregistered when that
        // returns, which is when the OS thread ends. exec() is the other way in, and there the two
        // lifetimes are nothing alike -- the main thread outlives its loop by the whole rest of the
        // program. Clearing here un-adopted it the moment exec() returned, so
        // Thread::currentThread() auto-adopted a fresh dummy and every thread-confined call the
        // main thread made afterwards -- startTimer(), processEvents(), a second exec() -- was
        // refused as coming from the wrong thread.
        //
        // What stops the pointer dangling is ~Thread(), which clears it if it still points here.
        // That is the same division QtLikeSignal uses, and the reason its exec() never touches the
        // registration at all.
    }

    //! @return true if this Thread describes a native thread that was already running -- one
    //! auto-adopted on demand, or the main thread taken over by CoreApplication -- rather than one
    //! start() created. An adopted Thread has no loop of its own unless someone exec()s it.
    bool Thread::isAdopted() const
    {
        return mAdopted.load();
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

    //! Return a view of the signal emitted when the thread event loop starts
    //!  (Qt-like QThread::started()).
    SignalView<>& Thread::getStarted() const
    {
        return mStarted.view();
    }

    //! Return a view of the signal emitted when the thread event loop exits
    //!  (Qt-like QThread::finished()).
    SignalView<>& Thread::getFinished() const
    {
        return mFinished.view();
    }

    //! @return this Thread's ThreadData. Outlives this Thread; its thread() reports
    //! nullptr once this Thread has been destroyed.
    std::shared_ptr<ThreadData> Thread::data() const
    {
        return mData;
    }

} // namespace QtMimic
