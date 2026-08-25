// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The Win32 backend: window class, window creation, and the window procedure that translates
//! mouse messages into WindowSystemInterface calls.

#include "QtLikeSignalGui/PlatformIntegrationWin32.hpp"

#include "QtLikeSignalGui/Window.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

// WM_MOUSEHWHEEL -- the horizontal wheel -- arrived in Windows Vista, and the two toolchains that
// build this file disagree about whether that matters. The MSVC SDK declares it unconditionally;
// mingw-w64, which the linux-2-win64-clang cross-compile uses, hides it behind _WIN32_WINNT >= 0x0600
// and so failed to compile this where MSVC did not.
//
// Defined here rather than by raising _WIN32_WINNT for the whole project. That would change which
// APIs every other translation unit can see, and silently allow calls that then fail to run on an
// older Windows -- a large and invisible change to make for one message id. The value is fixed by
// the ABI and cannot drift.
#ifndef WM_MOUSEHWHEEL
    #define WM_MOUSEHWHEEL 0x020E
#endif

namespace QtLikeSignalGui
{
    namespace
    {
        //! The string type the generic-text Win32 calls in this file take.
        using NativeString = std::basic_string<TCHAR>;

        //! Class name of every window this backend creates.
        //!
        //! Distinct from EventDispatcherWin32's own message-only window class. Both live in this
        //! process, and RegisterClass is process-wide, so sharing a name would mean sharing a window
        //! procedure -- and neither of these two is DefWindowProc.
        //!
        //! TEXT() rather than a bare literal, because every Win32 call in this file is the
        //! generic-text one. Those resolve to the W entry points, which want wchar_t, because every
        //! toolchain that builds this defines UNICODE -- both the MSVC ones and
        //! linux-2-win64-clang. TEXT() ties the literal to that decision rather than to a guess
        //! about it, so the setting stays in one place: the DEFINES in tools/toolchain-windows.py
        //! and tools/toolchain-linux.py.
        const TCHAR* const kWindowClassName = TEXT( "QtLikeSignalGui_Window" );

        //! Index of the per-window extra slot holding the mouse-tracking flag.
        //!
        //! A window's own storage, requested through WNDCLASS::cbWndExtra, rather than a table in
        //! the backend keyed by HWND. The flag is per window and is read on every mouse move, so
        //! keeping it where the window already is costs one load instead of a lookup, and it cannot
        //! go stale relative to the window it describes.
        const int kTrackingFlagSlot = 0;

        //! Converts a UTF-8 string into whatever the generic-text Win32 entry points want.
        //!
        //! The two branches are the same function for two settings of UNICODE, not a portability
        //! hedge: WindowSettings carries UTF-8 because that is what a cross-platform caller can
        //! reasonably be asked for, and exactly one conversion is needed to get from there to the
        //! API this file calls.
        NativeString toNativeString
            (
            const std::string& aUtf8   //!< Text in UTF-8.
            )
        {
            #if defined( UNICODE ) || defined( _UNICODE )
                if( aUtf8.empty() )
                {
                    return NativeString();
                }

                const int length = static_cast<int>( aUtf8.size() );
                const int needed = MultiByteToWideChar( CP_UTF8, 0, aUtf8.c_str(), length,
                    nullptr, 0 );
                if( needed <= 0 )
                {
                    return NativeString();
                }

                NativeString converted( static_cast<std::size_t>( needed ), TEXT( '\0' ) );
                MultiByteToWideChar( CP_UTF8, 0, aUtf8.c_str(), length, &converted[0], needed );
                return converted;
            #else
                return NativeString( aUtf8.begin(), aUtf8.end() );
            #endif
        }

        //! Translates a Win32 virtual-key code into a portable Key.
        //!
        //! **The letters and digits need no table.** Win32 gives A..Z and 0..9 their ASCII values
        //! as virtual-key codes, and Key uses the same numbering, so those two ranges map by
        //! identity. Only the keys with no ASCII meaning need naming, which is the switch below.
        //!
        //! @return the portable key, or Key::Unknown for anything not named here.
        Key keyFromVirtualKey
            (
            WPARAM aVirtualKey   //!< The VK_ code from a key message's wParam.
            )
        {
            const unsigned int code = static_cast<unsigned int>( aVirtualKey );

            const bool isLetter = ( code >= 0x41 && code <= 0x5a );
            const bool isDigit  = ( code >= 0x30 && code <= 0x39 );
            if( isLetter || isDigit )
            {
                return static_cast<Key>( code );
            }

            switch( code )
            {
            case VK_SPACE:      return Key::Space;
            case VK_OEM_7:      return Key::Apostrophe;
            case VK_OEM_COMMA:  return Key::Comma;
            case VK_OEM_MINUS:  return Key::Minus;
            case VK_OEM_PERIOD: return Key::Period;
            case VK_OEM_2:      return Key::Slash;
            case VK_OEM_1:      return Key::Semicolon;
            case VK_OEM_PLUS:   return Key::Equal;
            case VK_OEM_4:      return Key::BracketLeft;
            case VK_OEM_5:      return Key::Backslash;
            case VK_OEM_6:      return Key::BracketRight;
            case VK_OEM_3:      return Key::Grave;

            case VK_ESCAPE:   return Key::Escape;
            case VK_TAB:      return Key::Tab;
            case VK_BACK:     return Key::Backspace;
            case VK_RETURN:   return Key::Return;
            case VK_INSERT:   return Key::Insert;
            case VK_DELETE:   return Key::Delete;
            case VK_PAUSE:    return Key::Pause;
            case VK_SNAPSHOT: return Key::Print;

            case VK_HOME:  return Key::Home;
            case VK_END:   return Key::End;
            case VK_LEFT:  return Key::Left;
            case VK_UP:    return Key::Up;
            case VK_RIGHT: return Key::Right;
            case VK_DOWN:  return Key::Down;
            case VK_PRIOR: return Key::PageUp;
            case VK_NEXT:  return Key::PageDown;

            case VK_SHIFT:
            case VK_LSHIFT:
            case VK_RSHIFT:   return Key::Shift;
            case VK_CONTROL:
            case VK_LCONTROL:
            case VK_RCONTROL: return Key::Control;
            case VK_MENU:
            case VK_LMENU:
            case VK_RMENU:    return Key::Alt;
            case VK_LWIN:
            case VK_RWIN:     return Key::Meta;
            case VK_CAPITAL:  return Key::CapsLock;
            case VK_NUMLOCK:  return Key::NumLock;
            case VK_SCROLL:   return Key::ScrollLock;
            case VK_APPS:     return Key::Menu;

            case VK_F1:  return Key::F1;
            case VK_F2:  return Key::F2;
            case VK_F3:  return Key::F3;
            case VK_F4:  return Key::F4;
            case VK_F5:  return Key::F5;
            case VK_F6:  return Key::F6;
            case VK_F7:  return Key::F7;
            case VK_F8:  return Key::F8;
            case VK_F9:  return Key::F9;
            case VK_F10: return Key::F10;
            case VK_F11: return Key::F11;
            case VK_F12: return Key::F12;

            default: break;
            }

            return Key::Unknown;
        }

        //! Reads the modifiers currently held, as Win32 reports them.
        //!
        //! GetKeyState() rather than GetAsyncKeyState(): the former answers for the message being
        //! processed, the latter for this instant. In a queue that has fallen behind, the second
        //! would report modifiers the user pressed *after* the key being translated.
        KeyModifiers currentModifiers()
        {
            KeyModifiers modifiers;

            if( ( GetKeyState( VK_SHIFT ) & 0x8000 ) != 0 )
            {
                modifiers |= KeyModifier::Shift;
            }
            if( ( GetKeyState( VK_CONTROL ) & 0x8000 ) != 0 )
            {
                modifiers |= KeyModifier::Control;
            }
            if( ( GetKeyState( VK_MENU ) & 0x8000 ) != 0 )
            {
                modifiers |= KeyModifier::Alt;
            }
            if( ( ( GetKeyState( VK_LWIN ) | GetKeyState( VK_RWIN ) ) & 0x8000 ) != 0 )
            {
                modifiers |= KeyModifier::Meta;
            }

            // The lock keys report their latched state in the low bit rather than the high one:
            // they are a mode, not something being held.
            if( ( GetKeyState( VK_CAPITAL ) & 0x0001 ) != 0 )
            {
                modifiers |= KeyModifier::CapsLock;
            }
            if( ( GetKeyState( VK_NUMLOCK ) & 0x0001 ) != 0 )
            {
                modifiers |= KeyModifier::NumLock;
            }

            return modifiers;
        }

        //! Builds a KeyEvent from a WM_KEYDOWN/WM_KEYUP and its two SYS relatives.
        //!
        //! **The text is produced here rather than from WM_CHAR.** Letting TranslateMessage post a
        //! WM_CHAR would deliver the character in a *later* message, so a slot would see the key
        //! and its text as two unrelated events and have to pair them up itself. ToUnicode() asks
        //! the layout the same question directly and answers it inside this one event.
        KeyEvent keyEventFromMessage
            (
            UINT aMessage,   //!< WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN or WM_SYSKEYUP.
            WPARAM aWParam,  //!< The virtual-key code.
            LPARAM aLParam   //!< Repeat count, scan code and flags.
            )
        {
            KeyEvent event;

            event.mKey         = keyFromVirtualKey( aWParam );
            event.mModifiers   = currentModifiers();
            event.mNativeCode  = static_cast<unsigned int>( aWParam );
            event.mTimestampMs = static_cast<unsigned long>( GetMessageTime() );

            const bool isDown = ( aMessage == WM_KEYDOWN || aMessage == WM_SYSKEYDOWN );

            // Bit 30 of lParam is the previous key state: set means the key was already down, which
            // is exactly what an auto-repeat is. Meaningless on a release, hence the guard.
            event.mAutoRepeat = isDown && ( ( aLParam & ( 1 << 30 ) ) != 0 );

            // Bit 24 is the extended-key flag, and the keypad's Enter is the one key this library
            // names separately from the Return beside it.
            const bool extended = ( aLParam & ( 1 << 24 ) ) != 0;
            if( extended && aWParam == VK_RETURN )
            {
                event.mKey = Key::Enter;
                event.mModifiers |= KeyModifier::Keypad;
            }

            if( isDown )
            {
                BYTE keyboardState[256] {};
                if( GetKeyboardState( keyboardState ) != FALSE )
                {
                    const UINT scanCode = static_cast<UINT>( ( aLParam >> 16 ) & 0xff );

                    wchar_t wide[8] {};

                    // The final flag leaves the keyboard state untouched, so a dead key is not
                    // consumed by this call and still composes with whatever is pressed next.
                    const int written = ToUnicode( static_cast<UINT>( aWParam ), scanCode,
                        keyboardState, wide, 4, 1 << 2 );

                    // Control characters are what a Control chord and the navigation keys produce,
                    // and they are not text anybody wants inserted into a field.
                    const bool isControlCharacter = ( written == 1 && wide[0] < 0x20 );

                    if( written > 0 && !isControlCharacter )
                    {
                        WideCharToMultiByte( CP_UTF8, 0, wide, written, event.mText,
                            static_cast<int>( sizeof( event.mText ) ) - 1, nullptr, nullptr );
                    }
                }
            }

            return event;
        }

        //! Translates the MK_ key-state bits carried by a mouse message into a button set.
        MouseButtons buttonsFromKeyState
            (
            WPARAM aKeyState   //!< The key state, as GET_KEYSTATE_WPARAM() yields it.
            )
        {
            MouseButtons buttons;

            if( ( aKeyState & MK_LBUTTON ) != 0 )
            {
                buttons |= MouseButton::Left;
            }
            if( ( aKeyState & MK_MBUTTON ) != 0 )
            {
                buttons |= MouseButton::Middle;
            }
            if( ( aKeyState & MK_RBUTTON ) != 0 )
            {
                buttons |= MouseButton::Right;
            }
            if( ( aKeyState & MK_XBUTTON1 ) != 0 )
            {
                buttons |= MouseButton::Extra1;
            }
            if( ( aKeyState & MK_XBUTTON2 ) != 0 )
            {
                buttons |= MouseButton::Extra2;
            }

            return buttons;
        }

        //! Builds a MouseEvent from a mouse message whose lParam holds client coordinates.
        //!
        //! That is every mouse message except the two wheel ones, which report screen coordinates
        //! instead and are assembled separately.
        MouseEvent mouseEventFromMessage
            (
            HWND aWindowHandle,    //!< Window the message arrived for.
            WPARAM aWParam,        //!< The message's wParam; its low word is the key state.
            LPARAM aLParam,        //!< The message's lParam: x and y in client coordinates.
            MouseButton aChanged   //!< The button that changed, or None for a move.
            )
        {
            MouseEvent event;
            event.mPos.mX = GET_X_LPARAM( aLParam );
            event.mPos.mY = GET_Y_LPARAM( aLParam );

            POINT screen { event.mPos.mX, event.mPos.mY };
            ClientToScreen( aWindowHandle, &screen );
            event.mGlobalPos.mX = screen.x;
            event.mGlobalPos.mY = screen.y;

            event.mButton      = aChanged;
            event.mButtons     = buttonsFromKeyState( GET_KEYSTATE_WPARAM( aWParam ) );
            event.mTimestampMs = static_cast<unsigned long>( GetMessageTime() );
            return event;
        }

        //! Handles a button going down: takes the mouse, then reports the press.
        //!
        //! The capture is what makes a drag work. Without it the matching release is delivered to
        //! whatever window the pointer happens to be over when the button comes up, so a drag that
        //! leaves the window never ends as far as this one is concerned -- the button stays stuck
        //! down in mouseButtons() and in every later event's mButtons. Qt's Windows plugin takes
        //! the capture for the same reason.
        void handleButtonDown
            (
            HWND aWindowHandle,   //!< Window the press landed on.
            Window* aWindow,      //!< Its Window.
            MouseButton aButton,  //!< The button that went down.
            WPARAM aWParam,       //!< The message's wParam.
            LPARAM aLParam        //!< The message's lParam.
            )
        {
            if( GetCapture() != aWindowHandle )
            {
                SetCapture( aWindowHandle );
            }

            WindowSystemInterface::handleMousePressed( aWindow,
                mouseEventFromMessage( aWindowHandle, aWParam, aLParam, aButton ) );
        }

        //! Handles a button coming up: releases the mouse once nothing is held, then reports it.
        void handleButtonUp
            (
            HWND aWindowHandle,   //!< Window that holds the capture.
            Window* aWindow,      //!< Its Window.
            MouseButton aButton,  //!< The button that came up.
            WPARAM aWParam,       //!< The message's wParam.
            LPARAM aLParam        //!< The message's lParam.
            )
        {
            const MouseEvent event = mouseEventFromMessage( aWindowHandle, aWParam, aLParam,
                aButton );

            // The released button is already absent from the key state on an up message, so an
            // empty set here means this was the last one held. Released before the signal, so a slot
            // that opens a dialog or a menu is not fighting a capture this window still owns.
            if( !event.mButtons.any() && GetCapture() == aWindowHandle )
            {
                ReleaseCapture();
            }

            WindowSystemInterface::handleMouseReleased( aWindow, event );
        }

        //! The window procedure shared by every window this backend creates.
        //!
        //! Reached from EventDispatcherWin32::processPlatformEvents(), by way of DispatchMessage, so
        //! everything it emits is emitted inside a dispatch pass of this library's own loop. That is
        //! what makes the synchronous delivery WindowSystemInterface documents correct here.
        LRESULT CALLBACK windowProc
            (
            HWND aWindowHandle,   //!< The window the message is for.
            UINT aMessage,        //!< The WM_ message id.
            WPARAM aWParam,       //!< Message-specific.
            LPARAM aLParam        //!< Message-specific.
            )
        {
            // Attached by createWindow() after the Window exists, so the messages Windows sends
            // during CreateWindowEx itself -- WM_NCCREATE, WM_CREATE, the first WM_SIZE -- find
            // nothing here and go to the default handler. There is no Window to report them to yet,
            // and the window is not visible until show(), so nothing is lost.
            Window* const self = reinterpret_cast<Window*>(
                GetWindowLongPtr( aWindowHandle, GWLP_USERDATA ) );

            if( self == nullptr )
            {
                return DefWindowProc( aWindowHandle, aMessage, aWParam, aLParam );
            }

            switch( aMessage )
            {
            case WM_MOUSEMOVE:
            {
                const MouseEvent event = mouseEventFromMessage( aWindowHandle, aWParam, aLParam,
                    MouseButton::None );

                // Windows has no enter message. The way to learn that the pointer arrived is to
                // notice the first move since the last leave, and the way to be told about the
                // leave is to ask for it -- TrackMouseEvent arms exactly one WM_MOUSELEAVE and then
                // disarms itself, so it has to be re-armed after each one.
                if( GetWindowLongPtr( aWindowHandle, kTrackingFlagSlot ) == 0 )
                {
                    TRACKMOUSEEVENT tracking {};
                    tracking.cbSize    = sizeof( tracking );
                    tracking.dwFlags   = TME_LEAVE;
                    tracking.hwndTrack = aWindowHandle;

                    if( TrackMouseEvent( &tracking ) != FALSE )
                    {
                        SetWindowLongPtr( aWindowHandle, kTrackingFlagSlot, 1 );
                    }

                    WindowSystemInterface::handleMouseEntered( self, event );
                }

                WindowSystemInterface::handleMouseMoved( self, event );
                return 0;
            }

            case WM_MOUSELEAVE:
            {
                SetWindowLongPtr( aWindowHandle, kTrackingFlagSlot, 0 );
                WindowSystemInterface::handleMouseLeft( self );
                return 0;
            }

            case WM_LBUTTONDOWN:
                handleButtonDown( aWindowHandle, self, MouseButton::Left, aWParam, aLParam );
                return 0;

            case WM_MBUTTONDOWN:
                handleButtonDown( aWindowHandle, self, MouseButton::Middle, aWParam, aLParam );
                return 0;

            case WM_RBUTTONDOWN:
                handleButtonDown( aWindowHandle, self, MouseButton::Right, aWParam, aLParam );
                return 0;

            case WM_LBUTTONUP:
                handleButtonUp( aWindowHandle, self, MouseButton::Left, aWParam, aLParam );
                return 0;

            case WM_MBUTTONUP:
                handleButtonUp( aWindowHandle, self, MouseButton::Middle, aWParam, aLParam );
                return 0;

            case WM_RBUTTONUP:
                handleButtonUp( aWindowHandle, self, MouseButton::Right, aWParam, aLParam );
                return 0;

            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            {
                // The side buttons pack two things into wParam: the key state in the low word, like
                // every other mouse message, and *which* side button in the high word. And unlike
                // every other mouse message, these two must return TRUE rather than 0.
                const MouseButton button =
                    ( GET_XBUTTON_WPARAM( aWParam ) == XBUTTON1 ) ? MouseButton::Extra1
                                                                  : MouseButton::Extra2;

                if( aMessage == WM_XBUTTONDOWN )
                {
                    handleButtonDown( aWindowHandle, self, button, aWParam, aLParam );
                }
                else
                {
                    handleButtonUp( aWindowHandle, self, button, aWParam, aLParam );
                }

                return TRUE;
            }

            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            {
                WheelEvent event;

                // The wheel messages are the odd ones out: their lParam is in *screen* coordinates,
                // not client ones, because the wheel is delivered to the focused window rather than
                // to the one under the pointer. Reading them as client coordinates is the classic
                // way a scroll ends up hit-testing against the wrong widget.
                POINT position { GET_X_LPARAM( aLParam ), GET_Y_LPARAM( aLParam ) };
                event.mGlobalPos.mX = position.x;
                event.mGlobalPos.mY = position.y;

                ScreenToClient( aWindowHandle, &position );
                event.mPos.mX = position.x;
                event.mPos.mY = position.y;

                const int delta = GET_WHEEL_DELTA_WPARAM( aWParam );
                if( aMessage == WM_MOUSEWHEEL )
                {
                    event.mAngleDeltaY = delta;
                }
                else
                {
                    event.mAngleDeltaX = delta;
                }

                event.mButtons     = buttonsFromKeyState( GET_KEYSTATE_WPARAM( aWParam ) );
                event.mTimestampMs = static_cast<unsigned long>( GetMessageTime() );

                WindowSystemInterface::handleWheel( self, event );
                return 0;
            }

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            {
                WindowSystemInterface::handleKeyPressed( self,
                    keyEventFromMessage( aMessage, aWParam, aLParam ) );

                // WM_SYSKEYDOWN is reported and then passed on, not consumed: it carries the Alt
                // chords and F10 that open a menu, and swallowing it would break the menu bar this
                // backend goes to some trouble to support. Consuming the plain case is what stops
                // TranslateMessage posting a WM_CHAR for text this event already carries.
                if( aMessage == WM_SYSKEYDOWN )
                {
                    break;
                }
                return 0;
            }

            case WM_KEYUP:
            case WM_SYSKEYUP:
            {
                WindowSystemInterface::handleKeyReleased( self,
                    keyEventFromMessage( aMessage, aWParam, aLParam ) );

                if( aMessage == WM_SYSKEYUP )
                {
                    break;
                }
                return 0;
            }

            case WM_SIZE:
            {
                const int width  = static_cast<int>( LOWORD( aLParam ) );
                const int height = static_cast<int>( HIWORD( aLParam ) );

                // Minimising reports 0x0. Passing that on would have every renderer connected to
                // the resize signal build a zero-sized viewport or framebuffer, which is invalid in
                // OpenGL and is a real crash in more than one driver. The window still has its
                // previous size as far as this library reports, and the restore reports the real one
                // again, so nothing is missed by staying quiet here.
                if( width > 0 && height > 0 )
                {
                    WindowSystemInterface::handleResize( self, width, height );
                }

                return 0;
            }

            case WM_ERASEBKGND:
            {
                // Claimed, so Windows does not flood-fill the client area before the frame is drawn.
                // Whatever renders into this window covers every pixel itself; without this there
                // is a visible flash of the background brush on every resize.
                return 1;
            }

            case WM_PAINT:
            {
                // Validated *before* the signal, not after. WM_PAINT is synthesised for as long as
                // any part of the window is invalid, so leaving the region dirty here would spin the
                // loop at full speed. Doing it first also means a slot calling requestUpdate() --
                // which is how continuous rendering is driven -- re-invalidates the window and gets
                // the next frame, instead of having its request wiped by a validation that came
                // afterwards.
                ValidateRect( aWindowHandle, nullptr );
                WindowSystemInterface::handleExpose( self );
                return 0;
            }

            case WM_CLOSE:
            {
                // Not DestroyWindow(), and not PostQuitMessage(). The close box is a request, and
                // what happens next belongs to the application: the Window object owns the native
                // window's lifetime, and WM_QUIT would interrupt one dispatch pass and nothing more
                // -- Thread::exec() loops on its own exit flag and never reads what processEvents()
                // returned. An application built on this library ends its loop through quit(),
                // which is what GuiApplication does when its close policy calls for it.
                WindowSystemInterface::handleCloseRequest( self );
                return 0;
            }

            case WM_SETFOCUS:
                WindowSystemInterface::handleFocusChange( self, true );
                return 0;

            case WM_KILLFOCUS:
                WindowSystemInterface::handleFocusChange( self, false );
                return 0;

            default:
                break;
            }

            return DefWindowProc( aWindowHandle, aMessage, aWParam, aLParam );
        }
    }

    //! Constructs the backend. The window class is registered lazily, by the first createWindow().
    PlatformIntegrationWin32::PlatformIntegrationWin32()
    {
    }

    //! Destroys the backend, unregistering the window class it registered.
    //!
    //! Every window is gone by now: GuiApplication destroys its windows before releasing the
    //! backend, precisely so that this order holds. UnregisterClass would fail if one were left, and
    //! its result is ignored rather than reported -- a class that outlives the process by a few
    //! microseconds harms nothing, and there is no caller left to tell.
    PlatformIntegrationWin32::~PlatformIntegrationWin32()
    {
        if( mClassRegistered )
        {
            UnregisterClass( kWindowClassName, GetModuleHandle( nullptr ) );
        }
    }

    //! Gets which window system this backend is.
    PlatformType PlatformIntegrationWin32::type() const
    {
        return PlatformType::Windows;
    }

    //! Windows creates its own windows rather than adopting one.
    bool PlatformIntegrationWin32::canCreateWindows() const
    {
        return true;
    }

    //! There is no adoption path on Win32.
    //!
    //! Not an oversight and not hard to add -- a foreign HWND can be subclassed with
    //! SetWindowLongPtr( GWLP_WNDPROC ) -- but nothing needs it. The external library that creates
    //! windows on X11 and Wayland does not create them here: QtLikeSignalGui does, which is what makes
    //! the Win32 backend the one that owns its window procedure outright.
    bool PlatformIntegrationWin32::canAdoptWindows() const
    {
        return false;
    }

    //! Registers the window class, once.
    //!
    //! ERROR_CLASS_ALREADY_EXISTS is treated as success: the class is process-wide, and a second
    //! GuiApplication in the same process -- which the library warns about but does not prevent --
    //! would otherwise fail here for a reason that is not a failure.
    //!
    //! @return true if the class is registered and usable.
    bool PlatformIntegrationWin32::ensureWindowClass()
    {
        if( mClassRegistered )
        {
            return true;
        }

        WNDCLASS windowClass {};

        // CS_OWNDC is not decoration: it gives the window one private device context that stays
        // valid for its lifetime, which is what the ordinary WGL sequence -- GetDC once,
        // SetPixelFormat, wglCreateContext, wglMakeCurrent -- assumes. Without it the DC is drawn
        // from a shared pool and released back on every use, and the pixel format set on one is not
        // the format the next one has. CS_HREDRAW | CS_VREDRAW invalidate the whole client area on a
        // resize, so a renderer is asked for a full frame at the new size rather than for the
        // uncovered strip.
        windowClass.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc   = &windowProc;

        // One pointer-sized slot per window, for the mouse-tracking flag. See kTrackingFlagSlot.
        windowClass.cbWndExtra    = sizeof( LONG_PTR );
        windowClass.hInstance     = GetModuleHandle( nullptr );

        // IDC_ARROW is MAKEINTRESOURCE, which is the A or the W form depending on UNICODE, so it
        // has to be passed to the LoadCursor that resolves the same way. Naming LoadCursorW here
        // was a build break on the cross toolchain back when that one did not define UNICODE and
        // mingw-w64's IDC_ARROW was therefore LPSTR. Pairing the generic macro with the generic
        // resource id is what keeps the two in step without depending on the setting.
        windowClass.hCursor       = LoadCursor( nullptr, IDC_ARROW );

        // Null, so there is nothing for Windows to paint the client area with. WM_ERASEBKGND is
        // claimed as well; either alone still leaves a flash on some paths.
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClassName;

        if( RegisterClass( &windowClass ) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
        {
            std::fprintf( stderr, "QtLikeSignalGui: RegisterClass() failed (%lu)\n", GetLastError() );
            return false;
        }

        mClassRegistered = true;
        return true;
    }

    //! Creates a window and the Window that fronts it. The window starts hidden; call show().
    //!
    //! @return the new Window, or nullptr if the window could not be created.
    Window* PlatformIntegrationWin32::createWindow
        (
        const WindowSettings& aSettings   //!< Requested client size and caption.
        )
    {
        if( !ensureWindowClass() )
        {
            return nullptr;
        }

        // WS_CLIPCHILDREN | WS_CLIPSIBLINGS are required, not preferred: SetPixelFormat is
        // documented to fail on a window without them, so leaving them off would break the WGL
        // initialisation this window exists to enable, with an error raised somewhere else entirely.
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

        // The requested size is the client area -- what can be drawn on -- which is what a caller
        // asking for 1280x800 means. AdjustWindowRect grows it by whatever the border and caption
        // need to arrive at the outer size CreateWindowEx wants.
        //
        // FALSE for the menu argument, and correctly so: this window is created without one, and
        // reserving space for a menu that is not there would leave the client area too large. A menu
        // attached afterwards is setMenu()'s problem, and it solves it by measuring rather than by
        // asking AdjustWindowRect a question it cannot answer for a bar that has wrapped.
        RECT frame { 0, 0, aSettings.mWidth, aSettings.mHeight };
        AdjustWindowRect( &frame, style, FALSE );

        const NativeString title = toNativeString( aSettings.mTitle );

        const HWND handle = CreateWindowEx(
            0,
            kWindowClassName,
            title.c_str(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            frame.right - frame.left,
            frame.bottom - frame.top,
            nullptr,
            nullptr,
            GetModuleHandle( nullptr ),
            nullptr );

        if( handle == nullptr )
        {
            std::fprintf( stderr, "QtLikeSignalGui: CreateWindowEx() failed (%lu)\n", GetLastError() );
            return nullptr;
        }

        // Asked rather than assumed. AdjustWindowRect works from the style alone and knows nothing
        // about a DPI-scaled frame or a window the shell clamped to fit the screen, so the client
        // area that actually exists is not always the one that was requested -- and a renderer sized
        // from the request rather than from the window would be wrong from its first frame.
        RECT client {};
        GetClientRect( handle, &client );

        NativeWindow native;
        native.mSurface = handle;

        Window* const window = newWindow( this, native,
            static_cast<int>( client.right - client.left ),
            static_cast<int>( client.bottom - client.top ) );

        // Attached last, so the window procedure sees nothing until there is a Window to report to.
        SetWindowLongPtr( handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( window ) );

        return window;
    }

    //! Destroys the native window behind @p aWindow. This backend created it, so it destroys it.
    //!
    //! The procedure is detached first, and the Window's native fields cleared, before DestroyWindow
    //! is called. DestroyWindow sends WM_DESTROY and WM_NCDESTROY synchronously, and this runs from
    //! ~Window() -- so without the detach those messages would reach a window procedure holding a
    //! pointer to an object already being destroyed, and emit signals from it.
    void PlatformIntegrationWin32::releaseNativeWindow
        (
        Window* aWindow   //!< The window being destroyed.
        )
    {
        const HWND handle = static_cast<HWND>( aWindow->nativeHandle() );
        if( handle == nullptr )
        {
            return;
        }

        SetWindowLongPtr( handle, GWLP_USERDATA, 0 );
        assignNative( aWindow, NativeWindow() );
        DestroyWindow( handle );
    }

    //! Sets the window caption.
    void PlatformIntegrationWin32::setWindowTitle
        (
        Window* aWindow,           //!< Window to retitle.
        const std::string& aTitle  //!< New caption, in UTF-8.
        )
    {
        const HWND handle = static_cast<HWND>( aWindow->nativeHandle() );
        if( handle == nullptr )
        {
            return;
        }

        SetWindowText( handle, toNativeString( aTitle ).c_str() );
    }

    //! Shows or hides the window.
    //!
    //! No UpdateWindow() after showing, deliberately: it would force a WM_PAINT to be handled right
    //! here, and the expose signal would then be emitted from inside show() rather than from inside
    //! a dispatch pass. The paint arrives one pass later through the loop, like every other event.
    void PlatformIntegrationWin32::setWindowVisible
        (
        Window* aWindow,   //!< Window to show or hide.
        bool aVisible      //!< True to show it.
        )
    {
        const HWND handle = static_cast<HWND>( aWindow->nativeHandle() );
        if( handle == nullptr )
        {
            return;
        }

        ShowWindow( handle, aVisible ? SW_SHOW : SW_HIDE );
        assignVisible( aWindow, aVisible );
    }

    //! Resizes the window so its client area is exactly @p aWidth by @p aHeight.
    //!
    //! Measured, not calculated. AdjustWindowRect() works from the style alone, and there are three
    //! things it therefore gets wrong here: it is documented not to handle a menu bar that has
    //! wrapped onto more than one line, it knows nothing about a frame scaled by the display's DPI,
    //! and it cannot see a window the shell has clamped to fit the screen. Subtracting the live
    //! client rectangle from the live window rectangle gets the real chrome in every one of those
    //! cases, because it asks the window rather than the style.
    //!
    //! Looped, because the measurement can invalidate itself: changing the width may let a menu bar
    //! that needed two lines fit on one, or force the reverse, which changes the chrome height that
    //! was just measured. Each pass re-measures and stops as soon as the client area is right, so
    //! the ordinary no-menu case costs exactly one SetWindowPos and one confirming measurement.
    void PlatformIntegrationWin32::setClientSize
        (
        Window* aWindow,   //!< Window to resize.
        int aWidth,        //!< Desired client-area width in pixels.
        int aHeight        //!< Desired client-area height in pixels.
        )
    {
        const HWND handle = static_cast<HWND>( aWindow->nativeHandle() );
        if( handle == nullptr )
        {
            return;
        }

        // Three is enough for any real window: the first pass sets the size, the second corrects for
        // a menu that re-wrapped because of it, and a third would only be needed if that correction
        // re-wrapped it again, which would mean the menu is oscillating and no size satisfies it.
        // Bounded rather than "until it converges" for exactly that reason.
        for( int pass = 0; pass < 3; ++pass )
        {
            RECT client {};
            RECT frame {};
            GetClientRect( handle, &client );
            GetWindowRect( handle, &frame );

            const int clientWidth  = static_cast<int>( client.right - client.left );
            const int clientHeight = static_cast<int>( client.bottom - client.top );

            if( clientWidth == aWidth && clientHeight == aHeight )
            {
                return;
            }

            const int chromeWidth  = static_cast<int>( frame.right - frame.left ) - clientWidth;
            const int chromeHeight = static_cast<int>( frame.bottom - frame.top ) - clientHeight;

            // SWP_NOMOVE so the window stays where it is; a resize should not also relocate it.
            // SWP_NOACTIVATE so setting a size does not steal focus, which matters because this runs
            // during setMenu() and during setup, before the window is even shown.
            SetWindowPos( handle, nullptr, 0, 0, aWidth + chromeWidth, aHeight + chromeHeight,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
        }
    }

    //! Attaches or removes a menu bar, and puts the client area back to the size it was.
    //!
    //! A menu bar is not part of the client area, so SetMenu() on its own silently takes its height
    //! out of whatever the renderer had been given -- roughly twenty pixels, at the bottom, on every
    //! frame from then on. Measuring the client area first and restoring it afterwards is what keeps
    //! the drawable the size the caller asked for and grows the window instead.
    //!
    //! DrawMenuBar() because the window already exists: Windows does not repaint the menu bar of a
    //! live window on its own, and without it the bar is there but blank until something else forces
    //! a non-client repaint.
    void PlatformIntegrationWin32::setMenu
        (
        Window* aWindow,    //!< Window to attach the menu to.
        void* aMenuHandle   //!< HMENU to attach, or nullptr to remove the current one.
        )
    {
        const HWND handle = static_cast<HWND>( aWindow->nativeHandle() );
        if( handle == nullptr )
        {
            return;
        }

        // Read before the menu is attached, because attaching it is what changes them.
        const int width  = aWindow->width();
        const int height = aWindow->height();

        if( SetMenu( handle, static_cast<HMENU>( aMenuHandle ) ) == FALSE )
        {
            std::fprintf( stderr, "QtLikeSignalGui: SetMenu() failed (%lu)\n", GetLastError() );
            return;
        }

        DrawMenuBar( handle );

        if( width > 0 && height > 0 )
        {
            setClientSize( aWindow, width, height );
        }
    }

    //! Marks the whole client area as needing a repaint.
    //!
    //! FALSE for the erase flag: WM_ERASEBKGND is claimed anyway, and asking for an erase that is
    //! then refused only adds a message.
    void PlatformIntegrationWin32::requestUpdate
        (
        Window* aWindow   //!< Window to repaint.
        )
    {
        const HWND handle = static_cast<HWND>( aWindow->nativeHandle() );
        if( handle == nullptr )
        {
            return;
        }

        InvalidateRect( handle, nullptr, FALSE );
    }
}
