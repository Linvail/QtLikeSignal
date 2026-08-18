// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A real Windows program on QtLikeSignal: a 1280x800 window whose mouse events arrive through
//! QtLikeSignal's own event dispatcher and are delivered as signals.
//!
//! There is no message loop in this program. CoreApplication::exec() is the only loop, and
//! EventDispatcherWin32::processPlatformEvents() is what pumps the OS messages inside it -- so
//! every number that changes on screen is evidence that the dispatcher is servicing Windows
//! correctly. Stop pumping and the window freezes; spin instead of blocking and the CPU figure in
//! Task Manager goes to a full core.
//!
//! Every Win32 call here is the generic-text one -- TextOut, not TextOutA -- and every toolchain
//! that builds this defines UNICODE, so they resolve to the W entry points and the program is a
//! Unicode one. That is why the strings are TCHAR and the literals wear TEXT(): the character type
//! follows the build's setting instead of being hard-coded against it.
//!
//! Controls: move and click the mouse anywhere in the window; M opens a modal dialog with its own
//! message loop; Escape or the close box quits.

#include "GuiApplication.hpp"
#include "MouseEvent.hpp"

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Timer.hpp"

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

using namespace QtLikeSignalDemo;

namespace
{
    //! The string type the generic-text Win32 calls in this file take.
    using DemoString = std::basic_string<TCHAR>;

    //! A stream that builds one, so no format string or fixed buffer is needed.
    using DemoStream = std::basic_ostringstream<TCHAR>;

    //! How many recent events the on-screen log keeps.
    constexpr int kLogLines = 22;

    //! Records the mouse events it is sent and paints the window.
    //!
    //! An ordinary QtLikeSignal::Object: it knows nothing about Win32 except the device context it
    //! is handed to draw on. Everything it reacts to arrives as a signal.
    class EventLog : public QtLikeSignal::Object
    {
    public:
        //! Constructs the log against the application whose window it draws in.
        explicit EventLog
            (
            GuiApplication* aApplication   //!< Application owning the window. Not owned.
            )
            : mApplication( aApplication )
        {
        }

        //! Records a mouse press and repaints.
        void onMousePressed
            (
            MouseEvent aEvent   //!< The press.
            )
        {
            ++mPressCount;
            append( TEXT( "press  " ), aEvent );
        }

        //! Records a mouse release and repaints.
        void onMouseReleased
            (
            MouseEvent aEvent   //!< The release.
            )
        {
            ++mReleaseCount;
            append( TEXT( "release" ), aEvent );
        }

        //! Records a mouse move and repaints.
        //!
        //! Moves arrive in floods, so the log only takes one every few pixels; the counter and the
        //! live position still take every single one, which is what makes the position track the
        //! cursor smoothly.
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
                append( TEXT( "move   " ), aEvent );
            }
            else
            {
                mApplication->update();
            }
        }

        //! Counts one second of uptime and repaints.
        //!
        //! Driven by a QtLikeSignal::Timer, not by WM_TIMER. It keeps ticking while the mouse
        //! floods the queue, which is the point: one loop is servicing both.
        void onSecond()
        {
            ++mSeconds;
            mApplication->update();
        }

        //! Paints the whole window from a back buffer.
        void onPaint
            (
            void* aDeviceContext   //!< HDC from BeginPaint, valid only during this call.
            )
        {
            const HDC target = static_cast<HDC>( aDeviceContext );
            const HWND window = static_cast<HWND>( mApplication->nativeWindowHandle() );

            RECT client {};
            GetClientRect( window, &client );
            const int width  = client.right - client.left;
            const int height = client.bottom - client.top;

            // Drawn into a bitmap and blitted in one go. Mouse moves repaint constantly, and
            // painting straight onto the window flickers badly at that rate.
            const HDC memory    = CreateCompatibleDC( target );
            const HBITMAP bitmap    = CreateCompatibleBitmap( target, width, height );
            const HGDIOBJ oldBitmap = SelectObject( memory, bitmap );

            const HBRUSH background = CreateSolidBrush( RGB( 24, 26, 32 ) );
            FillRect( memory, &client, background );
            DeleteObject( background );

            const HFONT font = CreateFont( 18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, TEXT( "Consolas" ) );
            const HGDIOBJ oldFont = SelectObject( memory, font );
            SetBkMode( memory, TRANSPARENT );

            int y          = 16;
            const int lineHeight = 22;

            // One stream, emptied after each line, so no line needs a buffer size guessed for it.
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
            text << TEXT( "QtLikeSignal on Win32 -- OS messages through our own dispatcher" );
            flush();

            SetTextColor( memory, RGB( 150, 155, 170 ) );
            text << TEXT( "No GetMessage loop in this program. CoreApplication::exec() is the only "
                "loop." );
            flush();
            text << TEXT( "Move/click the mouse.  M = modal dialog (foreign message loop).  "
                "Esc = quit." );
            flush();
            y += lineHeight;

            SetTextColor( memory, RGB( 235, 235, 240 ) );
            text << TEXT( "client size   : " ) << width << TEXT( " x " ) << height;
            flush();
            text << TEXT( "cursor        : " ) << std::setw( 4 ) << mLastX << TEXT( ", " )
                 << std::setw( 4 ) << mLastY;
            flush();
            text << TEXT( "presses       : " ) << mPressCount;
            flush();
            text << TEXT( "releases      : " ) << mReleaseCount;
            flush();
            text << TEXT( "moves         : " ) << mMoveCount;
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
            for( const auto& entry : mLog )
            {
                TextOut( memory, 16, y, entry.c_str(), static_cast<int>( entry.size() ) );
                y += lineHeight;
            }

            // A crosshair at the last known cursor position, so the coordinates are visibly the
            // ones the OS reported rather than something invented.
            const HPEN pen    = CreatePen( PS_SOLID, 1, RGB( 90, 160, 220 ) );
            const HGDIOBJ oldPen = SelectObject( memory, pen );
            MoveToEx( memory, mLastX - 12, mLastY, nullptr );
            LineTo( memory, mLastX + 13, mLastY );
            MoveToEx( memory, mLastX, mLastY - 12, nullptr );
            LineTo( memory, mLastX, mLastY + 13 );
            SelectObject( memory, oldPen );
            DeleteObject( pen );

            BitBlt( target, 0, 0, width, height, memory, 0, 0, SRCCOPY );

            SelectObject( memory, oldFont );
            DeleteObject( font );
            SelectObject( memory, oldBitmap );
            DeleteObject( bitmap );
            DeleteDC( memory );
        }

    private:
        //! Adds one line to the log, drops the oldest, and repaints.
        void append
            (
            const TCHAR* aKind,       //!< Event kind, already padded to a fixed width.
            const MouseEvent& aEvent  //!< The event to describe.
            )
        {
            const TCHAR* button = TEXT( "none  " );
            switch( aEvent.mButton )
            {
            case MouseEvent::Button::Left:   button = TEXT( "left  " ); break;
            case MouseEvent::Button::Middle: button = TEXT( "middle" ); break;
            case MouseEvent::Button::Right:  button = TEXT( "right " ); break;
            case MouseEvent::Button::None:   break;
            }

            DemoStream entry;
            entry << TEXT( "  " ) << aKind << TEXT( "  " ) << button << TEXT( "  at " )
                  << std::setw( 4 ) << aEvent.mX << TEXT( ", " )
                  << std::setw( 4 ) << aEvent.mY;

            mLog.push_back( entry.str() );
            if( static_cast<int>( mLog.size() ) > kLogLines )
            {
                mLog.pop_front();
            }

            mApplication->update();
        }

        GuiApplication*        mApplication;          //!< Window owner. Not owned.
        std::deque<DemoString> mLog;                  //!< Most recent event descriptions.
        int mPressCount { 0 };                        //!< Total presses seen.
        int mReleaseCount { 0 };                      //!< Total releases seen.
        int mMoveCount { 0 };                         //!< Total moves seen.
        int mSeconds { 0 };                           //!< Seconds since start, from the timer.
        int mLastX { 0 };                             //!< Last reported cursor X.
        int mLastY { 0 };                             //!< Last reported cursor Y.
        int mLastLoggedX { 0 };                       //!< Cursor X when a move was last logged.
        int mLastLoggedY { 0 };                       //!< Cursor Y when a move was last logged.
    };
}

//! Builds the application, wires the signals to the log, and runs the one loop there is.
int main()
{
    GuiApplication app;

    if( !app.createWindow( TEXT( "QtLikeSignal demo -- Win32 mouse events" ), 1280, 800 ) )
    {
        std::fprintf( stderr, "failed to create the window\n" );
        return 1;
    }

    EventLog log( &app );

    // Direct connections: the window procedure runs on this thread, inside this thread's dispatch
    // pass, so the slots run there too. Auto would resolve to Direct for the same reason.
    QtLikeSignal::Object::connect( app.getMousePressed(), &log, &EventLog::onMousePressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getMouseReleased(), &log, &EventLog::onMouseReleased,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getMouseMoved(), &log, &EventLog::onMouseMoved,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( app.getPaintRequested(), &log, &EventLog::onPaint,
        QtLikeSignal::ConnectionType::Direct );

    // A QtLikeSignal timer, sharing the loop with the OS messages. If OS message pumping ever
    // starved the timers, or the timers starved the messages, this is where it would show.
    QtLikeSignal::Timer second;
    QtLikeSignal::Object::connect( second.getTimeout(), &log, &EventLog::onSecond,
        QtLikeSignal::ConnectionType::Direct );
    second.start( 1000 );

    // Narrow, and deliberately so: this is the C runtime writing to a console, not a Win32 text
    // API, so there is no A/W choice to make here.
    std::printf( "QtLikeSignal Win32 demo running.\n" );
    std::printf( "  window client area : 1280x800\n" );
    std::printf( "  event loop         : CoreApplication::exec()\n" );
    std::printf( "  OS message pump    : EventDispatcherWin32::processPlatformEvents()\n" );
    std::printf( "  no GetMessage loop in this program\n" );

    const int result = app.exec();

    std::printf( "loop finished, exit code %d\n", result );
    return result;
}
