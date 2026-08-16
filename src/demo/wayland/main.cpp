// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A real Wayland program on QtLikeSignal: one window whose compositor events arrive through
//! QtLikeSignal's own event dispatcher, interleaved with the library's own timers, posted work and
//! cross-thread queued signals.
//!
//! There is no wl_display_dispatch() loop in this program. CoreApplication::exec() is the only
//! loop, and the compositor connection reaches it through
//! EventDispatcherLinux::registerEventSource() -- the socket becomes one more descriptor in the
//! poll(2) call the loop already blocks in. Three sources share that one call:
//!
//! - **the compositor**, on the Wayland socket;
//! - **this thread's own events**, on the dispatcher's eventfd -- posted tasks and queued signals
//!   arriving from other threads;
//! - **timers**, as the poll() timeout.
//!
//! **Everything received is printed, and nothing is drawn.** The window is a single flat colour
//! that is filled once at startup, because a Wayland surface is not mapped -- and so receives no
//! input at all -- until a buffer is attached to it. That is the whole of this demo's rendering.
//! What it demonstrates is the loop, and the loop is visible in the console: pointer and keyboard
//! events as they arrive, a once-a-second summary from a QtLikeSignal::Timer, and a counter fed by
//! a queued signal from a second thread. If the compositor's events ever starved the timers, or the
//! timers starved the socket, the cadence of those lines is where it would show.
//!
//! Controls: move and click the pointer over the window, press keys; Escape or the close button
//! quits. Pass --seconds=N to quit on a timer instead, which is how the demo is run unattended.

#include "WaylandApplication.hpp"
#include "MouseEvent.hpp"

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace QtLikeSignalDemo;

namespace
{
    //! A worker thread that reports a counter through a queued signal.
    //!
    //! The other half of the demonstration. The main loop spends its time asleep in poll() on the
    //! Wayland socket; this thread emits from its own event loop, the connection is Queued, so the
    //! emission posts to the main thread's dispatcher and writes its eventfd. That write is what
    //! ends the poll() -- the same call, the same wait, a different descriptor. A dispatcher that
    //! only knew how to wait for compositor events would never deliver this.
    class TickerThread : public QtLikeSignal::Thread
    {
    public:
        //! Gets the tick signal, carrying the running count.
        QtLikeSignal::SignalView<int>& getTicked() const
        {
            return mTicked.view();
        }

    protected:
        //! Runs a timer on this thread and emits on every expiry, until quit() stops the loop.
        virtual void run() override
        {
            //! Counts expiries and publishes them through the thread's signal.
            //!
            //! An ordinary Object, constructed here rather than in the parent thread, so its thread
            //! affinity is this thread and the timer's events are delivered to it here.
            class Ticker : public QtLikeSignal::Object
            {
            public:
                //! Constructs the counter against the signal it reports through.
                explicit Ticker
                    (
                    QtLikeSignal::Signal<int>* aOut   //!< Signal to emit on. Not owned.
                    )
                    : mOut( aOut )
                {
                }

                //! Counts one expiry and emits it.
                void onTimeout()
                {
                    ++mCount;
                    mOut->emit( mCount );
                }

            private:
                QtLikeSignal::Signal<int>* mOut;   //!< Where the count goes. Not owned.
                int mCount { 0 };                  //!< Expiries so far.
            };

            Ticker ticker( &mTicked );
            QtLikeSignal::Timer timer;

            // Auto, and it resolves to Direct: the timer and the counter were both constructed on
            // this thread, so there is nothing to queue across.
            QtLikeSignal::Object::connect( timer.getTimeout(), &ticker, &Ticker::onTimeout,
                QtLikeSignal::ConnectionType::Auto );
            timer.start( 250 );

            exec();
        }

    private:
        //! Emitted on this thread every time the timer expires, with the running count.
        //!
        //! A member of the Thread rather than of the Ticker so the main thread can connect to it
        //! before start() is called; the Ticker itself does not exist until run() is under way.
        QtLikeSignal::Signal<int> mTicked;
    };

    //! Prints what arrives, and counts it.
    //!
    //! An ordinary QtLikeSignal::Object that knows nothing about Wayland. Everything it reacts to
    //! -- pointer, keyboard, compositor configure, timer, worker thread -- arrives as a signal, and
    //! everything it does with them is std::printf. There is deliberately no drawing anywhere in
    //! this program.
    class EventLog : public QtLikeSignal::Object
    {
    public:
        //! Records a pointer press and prints it.
        //!
        //! Wayland's button event carries no position -- the protocol says the pointer is wherever
        //! the last motion or enter put it -- so the coordinates printed here are the ones tracked
        //! from those, and they are the same ones the compositor means.
        void onMousePressed
            (
            MouseEvent aEvent   //!< The press. Position is not set by the protocol.
            )
        {
            ++mPressCount;
            std::printf( "  wl_pointer button   %s pressed  at %4d,%4d\n",
                nameOf( aEvent.mButton ), mLastX, mLastY );
        }

        //! Records a pointer release and prints it.
        void onMouseReleased
            (
            MouseEvent aEvent   //!< The release. Position is not set by the protocol.
            )
        {
            ++mReleaseCount;
            std::printf( "  wl_pointer button   %s released at %4d,%4d\n",
                nameOf( aEvent.mButton ), mLastX, mLastY );
        }

        //! Records a pointer motion.
        //!
        //! Not printed one line per event: motion arrives in floods and would bury everything else.
        //! The count in the once-a-second summary is taken from every single one.
        void onMouseMoved
            (
            MouseEvent aEvent   //!< The move.
            )
        {
            ++mMoveCount;
            mLastX = aEvent.mX;
            mLastY = aEvent.mY;
        }

        //! Notes the pointer arriving over the surface.
        void onPointerEntered()
        {
            std::printf( "  wl_pointer enter\n" );
        }

        //! Notes the pointer leaving the surface.
        void onPointerLeft()
        {
            std::printf( "  wl_pointer leave\n" );
        }

        //! Prints a key press by its raw evdev keycode.
        void onKeyPressed
            (
            unsigned int aKeyCode   //!< The evdev keycode; 1 is Escape.
            )
        {
            ++mKeyCount;
            std::printf( "  wl_keyboard key     evdev keycode %u\n", aKeyCode );
        }

        //! Prints the size the compositor suggested for the surface.
        void onSurfaceConfigured
            (
            int aWidth,   //!< Suggested width, or 0 for "you choose".
            int aHeight   //!< Suggested height, or 0 for "you choose".
            )
        {
            std::printf( "  xdg_toplevel config %d x %d\n", aWidth, aHeight );
        }

        //! Prints one line of totals, once a second.
        //!
        //! Driven by a QtLikeSignal::Timer, which is the poll() timeout rather than anything the
        //! compositor knows about. It keeps its cadence while the pointer floods the socket, which
        //! is the point of printing it alongside the motion count.
        void onSecond()
        {
            ++mSeconds;
            std::printf( "  [%3ds] motion %6d  press %3d  release %3d  key %3d  worker %5d\n",
                mSeconds, mMoveCount, mPressCount, mReleaseCount, mKeyCount, mWorkerTicks );
            std::fflush( stdout );
        }

        //! Takes a tick emitted on the worker thread, delivered here by a queued connection.
        void onWorkerTick
            (
            int aCount   //!< The worker's running count.
            )
        {
            mWorkerTicks = aCount;
        }

    private:
        //! Names a button for the log.
        static const char* nameOf
            (
            MouseEvent::Button aButton   //!< The button to name.
            )
        {
            switch( aButton )
            {
                case MouseEvent::Button::Left:   return "left  ";
                case MouseEvent::Button::Middle: return "middle";
                case MouseEvent::Button::Right:  return "right ";
                case MouseEvent::Button::None:   break;
            }
            return "other ";
        }

        int mPressCount { 0 };     //!< Total presses seen.
        int mReleaseCount { 0 };   //!< Total releases seen.
        int mMoveCount { 0 };      //!< Total motion events seen.
        int mKeyCount { 0 };       //!< Total key presses seen.
        int mSeconds { 0 };        //!< Seconds from the local timer.
        int mWorkerTicks { 0 };    //!< Last count from the worker thread.
        int mLastX { 0 };          //!< Last reported pointer x, from motion or enter.
        int mLastY { 0 };          //!< Last reported pointer y, from motion or enter.
    };

    //! Reads --seconds=N from the command line, or 0 if it was not given.
    int quitAfterSeconds
        (
        const std::vector<std::string>& aArguments   //!< The application's arguments.
        )
    {
        static const std::string prefix = "--seconds=";
        for( const std::string& argument : aArguments )
        {
            if( argument.compare( 0, prefix.size(), prefix ) == 0 )
            {
                return std::atoi( argument.c_str() + prefix.size() );
            }
        }
        return 0;
    }
}

//! Builds the application, wires every source to the log, and runs the one loop there is.
int main
    (
    int aArgc,     //!< Argument count.
    char** aArgv   //!< Argument values.
    )
{
    WaylandApplication app( aArgc, aArgv );

    if( !app.createWindow( "QtLikeSignal demo -- Wayland events and our own events", 800, 500 ) )
    {
        std::fprintf( stderr, "failed to create the window\n" );
        return 1;
    }

    EventLog log;

    // Direct connections: the drain runs on this thread, inside this thread's dispatch pass, so the
    // slots run there too. Auto would resolve to Direct for the same reason.
    QtLikeSignal::Object::connect( app.getMousePressed(), &log, &EventLog::onMousePressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getMouseReleased(), &log, &EventLog::onMouseReleased,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getMouseMoved(), &log, &EventLog::onMouseMoved,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getPointerEntered(), &log, &EventLog::onPointerEntered,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getPointerLeft(), &log, &EventLog::onPointerLeft,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getKeyPressed(), &log, &EventLog::onKeyPressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getSurfaceConfigured(), &log,
        &EventLog::onSurfaceConfigured, QtLikeSignal::ConnectionType::Direct );

    // A QtLikeSignal timer, sharing the loop with the Wayland socket. It is the poll() timeout, so
    // if platform event pumping ever starved the timers -- or the timers starved the socket -- this
    // is where it would show.
    QtLikeSignal::Timer second;
    QtLikeSignal::Object::connect( second.getTimeout(), &log, &EventLog::onSecond,
        QtLikeSignal::ConnectionType::Direct );
    second.start( 1000 );

    // The cross-thread half. Queued, so the emission on the worker thread posts to this thread's
    // dispatcher and writes its eventfd -- which is what brings the loop back out of a poll() that
    // is otherwise waiting on the Wayland socket.
    TickerThread worker;
    QtLikeSignal::Object::connect( worker.getTicked(), &log, &EventLog::onWorkerTick,
        QtLikeSignal::ConnectionType::Queued );
    worker.start();

    // Unattended runs: --seconds=N ends the loop without a human closing the window.
    QtLikeSignal::Timer autoQuit;
    const int seconds = quitAfterSeconds( app.arguments() );
    if( seconds > 0 )
    {
        autoQuit.setSingleShot( true );
        QtLikeSignal::Object::connect( autoQuit.getTimeout(), &log,
            []()
            {
                std::printf( "  --seconds elapsed, quitting\n" );
                QtLikeSignal::CoreApplication::quit();
            },
            QtLikeSignal::ConnectionType::Direct );
        autoQuit.start( seconds * 1000 );
    }

    std::printf( "QtLikeSignal Wayland demo running.\n" );
    std::printf( "  surface           : %dx%d on WAYLAND_DISPLAY=%s\n", app.width(), app.height(),
        std::getenv( "WAYLAND_DISPLAY" ) != nullptr
            ? std::getenv( "WAYLAND_DISPLAY" ) : "<unset>" );
    std::printf( "  event loop        : CoreApplication::exec()\n" );
    std::printf( "  wayland source    : EventDispatcherLinux::registerEventSource( "
        "wl_display_get_fd(), POLLIN )\n" );
    std::printf( "  our own events    : eventfd in the same poll() -- a queued signal from a "
        "worker thread\n" );
    std::printf( "  timers            : the poll() timeout\n" );
    std::printf( "  seat capabilities : pointer %s, keyboard %s\n",
        app.hasPointer() ? "yes" : "no", app.hasKeyboard() ? "yes" : "no" );
    std::printf( "  no wl_display_dispatch loop in this program, and no drawing at all\n" );
    std::fflush( stdout );

    const int result = app.exec();

    // Stop the worker before its Thread object goes out of scope; quit() ends its exec(), wait()
    // blocks until run() has returned.
    worker.quit();
    worker.wait();

    std::printf( "loop finished, exit code %d\n", result );
    return result;
}
