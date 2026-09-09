// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The Linux half of SignalWatcher: sigaction(2) with a self-pipe, or signalfd(2).

#include "QtLikeSignal/SignalWatcher.hpp"

#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <memory>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace QtLikeSignal
{
    namespace
    {
        //! The signals a watcher watches, and the whole of the list.
        //!
        //! SIGINT is Ctrl+C and SIGTERM is what a service manager sends before it resorts to
        //! SIGKILL, so between them they cover every ordinary "please stop" a program is asked.
        //! SIGHUP is deliberately absent: on a daemon it conventionally means "read your
        //! configuration again", which is not a shutdown, and treating it as one would stop
        //! programs that only asked to be configured again.
        const int kWatchedSignals[] = { SIGINT, SIGTERM };

        //! How many entries kWatchedSignals has.
        const std::size_t kWatchedSignalCount
            = sizeof( kWatchedSignals ) / sizeof( kWatchedSignals[0] );

        //! The write end of the living watcher's self-pipe, or -1 when there is no watcher.
        //!
        //! A file-scope variable because a signal handler has no other way to reach anything: the
        //! kernel calls it with the signal number and nothing else. volatile sig_atomic_t is the
        //! one type the standard promises a handler may read, which is why this is not an
        //! std::atomic<int> like the rest of this library's cross-thread state.
        //!
        //! One variable is enough only because at most one watcher exists in a process.
        volatile std::sig_atomic_t gHandlerWriteFd = -1;

        //! True while a watcher exists, so that a second one refuses instead of fighting the first.
        //!
        //! The state a watcher owns -- the dispositions, the handler's descriptor -- is
        //! process-global, so two of them would each put back what the other installed. Atomic
        //! because nothing stops two threads from constructing one at the same moment.
        std::atomic<bool> gWatcherExists { false };

        //! The dispositions that were in force before the watcher replaced them.
        //!
        //! At file scope rather than in the class, so that <signal.h> stays out of the public
        //! header. One watcher for each process makes one copy enough.
        struct sigaction gSavedActions[kWatchedSignalCount];

        //! The signal handler. Runs on whichever thread the kernel chose, with almost nothing
        //! legal inside it.
        //!
        //! It writes one byte and returns. It must not call quit(), post an event, or log: each of
        //! those takes a mutex, and taking a mutex is not async-signal-safe -- a signal that lands
        //! on the thread which already holds it stops the process for good. That failure passes
        //! every test for months and then hangs once in the field.
        //!
        //! write(2) is on the standard's list of what a handler may call. The byte is the signal
        //! number, so that the loop can report which signal arrived, and errno is saved and put
        //! back because the interrupted code has the right to find it unchanged.
        void signalHandler
            (
            int aSignal  //!< The signal that was delivered.
            )
        {
            const int savedErrno = errno;
            const int writeFd    = gHandlerWriteFd;

            if( writeFd >= 0 )
            {
                const unsigned char byte = static_cast<unsigned char>( aSignal );

                // One retry loop and no error handling. A full pipe means a notification is
                // already there to be read, and a failed write here has nobody to report to.
                ssize_t written = 0;
                do
                {
                    written = ::write( writeFd, &byte, 1 );
                }
                while( written < 0 && errno == EINTR );
            }

            errno = savedErrno;
        }

        //! Gets the calling thread's dispatcher if it is the Linux one, or null.
        //!
        //! Null is the answer for a thread that was never started as a Thread, and for one running
        //! EventDispatcherDefault, which has no way to poll a descriptor.
        std::shared_ptr<EventDispatcherLinux> currentLinuxDispatcher()
        {
            Thread* const current = Thread::currentThread();
            if( current == nullptr )
            {
                return nullptr;
            }

            return std::dynamic_pointer_cast<EventDispatcherLinux>( current->eventDispatcher() );
        }

        //! Fills @p aMask with the watched signals.
        void fillWatchedMask
            (
            sigset_t& aMask  //!< Set to fill; any previous content is discarded.
            )
        {
            sigemptyset( &aMask );
            for( std::size_t i = 0; i < kWatchedSignalCount; ++i )
            {
                sigaddset( &aMask, kWatchedSignals[i] );
            }
        }
    }

    //! Constructs the watcher and starts to watch, unless something in the way stops it.
    //!
    //! Every refusal path leaves isWatching() false and reports a critical record that names the
    //! reason. To refuse rather than install half of it is deliberate: a watcher that had installed
    //! the handlers but had nowhere to deliver would swallow the first Ctrl+C and do nothing.
    SignalWatcher::SignalWatcher
        (
        SignalWatchMode aMode  //!< How the signal is to be received.
        )
        : mMode( aMode )
    {
        bool expected = false;
        if( !gWatcherExists.compare_exchange_strong( expected, true ) )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: a watcher already exists in this process, so this one watches"
                << "nothing; the handlers and the saved dispositions are process-global and two"
                << "watchers would each undo the other";
            return;
        }

        const bool started = ( mMode == SignalWatchMode::SignalFd )
            ? startSignalFdMode()
            : startHandlerMode();

        if( !started )
        {
            gWatcherExists.store( false );
            return;
        }

        mWatching = true;
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

    //! Installs the handlers and the self-pipe they write to.
    //!
    //! Returns false, and undoes whatever it had already done, if any step fails.
    bool SignalWatcher::startHandlerMode()
    {
        const std::shared_ptr<EventDispatcherLinux> dispatcher = currentLinuxDispatcher();
        if( !dispatcher )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: this thread is not running EventDispatcherLinux, so a signal has"
                << "nowhere to be delivered; construct the watcher on the thread that calls exec()";
            return false;
        }

        int fds[2] = { -1, -1 };

        // Close-on-exec so that a fork()/exec() from user code does not leak the pair into the
        // child, and non-blocking on both ends: the handler must never block, and the loop's
        // callback reads until the pipe is empty and needs the read to report that instead of wait.
        if( ::pipe2( fds, O_CLOEXEC | O_NONBLOCK ) != 0 )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: pipe2() failed, so nothing is watched; errno" << errno;
            return false;
        }

        mNotifyFd = fds[0];
        mWriteFd  = fds[1];

        if( !dispatcher->registerEventSource( mNotifyFd, POLLIN,
            [this]( short )
            {
                onNotifyFdReady();
            } ) )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: registerEventSource() was refused for the notification pipe"
                << mNotifyFd;
            ::close( mNotifyFd );
            ::close( mWriteFd );
            mNotifyFd = -1;
            mWriteFd  = -1;
            return false;
        }

        // Published only now that there is a reader. A handler that fired against a descriptor
        // nobody polls would consume the signal and lose it.
        gHandlerWriteFd = mWriteFd;

        struct sigaction action;
        sigemptyset( &action.sa_mask );
        action.sa_handler = &signalHandler;

        // SA_RESETHAND is the whole of "the second signal kills": the kernel puts the default
        // disposition back as it enters the handler, so a second Ctrl+C stops the process without
        // any code of ours running. To do it inside the handler instead would be one more thing
        // that has to be correct in the least testable function in the library.
        //
        // SA_RESTART so that an interrupted read() somewhere else in the program continues instead
        // of failing with EINTR. It does not change the wait this library does: poll(2) is never
        // restarted whatever the flag says, and it does not have to be, because the byte the
        // handler just wrote makes the pipe readable and ends the wait anyway.
        action.sa_flags = SA_RESETHAND | SA_RESTART;

        for( std::size_t i = 0; i < kWatchedSignalCount; ++i )
        {
            if( sigaction( kWatchedSignals[i], &action, &gSavedActions[i] ) != 0 )
            {
                qCCritical( gLogSignalWatcher )
                    << "SignalWatcher: sigaction() failed for signal" << kWatchedSignals[i]
                    << "so nothing is watched; errno" << errno;

                // Put back the ones that did take, in the order they were replaced.
                for( std::size_t undo = 0; undo < i; ++undo )
                {
                    sigaction( kWatchedSignals[undo], &gSavedActions[undo], nullptr );
                }

                gHandlerWriteFd = -1;
                dispatcher->unregisterEventSource( mNotifyFd );
                ::close( mNotifyFd );
                ::close( mWriteFd );
                mNotifyFd = -1;
                mWriteFd  = -1;
                return false;
            }
        }

        qCInfo( gLogSignalWatcher )
            << "SignalWatcher: watching SIGINT and SIGTERM through a handler and a pipe";
        return true;
    }

    //! Opens a signalfd(2) and registers it, after a check of the condition the caller had to meet.
    //!
    //! Returns false, and undoes whatever it had already done, if any step fails.
    bool SignalWatcher::startSignalFdMode()
    {
        sigset_t watched;
        fillWatchedMask( watched );

        // The check that makes this mode honest. signalfd(2) reads signals that are blocked; an
        // unblocked one is delivered the ordinary way and never reaches the descriptor, so a
        // watcher built here without the block would look installed and report nothing.
        //
        // This can only see the calling thread's mask. That is the loop's own thread and the one
        // that matters most, but it is not the whole guarantee: a signal sent to the process goes
        // to any thread that does not block it, so a second thread which failed to inherit the
        // block would take the signal instead. Say what is checkable from here and no more.
        sigset_t current;
        sigemptyset( &current );
        if( pthread_sigmask( SIG_BLOCK, nullptr, &current ) != 0 )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: pthread_sigmask() could not be read, so SignalFd mode watches"
                << "nothing";
            return false;
        }

        for( std::size_t i = 0; i < kWatchedSignalCount; ++i )
        {
            if( sigismember( &current, kWatchedSignals[i] ) != 1 )
            {
                qCCritical( gLogSignalWatcher )
                    << "SignalWatcher: SignalFd mode needs signal" << kWatchedSignals[i]
                    << "blocked in every thread before any thread starts, and it is not blocked in"
                    << "this one, so nothing is watched; block it in main() with pthread_sigmask()"
                    << "before you start a thread, or use SignalWatchMode::Handler";
                return false;
            }
        }

        const std::shared_ptr<EventDispatcherLinux> dispatcher = currentLinuxDispatcher();
        if( !dispatcher )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: this thread is not running EventDispatcherLinux, so a signal has"
                << "nowhere to be delivered; construct the watcher on the thread that calls exec()";
            return false;
        }

        mNotifyFd = ::signalfd( -1, &watched, SFD_NONBLOCK | SFD_CLOEXEC );
        if( mNotifyFd < 0 )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: signalfd() failed, so nothing is watched; errno" << errno;
            mNotifyFd = -1;
            return false;
        }

        if( !dispatcher->registerEventSource( mNotifyFd, POLLIN,
            [this]( short )
            {
                onNotifyFdReady();
            } ) )
        {
            qCCritical( gLogSignalWatcher )
                << "SignalWatcher: registerEventSource() was refused for the signalfd" << mNotifyFd;
            ::close( mNotifyFd );
            mNotifyFd = -1;
            return false;
        }

        qCInfo( gLogSignalWatcher )
            << "SignalWatcher: watching SIGINT and SIGTERM through signalfd(2)";
        return true;
    }

    //! Reads the notification and reports the signal. Runs on the watcher's own thread.
    //!
    //! Both modes read the same way: empty the descriptor, keep the first signal number it gives,
    //! and report that one. A burst is one shutdown, not several.
    void SignalWatcher::onNotifyFdReady()
    {
        int arrived = 0;

        if( mMode == SignalWatchMode::SignalFd )
        {
            signalfd_siginfo info;
            while( ::read( mNotifyFd, &info, sizeof( info ) )
                == static_cast<ssize_t>( sizeof( info ) ) )
            {
                if( arrived == 0 )
                {
                    arrived = static_cast<int>( info.ssi_signo );
                }
            }
        }
        else
        {
            unsigned char buffer[16];
            ssize_t got = 0;
            while( ( got = ::read( mNotifyFd, buffer, sizeof( buffer ) ) ) > 0 )
            {
                if( arrived == 0 )
                {
                    arrived = static_cast<int>( buffer[0] );
                }
            }
        }

        if( arrived == 0 )
        {
            // Readable with nothing in it. Nothing to report, and nothing wrong either: an
            // unregister that races the poll(2) round which saw the readiness ends here.
            return;
        }

        if( mDelivered )
        {
            return;
        }

        // In SignalFd mode nothing has yet put the default behaviour back, because there is no
        // handler and thus no SA_RESETHAND to do it. Unblock the watched signals in this thread so
        // that a second one stops the process: a signal sent to the process goes to any thread
        // that does not block it, and after this that is this one.
        if( mMode == SignalWatchMode::SignalFd )
        {
            sigset_t watched;
            fillWatchedMask( watched );
            pthread_sigmask( SIG_UNBLOCK, &watched, nullptr );
        }

        deliver( arrived );
    }

    //! Takes the descriptor out of the loop, puts the dispositions back, and closes what it owns.
    //!
    //! Safe to call when nothing was ever installed, and safe to call two times.
    void SignalWatcher::stopWatching()
    {
        if( !mWatching )
        {
            return;
        }

        mWatching = false;

        if( mMode == SignalWatchMode::Handler )
        {
            gHandlerWriteFd = -1;

            // Not put back once a signal has been reported. SA_RESETHAND has already restored the
            // default disposition, and to write the previous one over it would undo the guarantee
            // that the next signal stops the process even if this shutdown is stuck.
            if( !mDelivered )
            {
                for( std::size_t i = 0; i < kWatchedSignalCount; ++i )
                {
                    sigaction( kWatchedSignals[i], &gSavedActions[i], nullptr );
                }
            }
        }

        if( mNotifyFd >= 0 )
        {
            // Unregistered from the loop's own thread, so EventDispatcherLinux's contract makes
            // this synchronous: the callback will not run again, not even for a readiness the
            // current poll(2) round has already seen. That is what makes it safe to close the
            // descriptor and destroy the object immediately afterwards.
            const std::shared_ptr<EventDispatcherLinux> dispatcher = currentLinuxDispatcher();
            if( dispatcher )
            {
                dispatcher->unregisterEventSource( mNotifyFd );
            }

            ::close( mNotifyFd );
            mNotifyFd = -1;
        }

        if( mWriteFd >= 0 )
        {
            ::close( mWriteFd );
            mWriteFd = -1;
        }

        gWatcherExists.store( false );
    }

    //! Reports the signal to the application, and then flushes the log.
    void SignalWatcher::deliver
        (
        int aSignal  //!< The signal number to report.
        )
    {
        mDelivered = true;

        qCInfo( gLogSignalWatcher )
            << "SignalWatcher: signal" << aSignal
            << "received; the next one will stop the process";

        mTriggered.emit( aSignal );

        // After the slots, not before. The records that explain a shutdown are the ones the slots
        // write on their way out, and those are exactly the ones a sink with a buffer still holds.
        Log::flush();
    }
}
