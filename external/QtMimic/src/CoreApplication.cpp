//! @file
//!
//! Implementation of QtMimic::CoreApplication.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "CoreApplication.hpp"

#include "AbstractEventDispatcher.hpp"
#include "EventDispatcherDefault.hpp"
#if defined( _WIN32 )
    #include "EventDispatcherWin32.hpp"
#elif defined( __linux__ )
    #include "EventDispatcherLinux.hpp"
#endif
#include "Thread.hpp"

#include <cstdio>

namespace QtMimic
{

    CoreApplication* CoreApplication::sInstance = nullptr;

    //! @brief Constructor - adopt the calling (main) thread, with no command-line arguments.
    CoreApplication::CoreApplication()
        : Object( nullptr )
    {
        adoptMainThread();
    }

    //! @brief Constructor - adopt the calling (main) thread and capture command-line arguments.
    //! Creates a singleton CoreApplication instance. There can be at most one instance.
    CoreApplication::CoreApplication
        (
        int aArgc,
        char** aArgv
        )
        : Object( nullptr )
    {

        mArgs.reserve( aArgc > 0 ? aArgc : 0 );
        for( int i = 0; i < aArgc; ++i )
        {
            mArgs.emplace_back( aArgv[i] ? aArgv[i] : "" );
        }

        adoptMainThread();
    }

    //! @brief Turn the calling thread into the main Thread and bind this application to it.
    //!
    //! Shared by both constructors. The calling thread is already adopted by the time this runs --
    //! this object's own Object base asked for currentThread() a moment ago, which adopted it if
    //! nobody had -- so all that remains is to record it and give it the platform dispatcher its
    //! loop needs.
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

        mMainThread = Thread::currentThread();

        // Swap the adopted thread's plain dispatcher for the platform one. Auto-adoption installs
        // only EventDispatcherDefault, deliberately -- allocating an eventfd or a message-only
        // window for every native thread that merely touches an Object would be wasteful. A thread
        // that actually runs a loop needs the platform one, and this is that thread.
        //
        // Nothing can be lost in the swap: the adoption happened during this constructor, so no
        // event can have been queued between then and now.
        #if defined( _WIN32 )
            mDispatcher = std::make_shared<EventDispatcherWin32>();
        #elif defined( __linux__ )
            mDispatcher = std::make_shared<EventDispatcherLinux>();
        #else
            mDispatcher = std::make_shared<EventDispatcherDefault>();
        #endif
        mMainThread->mData->setDispatcher( mDispatcher );
    }

    //! @brief Destructor - drains pending deferred deletes and clears the singleton instance.
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
            mDispatcher->close();
            mDispatcher->processDeferredDeletes();
        }

        // Hand the thread back the plain dispatcher auto-adoption would have given it, rather than
        // leaving it with the platform one (whose eventfd or message window should not outlive the
        // application) or with none at all (which would silently break every Object still living on
        // this thread). The thread itself stays adopted: it is owned by a thread_local in Thread and
        // released when the native thread exits, not by us.
        if( mMainThread )
        {
            mMainThread->mData->setDispatcher( std::make_shared<EventDispatcherDefault>() );
        }
        mDispatcher.reset();
        mMainThread = nullptr;

        if( sInstance == this )
        {
            sInstance = nullptr;
        }
    }

    //! @brief Get the single CoreApplication instance (or nullptr if none exists).
    CoreApplication* CoreApplication::instance()
    {
        return sInstance;
    }

    //! @brief Run the main event loop until quit() or exit() is called.
    //! @return The exit code passed to exit() (0 if quit() was used).
    int CoreApplication::exec()
    {
        if( !mMainThread )
        {
            return 0;
        }

        // The loop belongs to the thread that constructed the application. Running it anywhere else
        // would drain the main thread's queue on a foreign thread -- see Thread::processEvents(),
        // which refuses the same thing for the same reason.
        //
        // Compared against currentThread() rather than the old isCurrent(). That accessor existed
        // because loop() used to clear the per-thread registration on its way out, so a legitimate
        // second exec() after a quit() would have been compared against a freshly-adopted dummy and
        // rejected. threadBody() no longer clears it for an adopted thread, so the comparison is
        // sound and isCurrent() is gone.
        if( Thread::currentThread() != mMainThread )
        {
            std::fprintf( stderr,
                "CoreApplication::exec: must be called from the main thread\n" );
            return -1;
        }

        // Re-entering exec() from inside the running loop -- typically from a slot -- would nest a
        // second loop inside the first. The inner one then owns the quit: quit() ends it and
        // returns control to the outer loop, which keeps running, so the program does not stop when
        // it was told to. Refused rather than honoured, as Qt refuses it.
        if( mInExec.exchange( true ) )
        {
            std::fprintf( stderr,
                "CoreApplication::exec: the event loop is already running\n" );
            return -1;
        }

        // Clear any exit request left over from a previous run, so exec() can be entered again
        // after a quit(). The main thread is adopted and so never went through Thread::start(),
        // which is where a worker's flag gets cleared. An exit()/quit() issued *before* exec()
        // starts is therefore discarded rather than honoured, which is what Qt does too.
        mMainThread->mExiting.store( false );

        const int returnCode = mMainThread->exec();

        mInExec.store( false );
        return returnCode;
    }

    //! @brief Stop the main event loop with the given exit code. Thread-safe.
    //! @param aCode The exit code to return from exec().
    void CoreApplication::exit
        (
        int aCode
        )
    {
        if( sInstance && sInstance->mMainThread )
        {
            sInstance->mMainThread->exit( aCode );
        }
    }

    //! @brief Convenience for exit(0): stop the main event loop, returning 0 from exec(). Thread-safe.
    void CoreApplication::quit()
    {
        exit( 0 );
    }

    //! @brief Queue a task onto the main thread's event loop. Thread-safe. Static convenience
    //! method; operates on the singleton CoreApplication instance (if one exists).
    void CoreApplication::post
        (
        std::function<void()> aTask
        )
    {
        if( sInstance && sInstance->mMainThread )
        {
            sInstance->mMainThread->post( std::move( aTask ) );
        }
    }

} // namespace QtMimic
