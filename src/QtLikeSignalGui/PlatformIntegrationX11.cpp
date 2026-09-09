// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The X11 backend: adopting a window, joining the loop, and the drain that turns X events into
//! WindowSystemInterface calls.

#include "QtLikeSignalGui/PlatformIntegrationX11.hpp"

#include "QtLikeSignalGui/Window.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Thread.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignalGui/LogCategories.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <poll.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>

// For XVisualInfo and XGetVisualInfo, which createWindow() needs to turn a VisualID from a GL
// library's chosen config into the Visual* and depth XCreateWindow wants.
#include <X11/Xutil.h>
#include <X11/keysym.h>

// Xlib defines None as a bare `0L` macro, and MouseButton has a None enumerator; the two cannot
// coexist. The QtLikeSignalGui headers are included above, before the macro exists, so the
// enumerator is declared safely -- and undefining the macro here makes it usable again in the code
// below. Nothing in this file passes None to Xlib: where a null resource id is wanted, a literal 0
// says exactly the same thing to the server without the macro.
#undef None

// Xlib also typedefs Window as an XID. Inside namespace QtLikeSignalGui the class wins every
// lookup, and the few places that need the server's meaning say ::Window explicitly. Undefining is
// not an option here -- it is a typedef, not a macro -- so the qualification is the whole of the
// answer.

namespace QtLikeSignalGui
{
    namespace
    {
        //! The events this backend asks the server to send for an adopted window.
        //!
        //! StructureNotifyMask is what delivers ConfigureNotify, and therefore the only way a
        //! resize is ever seen; without it a renderer would keep drawing at the original size after
        //! the window manager resized the window.
        const long kEventMask = ExposureMask | ButtonPressMask | ButtonReleaseMask
            | PointerMotionMask | EnterWindowMask | LeaveWindowMask | StructureNotifyMask
            | FocusChangeMask | KeyPressMask | KeyReleaseMask;

        //! Translates an X11 keysym into a portable Key.
        //!
        //! **The Latin-1 range maps by arithmetic, not by table.** X11 gives the printable ASCII
        //! keysyms their ASCII values, and Key uses the same numbering -- but keysyms are the
        //! *shifted* interpretation, so "A" and "a" are different keysyms for one key. Key names
        //! the key, so the lower-case range is folded up into the upper-case one.
        //!
        //! @return the portable key, or Key::Unknown for anything not named here.
        Key keyFromKeySym
            (
            KeySym aKeySym   //!< The keysym XLookupString resolved.
            )
        {
            const unsigned long sym = static_cast<unsigned long>( aKeySym );

            // a..z folded onto A..Z, which is what Key stores.
            if( sym >= 0x61 && sym <= 0x7a )
            {
                return static_cast<Key>( sym - 0x20 );
            }

            if( sym >= 0x41 && sym <= 0x5a )
            {
                return static_cast<Key>( sym );
            }

            if( sym >= 0x30 && sym <= 0x39 )
            {
                return static_cast<Key>( sym );
            }

            switch( sym )
            {
            case XK_space:        return Key::Space;
            case XK_apostrophe:   return Key::Apostrophe;
            case XK_comma:        return Key::Comma;
            case XK_minus:        return Key::Minus;
            case XK_period:       return Key::Period;
            case XK_slash:        return Key::Slash;
            case XK_semicolon:    return Key::Semicolon;
            case XK_equal:        return Key::Equal;
            case XK_bracketleft:  return Key::BracketLeft;
            case XK_backslash:    return Key::Backslash;
            case XK_bracketright: return Key::BracketRight;
            case XK_grave:        return Key::Grave;

            case XK_Escape:    return Key::Escape;
            case XK_Tab:       return Key::Tab;
            case XK_BackSpace: return Key::Backspace;
            case XK_Return:    return Key::Return;
            case XK_KP_Enter:  return Key::Enter;
            case XK_Insert:    return Key::Insert;
            case XK_Delete:    return Key::Delete;
            case XK_Pause:     return Key::Pause;
            case XK_Print:     return Key::Print;

            case XK_Home:      return Key::Home;
            case XK_End:       return Key::End;
            case XK_Left:      return Key::Left;
            case XK_Up:        return Key::Up;
            case XK_Right:     return Key::Right;
            case XK_Down:      return Key::Down;
            case XK_Prior:     return Key::PageUp;
            case XK_Next:      return Key::PageDown;

            case XK_Shift_L:
            case XK_Shift_R:    return Key::Shift;
            case XK_Control_L:
            case XK_Control_R:  return Key::Control;
            case XK_Alt_L:
            case XK_Alt_R:      return Key::Alt;
            case XK_Super_L:
            case XK_Super_R:
            case XK_Meta_L:
            case XK_Meta_R:     return Key::Meta;
            case XK_Caps_Lock:  return Key::CapsLock;
            case XK_Num_Lock:   return Key::NumLock;
            case XK_Scroll_Lock: return Key::ScrollLock;
            case XK_Menu:       return Key::Menu;

            case XK_F1:  return Key::F1;
            case XK_F2:  return Key::F2;
            case XK_F3:  return Key::F3;
            case XK_F4:  return Key::F4;
            case XK_F5:  return Key::F5;
            case XK_F6:  return Key::F6;
            case XK_F7:  return Key::F7;
            case XK_F8:  return Key::F8;
            case XK_F9:  return Key::F9;
            case XK_F10: return Key::F10;
            case XK_F11: return Key::F11;
            case XK_F12: return Key::F12;

            default: break;
            }

            return Key::Unknown;
        }

        //! Translates an X11 event state mask into a modifier set.
        //!
        //! **The state is the one *before* the event**, which X11 documents and which matters for a
        //! modifier key's own press: pressing Shift arrives with ShiftMask clear. The caller fixes
        //! that up, exactly as it already does for mouse buttons, so that a press includes the
        //! modifier it pressed and a release excludes it.
        KeyModifiers modifiersFromState
            (
            unsigned int aState   //!< The state field of an XKeyEvent.
            )
        {
            KeyModifiers modifiers;

            if( ( aState & ShiftMask ) != 0 )
            {
                modifiers |= KeyModifier::Shift;
            }
            if( ( aState & ControlMask ) != 0 )
            {
                modifiers |= KeyModifier::Control;
            }
            if( ( aState & Mod1Mask ) != 0 )
            {
                modifiers |= KeyModifier::Alt;
            }
            if( ( aState & Mod4Mask ) != 0 )
            {
                modifiers |= KeyModifier::Meta;
            }
            if( ( aState & LockMask ) != 0 )
            {
                modifiers |= KeyModifier::CapsLock;
            }
            if( ( aState & Mod2Mask ) != 0 )
            {
                modifiers |= KeyModifier::NumLock;
            }

            return modifiers;
        }

        //! Returns the modifier a key is, or KeyModifier::None if it is an ordinary key.
        //!
        //! Used to correct the before-the-event state for a modifier key's own press and release.
        KeyModifier modifierForKey
            (
            Key aKey   //!< The key that changed.
            )
        {
            switch( aKey )
            {
            case Key::Shift:   return KeyModifier::Shift;
            case Key::Control: return KeyModifier::Control;
            case Key::Alt:     return KeyModifier::Alt;
            case Key::Meta:    return KeyModifier::Meta;
            default:           return KeyModifier::None;
            }
        }

        //! Builds a KeyEvent from an XKeyEvent, text and all.
        //!
        //! **XLookupString answers both questions at once**, which is why there is no separate text
        //! path here as there is on Win32: it returns the keysym that identifies the key and the
        //! bytes the layout produces for it, from one call against one event.
        KeyEvent keyEventFrom
            (
            const XKeyEvent& aEvent,   //!< The event, exactly as it came off the queue.
            bool aIsPress              //!< True for KeyPress, false for KeyRelease.
            )
        {
            KeyEvent event;

            KeySym keySym = NoSymbol;
            char text[8] {};

            // Copied because XLookupString takes a mutable pointer, while the dispatcher hands
            // every event out as const -- rightly, since nothing dispatching an event should be
            // editing it. The copy is a few dozen bytes and keeps that guarantee intact.
            XKeyEvent mutableEvent = aEvent;

            // Latin-1 bytes, which is all XLookupString promises without an input method. Anything
            // beyond that needs XIM or xkbcommon, and neither is a dependency this library takes.
            const int written = XLookupString( &mutableEvent, text,
                static_cast<int>( sizeof( text ) ) - 1, &keySym, nullptr );

            event.mKey         = keyFromKeySym( keySym );
            event.mNativeCode  = static_cast<unsigned int>( keySym );
            event.mTimestampMs = static_cast<unsigned long>( aEvent.time );
            event.mModifiers   = modifiersFromState( aEvent.state );

            // X11 reports the state *prior* to the event, so a modifier key's own press has to add
            // itself and its release has to take itself out. Without this, Shift+A would report no
            // Shift on the very press that started it.
            const KeyModifier own = modifierForKey( event.mKey );
            if( own != KeyModifier::None )
            {
                if( aIsPress )
                {
                    event.mModifiers |= own;
                }
                else
                {
                    event.mModifiers.remove( own );
                }
            }

            // Only a press carries text, and only if it is not a control character: Control chords
            // and the navigation keys produce those, and they are not text for a field to insert.
            const bool isControlCharacter = ( written == 1
                && static_cast<unsigned char>( text[0] ) < 0x20 );

            if( aIsPress && written > 0 && !isControlCharacter )
            {
                for( int index = 0; index < written
                    && index < static_cast<int>( sizeof( event.mText ) ) - 1; ++index )
                {
                    event.mText[index] = text[index];
                }
            }

            return event;
        }

        //! Gets the running thread's dispatcher as an EventDispatcherLinux, or null if it is not
        //! one.
        //!
        //! It will not be one if no CoreApplication has adopted this thread yet, which is the
        //! mistake worth catching by name rather than by a crash later.
        std::shared_ptr<QtLikeSignal::EventDispatcherLinux> currentLinuxDispatcher()
        {
            QtLikeSignal::Thread* const current = QtLikeSignal::Thread::currentThread();
            if( current == nullptr )
            {
                return nullptr;
            }

            return std::dynamic_pointer_cast<QtLikeSignal::EventDispatcherLinux>(
                current->eventDispatcher() );
        }

        //! Maps an X11 button number to the button this library reports.
        //!
        //! Buttons 4 to 7 are the wheel in X11's numbering rather than physical buttons, and are
        //! handled as wheel events before this is ever reached; 8 and 9 are the side buttons that
        //! Windows calls XBUTTON1 and XBUTTON2.
        MouseButton buttonOf
            (
            unsigned int aButton   //!< XButtonEvent::button, 1-based.
            )
        {
            switch( aButton )
            {
            case Button1:
                return MouseButton::Left;

            case Button2:
                return MouseButton::Middle;

            case Button3:
                return MouseButton::Right;

            case 8:
                return MouseButton::Extra1;

            case 9:
                return MouseButton::Extra2;

            default:
                return MouseButton::None;
            }
        }

        //! Translates the modifier-state mask carried by a pointer event into a button set.
        //!
        //! Only three bits exist to read. Button4Mask and Button5Mask are the wheel, which is not a
        //! held button, and the side buttons have no mask bit at all -- so a drag with button 8
        //! down reports an empty set here. That is X11's limit rather than a shortcut, and it is
        //! why the press and release paths adjust the result rather than trusting it whole.
        MouseButtons buttonsFromState
            (
            unsigned int aState   //!< XButtonEvent::state or XMotionEvent::state.
            )
        {
            MouseButtons buttons;

            if( ( aState & Button1Mask ) != 0 )
            {
                buttons |= MouseButton::Left;
            }
            if( ( aState & Button2Mask ) != 0 )
            {
                buttons |= MouseButton::Middle;
            }
            if( ( aState & Button3Mask ) != 0 )
            {
                buttons |= MouseButton::Right;
            }

            return buttons;
        }

        //! Returns the wheel rotation an X11 button number stands for, or 0,0 if it is not the
        //! wheel.
        //!
        //! X11 has no wheel event: the server reports a press and a release of buttons 4 to 7. The
        //! 120 is Windows' WHEEL_DELTA, which WheelEvent adopts as its unit so that one notch reads
        //! the same on both platforms.
        void wheelDeltaOf
            (
            unsigned int aButton,   //!< XButtonEvent::button.
            int& aDeltaX,           //!< Set to the horizontal rotation.
            int& aDeltaY            //!< Set to the vertical rotation.
            )
        {
            aDeltaX = 0;
            aDeltaY = 0;

            switch( aButton )
            {
            case Button4:
                aDeltaY = 120;
                break;

            case Button5:
                aDeltaY = -120;
                break;

            case 6:
                aDeltaX = -120;
                break;

            case 7:
                aDeltaX = 120;
                break;

            default:
                break;
            }
        }
    }

    //! Constructs the backend. It has nothing to do until a window is adopted.
    PlatformIntegrationX11::PlatformIntegrationX11()
    {
    }

    //! Stops listening, and closes the connection if this backend was the one that opened it.
    //!
    //! Every window is gone by now: GuiApplication destroys its windows before releasing the
    //! backend, precisely so that a created window's XDestroyWindow still has a connection to
    //! travel on. An adopted connection is left open, because closing somebody else's Display would
    //! take down every window on it including the ones this library never touched.
    PlatformIntegrationX11::~PlatformIntegrationX11()
    {
        Display* const owned = mOwnsDisplay ? static_cast<Display*>( mDisplay ) : nullptr;

        unregisterConnection();

        if( owned != nullptr )
        {
            XCloseDisplay( owned );
        }
    }

    //! Gets which window system this backend is.
    PlatformType PlatformIntegrationX11::type() const
    {
        return PlatformType::X11;
    }

    //! X11 windows can be created here.
    bool PlatformIntegrationX11::canCreateWindows() const
    {
        return true;
    }

    //! And a window an external library created can be adopted instead.
    bool PlatformIntegrationX11::canAdoptWindows() const
    {
        return true;
    }

    //! Gets the connection, opening one if there is not one yet.
    //!
    //! Opening on demand is the point rather than a convenience: a GL library has to choose its
    //! FBConfig or EGLConfig against a live Display *before* there is a window, because the visual
    //! that config implies is what the window then has to be created with. This is how it gets one.
    //!
    //! @return the Display*, or nullptr if no connection could be opened.
    void* PlatformIntegrationX11::nativeDisplay()
    {
        if( mDisplay == nullptr && !ensureOwnDisplay() )
        {
            return nullptr;
        }

        return mDisplay;
    }

    //! Creates a window and the Window that fronts it. The window starts unmapped; call show().
    //!
    //! Created with WindowSettings::mVisualId when one is given, and with the screen's default
    //! visual otherwise. A non-default visual brings two obligations with it, and XCreateWindow
    //! answers BadMatch if either is missed: a colormap made for that visual, and an explicit
    //! border pixel. Both are supplied below, and the colormap is remembered so it can be freed
    //! with the window -- it is not freed by XDestroyWindow.
    //!
    //! Nothing here touches GLX or EGL. The window is merely made *compatible* with whatever the
    //! caller's library will do to it, which is the same division the Win32 backend keeps by never
    //! calling SetPixelFormat.
    //!
    //! @return the new Window, or nullptr if it could not be created.
    Window* PlatformIntegrationX11::createWindow
        (
        const WindowSettings& aSettings   //!< Requested size, caption and visual.
        )
    {
        if( mDisplay == nullptr && !ensureOwnDisplay() )
        {
            return nullptr;
        }

        Display* const display = static_cast<Display*>( mDisplay );
        const int screen       = DefaultScreen( display );
        const ::Window root    = RootWindow( display, screen );

        Visual* visual = DefaultVisual( display, screen );
        int depth      = DefaultDepth( display, screen );
        XVisualInfo* chosen = nullptr;

        if( aSettings.mVisualId != 0 )
        {
            XVisualInfo request {};
            request.visualid = static_cast<VisualID>( aSettings.mVisualId );

            int count = 0;
            chosen = XGetVisualInfo( display, VisualIDMask, &request, &count );

            if( chosen == nullptr || count == 0 )
            {
                qCWarning( gLogGuiX11 )
                    << "QtLikeSignalGui: no visual with this id on this screen; visual"
                    << QtLikeSignal::logHex( aSettings.mVisualId );
                if( chosen != nullptr )
                {
                    XFree( chosen );
                }
                return nullptr;
            }

            visual = chosen->visual;
            depth  = chosen->depth;
        }

        const Colormap colormap = XCreateColormap( display, root, visual, AllocNone );

        XSetWindowAttributes attributes {};
        attributes.colormap     = colormap;

        // Explicit, because the default is CopyFromParent and the parent here is the root window,
        // whose visual may not be the one just chosen. That mismatch is a BadMatch.
        attributes.border_pixel = 0;

        // 0 is None: no background, so the server never paints the window before a frame is drawn.
        // The same decision as the Win32 backend's null background brush, and for the same reason
        // -- anything else is a visible flash on every resize.
        attributes.background_pixmap = 0;
        attributes.event_mask        = kEventMask;

        const ::Window windowId = XCreateWindow( display, root, 0, 0,
            static_cast<unsigned int>( aSettings.mWidth ),
            static_cast<unsigned int>( aSettings.mHeight ),
            0, depth, InputOutput, visual,
            CWColormap | CWBorderPixel | CWBackPixmap | CWEventMask, &attributes );

        if( chosen != nullptr )
        {
            XFree( chosen );
        }

        if( windowId == 0 )
        {
            qCWarning( gLogGuiX11 ) << "QtLikeSignalGui: XCreateWindow() failed";
            XFreeColormap( display, colormap );
            return nullptr;
        }

        if( !prepareConnection() )
        {
            XDestroyWindow( display, windowId );
            XFreeColormap( display, colormap );
            return nullptr;
        }

        // Without WM_DELETE_WINDOW among the protocols the close button makes the server drop the
        // connection instead of sending a ClientMessage, and Xlib's default I/O error handler then
        // calls exit() -- skipping every destructor in the program.
        Atom deleteAtom = static_cast<Atom>( mDeleteWindowAtom );
        XSetWMProtocols( display, windowId, &deleteAtom, 1 );

        NativeWindow native;
        native.mDisplay  = mDisplay;
        native.mWindowId = windowId;

        Window* const window = newWindow( this, native, aSettings.mWidth, aSettings.mHeight );
        reportDevicePixelRatio( window );

        Tracked tracked;
        tracked.mWindow        = window;
        tracked.mWindowId      = windowId;
        tracked.mUpdatePending = false;
        tracked.mCreated       = true;
        tracked.mColormap      = colormap;
        mWindows.push_back( tracked );

        setWindowTitle( window, aSettings.mTitle );

        XFlush( display );
        return window;
    }

    //! Takes charge of listening to a window an external library created.
    //!
    //! Three things happen, and each one is additive rather than authoritative, because this window
    //! belongs to somebody else: the event mask is OR-ed onto whatever they already selected,
    //! WM_DELETE_WINDOW is appended to whatever protocols they already registered, and the first
    //! adoption registers the connection with the loop. Replacing either property instead of adding
    //! to it would break the owner's own arrangements silently.
    //!
    //! @return the new Window, owned by the caller, or nullptr if the descriptor was unusable.
    Window* PlatformIntegrationX11::adoptWindow
        (
        const NativeWindow& aNative   //!< The Display* and Window id to take over.
        )
    {
        if( aNative.mDisplay == nullptr || aNative.mWindowId == 0 )
        {
            qCWarning( gLogGuiX11 )
                << "QtLikeSignalGui: adoptWindow on X11 needs both a Display* and a Window id";
            return nullptr;
        }

        if( mDisplay != nullptr && mDisplay != aNative.mDisplay )
        {
            // One registered descriptor means one connection. Refused rather than adopted-and-
            // ignored, because a window whose events are never polled looks exactly like a window
            // that is simply not being used. This also catches mixing the two entry points: a
            // createWindow() opened a connection of our own, and a window from a different one
            // cannot join it.
            qCWarning( gLogGuiX11 )
                << "QtLikeSignalGui: every X11 window must be on the same Display*; this one is"
                << "not, and would never receive events";
            return nullptr;
        }

        Display* const display = static_cast<Display*>( aNative.mDisplay );
        const ::Window windowId = static_cast< ::Window >( aNative.mWindowId );

        XWindowAttributes attributes {};
        if( XGetWindowAttributes( display, windowId, &attributes ) == 0 )
        {
            qCWarning( gLogGuiX11 )
                << "QtLikeSignalGui: XGetWindowAttributes() failed, so this window is not"
                << "usable; window" << QtLikeSignal::logHex( aNative.mWindowId );
            return nullptr;
        }

        if( mDisplay == nullptr )
        {
            // Theirs, so mOwnsDisplay stays false and the destructor will not close it.
            mDisplay = aNative.mDisplay;
        }

        if( !prepareConnection() )
        {
            return nullptr;
        }

        // OR-ed, not replaced: your_event_mask is what the window's creator selected, and taking it
        // away would stop whatever they are doing with it.
        XSelectInput( display, windowId, attributes.your_event_mask | kEventMask );

        // Appended, not replaced, for the same reason. Without WM_DELETE_WINDOW among the protocols
        // the close button makes the server drop the connection instead of sending a ClientMessage,
        // and Xlib's default I/O error handler then calls exit() -- skipping every destructor.
        Atom* existing = nullptr;
        int existingCount = 0;
        std::vector<Atom> protocols;

        if( XGetWMProtocols( display, windowId, &existing, &existingCount ) != 0 )
        {
            protocols.assign( existing, existing + existingCount );
            XFree( existing );
        }

        const Atom deleteAtom = static_cast<Atom>( mDeleteWindowAtom );
        if( std::find( protocols.begin(), protocols.end(), deleteAtom ) == protocols.end() )
        {
            protocols.push_back( deleteAtom );
            XSetWMProtocols( display, windowId, protocols.data(),
                static_cast<int>( protocols.size() ) );
        }

        Window* const window = newWindow( this, aNative, attributes.width, attributes.height );
        reportDevicePixelRatio( window );

        Tracked tracked;
        tracked.mWindow        = window;
        tracked.mWindowId      = aNative.mWindowId;
        tracked.mUpdatePending = false;

        // False, and this is the whole difference: releaseNativeWindow() will leave this window
        // standing, because the library that made it still owns it.
        tracked.mCreated       = false;
        tracked.mColormap      = 0;
        mWindows.push_back( tracked );

        // Everything above talked to the server, and the requests are sitting in Xlib's output
        // buffer. Push them out now rather than at the end of the next pass, so the selection is in
        // force before the first event that ought to match it.
        XFlush( display );

        return window;
    }

    //! Lets go of a window: destroys it if this backend created it, otherwise merely stops
    //! listening.
    //!
    //! An adopted window belongs to the library that created it, which is very likely still holding
    //! a GLX context bound to it -- so undoing the event selection is the whole of what this
    //! backend put there, and the whole of what it takes away. A created one is ours and goes away
    //! entirely, colormap included: XDestroyWindow does not free the colormap the window was made
    //! with.
    void PlatformIntegrationX11::releaseNativeWindow
        (
        Window* aWindow   //!< The window being destroyed.
        )
    {
        const unsigned long windowId = aWindow->nativeWindowId();

        bool created           = false;
        unsigned long colormap = 0;

        for( const Tracked& entry : mWindows )
        {
            if( entry.mWindow == aWindow )
            {
                created  = entry.mCreated;
                colormap = entry.mColormap;
                break;
            }
        }

        // Removed before the server calls, so nothing routed by windowFor() can reach a window that
        // is on its way out -- XDestroyWindow generates events of its own.
        mWindows.erase(
            std::remove_if( mWindows.begin(), mWindows.end(),
            [aWindow]( const Tracked& aEntry )
            {
                return aEntry.mWindow == aWindow;
            } ),
            mWindows.end() );

        if( mDisplay != nullptr && windowId != 0 )
        {
            Display* const display = static_cast<Display*>( mDisplay );

            if( created )
            {
                XDestroyWindow( display, static_cast< ::Window >( windowId ) );

                if( colormap != 0 )
                {
                    XFreeColormap( display, static_cast<Colormap>( colormap ) );
                }
            }
            else
            {
                // NoEventMask, not the mask we added: the creator's own selection went in the same
                // field, and this backend has no record of what it was by now. Better to leave the
                // window quiet than to guess at restoring a mask and get it wrong -- and a window
                // this library has let go of is one nothing here will read events from anyway.
                XSelectInput( display, static_cast< ::Window >( windowId ), NoEventMask );
            }

            XFlush( display );
        }

        assignNative( aWindow, NativeWindow() );
    }

    //! Sets the window caption, in UTF-8.
    //!
    //! Both properties are set. _NET_WM_NAME is the one every modern window manager reads and the
    //! only one that carries UTF-8; WM_NAME is the ancient Latin-1 fallback, kept because a
    //! window manager that reads neither shows an empty title bar and there is no way to tell.
    void PlatformIntegrationX11::setWindowTitle
        (
        Window* aWindow,           //!< Window to retitle.
        const std::string& aTitle  //!< New caption, in UTF-8.
        )
    {
        if( mDisplay == nullptr || aWindow->nativeWindowId() == 0 )
        {
            return;
        }

        Display* const display  = static_cast<Display*>( mDisplay );
        const ::Window windowId = static_cast< ::Window >( aWindow->nativeWindowId() );

        const Atom netWmName = XInternAtom( display, "_NET_WM_NAME", False );
        const Atom utf8String = XInternAtom( display, "UTF8_STRING", False );

        XChangeProperty( display, windowId, netWmName, utf8String, 8, PropModeReplace,
            reinterpret_cast<const unsigned char*>( aTitle.c_str() ),
            static_cast<int>( aTitle.size() ) );

        XStoreName( display, windowId, aTitle.c_str() );
        XFlush( display );
    }

    //! Maps or unmaps the window.
    void PlatformIntegrationX11::setWindowVisible
        (
        Window* aWindow,   //!< Window to show or hide.
        bool aVisible      //!< True to show it.
        )
    {
        if( mDisplay == nullptr || aWindow->nativeWindowId() == 0 )
        {
            return;
        }

        Display* const display  = static_cast<Display*>( mDisplay );
        const ::Window windowId = static_cast< ::Window >( aWindow->nativeWindowId() );

        if( aVisible )
        {
            XMapWindow( display, windowId );
        }
        else
        {
            XUnmapWindow( display, windowId );
        }

        XFlush( display );
        assignVisible( aWindow, aVisible );
    }

    //! Resizes the window so its drawable area is exactly @p aWidth by @p aHeight.
    //!
    //! Simpler than the Win32 counterpart, and not by accident: an X11 window's own geometry *is*
    //! its drawable area. The title bar and border are a separate window the window manager puts
    //! around it, outside this one's coordinate space, so there is no chrome to measure and add.
    //!
    //! The window manager is free to refuse or amend the request. Nothing is recorded here for that
    //! reason -- the size this library reports comes from the ConfigureNotify that follows, which
    //! is what actually happened rather than what was asked for.
    void PlatformIntegrationX11::setClientSize
        (
        Window* aWindow,   //!< Window to resize.
        int aWidth,        //!< Desired width in pixels.
        int aHeight        //!< Desired height in pixels.
        )
    {
        if( mDisplay == nullptr || aWindow->nativeWindowId() == 0 )
        {
            return;
        }

        Display* const display = static_cast<Display*>( mDisplay );

        XResizeWindow( display, static_cast< ::Window >( aWindow->nativeWindowId() ),
            static_cast<unsigned int>( aWidth ), static_cast<unsigned int>( aHeight ) );
        XFlush( display );
    }

    //! Asks for a repaint by sending the window a synthetic Expose.
    //!
    //! XClearArea with exposures would do it too, but it also clears the window to its background
    //! first -- a visible flash on anything that renders its own frames. Sending the event says the
    //! same thing to the loop without touching a pixel.
    //!
    //! Collapsed to one per pass by the pending flag: a drag calls this once per MotionNotify, and
    //! without the flag each one would put another Expose on the wire for the same frame.
    void PlatformIntegrationX11::requestUpdate
        (
        Window* aWindow   //!< Window to repaint.
        )
    {
        Tracked* const adopted = windowFor( aWindow->nativeWindowId() );
        if( adopted == nullptr || mDisplay == nullptr || adopted->mUpdatePending )
        {
            return;
        }

        Display* const display  = static_cast<Display*>( mDisplay );
        const ::Window windowId = static_cast< ::Window >( adopted->mWindowId );

        XEvent event {};
        event.type            = Expose;
        event.xexpose.type    = Expose;
        event.xexpose.display = display;
        event.xexpose.window  = windowId;
        event.xexpose.x       = 0;
        event.xexpose.y       = 0;
        event.xexpose.width   = aWindow->width();
        event.xexpose.height  = aWindow->height();

        // Zero, which is what marks the last event of an expose sequence. The drain acts only on
        // that one, so a synthetic event claiming more were coming would never be acted on at all.
        event.xexpose.count   = 0;

        XSendEvent( display, windowId, False, ExposureMask, &event );
        XFlush( display );

        adopted->mUpdatePending = true;
    }

    //! Reports whether there is an X server this process can actually reach.
    //!
    //! **Opens a connection and closes it, rather than reading DISPLAY.** A set variable says only
    //! that someone intended an X session, not that the server is listening: it survives into a
    //! console login, points at a server that has since exited, and is inherited by a process whose
    //! session was never X at all. Connecting is the same question the backend will ask a moment
    //! later, so an answer here cannot disagree with what createWindow() finds.
    //!
    //! Silent on failure, unlike ensureOwnDisplay(). This is a question being asked, and "no" is
    //! one of the two expected answers; the diagnostic belongs to the caller that had already
    //! decided X11 was the platform.
    //!
    //! @return true if a connection was opened.
    bool PlatformIntegrationX11::isAvailable()
    {
        Display* const display = XOpenDisplay( nullptr );
        if( display == nullptr )
        {
            return false;
        }

        XCloseDisplay( display );
        return true;
    }

    //! Opens a connection of this backend's own, for createWindow() and nativeDisplay().
    //!
    //! Only ever called when there is none: a connection handed in by adoptWindow() is used as it
    //! is, and a second one would need a second registration and a second drain.
    //!
    //! @return true if there is now a connection.
    bool PlatformIntegrationX11::ensureOwnDisplay()
    {
        Display* const display = XOpenDisplay( nullptr );
        if( display == nullptr )
        {
            qCWarning( gLogGuiX11 )
                <<
                "QtLikeSignalGui: XOpenDisplay() failed; is DISPLAY set and the server reachable?";
            return false;
        }

        mDisplay     = display;
        mOwnsDisplay = true;
        return true;
    }

    //! Interns the atoms and puts the connection in the loop's poll set, once.
    //!
    //! Idempotent, because both entry points call it and either may be the first: createWindow()
    //! and adoptWindow() each need the connection ready before they can finish, and neither knows
    //! whether the other has run.
    //!
    //! @return true if the descriptor is in the poll set.
    bool PlatformIntegrationX11::prepareConnection()
    {
        if( mConnectionFd >= 0 )
        {
            return true;
        }

        if( mDisplay == nullptr )
        {
            return false;
        }

        {
            Display* const display = static_cast<Display*>( mDisplay );
            mProtocolsAtom    = XInternAtom( display, "WM_PROTOCOLS", False );
            mDeleteWindowAtom = XInternAtom( display, "WM_DELETE_WINDOW", False );
        }

        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher =
            currentLinuxDispatcher();
        if( !dispatcher )
        {
            qCWarning( gLogGuiX11 )
                <<
                "QtLikeSignalGui: this thread is not running EventDispatcherLinux, so the display"
                << "connection cannot join the event loop; construct the GuiApplication on the"
                << "thread that will call exec()";
            return false;
        }

        Display* const display = static_cast<Display*>( mDisplay );
        mConnectionFd = XConnectionNumber( display );

        if( !dispatcher->registerEventSource( mConnectionFd, POLLIN,
            [this]( short aEvents )
            {
                pumpDisplay( aEvents );
            } ) )
        {
            qCWarning( gLogGuiX11 )
                << "QtLikeSignalGui: registerEventSource() was refused for the display connection"
                << mConnectionFd;
            mConnectionFd = -1;
            return false;
        }

        // One drain before the loop ever blocks. Interning the atoms above talked to the server,
        // and a reply read off the socket can bring events along with it -- they are in Xlib's
        // queue now, with an empty socket behind them. poll() would have nothing to report and that
        // first batch would wait for whatever unrelated thing happened next.
        pumpDisplay( POLLIN );
        return true;
    }

    //! Records the desktop's scale on @p aWindow, as X11 makes it available.
    //!
    //! **X11 has no per-window scale and no event when one changes**, so this is read once, at
    //! creation, and never revised. That is also why it records rather than reports: with no second
    //! reading there is never a change to announce. The number lives in the X resource database
    //! under `Xft.dpi`, which is what a desktop environment writes when the user picks a scaling
    //! percentage and what every toolkit reads: GTK and Qt both start here.
    //!
    //! Absent, unparseable or not positive means the desktop said nothing, and 1.0 is then the
    //! honest answer rather than a guess from the physical screen size -- `DisplayWidthMM` is
    //! notoriously wrong on real monitors, and a scale derived from it would be worse than none.
    void PlatformIntegrationX11::reportDevicePixelRatio
        (
        Window* aWindow   //!< The window to report for.
        )
    {
        Display* const display = static_cast<Display*>( mDisplay );
        if( display == nullptr || aWindow == nullptr )
        {
            return;
        }

        const char* const resources = XResourceManagerString( display );
        if( resources == nullptr )
        {
            return;
        }

        // Parsed by hand rather than through XrmGetResource, which would mean building a database,
        // looking one value up and destroying it again for a single number available as text.
        const char* const key = std::strstr( resources, "Xft.dpi:" );
        if( key == nullptr )
        {
            return;
        }

        const double dpi = std::atof( key + std::strlen( "Xft.dpi:" ) );
        if( dpi <= 0.0 )
        {
            return;
        }

        // Recorded rather than reported: both callers are still building the window, so there is
        // nobody connected to tell.
        WindowSystemInterface::setInitialDevicePixelRatio( aWindow, dpi / 96.0 );
    }

    //! Takes the connection back out of the poll set. Safe to call when it was never in it.
    void PlatformIntegrationX11::unregisterConnection()
    {
        if( mConnectionFd < 0 )
        {
            return;
        }

        // Unregistered from the loop's own thread, so EventDispatcherLinux's contract makes the
        // call synchronous: the callback will not run again, not even for a readiness the current
        // poll() round has already observed. That is what makes it safe to free everything the
        // callback touches immediately afterwards.
        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher =
            currentLinuxDispatcher();
        if( dispatcher )
        {
            dispatcher->unregisterEventSource( mConnectionFd );
        }

        mConnectionFd = -1;

        // Forgotten, not closed. The destructor is the only place that closes one, and it took its
        // own copy of the pointer before calling this precisely so that clearing it here is safe.
        mDisplay      = nullptr;
        mOwnsDisplay  = false;
    }

    //! Drains every event the connection has and delivers each one.
    //!
    //! Called by the dispatcher, on the dispatcher's thread, whenever poll() reports the connection
    //! ready -- and once from registerConnection() before the loop starts.
    //!
    //! **The loop is not decoration.** poll() reports that the socket has bytes; Xlib turns those
    //! bytes into events and holds them in a queue inside this process. Handle one per readiness
    //! and the rest stay in that queue behind an empty socket, so the loop blocks with events
    //! already in hand. XPending() flushes the output buffer, then hands back whatever is queued or
    //! reads more, so looping on it is what empties both the queue and the socket.
    void PlatformIntegrationX11::pumpDisplay
        (
        short aEvents   //!< poll(2) revents for the connection.
        )
    {
        Display* const display = static_cast<Display*>( mDisplay );
        if( display == nullptr )
        {
            return;
        }

        if( ( aEvents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
        {
            // The server went away. Reading further would hit Xlib's I/O error handler, which calls
            // exit() and would skip every destructor in the program -- including the one that
            // releases the external library's GLX context. Stop listening, then end the loop: there
            // is no window system left, so every alternative leaves a live loop with nothing to
            // service and no way for the program to learn why.
            qCCritical( gLogGuiX11 )
                <<
                "QtLikeSignalGui: the X11 display connection dropped, so this is quitting; revents"
                << QtLikeSignal::logHex( static_cast<unsigned int>( aEvents ) );
            unregisterConnection();
            QtLikeSignal::CoreApplication::quit();
            return;
        }

        while( XPending( display ) > 0 )
        {
            XEvent event;
            XNextEvent( display, &event );

            // Motion compression, and the reason it is a peek rather than an XCheckTypedWindowEvent
            // sweep: pulling every queued motion out at once would also pull them past events that
            // arrived between them, reordering a press against the moves around it. Looking at just
            // the next one keeps the stream in order and still collapses the flood, because a flood
            // is by definition consecutive.
            if( event.type == MotionNotify && XPending( display ) > 0 )
            {
                XEvent next;
                XPeekEvent( display, &next );
                if( next.type == MotionNotify && next.xmotion.window == event.xmotion.window )
                {
                    continue;
                }
            }

            dispatchNativeEvent( &event );
        }

        // The slots have just run and will have asked for drawing. Their requests are sitting in
        // Xlib's output buffer, and nothing else is about to flush it, so push them out before the
        // loop goes back to sleep -- otherwise the screen lags one event behind the program.
        XFlush( display );

        // Residual check, and cheap: XQLength() is the length of Xlib's own queue and touches no
        // descriptor. It can only be non-zero here if a slot put something there -- a round trip
        // such as XSync() reads events off the socket as a side effect of waiting for its reply.
        // Those events would be invisible to poll(), so take another pass through our own queue
        // rather than recursing, which keeps this pass bounded and lets timers and posted work
        // interleave.
        if( XQLength( display ) > 0 )
        {
            // Explicitly discarded, not ignored. A refusal here means the main loop is already gone
            // -- the application is shutting down -- and there is nothing left to pump into, so
            // dropping the pass is the correct answer rather than a swallowed failure.
            static_cast<void>( QtLikeSignal::CoreApplication::post( [this]()
                {
                    pumpDisplay( POLLIN );
                } ) );
        }
    }

    //! Translates one native event and reports it through WindowSystemInterface.
    //!
    //! The whole X11 vocabulary stops here: everything downstream sees a MouseEvent, a WheelEvent
    //! or a size. That is the split the Win32 backend makes in its window procedure, and the split
    //! Qt makes at QWindowSystemInterface.
    void PlatformIntegrationX11::dispatchNativeEvent
        (
        const void* aEvent   //!< The XEvent just taken off the queue.
        )
    {
        const XEvent& event = *static_cast<const XEvent*>( aEvent );

        // Every event this backend handles names its window in the same place, so the routing is
        // one lookup rather than one per case.
        const Tracked* const adopted = windowFor( event.xany.window );
        if( adopted == nullptr )
        {
            // Not ours: the connection belongs to an external library, and it may well have windows
            // of its own on it. Ignoring them is the correct answer, not a dropped event.
            return;
        }

        Window* const window = adopted->mWindow;

        switch( event.type )
        {
        case KeyPress:
        {
            WindowSystemInterface::handleKeyPressed( window,
                keyEventFrom( event.xkey, true ) );
            break;
        }

        case KeyRelease:
        {
            // No attempt to filter the release half of X11's auto-repeat, which arrives as a
            // release immediately followed by a press at the same timestamp. Detecting it needs
            // XkbSetDetectableAutoRepeat or a peek-ahead, and a consumer that cares can compare
            // timestamps -- whereas a library that silently swallowed releases would be lying about
            // what the server said.
            WindowSystemInterface::handleKeyReleased( window,
                keyEventFrom( event.xkey, false ) );
            break;
        }

        case ButtonPress:
        {
            int deltaX = 0;
            int deltaY = 0;
            wheelDeltaOf( event.xbutton.button, deltaX, deltaY );

            if( deltaX != 0 || deltaY != 0 )
            {
                WheelEvent wheel;
                wheel.mPos          = { event.xbutton.x, event.xbutton.y };
                wheel.mGlobalPos    = { event.xbutton.x_root, event.xbutton.y_root };
                wheel.mAngleDeltaX  = deltaX;
                wheel.mAngleDeltaY  = deltaY;
                wheel.mButtons      = buttonsFromState( event.xbutton.state );
                wheel.mTimestampMs  = static_cast<unsigned long>( event.xbutton.time );
                WindowSystemInterface::handleWheel( window, wheel );
                return;
            }

            const MouseButton button = buttonOf( event.xbutton.button );

            MouseEvent mouse;
            mouse.mPos         = { event.xbutton.x, event.xbutton.y };
            mouse.mGlobalPos   = { event.xbutton.x_root, event.xbutton.y_root };
            mouse.mButton      = button;

            // state is the state *just prior* to the event, so the button going down is not in it
            // yet and has to be added. Win32 reports the state after the change and needs no such
            // correction; MouseButtons::remove() carries the other half of this.
            mouse.mButtons     = buttonsFromState( event.xbutton.state );
            mouse.mButtons    |= button;
            mouse.mTimestampMs = static_cast<unsigned long>( event.xbutton.time );

            WindowSystemInterface::handleMousePressed( window, mouse );
            return;
        }

        case ButtonRelease:
        {
            int deltaX = 0;
            int deltaY = 0;
            wheelDeltaOf( event.xbutton.button, deltaX, deltaY );

            if( deltaX != 0 || deltaY != 0 )
            {
                // The wheel is a press and a release of the same button. The press already reported
                // the rotation; reporting it again here would double every scroll.
                return;
            }

            const MouseButton button = buttonOf( event.xbutton.button );

            MouseEvent mouse;
            mouse.mPos         = { event.xbutton.x, event.xbutton.y };
            mouse.mGlobalPos   = { event.xbutton.x_root, event.xbutton.y_root };
            mouse.mButton      = button;

            // Prior state again: the button coming up is still set in it, and has to come out for
            // mButtons to mean "held after this event" the way MouseEvent documents.
            mouse.mButtons     = buttonsFromState( event.xbutton.state );
            mouse.mButtons.remove( button );
            mouse.mTimestampMs = static_cast<unsigned long>( event.xbutton.time );

            WindowSystemInterface::handleMouseReleased( window, mouse );
            return;
        }

        case MotionNotify:
        {
            MouseEvent mouse;
            mouse.mPos         = { event.xmotion.x, event.xmotion.y };
            mouse.mGlobalPos   = { event.xmotion.x_root, event.xmotion.y_root };
            mouse.mButton      = MouseButton::None;
            mouse.mButtons     = buttonsFromState( event.xmotion.state );
            mouse.mTimestampMs = static_cast<unsigned long>( event.xmotion.time );

            WindowSystemInterface::handleMouseMoved( window, mouse );
            return;
        }

        case EnterNotify:
        {
            MouseEvent mouse;
            mouse.mPos         = { event.xcrossing.x, event.xcrossing.y };
            mouse.mGlobalPos   = { event.xcrossing.x_root, event.xcrossing.y_root };
            mouse.mButtons     = buttonsFromState( event.xcrossing.state );
            mouse.mTimestampMs = static_cast<unsigned long>( event.xcrossing.time );

            WindowSystemInterface::handleMouseEntered( window, mouse );
            return;
        }

        case LeaveNotify:
            WindowSystemInterface::handleMouseLeft( window );
            return;

        case ConfigureNotify:
        {
            // ConfigureNotify also arrives for a move, where the size has not changed. Reporting a
            // resize then would have every renderer rebuild its buffers because the user dragged
            // the title bar.
            if( event.xconfigure.width != window->width()
                || event.xconfigure.height != window->height() )
            {
                WindowSystemInterface::handleResize( window, event.xconfigure.width,
                    event.xconfigure.height );
            }
            return;
        }

        case Expose:
        {
            // X sends one Expose per exposed rectangle, counting down; only the last carries a
            // zero. Acting on that one alone is the compression the protocol hands us for free, and
            // it is what makes a window uncovered in four pieces render one frame rather than four.
            if( event.xexpose.count != 0 )
            {
                return;
            }

            // Cleared before the signal, so a slot calling requestUpdate() to drive the next frame
            // is not refused as a duplicate of the request being served right now.
            Tracked* const mutableEntry = windowFor( event.xany.window );
            if( mutableEntry != nullptr )
            {
                mutableEntry->mUpdatePending = false;
            }

            WindowSystemInterface::handleExpose( window );
            return;
        }

        case ClientMessage:
        {
            if( static_cast<unsigned long>( event.xclient.message_type ) == mProtocolsAtom
                && event.xclient.format == 32
                && static_cast<unsigned long>( event.xclient.data.l[0] ) == mDeleteWindowAtom )
            {
                WindowSystemInterface::handleCloseRequest( window );
            }
            return;
        }

        case FocusIn:
            WindowSystemInterface::handleFocusChange( window, true );
            return;

        case FocusOut:
            WindowSystemInterface::handleFocusChange( window, false );
            return;

        default:
            return;
        }
    }

    //! Finds the tracked window with the given resource id, or nullptr if it is not one of ours.
    //!
    //! A linear scan, deliberately. A program built on this has a handful of windows, and a hash
    //! table would cost more in the lookup's own overhead than the scan does at that size.
    PlatformIntegrationX11::Tracked* PlatformIntegrationX11::windowFor
        (
        unsigned long aWindowId   //!< The X11 Window id from the event.
        )
    {
        for( Tracked& entry : mWindows )
        {
            if( entry.mWindowId == aWindowId )
            {
                return &entry;
            }
        }

        return nullptr;
    }
}
