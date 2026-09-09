// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A real Windows program on QtLikeSignalGui: a window created by the library, whose mouse events
//! arrive through QtLikeSignal's own event dispatcher and are delivered as signals.
//!
//! This is the Win32 demo rewritten against the library instead of against Win32. Compare it with
//! demo/windows/: that one registers its own window class, writes its own window procedure, and
//! decodes WM_LBUTTONDOWN itself. Here none of that appears -- GuiApplication::createWindow() makes
//! the window and Window's signals carry the input, so the only Win32 left in this file is the
//! drawing, which is the part the library deliberately does not do.
//!
//! **That split is the point.** The window is created by QtLikeSignalGui and its handle is handed
//! to whatever draws: here a few GDI calls, in a real program an external library initialising WGL.
//! The renderer below takes Window::nativeHandle(), calls GetDC on it once, and never asks the
//! library for a drawing surface -- because the library has none to give and never chooses a pixel
//! format that would take the choice away.
//!
//! There is still no message loop in this program. exec() is the only loop, and
//! EventDispatcherWin32::processPlatformEvents() is what pumps the OS messages inside it -- so
//! every number that changes on screen is evidence that the dispatcher is servicing Windows
//! correctly.
//!
//! Controls: move, click and scroll anywhere in the window; Escape is not handled (there are no key
//! signals yet, by design); the close box quits.

#include "QtLikeSignalGui/GuiApplication.hpp"
#include "QtLikeSignalGui/Window.hpp"

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Property.hpp"
#include "QtLikeSignal/PropertyAnimation.hpp"
#include "QtLikeSignal/Timer.hpp"

#include "QtLikeSignal/Log.hpp"

#include <cstdio>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>
#include <tchar.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

//! This demo's category. Each demo defines its own, because each is its own program.
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogDemo, "demo.gui", "DGUI" )

using namespace QtLikeSignalGui;

namespace
{
    //! The string type the generic-text Win32 calls in this file take.
    using DemoString = std::basic_string<TCHAR>;

    //! A stream that builds one, so no format string or fixed buffer is needed.
    using DemoStream = std::basic_ostringstream<TCHAR>;

    //! How many recent events the on-screen log keeps.
    const int kLogLines = 18;

    //! Draws the window and records what it is told about.
    //!
    //! An ordinary QtLikeSignal::Object. It knows nothing about window creation, message queues or
    //! window procedures -- it was handed an HWND and it draws on it, which is exactly the position
    //! an external rendering library is in.
    class Renderer : public QtLikeSignal::Object
    {
    public:
        //! Constructs the renderer against the window it draws in.
        explicit Renderer
            (
            Window* aWindow   //!< The window. Not owned; the application owns it.
            )
            : mWindow( aWindow )
            , mWindowHandle( static_cast<HWND>( aWindow->nativeHandle() ) )
            // One GetDC for the lifetime of the window, and no matching ReleaseDC. That is what
            // CS_OWNDC buys and what the WGL sequence assumes: the device context is the window's
            // own and stays valid, rather than being borrowed from a shared pool per use. Releasing
            // it would be the mistake, not the cleanup.
            , mDeviceContext( GetDC( static_cast<HWND>( aWindow->nativeHandle() ) ) )
        {
        }

        //! Records a mouse press.
        void onMousePressed
            (
            MouseEvent aEvent   //!< The press.
            )
        {
            ++mPressCount;
            append( TEXT( "press  " ), aEvent );
        }

        //! Records a mouse release.
        void onMouseReleased
            (
            MouseEvent aEvent   //!< The release.
            )
        {
            ++mReleaseCount;
            append( TEXT( "release" ), aEvent );
        }

        //! Records a mouse move.
        //!
        //! Moves arrive in floods, so the log only takes one every few pixels; the counter and the
        //! live position take every single one, which is what makes the crosshair track smoothly.
        void onMouseMoved
            (
            MouseEvent aEvent   //!< The move.
            )
        {
            ++mMoveCount;
            mLast = aEvent;

            const int dx = aEvent.mPos.mX - mLastLoggedX;
            const int dy = aEvent.mPos.mY - mLastLoggedY;
            if( ( dx * dx + dy * dy ) >= ( 40 * 40 ) )
            {
                mLastLoggedX = aEvent.mPos.mX;
                mLastLoggedY = aEvent.mPos.mY;
                append( TEXT( "move   " ), aEvent );
            }
            else
            {
                mWindow->requestUpdate();
            }
        }

        //! Records the pointer entering the client area.
        void onMouseEntered
            (
            MouseEvent aEvent   //!< Where it entered.
            )
        {
            mInside = true;
            append( TEXT( "enter  " ), aEvent );
        }

        //! Records the pointer leaving the client area.
        void onMouseLeft()
        {
            mInside = false;
            mLog.push_back( DemoString( TEXT( "  leave" ) ) );
            trimLog();
            mWindow->requestUpdate();
        }

        //! Records a wheel turn.
        void onWheel
            (
            WheelEvent aEvent   //!< The rotation.
            )
        {
            mWheelTotal += aEvent.mAngleDeltaY;

            DemoStream entry;
            entry << TEXT( "  wheel    " ) << std::setw( 6 ) << aEvent.mAngleDeltaY
                  << TEXT( "  at " ) << std::setw( 4 ) << aEvent.mPos.mX
                  << TEXT( ", " ) << std::setw( 4 ) << aEvent.mPos.mY;
            mLog.push_back( entry.str() );
            trimLog();
            mWindow->requestUpdate();
        }

        //! Records a resize. A real renderer resizes its viewport and buffers here.
        void onResized
            (
            int aWidth,   //!< New client width.
            int aHeight   //!< New client height.
            )
        {
            DemoStream entry;
            entry << TEXT( "  resize   " ) << aWidth << TEXT( " x " ) << aHeight;
            mLog.push_back( entry.str() );
            trimLog();
        }

        //! Counts one second of uptime and repaints.
        //!
        //! Driven by a QtLikeSignal::Timer, not by WM_TIMER. It keeps ticking while the mouse
        //! floods the queue, which is the point: one loop is servicing both.
        void onSecond()
        {
            ++mSeconds;
            mWindow->requestUpdate();
        }

        //! Paints the whole window from a back buffer.
        //! @return the bar's position, which is what the animation in main() drives.
        QtLikeSignal::Property<int>& sweepPosition()
        {
            return mSweepPosition;
        }

        void onExposed()
        {
            const int width  = mWindow->width();
            const int height = mWindow->height();
            if( mDeviceContext == nullptr || width <= 0 || height <= 0 )
            {
                return;
            }

            RECT client { 0, 0, width, height };

            // Drawn into a bitmap and blitted in one go. Mouse moves repaint constantly, and
            // painting straight onto the window flickers badly at that rate.
            const HDC memory        = CreateCompatibleDC( mDeviceContext );
            const HBITMAP bitmap    = CreateCompatibleBitmap( mDeviceContext, width, height );
            const HGDIOBJ oldBitmap = SelectObject( memory, bitmap );

            const HBRUSH background = CreateSolidBrush( RGB( 24, 26, 32 ) );
            FillRect( memory, &client, background );
            DeleteObject( background );

            const HFONT font = CreateFont( 18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, TEXT( "Consolas" ) );
            const HGDIOBJ oldFont = SelectObject( memory, font );
            SetBkMode( memory, TRANSPARENT );

            int y = 16;
            const int lineHeight = 22;

            DemoStream text;
            auto flush = [&]()
                {
                    const DemoString built = text.str();
                    TextOut( memory, 16, y, built.c_str(), static_cast<int>( built.size() ) );
                    y += lineHeight;
                    text.str( DemoString() );
                    text.clear();
                };

            SetTextColor( memory, RGB( 120, 200, 255 ) );
            text << TEXT(
                "QtLikeSignalGui -- window and input through the library, drawing through the "
                "HWND" );
            flush();

            SetTextColor( memory, RGB( 150, 155, 170 ) );
            text << TEXT( "No window class, no window procedure and no message loop in this "
                "program." );
            flush();
            text << TEXT( "Move, click and scroll.  The close box quits." );
            flush();
            y += lineHeight;

            SetTextColor( memory, RGB( 235, 235, 240 ) );
            text << TEXT( "client size   : " ) << width << TEXT( " x " ) << height;
            flush();
            text << TEXT( "cursor        : " ) << std::setw( 4 ) << mLast.mPos.mX << TEXT( ", " )
                 << std::setw( 4 ) << mLast.mPos.mY
                 << TEXT( "   (screen " ) << mLast.mGlobalPos.mX << TEXT( ", " )
                 << mLast.mGlobalPos.mY << TEXT( ")" );
            flush();
            text << TEXT( "buttons held  : " ) << describeButtons();
            flush();
            text << TEXT( "pointer       : " ) << ( mInside ? TEXT( "inside" ) : TEXT( "outside" ) )
            ;
            flush();
            text << TEXT( "presses       : " ) << mPressCount;
            flush();
            text << TEXT( "releases      : " ) << mReleaseCount;
            flush();
            text << TEXT( "moves         : " ) << mMoveCount;
            flush();
            text << TEXT( "wheel total   : " ) << mWheelTotal;
            flush();

            SetTextColor( memory, RGB( 160, 230, 160 ) );
            text << TEXT( "timer uptime  : " ) << mSeconds
                 << TEXT( " s   (QtLikeSignal::Timer, not WM_TIMER -- still ticking)" );
            flush();
            y += lineHeight;

            SetTextColor( memory, RGB( 150, 155, 170 ) );
            text << TEXT( "recent events" );
            flush();

            SetTextColor( memory, RGB( 210, 210, 220 ) );
            for( const DemoString& entry : mLog )
            {
                TextOut( memory, 16, y, entry.c_str(), static_cast<int>( entry.size() ) );
                y += lineHeight;
            }

            // A crosshair at the last known cursor position, so the coordinates are visibly the
            // ones the OS reported rather than something invented.
            const HPEN pen       = CreatePen( PS_SOLID, 1, RGB( 90, 160, 220 ) );
            const HGDIOBJ oldPen = SelectObject( memory, pen );
            MoveToEx( memory, mLast.mPos.mX - 12, mLast.mPos.mY, nullptr );
            LineTo( memory, mLast.mPos.mX + 13, mLast.mPos.mY );
            MoveToEx( memory, mLast.mPos.mX, mLast.mPos.mY - 12, nullptr );
            LineTo( memory, mLast.mPos.mX, mLast.mPos.mY + 13 );
            SelectObject( memory, oldPen );
            DeleteObject( pen );

            // The animated bar. Nothing here knows an animation exists: the renderer reads a
            // value, exactly as it would if a slider had set it.
            const int barWidth = 40;
            const int travel   = width > barWidth ? width - barWidth : 0;
            const int barLeft  = ( mSweepPosition.get() * travel ) / 1000;
            RECT bar { barLeft, 8, barLeft + barWidth, 18 };
            const HBRUSH barBrush = CreateSolidBrush( RGB( 120, 200, 255 ) );
            FillRect( memory, &bar, barBrush );
            DeleteObject( barBrush );

            BitBlt( mDeviceContext, 0, 0, width, height, memory, 0, 0, SRCCOPY );

            SelectObject( memory, oldFont );
            DeleteObject( font );
            SelectObject( memory, oldBitmap );
            DeleteObject( bitmap );
            DeleteDC( memory );
        }

    private:
        //! Renders the held-button set as text.
        DemoString describeButtons() const
        {
            const MouseButtons held = GuiApplication::mouseButtons();
            if( !held.any() )
            {
                return DemoString( TEXT( "none" ) );
            }

            DemoStream text;
            if( held.test( MouseButton::Left ) )
            {
                text << TEXT( "left " );
            }
            if( held.test( MouseButton::Middle ) )
            {
                text << TEXT( "middle " );
            }
            if( held.test( MouseButton::Right ) )
            {
                text << TEXT( "right " );
            }
            if( held.test( MouseButton::Extra1 ) )
            {
                text << TEXT( "extra1 " );
            }
            if( held.test( MouseButton::Extra2 ) )
            {
                text << TEXT( "extra2 " );
            }
            return text.str();
        }

        //! Drops the oldest log lines once there are too many.
        void trimLog()
        {
            while( static_cast<int>( mLog.size() ) > kLogLines )
            {
                mLog.pop_front();
            }
        }

        //! Adds one line describing a mouse event, and repaints.
        void append
            (
            const TCHAR* aKind,       //!< Event kind, already padded to a fixed width.
            const MouseEvent& aEvent  //!< The event to describe.
            )
        {
            const TCHAR* button = TEXT( "none  " );
            switch( aEvent.mButton )
            {
            case MouseButton::Left:   button = TEXT( "left  " ); break;
            case MouseButton::Middle: button = TEXT( "middle" ); break;
            case MouseButton::Right:  button = TEXT( "right " ); break;
            case MouseButton::Extra1: button = TEXT( "extra1" ); break;
            case MouseButton::Extra2: button = TEXT( "extra2" ); break;
            case MouseButton::None:   break;
            }

            DemoStream entry;
            entry << TEXT( "  " ) << aKind << TEXT( "  " ) << button << TEXT( "  at " )
                  << std::setw( 4 ) << aEvent.mPos.mX << TEXT( ", " )
                  << std::setw( 4 ) << aEvent.mPos.mY;

            mLog.push_back( entry.str() );
            trimLog();
            mWindow->requestUpdate();
        }

        Window* mWindow;              //!< The window drawn in. Not owned.
        HWND mWindowHandle;           //!< Its handle, taken once from nativeHandle().
        HDC mDeviceContext;           //!< Its private device context; see the constructor.
        std::deque<DemoString> mLog;  //!< Most recent event descriptions.
        MouseEvent mLast;             //!< Most recent mouse position, for the crosshair.
        bool mInside { false };       //!< Whether the pointer is over the client area.
        int mPressCount { 0 };        //!< Total presses seen.
        int mReleaseCount { 0 };      //!< Total releases seen.
        int mMoveCount { 0 };         //!< Total moves seen.
        int mWheelTotal { 0 };        //!< Accumulated vertical wheel rotation.
        int mSeconds { 0 };           //!< Seconds since start, from the timer.
        //! Where the animated bar is, from 0 to 1000. Driven by a PropertyAnimation in main().
        QtLikeSignal::Property<int> mSweepPosition { 0 };

        int mLastLoggedX { 0 };       //!< Cursor X when a move was last logged.
        int mLastLoggedY { 0 };       //!< Cursor Y when a move was last logged.
    };
}

//! Builds the application, makes a window, wires its signals, and runs the one loop there is.
int main
    (
    int argc,      //!< Argument count.
    char** argv    //!< Argument vector; -p selects a backend on Linux, and is ignored here.
    )
{
    GuiApplication app( argc, argv );

    if( !app.hasPlatform() )
    {
        qCWarning( gLogDemo ) << "no window system available";
        return 1;
    }

    WindowSettings settings;
    settings.mWidth  = 1280;
    settings.mHeight = 720;
    settings.mTitle  = "QtLikeSignalGui demo -- Win32 mouse events";

    Window* const window = app.createWindow( settings );
    if( window == nullptr )
    {
        qCWarning( gLogDemo ) << "failed to create the window";
        return 1;
    }

    // Built before the window is shown, exactly where an external library would initialise WGL
    // against the same handle: everything that needs the window to exist happens here, and nothing
    // is on screen until show() below.
    Renderer renderer( window );

    // Direct connections: the window procedure runs on this thread, inside this thread's dispatch
    // pass, so the slots run there too. Auto would resolve to Direct for the same reason.
    QtLikeSignal::Object::connect( window->getMousePressed(), &renderer, &Renderer::onMousePressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseReleased(), &renderer,
        &Renderer::onMouseReleased, QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseMoved(), &renderer, &Renderer::onMouseMoved,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseEntered(), &renderer, &Renderer::onMouseEntered,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseLeft(), &renderer, &Renderer::onMouseLeft,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getWheel(), &renderer, &Renderer::onWheel,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getResized(), &renderer, &Renderer::onResized,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getExposed(), &renderer, &Renderer::onExposed,
        QtLikeSignal::ConnectionType::Direct );

    // A QtLikeSignal timer, sharing the loop with the OS messages. If OS message pumping ever
    // starved the timers, or the timers starved the messages, this is where it would show.
    QtLikeSignal::Timer second;
    QtLikeSignal::Object::connect( second.getTimeout(), &renderer, &Renderer::onSecond,
        QtLikeSignal::ConnectionType::Direct );
    second.start( 1000 );

    window->show();

    std::printf( "QtLikeSignalGui demo running.\n" );
    std::printf( "  platform           : %s\n", app.platformName() );
    std::printf( "  window client area : %d x %d\n", window->width(), window->height() );
    std::printf( "  device pixels      : %.2f per unit, so %d x %d real pixels\n",
        window->devicePixelRatio(),
        static_cast<int>( window->width() * window->devicePixelRatio() ),
        static_cast<int>( window->height() * window->devicePixelRatio() ) );
    std::printf( "  event loop         : GuiApplication::exec()\n" );
    std::printf( "  animation          : advanced by the internal 16 ms clock\n" );
    std::printf( "  OS message pump    : EventDispatcherWin32::processPlatformEvents()\n" );

    // Flushed, because the loop below can run for hours and stdout is block-buffered the
    // moment it is redirected to a file -- so without this the status above appears only
    // when the program exits, which is exactly when it stops being useful.
    std::fflush( stdout );

    // **The same twelve lines the Wayland demo uses, unchanged.** PropertyAnimation is in
    // QtLikeSignal rather than QtLikeSignalGui and has no platform code in it at all, so a demo
    // showing it needs no per-backend variation -- which is worth demonstrating rather than
    // asserting.
    //
    // The animation runs on the library's own 16 ms clock, and the repaint follows the value
    // rather than the other way round. Advancing it from inside the paint and asking for another
    // frame while it runs is the arrangement that looks right, and it is a loop with nothing
    // throttling it on a window with immediate update delivery -- which is what this one has.
    // AbstractAnimation::tickExternally() is there for a renderer that presents every frame
    // regardless; this program draws only when something changed.
    QtLikeSignal::PropertyAnimation<int> sweepAnimation( renderer.sweepPosition() );
    sweepAnimation.setDuration( 900 );
    sweepAnimation.setEasing( QtLikeSignal::Easing::Cubic_InOut );

    QtLikeSignal::Property<int>& bar = renderer.sweepPosition();
    QtLikeSignal::Object::connect( window->getMousePressed(), &renderer,
        [&sweepAnimation, &bar]( MouseEvent )
        {
            // The target is chosen from where the bar is, not from where it was last sent, so the
            // first click moves it. Only the end is set: leaving the start unset makes the
            // animation read the property at start(), so a click part way through picks the bar up
            // where it is rather than snapping it back.
            sweepAnimation.setEndValue( bar.get() >= 500 ? 0 : 1000 );
            sweepAnimation.start();
        }, QtLikeSignal::ConnectionType::Direct );

    // Each step changes the property, and that is what asks for the repaint. The animation keeps
    // advancing on its own clock whether or not a frame was drawn, so a step that leaves the value
    // where it was simply does not repaint and stalls nothing.
    QtLikeSignal::Object::connect( renderer.sweepPosition().getChanged(), &renderer,
        [window]( int )
        {
            window->requestUpdate();
        }, QtLikeSignal::ConnectionType::Direct );

    const int result = app.exec();

    std::printf( "loop finished, exit code %d\n", result );
    return result;
}
