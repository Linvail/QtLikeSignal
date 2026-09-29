// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::CoreApplication.

#include "QtLikeSignal/CoreApplication.hpp"

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/EventDispatcherDefault.hpp"
#if defined( _WIN32 )
    #include "QtLikeSignal/EventDispatcherWin32.hpp"
#elif defined( __linux__ )
    #include "QtLikeSignal/EventDispatcherLinux.hpp"
#endif
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <mutex>
#include <string>

namespace QtLikeSignal
{
    namespace
    {
        //! The organization and application names, which Settings uses to name its file.
        struct ApplicationNames
        {
            //! Guards the two names. They are read and set from any thread.
            std::mutex mMutex;

            //! The name set by setOrganizationName(). Empty until then.
            std::string mOrganization;

            //! The name set by setApplicationName(). Empty until then.
            std::string mApplication;
        };

        //! Returns the one set of names.
        //!
        //! Made on first use and never destroyed, because a Settings object with static storage
        //! duration can read the names after every other static is destroyed.
        ApplicationNames& applicationNames()
        {
            static ApplicationNames* const names = new ApplicationNames();
            return *names;
        }
    }

    std::atomic<CoreApplication*> CoreApplication::sInstance { nullptr };

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
    //! Shared by both constructors. The calling thread is already adopted by the time this runs, so
    //! all that remains is to record it and give it the platform dispatcher its loop needs.
    void CoreApplication::adoptMainThread()
    {
        // Qt asserts here ("there should be only one application object"). Warn rather than abort:
        // a diagnostic is more useful than killing the process, and the first instance stays the
        // one instance() reports so the damage is contained and visible.
        // One compare-exchange rather than a test and a store, so "the first one wins" holds even
        // under the misuse this branch exists to report.
        CoreApplication* noInstanceYet = nullptr;
        if( !sInstance.compare_exchange_strong( noInstanceYet, this ) )
        {
            qCWarning( gLogApp )
                << "CoreApplication: there should be only one application object; the existing"
                << "one is kept and this one will not be reachable through instance()";
        }

        // The calling thread is already adopted -- this object's own Object base asked for
        // currentThread() a moment ago, which adopted it if nobody had. So there is no second
        // Thread to create and no moveToThread() dance: this application already lives in the
        // thread it is about to run the loop for, and mMainThread is a non-owning pointer to the
        // Thread that the thread_local in Thread owns.
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
            mDispatcher->close();
            mDispatcher->processDeferredDeletes();
        }

        // Hand the thread back the plain dispatcher auto-adoption would have given it, rather than
        // leaving it with the platform one (whose eventfd or message window should not outlive the
        // application) or with none at all (which would silently break every Object still living on
        // this thread). The thread itself stays adopted: it is owned by a thread_local in Thread
        // and released when the native thread exits, not by us.
        if( mMainThread )
        {
            mMainThread->mData->setDispatcher( std::make_shared<EventDispatcherDefault>() );
        }
        mDispatcher.reset();
        mMainThread = nullptr;

        // Clears the pointer only if it is still ours, which is what a second application object
        // being destroyed first must not do.
        CoreApplication* self = this;
        sInstance.compare_exchange_strong( self, nullptr );
    }

    //! Returns the global application instance, or nullptr if none has been constructed.
    //! Thread-safe.
    CoreApplication* CoreApplication::instance()
    {
        return sInstance.load();
    }

    //! Runs the main thread's event loop until exit()/quit() is called; returns the exit code.
    //!
    //! **Must be called from the thread the application was constructed on**, and must not be
    //! nested. Both are rejected with a warning and a -1 return, matching Qt, which refuses the
    //! same two ("Must be called from the main thread" / "The event loop is already running").
    int CoreApplication::exec()
    {
        // No main thread means the application never adopted one, so there is no loop to run.
        if( !mMainThread )
        {
            return 0;
        }

        // The loop belongs to the thread that constructed the application. Running it anywhere else
        // would drain the main thread's queue on a foreign thread -- see Thread::processEvents(),
        // which refuses the same thing for the same reason.
        if( Thread::currentThread() != mMainThread )
        {
            qCWarning( gLogApp )
                << "CoreApplication::exec: must be called from the main thread";
            return -1;
        }

        // Re-entering exec() from inside the running loop -- typically from a slot -- would nest a
        // second loop inside the first. The inner one then owns the quit: quit() ends it and
        // returns control to the outer loop, which keeps running, so the program does not stop when
        // it was told to. Refused rather than honoured, as Qt refuses it.
        if( mInExec.exchange( true ) )
        {
            qCWarning( gLogApp )
                << "CoreApplication::exec: the event loop is already running";
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

    //! Stops the main event loop, making exec() return @p aReturnCode. Thread-safe.
    //!
    //! Static, so any thread can ask the application to stop without holding a pointer to it. Does
    //! nothing if no application exists.
    //!
    //! **Thread-safety note:** may be called from any thread. Thread-safety is not guaranteed if
    //! the CoreApplication object is being destroyed at the same time -- destroy it only after the
    //! threads that may call this have stopped. Qt states the same caveat for
    //! QCoreApplication::quit() and it has the same cause: the instance pointer is loaded, and then
    //! dereferenced.
    void CoreApplication::exit
        (
        int aReturnCode  //!< Value exec() should return.
        )
    {
        CoreApplication* app = sInstance.load();
        if( app && app->mMainThread )
        {
            app->mMainThread->exit( aReturnCode );
        }
    }

    //! Convenience for exit(0): stops the main event loop, returning 0 from exec(). Thread-safe.
    //!
    //! **Thread-safety note:** may be called from any thread. Thread-safety is not guaranteed if
    //! the CoreApplication object is being destroyed at the same time; see exit(), which this
    //! forwards to and which carries the reasoning.
    void CoreApplication::quit()
    {
        exit( 0 );
    }

    //! Queues a task onto the main thread's event loop. Thread-safe.
    //!
    //! Static, like exit()/quit(), so any thread can hand work to the main loop without holding a
    //! pointer to the application. See the declaration for what the return value means.
    //!
    //! **Thread-safety note:** may be called from any thread. Thread-safety is not guaranteed if
    //! the CoreApplication object is being destroyed at the same time; see exit() for why.
    bool CoreApplication::post
        (
        std::function<void()> aTask  //!< The callable to run on the main thread.
        )
    {
        CoreApplication* app = sInstance.load();
        if( app && app->mMainThread )
        {
            return app->mMainThread->post( std::move( aTask ) );
        }
        return false;
    }

    //! Sets the name of the organization that wrote the application, for example "Garmin".
    //! Thread-safe.
    //!
    //! Settings uses it to name its file when no name is given to its constructor. Static, and
    //! usable before an application object exists, as in Qt: the name must be set before the first
    //! Settings object is made, which can be before the application object is.
    void CoreApplication::setOrganizationName
        (
        const std::string& aName  //!< The name. It becomes a directory name, so avoid '/' and ''.
        )
    {
        ApplicationNames& names = applicationNames();
        std::lock_guard<std::mutex> lock( names.mMutex );
        names.mOrganization = aName;
    }

    //! Returns the name that setOrganizationName() set, or an empty string. Thread-safe.
    std::string CoreApplication::organizationName()
    {
        ApplicationNames& names = applicationNames();
        std::lock_guard<std::mutex> lock( names.mMutex );
        return names.mOrganization;
    }

    //! Sets the name of the application, for example "Chartplotter". Thread-safe.
    //!
    //! Settings uses it to name its file when no name is given to its constructor. Unlike Qt, there
    //! is no default taken from the program's file name: the file name of a program can change
    //! between versions, and the settings file must not move with it.
    void CoreApplication::setApplicationName
        (
        const std::string& aName  //!< The name. It becomes a file name, so avoid '/' and ''.
        )
    {
        ApplicationNames& names = applicationNames();
        std::lock_guard<std::mutex> lock( names.mMutex );
        names.mApplication = aName;
    }

    //! Returns the name that setApplicationName() set, or an empty string. Thread-safe.
    std::string CoreApplication::applicationName()
    {
        ApplicationNames& names = applicationNames();
        std::lock_guard<std::mutex> lock( names.mMutex );
        return names.mApplication;
    }

} // namespace QtLikeSignal
