// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! How a SignalWatcher gets told that an operating-system signal arrived.

#ifndef QT_LIKE_SIGNAL_SIGNALWATCHMODE_HPP
#define QT_LIKE_SIGNAL_SIGNALWATCHMODE_HPP

namespace QtLikeSignal
{
    //! How a SignalWatcher gets told that an operating-system signal arrived.
    //!
    //! The two modes are equivalent to a caller: the same signals are watched, and the same
    //! notification comes out on the event loop's own thread. They differ in what the process must
    //! already have done before the watcher is constructed, which is why the caller chooses and
    //! this library does not choose for it.
    //!
    //! **This is a Linux distinction.** Windows has one mechanism -- the console control handler --
    //! and the mode selects nothing there. See SignalWatcher for what Windows does instead.
    enum class SignalWatchMode
    {
        //! Install a handler with sigaction(2), and let the handler write one byte to a pipe.
        //!
        //! The default, because it puts no constraint on the application. Any program can construct
        //! a watcher in this mode at any point, on any thread that runs an event loop.
        //!
        //! The cost is that a handler exists. A handler runs on whichever thread the kernel picks,
        //! and almost nothing is legal inside it, so all it does is write() one byte. The event
        //! loop reads that byte on its own thread and does the real work there.
        Handler,

        //! Read the signals from a descriptor made by signalfd(2), with no handler at all.
        //!
        //! Cleaner, and the reason it is offered: there is no handler, so there is no
        //! async-signal-safety problem to reason about. A signal becomes a readable descriptor and
        //! nothing more.
        //!
        //! **It has a precondition this library cannot meet on its own.** signalfd(2) reads signals
        //! that are *blocked*; an unblocked signal is delivered the ordinary way and never reaches
        //! the descriptor. A signal mask is inherited by each thread from the thread that created
        //! it, so the block must happen in main() before any other thread starts. That is a
        //! constraint on the application's own startup, and a library that imposed it silently
        //! would break every program that starts a thread before it constructs a watcher.
        //!
        //! So the watcher checks instead of assuming: if the signals are not blocked in the calling
        //! thread, construction reports a critical record and watches nothing. See
        //! SignalWatcher::isWatching().
        SignalFd
    };
}

#endif // QT_LIKE_SIGNAL_SIGNALWATCHMODE_HPP
