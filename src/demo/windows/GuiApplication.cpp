// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GuiApplication implementation: window creation and the window procedure.

#include "GuiApplication.hpp"

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

namespace QtLikeSignalDemo
{
    namespace
    {
        //! Class name of the demo's window.
        //!
        //! Distinct from the dispatcher's own message-only window class. Both live in this process,
        //! and RegisterClass is process-wide, so sharing a name would mean sharing a window
        //! procedure -- and this one is not DefWindowProc.
        //!
        //! TEXT() rather than a bare literal, because every Win32 call in this file is the
        //! generic-text one: RegisterClass, not RegisterClassA. Those resolve to the W entry
        //! points, which want wchar_t, because every toolchain that builds this defines UNICODE --
        //! both the MSVC ones and linux-2-win64-clang. TEXT() is what ties the literal to that
        //! decision rather than to a guess about it, so the setting stays in one place: the
        //! DEFINES in tools/toolchain-windows.py and tools/toolchain-linux.py.
        const TCHAR* const kWindowClassName = TEXT( "QtLikeSignalDemo_MainWindow" );

        //! Maps a button-down or button-up message to the button it refers to.
        MouseEvent::Button buttonOf
            (
            UINT aMessage   //!< The WM_ message id.
            )
        {
            switch( aMessage )
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                return MouseEvent::Button::Left;

            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
                return MouseEvent::Button::Middle;

            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
                return MouseEvent::Button::Right;

            default:
                return MouseEvent::Button::None;
            }
        }
    }

    //! Constructs the application. The window is created separately, by createWindow().
    //!
    //! Split so a failure to create the window is reportable, rather than leaving a constructor
    //! that cannot fail having half-succeeded.
    GuiApplication::GuiApplication()
        : QtLikeSignal::CoreApplication()
    {
    }

    //! Destroys the window, if one was created.
    GuiApplication::~GuiApplication()
    {
        if( mWindow != nullptr )
        {
            DestroyWindow( static_cast<HWND>( mWindow ) );
            mWindow = nullptr;
        }
    }

    //! Registers the window class, creates the window, and shows it.
    //!
    //! @p aClientWidth and @p aClientHeight size the **client area**, not the outer frame: the
    //! requested size is what you can draw on, which is what a caller asking for 1280x800 means.
    //! AdjustWindowRect grows the rectangle by whatever the border and caption need.
    //! @return true if the window exists and is visible.
    bool GuiApplication::createWindow
        (
        const TCHAR* aTitle,    //!< Caption text.
        int aClientWidth,       //!< Desired client area width in pixels.
        int aClientHeight       //!< Desired client area height in pixels.
        )
    {
        const HINSTANCE instance = GetModuleHandle( nullptr );

        WNDCLASS windowClass {};
        windowClass.lpfnWndProc   = []( HWND aWindow, UINT aMessage, WPARAM aWParam, LPARAM aLParam
                                      )
            -> LRESULT
            {
                // The instance is attached in WM_NCCREATE, which is the first message a window
                // receives, so every message after that can find it. Anything arriving before it
                // goes to the default handler.
                if( aMessage == WM_NCCREATE )
                {
                    auto* create = reinterpret_cast<CREATESTRUCT*>( aLParam );
                    SetWindowLongPtr( aWindow, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>( create->lpCreateParams ) );
                    return DefWindowProc( aWindow, aMessage, aWParam, aLParam );
                }

                auto* self = reinterpret_cast<GuiApplication*>(
                    GetWindowLongPtr( aWindow, GWLP_USERDATA ) );
                if( self == nullptr )
                {
                    return DefWindowProc( aWindow, aMessage, aWParam, aLParam );
                }

                switch( aMessage )
                {
                case WM_MOUSEMOVE:
                {
                    MouseEvent event;
                    event.mX      = GET_X_LPARAM( aLParam );
                    event.mY      = GET_Y_LPARAM( aLParam );
                    event.mButton = MouseEvent::Button::None;
                    self->mMouseMoved.emit( event );
                    return 0;
                }

                case WM_LBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_RBUTTONDOWN:
                {
                    MouseEvent event;
                    event.mX      = GET_X_LPARAM( aLParam );
                    event.mY      = GET_Y_LPARAM( aLParam );
                    event.mButton = buttonOf( aMessage );
                    self->mMousePressed.emit( event );
                    return 0;
                }

                case WM_LBUTTONUP:
                case WM_MBUTTONUP:
                case WM_RBUTTONUP:
                {
                    MouseEvent event;
                    event.mX      = GET_X_LPARAM( aLParam );
                    event.mY      = GET_Y_LPARAM( aLParam );
                    event.mButton = buttonOf( aMessage );
                    self->mMouseReleased.emit( event );
                    return 0;
                }

                case WM_KEYDOWN:
                {
                    if( aWParam == 'M' )
                    {
                        // A foreign message loop, on purpose. MessageBox runs its own loop and
                        // drains this thread's queue, which includes any wakeup message the
                        // dispatcher had posted to its own window. Everything must still work
                        // after this returns; see the class comment and R34.
                        MessageBox( aWindow,
                            TEXT( "This dialog is running its own message loop, which is draining "
                            "this thread's message queue.\n\n"
                            "Close it: the mouse events, the uptime timer and the loop itself "
                            "all keep working." ),
                            TEXT( "Foreign message loop" ), MB_OK | MB_ICONINFORMATION );
                        return 0;
                    }
                    if( aWParam == VK_ESCAPE )
                    {
                        QtLikeSignal::CoreApplication::quit();
                        return 0;
                    }
                    return 0;
                }

                case WM_ERASEBKGND:
                {
                    // Claimed, so Windows does not flood-fill the client area before WM_PAINT.
                    // The paint slot covers every pixel from a back buffer instead.
                    return 1;
                }

                case WM_PAINT:
                {
                    PAINTSTRUCT paint {};
                    const HDC deviceContext = BeginPaint( aWindow, &paint );
                    self->mPaintRequested.emit( deviceContext );
                    EndPaint( aWindow, &paint );
                    return 0;
                }

                case WM_CLOSE:
                {
                    // quit(), not PostQuitMessage(). WM_QUIT would interrupt one dispatch pass
                    // and nothing more -- Thread::exec() loops on its own exit flag and never
                    // reads what processEvents() returned, so the program would not stop. That
                    // is the deliberate divergence from Qt recorded as R22, and it is why an
                    // application built on this library ends its loop through quit().
                    QtLikeSignal::CoreApplication::quit();
                    return 0;
                }

                default:
                    break;
                }

                return DefWindowProc( aWindow, aMessage, aWParam, aLParam );
            };
        windowClass.hInstance     = instance;
        windowClass.lpszClassName = kWindowClassName;
        // IDC_ARROW is MAKEINTRESOURCE, which is the A or the W form depending on UNICODE, so it
        // has to be passed to the LoadCursor that resolves the same way. Naming LoadCursorW here
        // was a build break on the cross toolchain back when that one did not define UNICODE and
        // mingw-w64's IDC_ARROW was therefore LPSTR. Both toolchains define it now, so the pair
        // would match either way -- but pairing the generic macro with the generic resource id is
        // what keeps that true without depending on the setting.
        windowClass.hCursor       = LoadCursor( nullptr, IDC_ARROW );
        windowClass.hbrBackground = nullptr;

        if( RegisterClass( &windowClass ) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
        {
            std::fprintf( stderr, "GuiApplication: RegisterClass() failed (%lu)\n",
                GetLastError() );
            return false;
        }

        // Grow the requested client size into the outer window size the style needs.
        const DWORD style = WS_OVERLAPPEDWINDOW;
        RECT frame { 0, 0, aClientWidth, aClientHeight };
        AdjustWindowRect( &frame, style, FALSE );

        const HWND window = CreateWindowEx(
            0,
            kWindowClassName,
            aTitle,
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            frame.right - frame.left,
            frame.bottom - frame.top,
            nullptr,
            nullptr,
            instance,
            this );          // reaches the window procedure as CREATESTRUCT::lpCreateParams

        if( window == nullptr )
        {
            std::fprintf( stderr, "GuiApplication: CreateWindowEx() failed (%lu)\n",
                GetLastError() );
            return false;
        }

        mWindow = window;
        ShowWindow( window, SW_SHOW );
        UpdateWindow( window );
        return true;
    }

    //! Asks for a repaint. The WM_PAINT arrives through the loop, like every other message.
    //!
    //! Named as Qt names it, and doing what Qt's does: mark the client area dirty and return
    //! immediately, rather than painting here.
    void GuiApplication::update()
    {
        if( mWindow != nullptr )
        {
            InvalidateRect( static_cast<HWND>( mWindow ), nullptr, FALSE );
        }
    }

    //! Gets the window handle as a void*, for a slot that needs to call Win32 on it.
    void* GuiApplication::nativeWindowHandle() const
    {
        return mWindow;
    }

    //! Gets the mouse-press signal.
    QtLikeSignal::SignalView<MouseEvent>& GuiApplication::getMousePressed() const
    {
        return mMousePressed.view();
    }

    //! Gets the mouse-release signal.
    QtLikeSignal::SignalView<MouseEvent>& GuiApplication::getMouseReleased() const
    {
        return mMouseReleased.view();
    }

    //! Gets the mouse-move signal.
    QtLikeSignal::SignalView<MouseEvent>& GuiApplication::getMouseMoved() const
    {
        return mMouseMoved.view();
    }

    //! Gets the paint signal, which carries the HDC to draw on.
    QtLikeSignal::SignalView<void*>& GuiApplication::getPaintRequested() const
    {
        return mPaintRequested.view();
    }
}
