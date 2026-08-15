//! @file
//!
//! QtMimic::CoreApplication - mimics Qt's QCoreApplication. It adopts the
//! program's main thread as a Thread so Objects created on it gain thread
//! affinity, and runs an event loop (exec()) that dispatches both queued
//! Object slot invocations and external/OS-level events via a pluggable
//! dispatcher.
//!
//! Typical usage:
//! @code
//!   QtMimic::CoreApplication app(aArgc, aArgv);
//!   ... create Objects, wire signals ...
//!   return app.exec();   // blocks until quit()/exit()
//! @endcode
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


    //----------------------------------------------------------------
    //! @class CoreApplication
    //!
    //! Event-loop owner for the main thread. There is at most one instance, and
    //! it must be created on the thread that will run exec() (the main thread).
    //----------------------------------------------------------------
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
        //! @return parsed command-line arguments.
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

        static void post
            (
            std::function<void()> aTask
            );


        // CoreApplication must remain bound to the main thread.
        void moveToThread
            (
            Thread* aThread
            ) = delete;

    private:
        void adoptMainThread();

        //! The process-wide application instance.
        //!
        //! Atomic because instance(), exit(), quit() and post() are all callable from any thread and
        //! all read it, while the constructor and destructor write it from the main thread. Qt 6 has
        //! the identical plain pointer and works around it internally; Qt 7 makes it atomic.
        static std::atomic<CoreApplication*> sInstance;

        //! The adopted main thread. Non-owning: the Thread is owned by a thread_local inside
        //! Thread and released when the native thread exits, not by this application.
        Thread* mMainThread { nullptr };

        //! The platform dispatcher this application installed on the main thread, kept so the
        //! destructor can drain it and hand the thread back a plain one.
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;

        //! True while exec() is running its loop, so a re-entrant call can be refused.
        //!
        //! Atomic because the rejecting read happens on whichever thread called exec(), which is
        //! not necessarily the one that set it -- the off-thread call is rejected by the check
        //! above this one, but only after this flag has been read.
        std::vector<std::string>                 mArgs;
        std::atomic<bool>                        mInExec { false };
    };

} // namespace QtMimic

#endif // QT_MIMIC_COREAPPLICATION_HPP
