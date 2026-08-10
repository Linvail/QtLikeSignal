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

#ifndef QT_MIMIC_CORE_APPLICATION_HPP
#define QT_MIMIC_CORE_APPLICATION_HPP

#include "Object.hpp"

#include <functional>
#include <string>
#include <vector>

namespace QtMimic
{

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

        ~CoreApplication();

        CoreApplication
            (
            const CoreApplication&
            ) = delete;

        CoreApplication& operator=
            (
            const CoreApplication&
            ) = delete;

        static CoreApplication* instance();

        int exec();

        static void exit
            (
            int aCode = 0
            );

        static void quit();

        static void post
            (
            std::function<void()> aTask
            );

        void setEventDispatcher
            (
            std::function<void( int aTimeoutMs )> aDispatcher
            );

        //! @return parsed command-line arguments.
        const std::vector<std::string>& arguments() const
        {
            return mArgs;
        }

        // CoreApplication must remain bound to the main thread.
        void moveToThread
            (
            Thread* aThread
            ) = delete;

    private:
        static CoreApplication* sInstance; //!< Single instance (Qt-style)
        std::vector<std::string> mArgs; //!< Command-line arguments
    };

} // namespace QtMimic

#endif // QT_MIMIC_CORE_APPLICATION_HPP
