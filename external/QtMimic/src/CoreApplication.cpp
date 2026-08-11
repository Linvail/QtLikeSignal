//! @file
//!
//! Implementation of QtMimic::CoreApplication.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "CoreApplication.hpp"

#include <cstdio>

namespace QtMimic
{

    CoreApplication* CoreApplication::sInstance = nullptr;

    //! @brief Constructor - adopt the calling (main) thread, with no command-line arguments.
    CoreApplication::CoreApplication()
        : Object( nullptr )
    {
        sInstance = this;
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

        sInstance = this;
    }

    //! @brief Destructor - clears the singleton instance.
    CoreApplication::~CoreApplication()
    {
        // Drain deferred deletes on the way out, mirroring what threadBody() does when a worker
        // finishes and what Qt's QCoreApplication destructor does via
        // sendPostedEvents( nullptr, DeferredDelete ). Without it every object that called
        // deleteLater() before the application shut down is leaked: the mailbox is discarded with
        // its contents un-run, so the closures that would have deleted them never fire.
        //
        // Only the deletes, not the whole mailbox: ordinary queued work must not be made to run at
        // teardown, which would execute user code at a moment the program never asked for.
        if( Thread* mainThread = thread() )
        {
            mainThread->processDeferredDeletes();
        }

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
        Thread* mainThread = thread();
        if( !mainThread )
        {
            return 0;
        }

        // The loop belongs to the thread that constructed the application. Running it anywhere else
        // would drain the main thread's queue on a foreign thread -- see Thread::processEvents(),
        // which refuses the same thing for the same reason.
        //
        // Asked via isCurrent(), which compares OS thread ids, rather than by comparing against
        // currentThread(). Two reasons. currentThread() auto-adopts, so using it here would create
        // a Thread as a side effect of answering a question. And it reads the per-thread adoption
        // pointer, which loop() clears when it exits -- so a perfectly legitimate second exec()
        // after a quit() would find itself compared against a freshly-adopted dummy and be
        // rejected. isCurrent() is immune to both.
        if( !mainThread->isCurrent() )
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

        const int returnCode = mainThread->exec();

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
        if( sInstance )
        {
            if( Thread* mainThread = sInstance->thread() )
            {
                mainThread->exit( aCode );
            }
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
        if( sInstance )
        {
            if( Thread* mainThread = sInstance->thread() )
            {
                mainThread->post( std::move( aTask ) );
            }
        }
    }

    //! @brief Install a dispatcher pumped each loop iteration to service external/OS events (file
    //! descriptors, timers, ...) alongside posted tasks. Its argument is the maximum time (ms) it
    //! may block; 0 means poll.
    void CoreApplication::setEventDispatcher
        (
        std::function<void( int aTimeoutMs )> aDispatcher
        )
    {
        if( Thread* mainThread = thread() )
        {
            mainThread->setDispatcher( std::move( aDispatcher ) );
        }
    }

} // namespace QtMimic
