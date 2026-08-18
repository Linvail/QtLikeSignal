// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A real X11 program on QtLikeSignal: one window whose events arrive through QtLikeSignal's own
//! event dispatcher, interleaved with the library's own timers, posted work and cross-thread queued
//! signals.
//!
//! There is no XNextEvent() loop in this program. CoreApplication::exec() is the only loop, and the
//! display connection reaches it through EventDispatcherLinux::registerEventSource() -- the socket
//! becomes one more descriptor in the poll(2) call the loop already blocks in. Three sources share
//! that one call:
//!
//! - **the X server**, on the display socket;
//! - **this thread's own events**, on the dispatcher's eventfd -- posted tasks, deferred deletes,
//!   and queued signals arriving from other threads;
//! - **timers**, as the poll() timeout.
//!
//! Everything on screen is evidence of that. The uptime counter is a QtLikeSignal::Timer and keeps
//! counting while the pointer floods the socket with motion; the worker counter is a queued signal
//! emitted on a second thread, which has to wake a loop asleep on the X socket to be delivered at
//! all; and the repaints themselves are posted through CoreApplication::post() rather than asked of
//! the server. If any of the three starved the others, this window is where it would show.
//!
//! Controls: move and click the pointer anywhere in the window; Escape or the close button quits.
//! Pass --seconds=N to quit on a timer instead, which is how the demo is run unattended.

#include "X11Application.hpp"
#include "MouseEvent.hpp"

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <X11/Xlib.h>

// Xlib's None macro against MouseEvent::Button::None; see the same note in X11Application.cpp. The
// demo headers above are included first, so the enumerator is declared before the macro exists.
#undef None

using namespace QtLikeSignalDemo;

namespace
{
    //! How many recent events the on-screen log keeps.
    constexpr int kLogLines = 18;

    //! Allocates a colour by name, falling back to white so a failure cannot leave text invisible.
    unsigned long allocateColour
        (
        Display* aDisplay,     //!< Connection to allocate on.
        const char* aName      //!< An X colour name or "#rrggbb".
        )
    {
        const int screen = DefaultScreen( aDisplay );
        const Colormap map = DefaultColormap( aDisplay, screen );

        XColor closest {};
        XColor exact {};
        if( XAllocNamedColor( aDisplay, map, aName, &closest, &exact ) == 0 )
        {
            return WhitePixel( aDisplay, screen );
        }
        return closest.pixel;
    }

    //! Loads a fixed-pitch core font, or nullptr if the server offers none.
    //!
    //! Core fonts are a legacy path that a bare X server -- Xwayland with no font path, for
    //! instance -- may simply not have. Drawing text with a font the server rejected raises a
    //! BadFont, and Xlib's default error handler ends the process, so a missing font has to mean
    //! "draw no text" rather than "draw text anyway".
    XFontStruct* loadFont
        (
        Display* aDisplay   //!< Connection to load on.
        )
    {
        static const char* const candidates[] =
        {
            "9x15",
            "fixed",
            "-*-*-medium-r-normal--15-*-*-*-*-*-iso8859-1",
            "-*-*-*-*-*-*-*-*-*-*-*-*-*-*"
        };

        for( const char* const name : candidates )
        {
            XFontStruct* const font = XLoadQueryFont( aDisplay, name );
            if( font != nullptr )
            {
                return font;
            }
        }
        return nullptr;
    }

    //! A worker thread that reports a counter through a queued signal.
    //!
    //! The other half of the demonstration. The main loop spends its time asleep in poll() on the X
    //! socket; this thread emits from its own event loop, the connection is Queued, so the emission
    //! posts to the main thread's dispatcher and writes its eventfd. That write is what ends the
    //! poll() -- the same call, the same wait, a different descriptor. A dispatcher that only knew
    //! how to wait for X events would never deliver this.
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

    //! Records what arrives and paints the window.
    //!
    //! An ordinary QtLikeSignal::Object. It knows about Xlib only because it draws; every single
    //! thing it reacts to -- pointer, keyboard, timer, worker thread -- arrives as a signal.
    class EventLog : public QtLikeSignal::Object
    {
    public:
        //! Constructs the log against the application whose window it draws in.
        explicit EventLog
            (
            X11Application* aApplication   //!< Application owning the window. Not owned.
            )
            : mApplication( aApplication )
        {
            Display* const display = static_cast<Display*>( aApplication->nativeDisplay() );

            mBackground = allocateColour( display, "#181a20" );
            mHeading    = allocateColour( display, "#78c8ff" );
            mDim        = allocateColour( display, "#969bb0" );
            mText       = allocateColour( display, "#ebebf0" );
            mAccent     = allocateColour( display, "#a0e6a0" );
            mCrosshair  = allocateColour( display, "#5aa0dc" );

            mFont = loadFont( display );
            if( mFont != nullptr )
            {
                XSetFont( display, static_cast<GC>( aApplication->nativeGraphicsContext() ),
                    mFont->fid );
                mLineHeight = mFont->ascent + mFont->descent + 4;
            }
            else
            {
                std::printf( "  no core font available; drawing shapes only, counters go to "
                    "stdout\n" );
            }
        }

        //! Frees the font, if one was loaded.
        virtual ~EventLog() override
        {
            if( mFont != nullptr )
            {
                XFreeFont( static_cast<Display*>( mApplication->nativeDisplay() ), mFont );
                mFont = nullptr;
            }
        }

        //! Records a pointer press and repaints.
        void onMousePressed
            (
            MouseEvent aEvent   //!< The press.
            )
        {
            ++mPressCount;
            mHeld = aEvent.mButton;
            append( "press  ", aEvent );
            std::printf( "  X11 ButtonPress    %s at %4d,%4d\n", nameOf( aEvent.mButton ),
                aEvent.mX, aEvent.mY );
        }

        //! Records a pointer release and repaints.
        void onMouseReleased
            (
            MouseEvent aEvent   //!< The release.
            )
        {
            ++mReleaseCount;
            mHeld = MouseEvent::Button::None;
            append( "release", aEvent );
            std::printf( "  X11 ButtonRelease  %s at %4d,%4d\n", nameOf( aEvent.mButton ),
                aEvent.mX, aEvent.mY );
        }

        //! Records a pointer move and repaints.
        //!
        //! Motion arrives in floods, so the log only takes one every few pixels; the counter and
        //! the crosshair take every single one, which is what makes the crosshair track smoothly.
        void onMouseMoved
            (
            MouseEvent aEvent   //!< The move.
            )
        {
            ++mMoveCount;
            mLastX = aEvent.mX;
            mLastY = aEvent.mY;

            const int dx = aEvent.mX - mLastLoggedX;
            const int dy = aEvent.mY - mLastLoggedY;
            if( ( dx * dx + dy * dy ) >= ( 40 * 40 ) )
            {
                mLastLoggedX = aEvent.mX;
                mLastLoggedY = aEvent.mY;
                append( "move   ", aEvent );
            }
            else
            {
                mApplication->update();
            }
        }

        //! Records a key press.
        void onKeyPressed
            (
            unsigned long aKeySym   //!< The KeySym the keycode mapped to.
            )
        {
            ++mKeyCount;
            std::printf( "  X11 KeyPress       keysym 0x%lx\n", aKeySym );
            mApplication->update();
        }

        //! Counts one second of uptime and repaints.
        //!
        //! Driven by a QtLikeSignal::Timer, which is the poll() timeout rather than anything the X
        //! server knows about. It keeps counting while the pointer floods the socket.
        void onSecond()
        {
            ++mSeconds;
            std::printf( "  [%3ds] motion %6d  press %3d  release %3d  key %3d  worker %5d\n",
                mSeconds, mMoveCount, mPressCount, mReleaseCount, mKeyCount, mWorkerTicks );
            std::fflush( stdout );
            mApplication->update();
        }

        //! Takes a tick emitted on the worker thread, delivered here by a queued connection.
        void onWorkerTick
            (
            int aCount   //!< The worker's running count.
            )
        {
            mWorkerTicks = aCount;
            mApplication->update();
        }

        //! Paints the whole window into the back buffer the application handed over.
        void onPaint
            (
            unsigned long aDrawable   //!< The off-screen pixmap to draw into.
            )
        {
            Display* const display = static_cast<Display*>( mApplication->nativeDisplay() );
            const GC gc = static_cast<GC>( mApplication->nativeGraphicsContext() );
            const int width  = mApplication->width();
            const int height = mApplication->height();

            XSetForeground( display, gc, mBackground );
            XFillRectangle( display, aDrawable, gc, 0, 0, static_cast<unsigned int>( width ),
                static_cast<unsigned int>( height ) );

            int y = mLineHeight + 6;

            XSetForeground( display, gc, mHeading );
            drawText( aDrawable, 16, y,
                "QtLikeSignal on X11 -- one poll() for the X socket, our events and our timers" );
            y += mLineHeight;

            XSetForeground( display, gc, mDim );
            drawText( aDrawable, 16, y,
                "No XNextEvent loop in this program. CoreApplication::exec() is the only loop." );
            y += mLineHeight;
            drawText( aDrawable, 16, y,
                "Move/click the pointer, press keys.  Esc or the close button quits." );
            y += mLineHeight * 2;

            XSetForeground( display, gc, mText );
            drawLine( aDrawable, 16, y, "window size   : ", width, " x ", height );
            y += mLineHeight;
            drawLine( aDrawable, 16, y, "pointer       : ", mLastX, ", ", mLastY );
            y += mLineHeight;
            drawLine( aDrawable, 16, y, "presses       : ", mPressCount );
            y += mLineHeight;
            drawLine( aDrawable, 16, y, "releases      : ", mReleaseCount );
            y += mLineHeight;
            drawLine( aDrawable, 16, y, "motion        : ", mMoveCount );
            y += mLineHeight;
            drawLine( aDrawable, 16, y, "key presses   : ", mKeyCount );
            y += mLineHeight;

            XSetForeground( display, gc, mAccent );
            drawLine( aDrawable, 16, y, "timer uptime  : ", mSeconds,
                " s   (QtLikeSignal::Timer -- the poll() timeout)" );
            y += mLineHeight;
            drawLine( aDrawable, 16, y, "worker ticks  : ", mWorkerTicks,
                "   (queued signal from a second thread, through the eventfd)" );
            y += mLineHeight * 2;

            // Three squares, filled while their button is held. Nothing here is subtle; it exists
            // so a press is visible without reading the numbers.
            static const MouseEvent::Button drawn[3] =
            {
                MouseEvent::Button::Left,
                MouseEvent::Button::Middle,
                MouseEvent::Button::Right
            };
            for( int button = 0; button < 3; ++button )
            {
                const bool held = ( mHeld == drawn[button] );
                XSetForeground( display, gc, held ? mAccent : mDim );
                if( held )
                {
                    XFillRectangle( display, aDrawable, gc, 16 + button * 34, y, 26u, 26u );
                }
                else
                {
                    XDrawRectangle( display, aDrawable, gc, 16 + button * 34, y, 26u, 26u );
                }
            }
            y += 26 + mLineHeight;

            XSetForeground( display, gc, mDim );
            drawText( aDrawable, 16, y, "recent events" );
            y += mLineHeight;

            XSetForeground( display, gc, mText );
            for( const std::string& entry : mLog )
            {
                drawText( aDrawable, 16, y, entry.c_str() );
                y += mLineHeight;
            }

            // A crosshair at the last reported pointer position, so the coordinates are visibly the
            // ones the server sent rather than something invented.
            XSetForeground( display, gc, mCrosshair );
            XDrawLine( display, aDrawable, gc, mLastX - 12, mLastY, mLastX + 12, mLastY );
            XDrawLine( display, aDrawable, gc, mLastX, mLastY - 12, mLastX, mLastY + 12 );
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
            return "none  ";
        }

        //! Adds one line to the log, drops the oldest, and asks for a repaint.
        void append
            (
            const char* aKind,        //!< Event kind, already padded to a fixed width.
            const MouseEvent& aEvent  //!< The event to describe.
            )
        {
            std::ostringstream entry;
            entry << "  " << aKind << "  " << nameOf( aEvent.mButton ) << "  at "
                  << std::setw( 4 ) << aEvent.mX << ", " << std::setw( 4 ) << aEvent.mY;

            mLog.push_back( entry.str() );
            if( static_cast<int>( mLog.size() ) > kLogLines )
            {
                mLog.pop_front();
            }

            mApplication->update();
        }

        //! Draws one string, or nothing at all if the server had no core font to offer.
        void drawText
            (
            unsigned long aDrawable,   //!< Where to draw.
            int aX,                    //!< Left edge.
            int aY,                    //!< Text baseline.
            const char* aText          //!< The string.
            )
        {
            if( mFont == nullptr )
            {
                return;
            }

            XDrawString( static_cast<Display*>( mApplication->nativeDisplay() ), aDrawable,
                static_cast<GC>( mApplication->nativeGraphicsContext() ), aX, aY, aText,
                static_cast<int>( std::strlen( aText ) ) );
        }

        //! Draws a label followed by any number of values, built through a stream.
        //!
        //! Variadic because the status lines have between one and three values each, and the
        //! alternative -- a format string with a fixed buffer -- means guessing a size for every
        //! line. @p aRest is a parameter pack of anything std::ostream can take: ints, string
        //! literals used as separators, in the order they should appear. The fold expression
        //! `( ( built << aRest ), ... )` expands to one `built << arg` per element, left to right,
        //! which is C++17's way of saying "stream them all in order" without a recursive helper.
        //!
        //! @code
        //!   drawLine( d, 16, y, "window size   : ", width, " x ", height );
        //!   // -> "window size   : 1024 x 640"
        //! @endcode
        template <typename ... Rest>
        void drawLine
            (
            unsigned long aDrawable,   //!< Where to draw.
            int aX,                    //!< Left edge.
            int aY,                    //!< Text baseline.
            const char* aLabel,        //!< Fixed-width label.
            Rest&&... aRest            //!< Values and separators, streamed in order.
            )
        {
            if( mFont == nullptr )
            {
                return;
            }

            std::ostringstream built;
            built << aLabel;
            ( ( built << aRest ), ... );

            const std::string text = built.str();
            drawText( aDrawable, aX, aY, text.c_str() );
        }

        X11Application*        mApplication;                       //!< Window owner. Not owned.
        std::deque<std::string> mLog;                              //!< Recent event descriptions.
        XFontStruct*           mFont { nullptr };                  //!< Core font, or null.
        int mLineHeight { 16 };                                    //!< Baseline-to-baseline pixels.
        int mPressCount { 0 };                                     //!< Total presses seen.
        int mReleaseCount { 0 };                                   //!< Total releases seen.
        int mMoveCount { 0 };                                      //!< Total motion events seen.
        int mKeyCount { 0 };                                       //!< Total key presses seen.
        int mSeconds { 0 };                                        //!< Seconds from the local timer.
        int mWorkerTicks { 0 };                                    //!< Last count from the worker.
        int mLastX { 0 };                                          //!< Last reported pointer X.
        int mLastY { 0 };                                          //!< Last reported pointer Y.
        int mLastLoggedX { 0 };                                    //!< Pointer X when last logged.
        int mLastLoggedY { 0 };                                    //!< Pointer Y when last logged.
        MouseEvent::Button mHeld { MouseEvent::Button::None };     //!< Button currently held down.
        unsigned long mBackground { 0 };                           //!< Window background pixel.
        unsigned long mHeading { 0 };                              //!< Heading text pixel.
        unsigned long mDim { 0 };                                  //!< Secondary text pixel.
        unsigned long mText { 0 };                                 //!< Primary text pixel.
        unsigned long mAccent { 0 };                               //!< Highlighted value pixel.
        unsigned long mCrosshair { 0 };                            //!< Pointer crosshair pixel.
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
    X11Application app( aArgc, aArgv );

    if( !app.createWindow( "QtLikeSignal demo -- X11 events and our own events", 1024, 640 ) )
    {
        std::fprintf( stderr, "failed to create the window\n" );
        return 1;
    }

    EventLog log( &app );

    // Direct connections: the drain runs on this thread, inside this thread's dispatch pass, so the
    // slots run there too. Auto would resolve to Direct for the same reason.
    QtLikeSignal::Object::connect( app.getMousePressed(), &log, &EventLog::onMousePressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getMouseReleased(), &log, &EventLog::onMouseReleased,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getMouseMoved(), &log, &EventLog::onMouseMoved,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getKeyPressed(), &log, &EventLog::onKeyPressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getPaintRequested(), &log, &EventLog::onPaint,
        QtLikeSignal::ConnectionType::Direct );

    // A QtLikeSignal timer, sharing the loop with the X socket. It is the poll() timeout, so if OS
    // event pumping ever starved the timers -- or the timers starved the socket -- this is where it
    // would show.
    QtLikeSignal::Timer second;
    QtLikeSignal::Object::connect( second.getTimeout(), &log, &EventLog::onSecond,
        QtLikeSignal::ConnectionType::Direct );
    second.start( 1000 );

    // The cross-thread half. Queued, so the emission on the worker thread posts to this thread's
    // dispatcher and writes its eventfd -- which is what brings the loop back out of a poll() that
    // is otherwise waiting on the X socket.
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

    std::printf( "QtLikeSignal X11 demo running.\n" );
    std::printf( "  window            : 1024x640 on DISPLAY=%s\n",
        std::getenv( "DISPLAY" ) != nullptr ? std::getenv( "DISPLAY" ) : "<unset>" );
    std::printf( "  event loop        : CoreApplication::exec()\n" );
    std::printf( "  X event source    : EventDispatcherLinux::registerEventSource( "
        "XConnectionNumber(), POLLIN )\n" );
    std::printf( "  our own events    : eventfd in the same poll() -- posted repaints and a "
        "queued signal from a worker thread\n" );
    std::printf( "  timers            : the poll() timeout\n" );
    std::printf( "  no XNextEvent loop in this program\n" );
    std::fflush( stdout );

    const int result = app.exec();

    // Stop the worker before its Thread object goes out of scope; quit() ends its exec(), wait()
    // blocks until run() has returned.
    worker.quit();
    worker.wait();

    std::printf( "loop finished, exit code %d\n", result );
    return result;
}
