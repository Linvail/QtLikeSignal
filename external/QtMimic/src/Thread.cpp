//! @file
//!
//! Implementation of QtMimic::Thread (event loop).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Thread.hpp"

#include "EventDispatcherDefault.hpp"
#if defined( _WIN32 )
    #include "EventDispatcherWin32.hpp"
#elif defined( __linux__ )
    #include "EventDispatcherLinux.hpp"
#endif

#include <cstdio>
#include <memory>

namespace QtMimic
{
    thread_local Thread* Thread::sCurrentThread = nullptr;
    thread_local std::unique_ptr<Thread> Thread::sAdoptedThread;
    thread_local bool Thread::sAdopting = false;

    //! @brief Constructor - initialize a Thread with an optional name.
    //! The thread is not started until start() or exec() is called.
    Thread::Thread
        (
        const std::string& aName
        )
        : Object()
        , mName( aName )
    {
        mData = std::make_shared<ThreadData>();
        mData->setThread( this );
    }

    //! @brief Destructor - stops the event loop (draining any pending tasks) and joins the thread.
    //! Also nulls the ThreadData back-pointer so any surviving Object still holding this
    //! thread's data sees thread() == nullptr instead of a dangling pointer.
    Thread::~Thread()
    {
        quit();
        wait();

        // Drain deferred deletes, then release the dispatcher -- BEFORE clearing the back-pointer
        // below, so there is never a moment where thread() reports nullptr while a working
        // dispatcher is still reachable through this ThreadData.
        //
        // threadBody() already does both for a worker that ran a loop, so this is normally a no-op
        // there. It exists for the case that had no equivalent: an *adopted* thread, whose Thread is
        // destroyed by its thread_local owner as the native thread exits. Its dispatcher was never
        // released, so it outlived the thread and kept accepting work nothing would ever run, and
        // any deleteLater() still queued was freed as an event without its target ever being
        // deleted. Draining runs on the exiting thread itself, which is the thread those
        // destructors expect.
        if( auto dispatcher = mData->dispatcher() )
        {
            dispatcher->close();
            dispatcher->processDeferredDeletes();
        }
        mData->setDispatcher( nullptr );

        // An adopted Thread is destroyed by the thread_local that owns it as the native thread
        // exits, and a Thread that ran exec() is destroyed by whoever created it; neither path
        // goes through threadBody(), which is what unregisters a started worker. Leaving the
        // registration set would hand out a pointer to freed memory -- currentThread() would
        // return it, and the first caller to follow it reads destroyed storage.
        if( sCurrentThread == this )
        {
            sCurrentThread = nullptr;
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
        if( mAdopted.load() )
        {
            return; // an adopted thread already represents an already-running native thread
        }

        if( mData->isThreadRunning() )
        {
            return;
        }

        // A previous run may have finished without anyone calling wait(), leaving its OS thread
        // unreaped. Reap it before its handle is overwritten -- Qt's start() likewise waits out a
        // thread it finds in the Finishing state. Returns immediately when there is nothing to
        // reap.
        wait();

        // Held across thread creation so a UNIX priority fix-up inside run() can never run
        // before the priority meant for THIS run has been decided, and so setPriority()/
        // priority() can never observe a handle published for a run whose priority is still
        // being set up.
        std::lock_guard<std::mutex> startLock( mPriorityMutex );

        // Set before the thread exists rather than from inside it, so isRunning() can never report
        // false for a thread that has already begun executing. Qt publishes Running from start()
        // for the same reason.
        mData->setThreadRunning( true );
        mHasFinished.store( false );
        mFinishing.store( false );
        mExiting.store( false );
        mPriorityNeedsReset = false;
        // Each run starts from what start() was given, never from what the previous run ended
        // at: a priority set on an earlier run said nothing about this one.
        mPriority = aPriority;

        startPlatformSpecific();
    }

    //! @brief Everything the new thread must do whether or not run() is overridden: publish the
    //! thread id, take affinity for itself, create the dispatcher, apply a priority the kernel
    //! refused at creation, then hand over to run().
    //! @private
    void Thread::threadBody()
    {
        mId.store( std::this_thread::get_id() );
        sCurrentThread = this;
        // Not moveToThread(this): see bindAffinityToSelf(). Once every thread is adopted, this
        // Thread object already has an affinity (to whichever thread constructed it), so
        // moveToThread() would correctly refuse to let a *different* thread re-home it -- even
        // though that different thread is this one, taking ownership of the object representing it.
        bindAffinityToSelf();

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

        bool createdDispatcher = false;
        if( !mData->dispatcher() )
        {
            #if defined( _WIN32 )
                mData->setDispatcher( std::make_shared<EventDispatcherWin32>() );
            #elif defined( __linux__ )
                mData->setDispatcher( std::make_shared<EventDispatcherLinux>() );
            #else
                mData->setDispatcher( std::make_shared<EventDispatcherDefault>() );
            #endif
            createdDispatcher = true;
        }

        mStarted.emit();

        run();

        // The run is over: report it before announcing it, so a finished() handler sees the same
        // state Qt would show it -- isRunning() false, isFinished() true. Qt sets threadState =
        // Finishing inside finish() and emits finished() immediately after, in that order.
        {
            // Under the same mutex setPriority() uses, so a setPriority() that holds the lock and
            // sees the flag still set knows this store has not happened yet and the OS thread is
            // therefore still alive -- which is what makes using the native handle there safe
            // rather than merely likely.
            std::lock_guard<std::mutex> priorityLock( mPriorityMutex );
            mData->setThreadRunning( false );
        }
        mFinishing.store( true );

        mFinished.emit();

        // Drain deferred deletes before letting go of the dispatcher, mirroring Qt's
        // QThreadPrivate::finish(), which calls sendPostedEvents(nullptr, DeferredDelete) right
        // after emitting finished() and before cleanup() destroys the dispatcher. Without this,
        // anything that called deleteLater() before the loop stopped is never destroyed: the
        // dispatcher's destructor can free the queued events but has no way to free their
        // receivers.
        // Refuse further events BEFORE draining, so a deleteLater() racing this shutdown is
        // rejected -- and falls back to a synchronous delete -- rather than landing in a queue
        // nothing will drain again. Without the close() the post could slip between the drain
        // below and the dispatcher being released, and the object would be neither run nor
        // deleted.
        if( auto disp = mData->dispatcher() )
        {
            disp->close();
            disp->processDeferredDeletes();
        }

        if( createdDispatcher )
        {
            // Just drop this thread's reference. Any other thread that is part-way through a call
            // still holds its own strong reference from ThreadData::dispatcher(), so the
            // dispatcher stays alive until that call finishes rather than being freed underneath
            // it.
            mData->setDispatcher( nullptr );
        }

        // Only now, once the dispatcher is released and nothing here will touch this object
        // again, is it safe to release wait() -- whose caller may destroy this Thread the instant
        // it returns. mFinishing above is what isFinished() reports; this is what wait() waits for.
        mHasFinished.store( true );
        {
            std::lock_guard<std::mutex> locker( mWaitMutex );
            mWaitCv.notify_all();
        }

        // Safe after the release above: this is a thread_local, not a member of the Thread the
        // waiter may already have destroyed.
        sCurrentThread = nullptr;
    }

    //! @brief The body the new thread executes. The default runs the event loop until quit()/exit();
    //! override to do something else, as with QThread::run().
    void Thread::run()
    {
        exec();
    }

    //! @brief Enter the event loop and block until exit()/quit() is called.
    //! @return The exit code passed to exit() (0 if quit() was used).
    //!
    //! Deliberately does NOT clear mExiting on entry. start() already cleared it, and clearing it
    //! again here loses a quit() issued in the window between start() returning and this thread
    //! reaching exec() -- the loop would then run forever with nothing left to stop it. An adopted
    //! thread has no start() to do the clearing, so CoreApplication::exec() does it instead, which
    //! also makes an exit()/quit() issued *before* exec() discarded rather than honoured. Qt makes
    //! the same choice in the same place (`threadData->quitNow = false`).
    int Thread::exec()
    {
        // Re-fetched each iteration, and held as a strong reference across processEvents() so the
        // dispatcher cannot be destroyed mid-call.
        auto dispatcher = mData->dispatcher();
        while( !mExiting.load() && dispatcher )
        {
            dispatcher->processEvents();
            dispatcher = mData->dispatcher();
        }
        return mExitCode.load();
    }

    //! @brief Request the event loop to stop.
    //! Already-queued tasks are still drained before the loop exits. Thread-safe.
    void Thread::quit()
    {
        exit( 0 );
    }

    //! @brief Stop the loop and set the exec() return code. Thread-safe.
    //! @param aCode The exit code to return from exec().
    void Thread::exit
        (
        int aCode
        )
    {
        mExitCode.store( aCode );
        mExiting.store( true );
        if( auto dispatcher = mData->dispatcher() )
        {
            dispatcher->interrupt();
            dispatcher->wakeUp();
        }
    }

    //! @brief Queue an arbitrary task to run on this thread's event loop.
    //!
    //! Always deferred to a later iteration of this thread's loop -- never run inline, even when
    //! post() is called from this thread itself. Implemented as a thin wrapper over
    //! Object::dispatchMetaCallTo() targeting this Thread as receiver, so it goes through the exact
    //! same queue, MetaCallEvent and lifetime handling as every other queued call in this library
    //! (removeEventsForReceiver() on destruction, processDeferredDeletes() on shutdown) rather than
    //! a second, parallel task queue. Thread-safe.
    //! @return true if the task was queued; false if this thread has no dispatcher yet (before
    //!         start()/exec(), or after it has fully finished and released it), in which case the
    //!         task is dropped rather than run.
    bool Thread::post
        (
        std::function<void()> aTask
        )
    {
        // Basic sanity check. Usually ThreadData should outlive Thread.
        if( !aTask || mData == nullptr || mData->thread() == nullptr )
        {
            return false;
        }
        return dispatchMetaCallTo( mData, this, std::move( aTask ) );
    }

    //! @brief Run one pass of this thread's event loop, then return.
    //!
    //! For a thread that has its own native loop and therefore never calls exec(): call this from
    //! that loop to deliver queued slot invocations, deferred deletes and timer events. Combine it
    //! with setWakeCallback() so the native loop knows when there is something to drain, instead of
    //! polling for it.
    //!
    //! **Must be called from this thread**; it dispatches to objects that live here, and their
    //! handlers expect to run here. A call from elsewhere is rejected with a warning.
    void Thread::processEvents()
    {
        if( this != currentThread() )
        {
            std::fprintf( stderr,
                "Thread::processEvents: must be called from the thread it belongs to\n" );
            return;
        }

        if( auto dispatcher = mData->dispatcher() )
        {
            dispatcher->processEvents();
        }
    }

    //! @brief Install a callback invoked whenever work is queued for this thread. Thread-safe.
    //!
    //! The other half of the adopted-thread story: it runs on whichever thread posted the work, and
    //! its job is to nudge this thread's own native loop -- typically by sending it a private event
    //! -- so that the loop knows to call processEvents(). Pass nullptr to remove it.
    //!
    //! May be called with the dispatcher's internals locked, so it must not block or re-enter the
    //! dispatcher; post to the native loop and return.
    void Thread::setWakeCallback
        (
        std::function<void()> aWake
        )
    {
        if( auto dispatcher = mData->dispatcher() )
        {
            dispatcher->setWakeCallback( std::move( aWake ) );
        }
    }

    //! @brief Get the event dispatcher for this thread. Thread-safe.
    //!
    //! Read-only by design. There is deliberately no setter: swapping a running thread's dispatcher
    //! races that thread's own start/finish lifecycle, and could delete a dispatcher an active
    //! exec()/processEvents() loop was still calling into. A thread creates and owns its dispatcher
    //! in threadBody(); CoreApplication supplies the main thread's.
    //!
    //! Returns a strong reference rather than a raw pointer, so the dispatcher cannot be destroyed
    //! by its owning thread finishing while the caller is still using it.
    //! @return the dispatcher, or nullptr before start().
    std::shared_ptr<AbstractEventDispatcher> Thread::eventDispatcher() const
    {
        return mData->dispatcher();
    }

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
        return sCurrentThread == this;
    }

    //! @return true if this thread is running: start()ed and not yet finished, or adopted.
    //!
    //! Matches Qt's isRunning(), which reads threadState == Running and which an adopted QThread
    //! sits in for its whole life. Thread-safe.
    bool Thread::isRunning() const
    {
        return mData->isThreadRunning();
    }

    //! @return true once this thread's body has begun winding down. Always false for an adopted
    //! thread, which never leaves the running state -- again matching Qt. Thread-safe.
    //!
    //! Reports mFinishing, not the later flag wait() blocks on: Qt's isFinished() tests
    //! threadState >= Finishing, so it is already true inside a finished() handler.
    bool Thread::isFinished() const
    {
        return mFinishing.load();
    }

    //! @brief Whether this Thread wraps a pre-existing native thread rather than one it started.
    bool Thread::isAdopted() const
    {
        return mAdopted.load();
    }

    //! @return the id of the OS thread this Thread runs on, valid once start() has published it.
    //! Useful mainly for asserting which thread a slot ran on.
    std::thread::id Thread::id() const
    {
        return mId.load();
    }

    //! @return this thread's descriptive name, empty if it was not given one. Thread-safe: the
    //! name is set at construction and never changes.
    const std::string& Thread::name() const
    {
        return mName;
    }

    //! @return a subscription-only view of the signal emitted when this thread's event loop starts
    //! running (Qt-like QThread::started()). Thread-safe.
    SignalView<>& Thread::getStarted() const
    {
        return mStarted.view();
    }

    //! @return a subscription-only view of the signal emitted when this thread's event loop has
    //! exited (Qt-like QThread::finished()). Thread-safe.
    SignalView<>& Thread::getFinished() const
    {
        return mFinished.view();
    }

    //! @brief Get the Thread the calling thread is running as, never nullptr.
    //!
    //! If the caller was not started through this class -- the process's main thread, or a raw
    //! std::thread -- it is *adopted* on the spot: a Thread is created to represent it, given a
    //! default event dispatcher, and owned by a thread_local that releases it when the native
    //! thread exits. Qt does the same, and the guarantee it buys is that every Object has a thread
    //! affinity. Without it, an Object created off-thread had none, and a queued connection whose
    //! emitter was also affinity-less compared nullptr == nullptr, decided "same thread", and ran
    //! the slot directly on the emitting thread -- an unsynchronised cross-thread call that Qt
    //! explicitly does not perform.
    //!
    //! An adopted thread has a queue but no loop running on it, so queued work accumulates until
    //! the thread drains it with processEvents() (or runs exec()). That matches Qt, where posted
    //! events sit in the thread's list until something dispatches them.
    //!
    //! @note Do not store the returned pointer anywhere that may outlive the thread. For an adopted
    //! thread that is only until the native thread exits.
    Thread* Thread::currentThread()
    {
        // sAdopting breaks the recursion: constructing the Thread below runs Object's constructor,
        // which calls straight back into here.
        if( sCurrentThread == nullptr && !sAdopting )
        {
            sAdopting = true;
            // Built with no affinity (currentThread() reports nullptr while sAdopting is set);
            // adoptCallingThread() points it at itself immediately.
            sAdoptedThread.reset( new Thread( "adopted" ) );
            sAdopting = false;

            sAdoptedThread->adoptCallingThread();
        }
        return sCurrentThread;
    }

    //! @brief Make this Thread represent the calling native thread.
    //!
    //! Used only for auto-adoption. Registers as the current thread, records the OS thread id, and
    //! installs a plain cross-platform dispatcher -- not the platform one, which would allocate an
    //! eventfd or a message-only window for every native thread that merely touches an Object. A
    //! thread that actually runs a loop gets the platform dispatcher instead: worker threads in
    //! threadBody(), the main thread in CoreApplication.
    void Thread::adoptCallingThread()
    {
        mAdopted.store( true );
        sCurrentThread = this;
        mId.store( std::this_thread::get_id() );

        // An adopted thread is running -- it is executing this very call. Qt states the same and
        // for the same reason, setting threadState = Running in the QThread constructor used for
        // adoption with the comment "thread should be running and not finished for the lifetime of
        // the application".
        //
        // It also makes start() on an adopted Thread the no-op it should be. setPriority() still
        // refuses, because it additionally requires a native handle, which an adopted thread has
        // never had.
        mData->setThreadRunning( true );

        bindAffinityToSelf();

        if( !mData->dispatcher() )
        {
            mData->setDispatcher( std::make_shared<EventDispatcherDefault>() );
        }
    }

    //! @brief Point this Thread's own Object affinity at the thread it represents.
    //!
    //! Deliberately bypasses moveToThread(), which would refuse. moveToThread() enforces that only
    //! the thread currently owning an object may re-home it, with an exception for objects that
    //! have no affinity yet -- and once every thread is adopted, a Thread constructed on thread A
    //! always *does* have affinity (to A), so a worker starting up would be refused permission to
    //! adopt itself. That rule exists to stop one thread yanking another thread's live object away,
    //! which is not what is happening here: this is the thread in question taking ownership of the
    //! object that represents it, at the only moment that can possibly be correct.
    //!
    //! Skips moveToThread()'s timer migration too. A Thread object with timers registered against
    //! it before its loop starts would keep them on the creating thread's dispatcher, which is a
    //! corner case not worth the confinement violation of migrating them from here.
    void Thread::bindAffinityToSelf()
    {
        mAffinity->setData( mData );
    }

} // namespace QtMimic
