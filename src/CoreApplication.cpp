#include "CoreApplication.h"

#include "AbstractEventDispatcher.h"
#include "EventDispatcherDefault.h"
#if defined( _WIN32 )
    #include "EventDispatcherWin32.h"
#elif defined( __linux__ )
    #include "EventDispatcherLinux.h"
#endif
#include "Event.h"

namespace QtLikeSignal
{
    CoreApplication* CoreApplication::sInstance = nullptr;

    //! Constructs the application object.
    CoreApplication::CoreApplication
        (
        int& aArgc,     //!< Argument count.
        char** aArgv    //!< Argument vector.
        )
        : Object()
    {
        ( void )aArgc;
        ( void )aArgv;
        sInstance = this;

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

        // Self-adopt, mirroring exactly what Thread::start()'s lambda does for a worker thread
        // (sCurrentThread = this; this->moveToThread(this);). Without this, mMainThread's own
        // Object-inherited thread affinity (its Affinity box, pointed via moveToThread) never gets
        // pointed at its own dispatcher-holding ThreadData (mData) -- those are two separate fields
        // that only coincide once an object has been moved onto itself. Anything that targets the
        // Thread object directly (dispatchMetaCall(mainThreadPtr, ...), e.g. via Thread::post())
        // would silently fail to find a dispatcher without this, even though one exists.
        mMainThread->moveToThread( mMainThread.get() );

        this->moveToThread( mMainThread.get() );
    }

    //! Destroys the application object.
    CoreApplication::~CoreApplication()
    {
        this->moveToThread( nullptr );
        mMainThread->mData->setDispatcher( nullptr );
        Thread::sCurrentThread = nullptr;
        sInstance = nullptr;
    }

    //! Returns the global application instance. Thread-safe.
    CoreApplication* CoreApplication::instance()
    {
        return sInstance;
    }

    //! Enters the main event loop and waits until quit() is called. Returns the exit code.
    int CoreApplication::exec()
    {
        mExiting.store( false );
        if( mDispatcher )
        {
            while( !mExiting.load() )
            {
                mDispatcher->processEvents();
            }
        }
        return 0;
    }

    //! Tells the application to exit with return code 0. Thread-safe.
    void CoreApplication::quit()
    {
        mExiting.store( true );
        if( mDispatcher )
        {
            mDispatcher->interrupt();
            mDispatcher->wakeUp();
        }
    }
}
