// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The Windows half of SignalWatcher: the console control handler.

#include "QtLikeSignal/SignalWatcher.hpp"

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    // windows.h defines min/max macros that collide with std::min and std::max.
    #define NOMINMAX
#endif
#include <windows.h>

namespace QtLikeSignal
{
    namespace
    {
        //! How long the console handler waits for the event loop to say it is finished.
        //!
        //! Windows gives a process about five seconds after the handler returns from
        //! CTRL_CLOSE_EVENT before it stops it, so this has to be under that with room for the
        //! shutdown work itself. A loop that is stuck costs this delay and no more -- the
        //! alternative, a wait with no limit, turns a stuck shutdown into a window that will not
        //! close.
        const std::chrono::milliseconds kAckTimeout( 4000 );

        //! Guards every variable below, and the console handler's use of the watcher it finds.
        //!
        //! One mutex for all of it rather than one atomic for each. The atomics were not enough:
        //! the handler runs on a thread the operating system creates, and between reading the
        //! watcher pointer and calling a method on it, the loop's own thread can run
        //! ~SignalWatcher and free the object. Only a lock the destructor also takes can close
        //! that window, and once there is a lock the separate atomics buy nothing.
        //!
        //! **The lock order is handler mutex, then the dispatcher's.** consoleHandler() holds this
        //! while it queues the call, which takes the dispatcher's mutex inside. Nothing takes them
        //! the other way round: the dispatcher releases its own mutex before it runs a slot, so the
        //! slot that acknowledges below asks for this one holding nothing.
        std::mutex gHandlerMutex;

        //! Signalled when the event loop has finished with the console event.
        //!
        //! A condition variable and not a Win32 event, which is what this used to be. A HANDLE has
        //! to be closed by somebody, and the somebody was the destructor -- which could close it
        //! between a handler reading it and waiting on it, leaving the handler waiting on a closed
        //! handle, or on whatever the operating system next gave that value to.
        std::condition_variable gAckCondition;

        //! True once the loop has dealt with the console event, or once the watcher has gone.
        //!
        //! The predicate gAckCondition is waited on with, so a spurious wake does not end the wait
        //! early and a notify that arrives first is not missed. Guarded by gHandlerMutex.
        bool gAcknowledged = false;

        //! The living watcher, or null when there is none.
        //!
        //! The console handler is a plain function the operating system calls, so it has no other
        //! way to reach the object. One pointer is enough only because at most one watcher exists
        //! in a process. Guarded by gHandlerMutex, and cleared before the object it names is
        //! destroyed.
        SignalWatcher* gWatcher = nullptr;

        //! True from the moment the first console event is accepted. Guarded by gHandlerMutex.
        //!
        //! It is what makes the second Ctrl+C stop the process: Windows calls the handler on a new
        //! thread for each event, so a second one finds this already set, returns FALSE, and lets
        //! the default handler end the process.
        bool gDelivering = false;

        //! True while a watcher exists, so that a second one refuses instead of fighting the first.
        //! Guarded by gHandlerMutex.
        bool gWatcherExists = false;

        //! Maps a console control event to the signal number that means the same thing.
        //!
        //! Windows delivers these events instead of signals -- SIGTERM is essentially never
        //! delivered there, because nothing sends it -- so the numbers are chosen to let one
        //! cross-platform slot read them. Returns 0 for an event this class does not watch.
        int signalNumberOf
            (
            unsigned long aType  //!< The CTRL_*_EVENT value the operating system passed.
            )
        {
            switch( aType )
            {
            case CTRL_C_EVENT:
                return SIGINT;

            case CTRL_BREAK_EVENT:
                return SIGBREAK;

            // The three that give the process only a few seconds to live. They are the closest
            // Windows has to SIGTERM: an outside agent has decided this program stops now.
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                return SIGTERM;

            default:
                return 0;
            }
        }

        //! Releases a console handler that is waiting, from any thread.
        void acknowledge()
        {
            {
                std::lock_guard<std::mutex> lock( gHandlerMutex );
                gAcknowledged = true;
            }
            gAckCondition.notify_all();
        }
    }

    //! Constructs the watcher and starts to watch, unless something in the way stops it.
    //!
    //! Every refusal path leaves isWatching() false and reports a critical record that names the
    //! reason.
    //!
    //! @p aMode chooses nothing here. Windows has one mechanism, and mode() answers what was asked
    //! for so that a program which selects SignalWatchMode::SignalFd for Linux still builds and
    //! still reports its own choice.
    SignalWatcher::SignalWatcher
        (
        SignalWatchMode aMode  //!< Remembered, and otherwise not used on this platform.
        )
        : mMode( aMode )
    {
        if( thread() == nullptr )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: this object has no thread affinity, so a console event has"
                << "nowhere to be delivered; construct the watcher on the thread that calls exec()";
            return;
        }

        {
            std::lock_guard<std::mutex> lock( gHandlerMutex );

            if( gWatcherExists )
            {
                qCCritical( gLogSignalWatcher )
                    << "SignalWatcher: a watcher already exists in this process, so this one"
                    << "watches nothing; the console control handler is process-global and two"
                    << "watchers would each undo the other";
                return;
            }

            gWatcherExists = true;
            gDelivering    = false;
            gAcknowledged  = false;

            // Published before the handler is installed, which is safe in that order and not in the
            // other: no handler can run yet, and one that could would find a null pointer.
            gWatcher = this;
        }

        if( ::SetConsoleCtrlHandler( &SignalWatcher::consoleHandler, TRUE ) == 0 )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: SetConsoleCtrlHandler() failed, so nothing is watched; error"
                << ::GetLastError();

            std::lock_guard<std::mutex> lock( gHandlerMutex );
            gWatcher       = nullptr;
            gWatcherExists = false;
            return;
        }

        mWatching = true;

        qCInfo( gLogSignalWatcher )
            << "SignalWatcher: watching Ctrl+C, Ctrl+Break, console close, logoff and shutdown";
    }

    //! Destroys the watcher, and puts back what it replaced.
    SignalWatcher::~SignalWatcher()
    {
        stopWatching();
    }

    //! Gets the subscribe-only view of the signal this watcher emits.
    SignalView<int>& SignalWatcher::getTriggered() const
    {
        return mTriggered.view();
    }

    //! Removes the console control handler and forgets the watcher.
    //!
    //! Safe to call when nothing was ever installed, and safe to call two times.
    void SignalWatcher::stopWatching()
    {
        if( !mWatching )
        {
            return;
        }

        mWatching = false;

        // Removed first, so that no new handler thread can start while the rest of this runs.
        ::SetConsoleCtrlHandler( &SignalWatcher::consoleHandler, FALSE );

        {
            // The lock is what makes this safe against a handler already in flight. A handler
            // holds it from the moment it reads gWatcher until it begins to wait, so taking it
            // here means no handler is part way through using the object this destructor is about
            // to finish destroying. One woken by the acknowledgement below has nothing left to do
            // but return.
            std::lock_guard<std::mutex> lock( gHandlerMutex );
            gWatcher       = nullptr;
            gWatcherExists = false;
            gAcknowledged  = true;
        }
        gAckCondition.notify_all();

        // Anything this object had queued on its own loop goes with it: the queued call below names
        // the watcher as its receiver, so ~Object() strips it. Nothing here has to chase it.
    }

    //! Called by the operating system on a thread it creates, for each console control event.
    //!
    //! **This half waits and the Linux half must not**, which is the whole difference between the
    //! two. For CTRL_CLOSE_EVENT the process is stopped a few seconds after this function returns,
    //! so to return at once would end the program in the middle of its own shutdown. The work is
    //! queued onto the event loop and this thread waits for the loop to report it done.
    //!
    //! Returns TRUE when the event was handled and FALSE to let the next handler, and in the end
    //! the default one, deal with it. FALSE on the second event is what stops the process.
    int __stdcall SignalWatcher::consoleHandler
        (
        unsigned long aType  //!< CTRL_C_EVENT and the four others; a Win32 DWORD.
        )
    {
        const int signalNumber = signalNumberOf( aType );
        if( signalNumber == 0 )
        {
            return FALSE;
        }

        // Held for the whole of this function, and released only inside the wait. It is what stops
        // the watcher from being destroyed between the read below and the queued call.
        std::unique_lock<std::mutex> lock( gHandlerMutex );

        SignalWatcher* const watcher = gWatcher;
        if( watcher == nullptr )
        {
            return FALSE;
        }

        if( gDelivering )
        {
            // The second event. A graceful shutdown that hangs must not make the program
            // unkillable, so this one goes to the default handler and ends the process.
            return FALSE;
        }

        // Nothing to wait for if the loop cannot take work: to answer TRUE would swallow the event
        // and leave the program running with nobody told.
        Thread* const target = watcher->thread();
        if( target == nullptr || !target->eventDispatcher() )
        {
            return FALSE;
        }

        gDelivering   = true;
        gAcknowledged = false;

        // callLater() and not Thread::post(). post() names the *Thread* as the receiver, so a
        // queued task outlives the watcher it captured and calls a method on freed memory once the
        // loop reaches it. Named this way the receiver is the watcher itself, which gives two
        // guarantees the library already implements: ~Object() strips the queued call, and the
        // invoker re-checks that the object is alive before it invokes.
        Object::callLater( watcher, &SignalWatcher::deliverFromConsole, signalNumber );

        gAckCondition.wait_for( lock, kAckTimeout, []()
            {
                return gAcknowledged;
            } );

        return TRUE;
    }

    //! Reports the console event and then releases the handler thread. Runs on the loop's thread.
    void SignalWatcher::deliverFromConsole
        (
        int aSignal  //!< The signal number the console event maps to.
        )
    {
        deliver( aSignal );
        acknowledge();
    }

    //! Reports the signal to the application, and then flushes the log.
    void SignalWatcher::deliver
        (
        int aSignal  //!< The signal number to report.
        )
    {
        if( mDelivered )
        {
            return;
        }

        mDelivered = true;

        qCInfo( gLogSignalWatcher )
            << "SignalWatcher: console event" << aSignal
            << "received; the next one will stop the process";

        mTriggered.emit( aSignal );

        // After the slots, not before. The records that explain a shutdown are the ones the slots
        // write on their way out, and those are exactly the ones a sink with a buffer still holds.
        Log::flush();
    }
}
