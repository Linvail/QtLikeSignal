//! @file
//!
//! QtMimic::Thread - an event-loop thread. Every Object "lives" in one
//! Thread; queued slot invocations are executed in that thread's event loop.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_THREAD_HPP
#define QT_MIMIC_THREAD_HPP

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
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

#include "Signal.hpp"
#include "ThreadData.hpp"

#include <memory>

namespace QtMimic
{

    //----------------------------------------------------------------
    //! @class Thread
    //!
    //! An event-loop thread. Objects bound to this thread have their queued slots
    //! and posted tasks executed here, one at a time, in FIFO order.
    //!
    //! Usage:
    //! @code
    //!   QtMimic::Thread worker("worker");
    //!   worker.start();        // begins the event loop
    //!   ...
    //!   worker.quit();         // ask the loop to drain and exit (also done by dtor)
    //! @endcode
    //----------------------------------------------------------------
    class Thread
    {
    public:
        explicit Thread
            (
            const std::string& aName = std::string()
            );

        //! Virtual so a subclass overriding run() can be destroyed through a Thread*, which the
        //! subclassing idiom below makes reachable. See run().
        virtual ~Thread();

        Thread
            (
            const Thread&
            ) = delete;

        Thread& operator=
            (
            const Thread&
            ) = delete;

        Thread
            (
            Thread&&
            ) = delete;

        Thread& operator=
            (
            Thread&&
            ) = delete;

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

        void setPriority
            (
            Priority aPriority
            );

        Priority priority() const;

        int exec();

        void setDispatcher
            (
            std::function<void( int aTimeoutMs )> aDispatcher
            );

        bool post
            (
            std::function<void()> aTask
            );

        void processEvents();

        void setWakeCallback
            (
            std::function<void()> aWake
            );

        void setWaiter
            (
            std::function<void( int aTimeoutMs )> aWaiter,
            int aTimeoutMs = -1
            );

        void quit();

        void exit
            (
            int aCode
            );

        void wait();

        bool isCurrent() const;

        std::thread::id id() const;

        const std::string& name() const;

        SignalView<>& getStarted() const;

        SignalView<>& getFinished() const;

        static Thread* currentThread();

        static std::shared_ptr<ThreadData> currentData();

        std::shared_ptr<ThreadData> data() const;

    protected:
        //! The body the new thread executes. Override to do something other than run an event
        //! loop, exactly as QThread::run() is overridden; the default enters the loop and stays
        //! there until quit()/exit().
        //!
        //! An override that wants an event loop after doing its own setup should call exec().
        //! One that does not call either simply returns, and the thread ends -- which is the Qt
        //! behaviour too, and means queued slots for objects on this thread will never run.
        //!
        //! Note what this is NOT responsible for: publishing the thread id and applying a
        //! priority the kernel refused at creation both happen in threadBody() before this is
        //! called, so an override cannot skip them by forgetting to chain up.
        virtual void run();

    private:
        //! Everything the new thread must do whether or not run() is overridden.
        //!
        //! Kept separate from run() precisely so it cannot be overridden away: an override that
        //! did not call Thread::run() would otherwise leave the thread id unpublished and a
        //! refused priority unapplied.
        void threadBody();

        void loop();

        void adopt();

        //! Platform half of start(): creates the OS thread (already at mPriority when it
        //! executes its first instruction) and publishes its handle. Implemented in
        //! ThreadWin.cpp / ThreadPosix.cpp so the platform code sits in one place per platform
        //! instead of scattered through #if blocks. Called with mPriorityMutex held.
        void startPlatformSpecific();

        void applyPriority
            (
            Priority aPriority
            );

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

        //! Per-thread state that outlives this Thread. Created in the constructor with a
        //! back-pointer to this, cleared in ~Thread(). Anything that needs to remember "which
        //! thread" holds this rather than a raw Thread*, so it can never dangle. It also owns the
        //! event mailbox (task queue + its mutex/condvar/accepting flag + wake callback), so posting
        //! goes through the ThreadData and never dereferences this Thread.
        std::shared_ptr<ThreadData> mData;
        std::string mName;                        //!< Thread name

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
            //! Joining twice is undefined, so this is what makes wait() a no-op once a previous
            //! wait() has already reaped it. Guarded by mPriorityMutex.
            bool mJoinable { false };
        #endif

        std::thread::id mId;                      //!< Id of the OS thread, set from inside it.
        bool mAdopted = false; //!< True if this represents an already-running native thread
        int mExitCode = 0; //!< Value returned by exec()
        std::function<void( int )> mDispatcher; //!< External event pump (optional)
        std::function<void( int )> mWaiter;   //!< External blocking wait (optional)
        int mWaiterTimeoutMs = -1;            //!< Timeout for external wait (optional)
        Signal<> mStarted;         //!< Emitted when loop starts
        Signal<> mFinished;        //!< Emitted when loop exits

        //! Guards mPriority, mPriorityNeedsReset, and the native handle members above, including
        //! every use of that handle (creation, priority application, wait()).
        //!
        //! start() holds it across thread creation, so a UNIX priority fix-up inside run() can
        //! never run before the priority meant for THIS run has been decided, and
        //! setPriority()/priority() can never observe a handle published for a run whose
        //! priority is still being set up.
        mutable std::mutex mPriorityMutex;
        Priority mPriority { InheritPriority };  //!< Priority applied to the current/most recent run.

        //! Set when the new thread has to apply its own priority instead of being born with it.
        //!
        //! Only ever true on UNIX, and only when the kernel refused the scheduling attributes
        //! passed to pthread_create(); the thread then inherits the caller's priority and fixes
        //! it up itself. Guarded by mPriorityMutex, which is also what makes the fix-up wait for
        //! start() to publish the handle it needs.
        bool mPriorityNeedsReset { false };
    };

} // namespace QtMimic

#endif // QT_MIMIC_THREAD_HPP
