// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SignalWatcher -- an opt-in watcher that turns SIGTERM and SIGINT into an ordinary
//! signal delivered on the event loop's own thread.

#ifndef QT_LIKE_SIGNAL_SIGNALWATCHER_HPP
#define QT_LIKE_SIGNAL_SIGNALWATCHER_HPP

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/SignalWatchMode.hpp"

namespace QtLikeSignal
{
    //----------------------------------------------------------------
    //! @class SignalWatcher
    //!
    //! Turns a shutdown request from the operating system into a signal on the event loop.
    //!
    //! `systemctl stop` sends SIGTERM and then SIGKILL after TimeoutStopSec. A program that gets
    //! SIGKILLed has released nothing: no display master, no flushed log, no saved state. Ctrl+C
    //! during development is the same mechanism with a shorter fuse. This class is how a program
    //! gets the few hundred milliseconds between the two, on a thread where ordinary code is legal.
    //!
    //! @code
    //!   int main()
    //!   {
    //!       MyApplication app;
    //!       SignalWatcher watcher;                 // construct on the thread that calls exec()
    //!
    //!       Object::connect( watcher.getTriggered(), &app,
    //!           []( int aSignal )
    //!           {
    //!               // On the main thread, with no restriction on what may be called.
    //!               CoreApplication::quit();
    //!           } );
    //!
    //!       return app.exec();
    //!   }
    //! @endcode
    //!
    //! **Opt-in, and never installed by CoreApplication.** sigaction(2) is process-global, so a
    //! library that installed a handler would silently replace whatever the application, or another
    //! library in the same process, had installed. Qt reaches the same conclusion and installs
    //! nothing: the only signals qtbase touches are SIGPIPE, which it ignores, and SIGTERM in
    //! QProcess, which it *sends* to a child. In a vehicle the application is usually under a
    //! lifecycle manager with its own view of SIGTERM, so the decision is the application's.
    //!
    //! **At most one instance per process.** The handlers and the saved dispositions are
    //! process-global state, and two watchers would each restore what the other installed. A second
    //! one reports a critical record and watches nothing; isWatching() says so.
    //!
    //! **The second signal kills.** On the first delivery the watched signals go back to their
    //! default disposition, so a second Ctrl+C terminates the process the ordinary way. A graceful
    //! shutdown that hangs must not make the program unkillable -- that is how a control unit ends
    //! up power-cycled. It also means getTriggered() is emitted at most once per watcher.
    //!
    //! **What it watches:** SIGINT and SIGTERM on Linux; Ctrl+C, Ctrl+Break, console close, logoff
    //! and system shutdown on Windows. The signal number is the argument of getTriggered(), and on
    //! Windows the console events are reported as the number they correspond to -- SIGINT for
    //! Ctrl+C, SIGBREAK for Ctrl+Break, SIGTERM for the other three.
    //!
    //! **Windows is a different mechanism, not a port.** SIGTERM is essentially never delivered
    //! there because nothing sends it; the events arrive through SetConsoleCtrlHandler, on a thread
    //! the operating system creates for the purpose. For a console-close event the process dies
    //! about five seconds after the handler returns, so the Windows half posts to the loop and then
    //! *waits* for the loop to acknowledge -- the opposite of the Linux half, which must return at
    //! once. The wait has its own timeout, so a stopped loop costs a bounded delay rather than a
    //! hang.
    //!
    //! **The log is flushed after the slots run**, not before, because the records that explain the
    //! shutdown are the ones the slots write.
    //!
    //! **Construct it on the thread that runs the event loop.** On Linux the notification arrives
    //! as a file descriptor registered with that thread's EventDispatcherLinux, so a watcher built
    //! on a thread with a different dispatcher has nowhere to deliver and reports a critical
    //! record. Destroy it on the same thread.
    //----------------------------------------------------------------
    class SignalWatcher : public Object
    {
    public:
        //! Constructs the watcher and starts watching. Construct on the loop's own thread.
        explicit SignalWatcher
            (
            SignalWatchMode aMode = SignalWatchMode::Handler  //!< How the signal is to be received.
            );

        virtual ~SignalWatcher() override;

        //! A watcher owns process-global state and cannot be copied.
        SignalWatcher
            (
            const SignalWatcher&
            ) = delete;

        SignalWatcher& operator=
            (
            const SignalWatcher&
            ) = delete;

        SignalView<int>& getTriggered() const;

        //! @return true if the watcher installed itself and a signal will be reported.
        //!
        //! False means every reason the constructor could refuse: another watcher already exists,
        //! the thread runs no dispatcher that can carry the notification, a system call failed, or
        //! SignalWatchMode::SignalFd was asked for without the signals being blocked first. Each
        //! case reports a critical record saying which. **Check it.** A watcher that is not
        //! watching is silent, and the failure is one that only shows up when the program is asked
        //! to stop.
        bool isWatching() const
        {
            return mWatching;
        }

        //! @return the mode this watcher was constructed with, whether or not it is watching.
        SignalWatchMode mode() const
        {
            return mMode;
        }

    private:
        //! Emits mTriggered and then flushes the log. Runs on the watcher's own thread.
        void deliver
            (
            int aSignal  //!< The signal number to report.
            );

        void stopWatching();

        #if defined( _WIN32 )
            //! The console control handler the operating system calls, on a thread it creates.
            //!
            //! A private static member rather than a file-scope function, so that it may reach
            //! deliver() without any of this being public. Typed with `int` and `unsigned long` in
            //! place of BOOL and DWORD, and __stdcall spelled out in place of WINAPI, so that this
            //! header does not pull <windows.h> -- and its macros -- into every translation unit
            //! that includes it. Thread.hpp keeps its Windows thread handle out of the header for
            //! the same reason.
            static int __stdcall consoleHandler
                (
                unsigned long aType  //!< CTRL_C_EVENT and the four others; a Win32 DWORD.
                );

            void deliverFromConsole
                (
                int aSignal  //!< The signal number the console event maps to.
                );

        #else
            bool startHandlerMode();

            bool startSignalFdMode();

            void onNotifyFdReady();

        #endif

        //! Emitted, on the watcher's own thread, when a watched signal arrives. Emitted once.
        //!
        //! Private and handed out only as a view, for the reason Timer's mTimeout is: reporting a
        //! shutdown that the operating system did not ask for is worse than useless, so only the
        //! watcher may emit it.
        Signal<int> mTriggered;

        //! The mode asked for at construction. Never changes.
        SignalWatchMode mMode;

        //! True between a successful install and stopWatching(). Written on the owning thread only.
        bool mWatching { false };

        //! True once a watched signal has been reported. Written on the owning thread only.
        //!
        //! It is what makes getTriggered() fire at most once, and it also stops the destructor from
        //! putting the previous disposition back. Putting it back would undo the one guarantee that
        //! matters most here: after the first signal the watched signals are left at their default
        //! disposition, so the next one terminates the process even if the shutdown is stuck.
        bool mDelivered { false };

        #if !defined( _WIN32 )
            //! The descriptor the event loop polls: the read end of the self-pipe in
            //! SignalWatchMode::Handler, or the signalfd(2) descriptor in
            //! SignalWatchMode::SignalFd. -1 when nothing is registered.
            int mNotifyFd { -1 };

            //! The write end of the self-pipe, which is all the handler is allowed to touch. -1 in
            //! SignalWatchMode::SignalFd, which has no pipe and no handler.
            int mWriteFd { -1 };

        #endif
    };
}

#endif // QT_LIKE_SIGNAL_SIGNALWATCHER_HPP
