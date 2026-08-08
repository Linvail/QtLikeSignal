#ifndef THREAD_H
#define THREAD_H

#include "Object.h"
#include "Signal.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#if !defined( _WIN32 )
    // For pthread_t only. Included here rather than hidden behind an opaque handle because this
    // is a source-only library with no ABI to protect, and a real pthread_t is honest about what
    // the member is. The Windows handle is a void* instead, purely to keep <windows.h> -- and its
    // macros -- out of every translation unit that includes this header.
    #include <pthread.h>
#endif

namespace QtLikeSignal
{
    class AbstractEventDispatcher;

    //! Manages a platform execution thread with an event loop.
    class Thread : public Object
    {
    public:
        Thread();

        virtual ~Thread() override;

        //! Scheduling priority of a thread, mirroring QThread::Priority.
        //!
        //! The numeric order is load-bearing, not cosmetic: the UNIX backend scales these values
        //! arithmetically onto whatever range the platform scheduler reports, so IdlePriority must
        //! stay lowest and TimeCriticalPriority highest, with no gaps introduced between them.
        //!
        //! InheritPriority means "whatever the thread that called start() was running at". It is the
        //! state a thread begins in and is reported for a thread that is not running. start() accepts
        //! it -- it is the default -- but setPriority() does not, because once a thread is running
        //! there is no operation that corresponds to re-inheriting.

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

        std::shared_ptr<AbstractEventDispatcher> eventDispatcher() const;

        bool post
            (
            std::function<void()> aTask
            );

        //! Signal emitted when the thread starts running.
        Signal<> started;

        //! Signal emitted when the thread finishes execution.
        Signal<> finished;

        //! Creates and starts a Thread executing the specified function. Function is the callable
        //! type and Args its argument types. Thread-safe.
        template <typename Function, typename ... Args>
        static Thread* create( Function&& aF, Args&&... aArgs );

    protected:
        virtual void run();

        int exec();

    private:
        //! Gets the thread's internal data container holding the event dispatcher.
        //!
        //! Private for the same reason as Object::threadData(): it is the handle onto the dispatcher
        //! plumbing, not API. Object reaches it when adopting a thread's affinity.
        std::shared_ptr<ThreadData> threadData() const
        {
            return mData;
        }

        void applyPriority
            (
            Priority aPriority
            );

        void threadBody();

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
            //! The OS thread handle from _beginthreadex(), or nullptr when there is none to reap.
            //!
            //! Typed void* so this header does not pull in <windows.h>. Owned: closed by
            //! whichever wait() sees the thread finish with no other waiter left inside the wait.
            void* mHandle { nullptr };

            //! How many calls are currently blocked inside WaitForSingleObject() on mHandle.
            //!
            //! Closing the handle out from under one of them would be a use-after-close, so the
            //! last one out closes it. Guarded by mPriorityMutex.
            int mWaiters { 0 };
        #else
            //! The OS thread from pthread_create(). Meaningful only while mJoinable.
            pthread_t mThreadId {};

            //! True while mThreadId names a thread that has been created and not yet joined.
            //!
            //! Joining twice is undefined, so this is what makes the join happen exactly once no
            //! matter how many callers reach wait(). Guarded by mPriorityMutex.
            bool mJoinable { false };
        #endif

        std::shared_ptr<ThreadData> mData;        //!< This thread's dispatcher-holding data.
        std::atomic<bool> mFinished { false };     //!< True once the OS thread has finished.
        std::atomic<bool> mExiting { false };      //!< Set by exit()/quit() to stop exec()'s loop.
        std::atomic<int> mExitCode { 0 };          //!< Return code passed to exit(), reported by exec().
        mutable std::mutex mWaitMutex;             //!< Guards mWaitCv's predicate.
        std::condition_variable mWaitCv;           //!< Notified when the thread finishes, for wait().

        //! Guards mPriority, the native handle members, and every use of that handle.
        //!
        //! Not merely protecting the enum. The run body clears the running flag while holding
        //! this mutex, so a setPriority() that has observed it true under the same lock is
        //! guaranteed the OS thread has not yet reached the end of its body -- without that, the
        //! handle could be touched after the thread had exited. start() holds it across thread
        //! creation for the same reason in reverse: nobody may see the flag true before the
        //! handle exists. It is never held across a blocking wait, so a waiter cannot keep the
        //! finishing thread from taking it.
        mutable std::mutex mPriorityMutex;
        Priority mPriority { InheritPriority };  //!< Priority applied to the current/most recent run.

        //! Set when the new thread has to apply its own priority instead of being born with it.
        //!
        //! Only ever true on UNIX, and only when the kernel refused the scheduling attributes
        //! passed to pthread_create(); the thread then inherits the caller's priority and fixes
        //! it up itself. Guarded by mPriorityMutex, which is also what makes the fix-up wait for
        //! start() to publish the handle it needs.
        bool mPriorityNeedsReset { false };

        static thread_local Thread* sCurrentThread;  //!< The Thread running on this OS thread, if any.
        friend class CoreApplication;
        //! Grants Object access to threadData() when adopting or releasing thread affinity.
        friend class Object;
    };

    //! Creates and starts a Thread executing the specified function.
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

        auto* threadObj = new FuncThread( task );
        threadObj->start();
        return threadObj;
    }
}

#endif // THREAD_H
