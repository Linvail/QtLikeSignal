#ifndef QT_LIKE_SIGNAL_THREAD_H
#define QT_LIKE_SIGNAL_THREAD_H

#include "Object.h"
#include "Signal.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
        //! Constructs an unstarted thread with an optional name.
        //!
        //! The name is descriptive only -- it is not pushed to the OS and nothing keys off it. It
        //! exists so a thread can identify itself in a log or a test failure, which matters most
        //! exactly when several are running at once.
        explicit Thread
            (
            const std::string& aName = std::string()
            );

        virtual ~Thread() override;

        const std::string& name() const;

        std::thread::id id() const;

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

        void processEvents();

        void setWakeCallback
            (
            std::function<void()> aCallback
            );

        bool isAdopted() const;

        std::shared_ptr<AbstractEventDispatcher> eventDispatcher() const;

        bool post
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
        [[nodiscard]] static Thread* create( Function&& aF, Args&&... aArgs );

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

        std::string mName;                        //!< Descriptive name; see the constructor.

        //! The running OS thread's std::thread::id, published by threadBody() before anything
        //! else. Default-constructed -- and so equal to no live thread -- until then.
        std::atomic<std::thread::id> mId {};
        std::shared_ptr<ThreadData> mData;        //!< This thread's dispatcher-holding data.
        //! True once the run body has finished and the OS thread has been reaped. This is what
        //! wait() waits for, and it is deliberately NOT what isFinished() reports -- see mFinishing.
        std::atomic<bool> mHasFinished { false };

        //! True from the moment the run body starts winding down, before finished() is emitted.
        //!
        //! Qt draws exactly this distinction: threadState goes Running -> Finishing -> Finished,
        //! isFinished() tests >= Finishing, and wait() waits for Finished. One flag cannot do both
        //! jobs. Reporting "finished" only at the later point would tell a finished() handler that
        //! the thread is still running; waking wait() at the earlier one would release a waiter
        //! while the thread is still tearing itself down, and the caller could then destroy the
        //! Thread out from under it.
        std::atomic<bool> mFinishing { false };

        //! Emitted when the event loop starts running. Private, handed out by getStarted() as a
        //! view: only this thread may announce that it started.
        Signal<> mStarted;

        //! Emitted when the event loop has exited. Private for the same reason as mStarted.
        Signal<> mFinished;
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

        //! True for a Thread that wraps an already-running native thread rather than one it started.
        //!
        //! An adopted Thread has no OS thread of its own to start, join or prioritise: it exists to
        //! give the native thread an identity and an event queue.
        bool mAdopted { false };

        static thread_local Thread* sCurrentThread;  //!< The Thread running on this OS thread, if any.

        //! Owns the Thread auto-created to represent a native thread nobody started through us.
        //!
        //! Lives exactly as long as that native thread: destroyed when the thread exits, which nulls
        //! its ThreadData back-pointer so anything still holding that data sees thread() == nullptr
        //! rather than a dangling pointer. Mirrors Qt adopting a foreign QThread.
        static thread_local std::unique_ptr<Thread> sAdoptedThread;

        //! Guards against re-entering auto-adoption while it is constructing the adopted Thread.
        //!
        //! Thread derives from Object, and Object's constructor asks for currentThread() -- so
        //! creating the adopted Thread re-enters currentThread() and, without this, would recurse
        //! until the stack ran out. While set, currentThread() reports nullptr, so the adopted
        //! Thread's own Object base is simply built with no affinity and bindAffinityToSelf() points
        //! it at itself immediately afterwards.
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
}

#endif // QT_LIKE_SIGNAL_THREAD_H
