#ifndef COREAPPLICATION_H
#define COREAPPLICATION_H

#include "Object.h"
#include "Thread.h"
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

        static CoreApplication* sInstance;  //!< The process-wide application instance.

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

#endif // COREAPPLICATION_H
