#include "CoreApplication.h"

#include "AbstractEventDispatcher.h"
#include "EventDispatcherDefault.h"
#if defined( _WIN32 )
    #include "EventDispatcherWin32.h"
#elif defined( __linux__ )
    #include "EventDispatcherLinux.h"
#endif
#include "Event.h"

#include <cstdio>

namespace QtLikeSignal
{
    CoreApplication* CoreApplication::sInstance = nullptr;

    //! Constructs the application and adopts the calling thread as the main thread.
    CoreApplication::CoreApplication()
        : Object()
    {
        adoptMainThread();
    }

    //! Constructs the application, capturing the command line, and adopts the calling thread.
    CoreApplication::CoreApplication
        (
        int aArgc,     //!< Argument count, as handed to main().
        char** aArgv   //!< Argument vector, as handed to main().
        )
        : Object()
    {
        mArgs.reserve( aArgc > 0 ? static_cast<size_t>( aArgc ) : 0 );
        for( int i = 0; i < aArgc; ++i )
        {
            mArgs.emplace_back( ( aArgv && aArgv[i] ) ? aArgv[i] : "" );
        }

        adoptMainThread();
    }

    //! Turns the calling thread into the main Thread and binds this application to it.
    //!
    //! Shared by both constructors. Creates the platform dispatcher, wraps the calling thread in a
    //! Thread that owns it, and registers that Thread as the current one so Objects constructed
    //! from here on gain main-thread affinity.
    void CoreApplication::adoptMainThread()
    {
        // Qt asserts here ("there should be only one application object"). Warn rather than abort:
        // a diagnostic is more useful than killing the process, and the first instance stays the
        // one instance() reports so the damage is contained and visible.
        if( sInstance )
        {
            std::fprintf( stderr,
                "CoreApplication: there should be only one application object; the existing one "
                "is kept and this one will not be reachable through instance()\n" );
        }
        else
        {
            sInstance = this;
        }

        #if defined( _WIN32 )
            mDispatcher = std::make_shared<EventDispatcherWin32>();
        #elif defined( __linux__ )
            mDispatcher = std::make_shared<EventDispatcherLinux>();
        #else
            mDispatcher = std::make_shared<EventDispatcherDefault>();
        #endif

        mMainThread = std::make_unique<Thread>();
        mMainThread->mData->setDispatcher( mDispatcher );
        Thread::sCurrentThread = mMainThread.get();

        // Self-adopt, mirroring exactly what Thread::threadBody() does for a worker thread
        // (sCurrentThread = this; this->moveToThread(this);). Without this, mMainThread's own
        // Object-inherited thread affinity (its Affinity box, pointed via moveToThread) never gets
        // pointed at its own dispatcher-holding ThreadData (mData) -- those are two separate fields
        // that only coincide once an object has been moved onto itself. Anything that targets the
        // Thread object directly (dispatchMetaCall(mainThreadPtr, ...), e.g. via Thread::post())
        // would silently fail to find a dispatcher without this, even though one exists.
        mMainThread->moveToThread( mMainThread.get() );

        // Object::moveToThread rather than moveToThread: the derived name is deleted, precisely so
        // that nobody re-homes the application later. Doing it here is the one legitimate use.
        Object::moveToThread( mMainThread.get() );
    }

    //! Destroys the application, releasing the main thread it adopted.
    CoreApplication::~CoreApplication()
    {
        // Drain deferred deletes before letting go of the dispatcher, mirroring what
        // Thread::threadBody() does when a worker finishes and what Qt's QThreadPrivate::finish()
        // does via sendPostedEvents(nullptr, DeferredDelete). Without it, every object that called
        // deleteLater() before the application shut down is leaked: the dispatcher's destructor can
        // free the queued events but has no way to free the objects they target. This was already
        // handled for worker threads; the main thread had no equivalent.
        if( mDispatcher )
        {
            mDispatcher->processDeferredDeletes();
        }

        Object::moveToThread( nullptr );
        mMainThread->mData->setDispatcher( nullptr );
        Thread::sCurrentThread = nullptr;
        if( sInstance == this )
        {
            sInstance = nullptr;
        }
    }

    //! Returns the global application instance, or nullptr if none has been constructed.
    //! Thread-safe.
    CoreApplication* CoreApplication::instance()
    {
        return sInstance;
    }

    //! Runs the main thread's event loop until exit()/quit() is called; returns the exit code.
    //!
    //! **Must be called from the thread the application was constructed on**, and must not be
    //! nested. Both are rejected with a warning and a -1 return, matching Qt, which refuses the
    //! same two ("Must be called from the main thread" / "The event loop is already running").
    int CoreApplication::exec()
    {
        if( Thread::currentThread() != mMainThread.get() )
        {
            std::fprintf( stderr, "CoreApplication::exec: must be called from the main thread\n" );
            return -1;
        }

        if( mInExec.exchange( true ) )
        {
            std::fprintf( stderr, "CoreApplication::exec: the event loop is already running\n" );
            return -1;
        }

        // Clear any exit request left over from a previous run, so exec() can be entered again
        // after a quit(). Qt does the same at this point (`threadData->quitNow = false`). An
        // exit()/quit() issued *before* exec() starts is therefore discarded rather than honoured,
        // which is also what Qt does.
        mMainThread->mExiting.store( false );

        const int returnCode = mMainThread->exec();

        mInExec.store( false );
        return returnCode;
    }

    //! Stops the main event loop, making exec() return @p aReturnCode. Thread-safe.
    //!
    //! Static, like Qt's QCoreApplication::exit(), so any thread can ask the application to stop
    //! without holding a pointer to it. Does nothing if no application exists.
    void CoreApplication::exit
        (
        int aReturnCode  //!< Value exec() should return.
        )
    {
        if( sInstance && sInstance->mMainThread )
        {
            sInstance->mMainThread->exit( aReturnCode );
        }
    }

    //! Convenience for exit(0): stops the main event loop, returning 0 from exec(). Thread-safe.
    void CoreApplication::quit()
    {
        exit( 0 );
    }
}
