// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::Thread - an event-loop thread. Every Object "lives" in one Thread; queued slot
//! invocations are executed in that thread's event loop.
//!
//! A Thread owns a ThreadData, which owns the thread's event dispatcher, which owns the event queue
//! and the timer list. Posting therefore goes through the ThreadData and never dereferences a
//! Thread* a concurrent ~Thread() could free.

#ifndef QT_LIKE_SIGNAL_THREAD_HPP
#define QT_LIKE_SIGNAL_THREAD_HPP

// For ProcessEventsFlag, which appears in processEvents()'s default argument and so cannot be
// forward declared.
#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if !defined( _WIN32 )
    // For pthread_t only. A real pthread_t is more honest about what the member is than an
    // opaque handle would be, and this is a source-only library with no ABI to protect. The
    // Windows handle is a void* instead, purely to keep <windows.h> -- and its macros -- out of
    // every translation unit that includes this header.
    #include <pthread.h>
#endif

namespace QtLikeSignal
{
    class AbstractEventDispatcher;

    //----------------------------------------------------------------
    //! @class Thread
    //!
    //! An event-loop thread. Objects bound to this thread have their queued slots and posted tasks
    //! executed here, one at a time, in FIFO order.
    //!
    //! Usage:
    //! @code
    //!   QtLikeSignal::Thread worker("worker");
    //!   worker.start();        // begins the event loop
    //!   ...
    //!   worker.quit();         // ask the loop to drain and exit (also done by dtor)
    //! @endcode
    //----------------------------------------------------------------
    class Thread : public Object
    {
    public:
        //! Constructs an unstarted thread with an optional name and parent.
        //!
        //! The name is descriptive only -- it is not pushed to the OS and nothing keys off it. It
        //! exists so a thread can identify itself in a log or a test failure, which matters most
        //! exactly when several are running at once.
        //!
        //! **A parent owns the thread and will delete it**, as it would any other child, which for
        //! a Thread means the destructor's quit() and wait() run from the parent's destructor. That
        //! is safe here and is not in Qt: ~QThread() aborts with "Destroyed while thread is still
        //! running", whereas ~Thread() stops the loop and joins.
        //!
        //! The cost of that safety is worth knowing before using it: deleting the parent **blocks**
        //! until the thread finishes whatever it is doing. On a latency-sensitive teardown path,
        //! stop the thread yourself first and let the parent's delete find it already finished.
        //!
        //! The parent must live in the thread that is constructing this one, which is the ordinary
        //! parent-child rule; setParent() reports a refusal on stderr if it does not.
        explicit Thread
            (
            const std::string& aName = std::string(),
            Object* aParent = nullptr
            );

        //! Virtual so a subclass overriding run() can be destroyed through a Thread*, which the
        //! subclassing idiom below makes reachable. See run().
        virtual ~Thread() override;

        Thread
            (
            const Thread&
            ) = delete;

        Thread& operator=
            (
            const Thread&
            ) = delete;

        const std::string& name() const;

        std::thread::id id() const;

        //! Scheduling priority of a thread, mirroring QThread::Priority.
        //!
        //! The numeric order is load-bearing, not cosmetic: the UNIX backend scales these values
        //! arithmetically onto whatever range the platform scheduler reports, so IdlePriority
        //! must stay lowest and TimeCriticalPriority highest, with no gaps introduced between
        //! them.
        //!
        //! InheritPriority means "whatever the thread that called start() was running at". It is
        //! the state reported when there is no OS thread to ask. start() accepts it -- it is the
        //! default -- but setPriority() does not, because once a thread is running there is no
        //! operation that corresponds to re-inheriting.
        enum Priority
        {
            IdlePriority,

            LowestPriority,
            LowPriority,
            NormalPriority,
            HighPriority,
            HighestPriority,

            TimeCriticalPriority,

            InheritPriority
        };

        void start
            (
            Priority aPriority = InheritPriority
            );

        void startPlatformSpecific();

        void quit();

        void exit
            (
            int aReturnCode = 0
            );

        //! Blocks until the thread has finished, or @p aTime milliseconds have passed.
        //!
        //! Returns bool with a defaulted timeout, as QThread::wait() does, so the no-argument
        //! call keeps meaning "block until finished".
        //! @return true if the thread finished (or there was nothing to wait for); false if the
        //!         timeout expired first.
        bool wait
            (
            unsigned long aTime = ULONG_MAX
            );

        bool isRunning() const;

        bool isFinished() const;

        void setPriority
            (
            Priority aPriority
            );

        Priority priority() const;

        static Thread* currentThread();

        void processEvents
            (
            AbstractEventDispatcher::ProcessEventsFlag aFlag
            = AbstractEventDispatcher::ProcessEventsFlag::AllEvents
            );

        void setWakeCallback
            (
            std::function<void()> aCallback
            );

        bool isAdopted() const;

        std::shared_ptr<AbstractEventDispatcher> eventDispatcher() const;

        //! @return true if the task was queued; false if it was refused, in which case **it will
        //! never run**.
        //!
        //! [[nodiscard]] because a discarded false is a task that silently disappears, and the
        //! compiler is the only thing that can see every call site. A caller that genuinely does
        //! not care must say so with a cast to void, so that the decision is visible in the source
        //! rather than implied by its absence. The build promotes the diagnostic to an error --
        //! /we4834 on MSVC, -Werror=unused-result elsewhere; see README.md, "Compiler
        //! configuration".
        //!
        //! Refusal means the thread has no dispatcher and is not on its way to having one: it was
        //! never started, or it has already finished. A post to a thread that is *starting* is
        //! parked and delivered when its loop comes up, so that case does not refuse.
        [[nodiscard]] bool post
            (
            std::function<void()> aTask
            );

        //! Runs @p aTask on this thread's loop and blocks until it has finished.
        //!
        //! What post() is missing: post() says the task was *queued*, this says it is *done*. It is
        //! the primitive behind ConnectionType::BlockingQueued, and is offered separately because
        //! plenty of callers want to hand a thread some work and wait without there being a signal
        //! anywhere in sight -- a render loop asking its render thread to sync is the usual case.
        //!
        //! **Returns a value by writing into the caller's own frame.** There is no templated
        //! return type here because none is needed: the caller is blocked for the whole call, so a
        //! local captured by reference is alive and safe to write.
        //!
        //! ```
        //! int answer = 0;
        //! const bool ran = worker.invokeAndWait( [&answer]() { answer = compute(); } );
        //! ```
        //!
        //! Called from this thread itself, the task runs inline rather than deadlocking. See
        //! ConnectionType::BlockingQueued for why that is the answer rather than an assertion.
        //!
        //! @return true if the task ran to completion; false if it was refused, or was discarded
        //!         undelivered because this thread's loop ended first. **false means the task did
        //!         not run**, so anything the caller expected it to write is untouched.
        [[nodiscard]] bool invokeAndWait
            (
            std::function<void()> aTask
            );

        SignalView<>& getStarted() const;

        SignalView<>& getFinished() const;

        //! Creates a Thread that will execute the specified function. Function is the callable
        //! type and Args its argument types. Thread-safe.
        //!
        //! **The caller acquires ownership of the returned Thread**, and must eventually delete it
        //! (directly, or by connecting finished to its own deleteLater()). Returned raw rather than
        //! in a unique_ptr precisely so that self-deletion stays possible; a unique_ptr would turn
        //! the usual "delete myself once my loop ends" idiom into a double delete. Qt returns a raw
        //! QThread* and documents ownership the same way, for the same reason.
        //!
        //! **The thread is not started.** Call start() when ready. That gap is the point: it is the
        //! only window in which you can connect to started, move Objects onto the thread, or choose
        //! its priority -- setPriority() refuses on a thread that is not running, so a thread that
        //! started itself can never be given one without a race. Qt's QThread::create() leaves the
        //! thread unstarted for exactly these reasons.
        template <typename Function, typename ... Args>
        [[nodiscard]] static Thread* create
            (
            Function&& aF,
            Args&&... aArgs
            );

    protected:
        //! The body the new thread executes. Override to do something other than run an event
        //! loop, exactly as QThread::run() is overridden; the default enters the loop and stays
        //! there until quit()/exit().
        //!
        //! An override that wants an event loop after doing its own setup should call exec().
        //! One that does not call either simply returns, and the thread ends -- which is the Qt
        //! behaviour too, and means queued slots for objects on this thread will never run.
        //!
        //! Note what this is NOT responsible for: publishing the thread id, creating the
        //! dispatcher, and applying a priority the kernel refused at creation all happen in
        //! threadBody() before this is called, so an override cannot skip them by forgetting to
        //! chain up.
        virtual void run();

        int exec();

    private:
        //! Gets the thread's internal data container holding the event dispatcher.
        //!
        //! Private for the same reason as Object::threadData(): it is the handle onto the
        //! dispatcher plumbing, not API. Object reaches it when adopting a thread's affinity.
        ThreadData* threadData() const
        {
            return mData.get();
        }

        //! Platform half of start(): creates the OS thread (already at mPriority when it
        //! executes its first instruction) and publishes its handle. Implemented in
        //! ThreadWin.cpp / ThreadPosix.cpp so the platform code sits in one place per platform
        //! instead of scattered through #if blocks. Called with mPriorityMutex held.
        void applyPriority
            (
            Priority aPriority
            );

        //! Everything the new thread must do whether or not run() is overridden.
        //!
        //! Kept separate from run() precisely so it cannot be overridden away: an override that
        //! did not call Thread::run() would otherwise leave the thread id unpublished, the
        //! dispatcher uncreated and a refused priority unapplied.
        void threadBody();

        void adoptCallingThread();

        void bindAffinityToSelf();

        #if defined( _WIN32 )
            static unsigned int __stdcall threadEntry
                (
                void* aArg
                );

        #else
            static void* threadEntry
                (
                void* aArg
                );

        #endif

        #if defined( _WIN32 )
            //! The OS thread handle from _beginthreadex(), or nullptr when there is none to
            //! reap. Typed void* so this header does not pull in <windows.h>. Guarded by
            //! mPriorityMutex; owned, and closed by whichever wait() sees the thread finish with
            //! no other waiter left inside the wait.
            void* mHandle { nullptr };

            //! How many calls are currently blocked inside WaitForSingleObject() on mHandle.
            //! Closing the handle out from under one of them would be a use-after-close, so the
            //! last one out closes it. Guarded by mPriorityMutex.
            int mWaiters { 0 };

        #else
            //! The OS thread from pthread_create(). Meaningful only while mJoinable.
            pthread_t mThreadId {};

            //! True while mThreadId names a thread that has been created and not yet joined.
            //! Joining twice is undefined, so this is what makes the join happen exactly once no
            //! matter how many callers reach wait(). Guarded by mPriorityMutex.
            bool mJoinable { false };

        #endif

        std::string mName;                        //!< Thread name

        //! Id of the OS thread, published by threadBody() before anything else. Atomic because
        //! id() may be asked from any thread while the thread being described publishes it.
        std::atomic<std::thread::id> mId {};

        //! Per-thread state that outlives this Thread. Created in the constructor with a
        //! back-pointer to this, cleared in ~Thread(). Anything that needs to remember "which
        //! thread" holds this rather than a raw Thread*, so it can never dangle. It also owns the
        //! event dispatcher, so posting goes through the ThreadData and never dereferences this
        //! Thread.
        std::shared_ptr<ThreadData> mData;

        //! True once the run body is completely done. This is what wait() waits for, and it is
        //! deliberately NOT what isFinished() reports -- see mFinishing.
        //!
        //! Qt draws exactly this distinction: threadState goes Running -> Finishing -> Finished,
        //! isFinished() tests >= Finishing, and wait() waits for Finished. One flag cannot do both
        //! jobs. Reporting "finished" only at the later point would tell a finished() handler that
        //! the thread is still running; waking wait() at the earlier one would release a waiter
        //! while the thread is still emitting finished(), and the caller could then destroy the
        //! Thread -- and the signal being emitted -- out from under it.
        std::atomic<bool> mHasFinished { false };

        //! True from the moment the run body starts winding down, before finished() is emitted.
        //! Mirrors Qt's threadState >= Finishing, and stays false forever for an adopted thread,
        //! which Qt never moves out of Running. This is what isFinished() reports.
        std::atomic<bool> mFinishing { false };

        Signal<> mStarted;         //!< Emitted when loop starts

        Signal<> mFinished;        //!< Emitted when loop exits

        std::atomic<bool> mExiting { false };  //!< Set by exit()/quit() to stop exec()'s loop.

        //! Value returned by exec(). Atomic because exit() may be called from any thread while
        //! the loop thread is about to read it -- ThreadSanitizer flags the plain int.
        std::atomic<int> mExitCode { 0 };

        //! Guards mWaitCv's predicate (mHasFinished).
        //!
        //! Separate from mPriorityMutex because wait() must not hold that one across a blocking
        //! wait: run()'s priority fix-up needs it to get past its very first step, and the thread
        //! cannot finish until it does.
        mutable std::mutex mWaitMutex;

        //! Notified by threadBody() once the run body is completely done, for wait().
        //!
        //! POSIX needs this: pthread_join() has no portable timed form, so the timeout is served
        //! here and the join that follows is only ever the already-finished kind. Windows waits on
        //! the OS thread handle instead, which takes a timeout directly.
        std::condition_variable mWaitCv;

        //! Guards mPriority, mPriorityNeedsReset, and the native handle members above, including
        //! every use of that handle (creation, priority application, wait()).
        //!
        //! Not merely protecting the enum. The run body clears the running flag while holding this
        //! mutex, so a setPriority() that has observed it true under the same lock is guaranteed the
        //! OS thread has not yet reached the end of its body -- without that, the handle could be
        //! touched after the thread had exited. start() holds it across thread creation for the same
        //! reason in reverse: nobody may see the flag true before the handle exists, and a UNIX
        //! priority fix-up inside run() cannot run before the priority meant for THIS run is decided.
        //!
        //! It is never held across a blocking wait, so a waiter cannot keep the finishing thread from
        //! taking it.
        mutable std::mutex mPriorityMutex;

        Priority mPriority { InheritPriority };  //!< Priority applied to the current/most recent run.

        //! Set when the new thread has to apply its own priority instead of being born with it.
        //!
        //! Only ever true on UNIX, and only when the kernel refused the scheduling attributes
        //! passed to pthread_create(); the thread then inherits the caller's priority and fixes
        //! it up itself. Guarded by mPriorityMutex, which is also what makes the fix-up wait for
        //! start() to publish the handle it needs.
        bool mPriorityNeedsReset { false };

        //! True if this represents an already-running native thread rather than one start()
        //! created. An adopted Thread has no OS thread of its own to start, join or prioritise: it
        //! exists to give the native thread an identity and an event queue.
        std::atomic<bool> mAdopted { false };

        static thread_local Thread* sCurrentThread;  //!< The Thread running on this OS thread, if any.

        //! Owns the Thread auto-created to represent a native thread nobody started through us.
        //!
        //! Lives exactly as long as that native thread: destroyed when the thread exits, which
        //! nulls its ThreadData back-pointer so anything still holding that data sees
        //! thread() == nullptr rather than a dangling pointer. Mirrors Qt adopting a foreign
        //! QThread.
        static thread_local std::unique_ptr<Thread> sAdoptedThread;

        //! Guards against re-entering auto-adoption while it is constructing the adopted Thread.
        //!
        //! Thread derives from Object, and Object's constructor asks for currentThread() -- so
        //! creating the adopted Thread re-enters currentThread() and, without this, would recurse
        //! until the stack ran out. While set, currentThread() reports nullptr, so the adopted
        //! Thread's own Object base is simply built with no affinity and bindAffinityToSelf()
        //! points it at itself immediately afterwards.
        static thread_local bool sAdopting;

        //! The Thread this thread is registered as, or nullptr if it is not registered.
        //!
        //! currentThread() answers the same question but adopts the caller when the answer would
        //! be nullptr, which makes it unusable anywhere that must not allocate or must not run
        //! during thread_local teardown -- ~Object()'s misuse diagnostic is both. Adopting there
        //! re-enters the very unique_ptr being destroyed.
        static Thread* currentThreadOrNull()
        {
            return sCurrentThread;
        }

        friend class CoreApplication;

        //! Grants Object access to threadData() when adopting or releasing thread affinity.
        friend class Object;
    };

    //! Creates a Thread that will execute the specified function, without starting it.
    //!
    //! The wrapping FuncThread subclass exists purely so create() can hand back a plain Thread*
    //! without requiring callers to declare their own subclass just to run a callable.
    template <typename Function, typename ... Args>
    Thread* Thread::create
        (
        Function&& aF,      //!< Function to execute.
        Args&&... aArgs      //!< Arguments to pass.
        )
    {
        auto task = std::bind( std::forward<Function>( aF ), std::forward<Args>( aArgs )... );

        //! Adapts an arbitrary bound callable into a Thread by running it from run().
        class FuncThread : public Thread
        {
        public:
            FuncThread
                (
                std::function<void()> aFn
                )
                : mFn( std::move( aFn ) )
            {
            }

        protected:
            virtual void run() override
            {
                if( mFn )
                {
                    mFn();
                }
            }

        private:
            std::function<void()> mFn;
        };

        // Deliberately NOT started here -- see the declaration. Starting it would close the only
        // window in which the caller can connect to started, re-home Objects onto it, or set its
        // priority.
        return new FuncThread( task );
    }

} // namespace QtLikeSignal

#endif // QT_LIKE_SIGNAL_THREAD_HPP
