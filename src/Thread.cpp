#include "Thread.hpp"

#include "EventDispatcherDefault.hpp"
#if defined( _WIN32 )
    #include "EventDispatcherWin32.hpp"
#elif defined( __linux__ )
    #include "EventDispatcherLinux.hpp"
#endif

#include <cstdio>
#include <memory>

namespace QtLikeSignal
{
    thread_local Thread* Thread::sCurrentThread = nullptr;
    thread_local std::unique_ptr<Thread> Thread::sAdoptedThread;
    thread_local bool Thread::sAdopting = false;

    //! Constructs a new thread object with an optional descriptive name.
    Thread::Thread
        (
        const std::string& aName  //!< Descriptive name; empty by default.
        )
        : Object()
        , mName( aName )
    {
        mData = std::make_shared<ThreadData>();
        mData->setThread( this );
    }

    //! Destroys the thread, waiting for it to finish if running.
    Thread::~Thread()
    {
        quit();
        wait();

        // Drain deferred deletes, then release the dispatcher -- BEFORE clearing the back-pointer
        // below, so there is never a moment where thread() reports nullptr while a working
        // dispatcher is still reachable through this ThreadData. QtMimic states the same invariant
        // for its mailbox: "Done BEFORE clearing the back-pointer, so the invariant 'thread() ==
        // nullptr implies not accepting' holds."
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

        // An adopted Thread is destroyed by the thread_local that owns it, as the native thread
        // exits; nothing else clears the registration for it the way threadBody() does for a worker.
        // Leaving it set would hand out a pointer to freed memory on the way out.
        if( sCurrentThread == this )
        {
            sCurrentThread = nullptr;
        }

        // Clear the back-pointer LAST, once the OS thread is guaranteed stopped. Anything still
        // holding this ThreadData (an Object living here, an Affinity captured by a connect() made
        // to one) now sees thread() == nullptr instead of a dangling Thread*, the same thing Qt does
        // in ~QThread() with `d->data->thread.storeRelease(nullptr)`.
        mData->setThread( nullptr );
    }

    //! Starts execution of the thread by invoking run(). Thread-safe.
    //!
    //! The thread is already at aPriority before it executes its first instruction, as in Qt: on
    //! Windows it is created suspended, given the priority, then resumed; on UNIX the priority
    //! travels in the pthread attributes handed to pthread_create(). Nothing runs at the wrong
    //! priority, not even briefly.
    //!
    //! The one exception is a UNIX kernel that refuses the scheduling attributes outright, where
    //! the thread is created inheriting the caller's priority and applies the requested one to
    //! itself as its first action -- still before started() is emitted and before run() is
    //! entered. Qt falls back the same way.
    void Thread::start
        (
        Priority aPriority  //!< Priority for the new thread. InheritPriority, the default, keeps the
                            //!< creating thread's priority and preserves the behaviour of the
                            //!< no-argument call.
        )
    {
        if( mAdopted.load() )
        {
            return;
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

        // Held across thread creation so setPriority() can never observe the running flag set
        // while the handle is still the previous run's (or absent). A run body that finishes before
        // this scope ends simply waits for the lock at its tail.
        std::lock_guard<std::mutex> startLock( mPriorityMutex );

        mData->setThreadRunning( true );
        mHasFinished.store( false );
        mFinishing.store( false );
        mExiting.store( false );
        mPriorityNeedsReset = false;
        // Each run starts from what start() was given, never from what the previous run ended at: a
        // priority set on an earlier run said nothing about this one, and reporting the stale value
        // would be a lie about a thread that never got it.
        mPriority = aPriority;

        startPlatformSpecific();
    }

    //! Body the OS thread runs: dispatcher setup, run(), then teardown.
    //!
    //! Everything between the OS entry point and the end of the thread's life. Called only by
    //! threadEntry().
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
            // Blocks here until start() releases the lock, which is what guarantees the native
            // handle is published before anything below can use it. There is normally no
            // priority work left to do -- Windows set it on the suspended thread, UNIX passed it
            // to pthread_create() -- so this only bites when the UNIX scheduling attributes were
            // refused, and even then it still lands before started() is emitted and before run()
            // is entered.
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
            std::lock_guard<std::mutex> lock( mWaitMutex );
            mWaitCv.notify_all();
        }
        sCurrentThread = nullptr;
    }

    //! Starting point for thread execution. Can be overridden. Default calls exec().
    void Thread::run()
    {
        exec();
    }

    //! Enters the event loop and waits until exit() is called. Returns the exit code.
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

    //! Requests the thread's event loop to quit with return code 0. Thread-safe.
    void Thread::quit()
    {
        exit( 0 );
    }

    //! Requests the thread's event loop to exit with specified return code. Thread-safe.
    void Thread::exit
        (
        int aReturnCode  //!< Exit return code.
        )
    {
        mExitCode.store( aReturnCode );
        mExiting.store( true );
        if( auto dispatcher = mData->dispatcher() )
        {
            dispatcher->interrupt();
            dispatcher->wakeUp();
        }
    }

    //! Queues an arbitrary task to run on this thread's event loop.
    //!
    //! Always deferred to a later iteration of this thread's loop -- never run inline, even when
    //! post() is called from this thread itself. Implemented as a thin wrapper over
    //! Object::dispatchMetaCall() targeting this Thread as both context and receiver, so it goes
    //! through the exact same queue, MetaCallEvent, and lifetime handling as every other queued
    //! call in this library (removeEventsForReceiver() on destruction, processDeferredDeletes() on
    //! shutdown, etc.) rather than a second, parallel task queue. Returns true if the task was
    //! queued; false if this thread has no dispatcher yet (before start()/exec(), or after it has
    //! fully finished and released it), in which case the task is dropped rather than run.
    bool Thread::post
        (
        std::function<void()> aTask  //!< The callable to run on this thread. Ignored (returns false) if empty.
        )
    {
        // Basic sanity check. Usually ThreadData should outlive Thread.
        if( !aTask || mData == nullptr || mData->thread() == nullptr )
        {
            return false;
        }
        return dispatchMetaCallTo( mData, this, std::move( aTask ) );
    }

    //! Runs one pass of this thread's event loop, then returns.
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

    //! Installs a callback invoked whenever work is queued for this thread. Thread-safe.
    //!
    //! The other half of the adopted-thread story: it runs on whichever thread posted the work, and
    //! its job is to nudge this thread's own native loop -- typically by sending it a private event
    //! -- so that the loop knows to call processEvents(). Pass nullptr to remove it.
    //!
    //! Runs on the posting thread with no dispatcher lock held, so it may call back into this
    //! thread's dispatcher. It should still not block: it is on the critical path of every post,
    //! and blocking there stalls the poster rather than this thread. Nudge the native loop and
    //! return.
    void Thread::setWakeCallback
        (
        std::function<void()> aCallback  //!< Invoked on post; nullptr clears.
        )
    {
        if( auto dispatcher = mData->dispatcher() )
        {
            dispatcher->setWakeCallback( std::move( aCallback ) );
        }
    }

    //! Gets the event dispatcher for this thread. Thread-safe.
    //!
    //! Read-only by design. There is deliberately no setter: swapping a running thread's
    //! dispatcher raced against that thread's own start/finish lifecycle, which could delete a
    //! dispatcher an active exec()/processEvents() loop was still calling into. A thread creates
    //! and owns its dispatcher in start(); CoreApplication supplies the main thread's.
    //!
    //! Returns a strong reference rather than a raw pointer, so the dispatcher cannot be destroyed
    //! by its owning thread finishing while the caller is still using it. Returns nullptr before
    //! start().
    std::shared_ptr<AbstractEventDispatcher> Thread::eventDispatcher() const
    {
        return mData->dispatcher();
    }

    //! Sets the scheduling priority of this thread. Thread-safe.
    //!
    //! Only meaningful while the thread is running, as in Qt: there is no OS thread to act on
    //! before start(), and the value is deliberately not remembered for a later start() either.
    //! A call made when the thread is not running is rejected with a warning and changes nothing,
    //! so priority() will still report InheritPriority afterwards. To give a thread a priority
    //! from the outset, pass one to start() instead.
    //!
    //! What the OS does with the request varies, and a successful call does not promise the
    //! thread's scheduling actually changed. On Linux the default SCHED_OTHER policy reports a
    //! priority range of exactly one value, so every priority maps onto the same number and the
    //! call is accepted but has no effect; real prioritisation there needs a realtime policy and
    //! the privileges to select it. Qt behaves the same way. Windows applies all seven levels.
    void Thread::setPriority
        (
        Priority aPriority  //!< The priority to apply. InheritPriority is not accepted; rejected with a warning.
        )
    {
        if( aPriority == InheritPriority )
        {
            std::fprintf( stderr,
                "Thread::setPriority: InheritPriority cannot be set, only reported\n" );
            return;
        }

        std::lock_guard<std::mutex> lock( mPriorityMutex );

        // Qt refuses the same way. There is no OS thread to act on yet, and quietly stashing the
        // value for a future start() would promise a thread priority this class does not deliver.
        #if defined( _WIN32 )
            const bool haveThread = ( mHandle != nullptr );
        #else
            const bool haveThread = mJoinable;
        #endif
        if( !mData->isThreadRunning() || !haveThread )
        {
            std::fprintf( stderr,
                "Thread::setPriority: cannot set priority, thread is not running\n" );
            return;
        }

        mPriority = aPriority;
        applyPriority( aPriority );
    }

    //! Gets the scheduling priority of this thread. Thread-safe. Returns the priority last set on
    //! the running thread, or InheritPriority if the thread is not running or no priority has been
    //! set on this run.
    Thread::Priority Thread::priority() const
    {
        std::lock_guard<std::mutex> lock( mPriorityMutex );
        if( !mData->isThreadRunning() )
        {
            return InheritPriority;
        }
        return mPriority;
    }

    //! Checks if the thread is currently running. Thread-safe, and stale on return: the thread may
    //! start or finish before you act on the answer, so this reports an instant that has passed. To
    //! synchronise with a thread's end, call wait(). Qt attaches the same warning to
    //! QThread::isRunning(). See Global.hpp.
    bool Thread::isRunning() const
    {
        return mData->isThreadRunning();
    }

    //! Checks if the thread has finished execution. Thread-safe, and stale on return; see
    //! isRunning() above and Global.hpp.
    bool Thread::isFinished() const
    {
        return mFinishing.load();
    }

    //! Whether this Thread wraps a pre-existing native thread rather than one it started.
    bool Thread::isAdopted() const
    {
        return mAdopted.load();
    }

    //! Gets the underlying OS thread's id, valid once start() has published it. Useful mainly for
    //! asserting which thread a slot ran on.
    std::thread::id Thread::id() const
    {
        return mId.load();
    }

    //! Gets this thread's descriptive name, empty if it was not given one. Thread-safe: the name
    //! is set at construction and never changes.
    const std::string& Thread::name() const
    {
        return mName;
    }

    //! Gets a subscription-only view of the signal emitted when this thread's event loop starts
    //! running (Qt-like QThread::started()). Thread-safe.
    SignalView<>& Thread::getStarted() const
    {
        return mStarted.view();
    }

    //! Gets a subscription-only view of the signal emitted when this thread's event loop has
    //! exited (Qt-like QThread::finished()). Thread-safe.
    SignalView<>& Thread::getFinished() const
    {
        return mFinished.view();
    }

    //! Gets the Thread the calling thread is running as, never nullptr.
    //!
    //! If the caller was not started through this class -- the process's main thread, or a raw
    //! std::thread -- it is *adopted* on the spot: a Thread is created to represent it, given a
    //! default event dispatcher, and owned by a thread_local that releases it when the native
    //! thread exits. Qt does the same, and the guarantee it buys is that every Object has a thread
    //! affinity. Without it, an Object created off-thread had none, and a queued connection whose
    //! emitter was also affinity-less compared nullptr == nullptr, decided "same thread", and ran
    //! the slot directly on the emitting thread -- an unsynchronised cross-thread call that Qt
    //! explicitly does not perform ("if a QObject has no thread affinity ... it cannot receive
    //! queued signals or posted events").
    //!
    //! An adopted thread has a queue but no loop running on it, so queued work accumulates until
    //! the thread drains it with processEvents() (or runs exec()). That matches Qt, where posted
    //! events sit in the thread's list until something dispatches them.
    //!
    //! @note Do not store the returned pointer anywhere that may outlive the thread. For an adopted
    //! thread that is only until the native thread exits. Hold the ThreadData instead, which stays
    //! valid and reports thread() == nullptr once its Thread is gone.
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

    //! Makes this Thread represent the calling native thread.
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
        // the application". Without this, Thread::currentThread()->isRunning() answered false on
        // the main thread, which is both wrong and the opposite of Qt.
        //
        // It also makes start() on an adopted Thread the no-op it should be: the early-out there
        // tests this flag, and previously an adopted Thread would have gone on to create a second
        // OS thread underneath itself. setPriority() still refuses, because it additionally
        // requires a native handle, which an adopted thread has never had.
        mData->setThreadRunning( true );

        bindAffinityToSelf();

        if( !mData->dispatcher() )
        {
            mData->setDispatcher( std::make_shared<EventDispatcherDefault>() );
        }
    }

    //! Points this Thread's own Object affinity at the thread it represents.
    //!
    //! Deliberately bypasses moveToThread(), which would refuse. moveToThread() enforces that only
    //! the thread currently owning an object may re-home it, with an exception for objects that have
    //! no affinity yet -- and once every thread is adopted, a Thread constructed on thread A always
    //! *does* have affinity (to A), so a worker starting up would be refused permission to adopt
    //! itself. That rule exists to stop one thread yanking another thread's live object away, which
    //! is not what is happening here: this is the thread in question taking ownership of the object
    //! that represents it, at the only moment that can possibly be correct.
    //!
    //! Skips moveToThread()'s timer migration too. A Thread object with timers registered against it
    //! before its loop starts would keep them on the creating thread's dispatcher, which is a corner
    //! case not worth the confinement violation of migrating them from here.
    void Thread::bindAffinityToSelf()
    {
        mAffinity->setData( mData );
    }
}
