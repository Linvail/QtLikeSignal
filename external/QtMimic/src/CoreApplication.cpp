//! @file
//!
//! Implementation of QtMimic::CoreApplication.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "CoreApplication.hpp"

namespace QtMimic
{

    CoreApplication* CoreApplication::sInstance = nullptr;

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
        return mainThread ? mainThread->exec() : 0;
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
