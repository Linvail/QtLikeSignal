// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

#ifndef QT_LIKE_SIGNAL_COREAPPLICATION_HPP
#define QT_LIKE_SIGNAL_COREAPPLICATION_HPP

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace QtLikeSignal
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
        //!
        //! Qt 6 has the identical plain pointer and works around it: QCoreApplication keeps a
        //! second, atomic g_self for its own use, and documents instanceExists() as "a Qt 6
        //! thread-safe (no data races) version of instance() != nullptr". Qt 7 makes the pointer
        //! itself atomic, behind a #warning to audit the call sites. This is that change.
        static std::atomic<CoreApplication*> sInstance;

        //! The adopted main thread. Non-owning: the thread_local inside Thread owns it, and
        //! releases it when the native thread exits.
        Thread* mMainThread { nullptr };
        // shared_ptr rather than unique_ptr: ThreadData hands out strong references, so a dispatcher
        // cannot be destroyed while another thread is part-way through a call into it.
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;  //!< The main thread's event dispatcher.
        std::vector<std::string>                 mArgs;        //!< Command-line arguments, if supplied.
        std::atomic<bool>                        mInExec { false };  //!< True while exec() is running, to reject nesting.
    };
}

#endif // QT_LIKE_SIGNAL_COREAPPLICATION_HPP
