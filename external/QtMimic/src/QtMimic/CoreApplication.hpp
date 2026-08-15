//! @file
//!
//! QtMimic::CoreApplication -- mimics Qt's QCoreApplication. It adopts the program's main thread as a
//! Thread so Objects created on it gain thread affinity, and runs an event loop (exec()) that
//! dispatches both queued Object slot invocations and external events through a pluggable
//! dispatcher. The class documentation below carries the usage example.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_COREAPPLICATION_HPP
#define QT_MIMIC_COREAPPLICATION_HPP

#include "QtMimic/Object.hpp"
#include "QtMimic/Thread.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace QtMimic
{
    class AbstractEventDispatcher;
    class Event;


    //! Owns the main thread's event loop; the base class a real application derives from.
    //!
    //! Construct one instance, on the thread that will run exec(). Constructing it *adopts* that
    //! thread: it becomes a Thread with an event dispatcher, so Objects created on it gain thread
    //! affinity and can receive queued signals, posted events, timers and deleteLater().
    //!
    //! @code
    //!   class MyApplication : public CoreApplication
    //!   {
    //!   public:
    //!       void init();
    //!       void deInit();
    //!   };
    //!
    //!   int main( int argc, char** argv )
    //!   {
    //!       MyApplication app;
    //!       app.init();
    //!       const int ret = app.exec();   // the main thread's event loop runs here
    //!       app.deInit();
    //!       return ret;
    //!   }
    //! @endcode
    //!
    //! There must be at most one instance. Unlike Qt this class does not own the loop itself: the
    //! adopted main Thread does, and exec() simply runs it, so the main thread and a worker thread
    //! use one event loop implementation rather than two that can drift apart.
    class CoreApplication : public Object
    {
    public:
        //! Constructs an application with no command-line arguments, for code that has none to
        //! pass on -- a test, or a program embedding the loop. arguments() is then empty.
        CoreApplication();

        CoreApplication
            (
            int aArgc,
            char** aArgv
            );

        virtual ~CoreApplication() override;

        CoreApplication
            (
            const CoreApplication&
            ) = delete;

        CoreApplication& operator=
            (
            const CoreApplication&
            ) = delete;

        static CoreApplication* instance();
        //! Gets the command-line arguments, or an empty list if the default constructor was used.
        const std::vector<std::string>& arguments() const
        {
            return mArgs;
        }

        int exec();

        static void exit
            (
            int aReturnCode = 0
            );

        static void quit();

        //! Queues a task onto the main thread's event loop. Thread-safe.
        //!
        //! Static, like exit()/quit(), so any thread can hand work to the main loop without holding
        //! a pointer to the application. Does nothing if no application exists.
        //!
        //! Thread-safety is not guaranteed if the CoreApplication object is being destroyed at the
        //! same time. This holds for exit() and quit() too; the definitions carry the reasoning.
        static void post
            (
            std::function<void()> aTask
            );


        // CoreApplication must remain bound to the main thread.
        //! The application is bound to the thread it adopted and cannot be re-homed.
        //!
        //! Deleted rather than merely documented: moving it would leave exec() running a loop on a
        //! thread the application no longer lives in, and every guard in exec() reasoning about
        //! "the main thread" would silently become wrong.
        void moveToThread
            (
            Thread* aThread
            ) = delete;

    private:
        void adoptMainThread();

        //! The process-wide application instance.
        //!
        //! Atomic because instance(), exit(), quit() and post() are all callable from any thread
        //! and all read it, while the constructor and destructor write it from the main thread. A
        //! plain pointer made every one of those a data race, which is what the "Thread-safe" on
        //! each of them promised it was not.
        static std::atomic<CoreApplication*> sInstance;

        //! The adopted main thread. Non-owning: the thread_local inside Thread owns it, and
        //! releases it when the native thread exits.
        Thread* mMainThread { nullptr };

        //! The platform dispatcher this application installed on the main thread, kept so the
        //! destructor can drain it and hand the thread back a plain one.
        //
        // shared_ptr rather than unique_ptr: ThreadData hands out strong references, so a
        // dispatcher cannot be destroyed while another thread is part-way through a call into it.
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;

        //! Command-line arguments, if supplied.
        std::vector<std::string> mArgs;

        //! True while exec() is running its loop, so a re-entrant call can be refused.
        //!
        //! Atomic because the rejecting read happens on whichever thread called exec(), which is
        //! not necessarily the one that set it -- the off-thread call is rejected by the check
        //! above this one, but only after this flag has been read.
        std::atomic<bool> mInExec { false };
    };

} // namespace QtMimic

#endif // QT_MIMIC_COREAPPLICATION_HPP
