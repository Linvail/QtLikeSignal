// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The Wayland backend: connecting, creating a surface on either shell, joining the loop, and
//! turning the seat's events into WindowSystemInterface calls.
//!
//! Ported from GGL's Wayland library. The protocol work is the same work; what changed is that the
//! reading is driven by this library's event loop instead of by a poll() of its own, and that input
//! is reported through WindowSystemInterface instead of through boost signals.

#include "QtLikeSignalGui/PlatformIntegrationWayland.hpp"

#include "QtLikeSignalGui/Window.hpp"
#include "QtLikeSignalGui/KeyTranslation.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <poll.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include <libdecor.h>

// Generated beside this file by wayland-scanner; see the wscript. xdg-shell comes from
// wayland-protocols, ivi-application from the copy vendored under deps/.
#include "ivi-application-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace QtLikeSignalGui
{
    //! Every Wayland handle this backend owns.
    struct PlatformIntegrationWayland::Internals
    {
        wl_display* mDisplay { nullptr };         //!< The compositor connection.
        wl_registry* mRegistry { nullptr };       //!< The global registry.
        wl_compositor* mCompositor { nullptr };   //!< Makes surfaces.
        wl_shm* mShm { nullptr };                 //!< Shared memory, needed by the cursor theme.
        wl_seat* mSeat { nullptr };               //!< The input seat.
        xdg_wm_base* mWmBase { nullptr };         //!< The desktop shell, when the compositor has it.
        ivi_application* mIvi { nullptr };        //!< The automotive shell, when it has that.

        wl_pointer* mPointer { nullptr };         //!< Pointer, while the seat reports one.
        wl_keyboard* mKeyboard { nullptr };       //!< Keyboard, while the seat reports one.
        wl_touch* mTouch { nullptr };             //!< Touch, while the seat reports one.

        wl_surface* mSurface { nullptr };         //!< The window's surface.
        xdg_surface* mXdgSurface { nullptr };     //!< Its xdg role, on the desktop shell.
        xdg_toplevel* mXdgToplevel { nullptr };   //!< Its toplevel, on the desktop shell.
        ivi_surface* mIviSurface { nullptr };     //!< Its ivi role, on the automotive shell.

        libdecor* mDecor { nullptr };             //!< The decoration context, when libdecor loaded.
        libdecor_frame* mDecorFrame { nullptr };  //!< The window's decoration frame.
        wl_callback* mDecorReadyCallback { nullptr };   //!< Sync that reports libdecor is ready.
        bool mDecorReady { false };               //!< Set by that sync.

        wl_cursor_theme* mCursorTheme { nullptr };   //!< Where the pointer image comes from.
        wl_surface* mCursorSurface { nullptr };      //!< The surface the pointer image is on.

        //! The most recent enter serial, which wl_pointer_set_cursor has to quote back.
        std::uint32_t mPointerEnterSerial { 0 };

        //! True while the pointer is over our surface.
        bool mPointerOver { false };

        //! Where the pointer is, in surface coordinates.
        double mPointerX { 0.0 };
        double mPointerY { 0.0 };

        //! Every mouse button currently held.
        //!
        //! Accumulated from the button events, because wl_pointer reports the change rather than
        //! the resulting state -- the opposite of what MouseEvent::mButtons has to carry. The DRM
        //! backend keeps the same running total for the same reason.
        MouseButtons mButtons;

        //! Every keyboard modifier currently held.
        //!
        //! Accumulated from the modifier keys themselves rather than read from wl_keyboard::
        //! modifiers, whose masks are indices into a keymap this library never parses. See
        //! KeyTranslation.hpp; the DRM backend does the same thing for the same reason.
        KeyModifiers mModifiers;

        //! True while our surface has keyboard focus, as wl_keyboard::enter and leave report it.
        bool mKeyboardFocus { false };

        //! The surface size, as this backend last reported it.
        int mWidth { 0 };
        int mHeight { 0 };

        //! The app id and title, kept because libdecor and xdg both want them set after creation.
        std::string mAppId;
        std::string mTitle;
    };

    namespace
    {
        //! Gets the running thread's dispatcher as an EventDispatcherLinux, or null if it is not one.
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

        //! Reads an environment variable, returning an empty string when it is unset or empty.
        std::string environmentValue
            (
            const char* aName   //!< Variable to read.
            )
        {
            const char* const value = std::getenv( aName );
            return ( value != nullptr ) ? std::string( value ) : std::string();
        }

        //! Returns false when the caller has asked for an undecorated window.
        //!
        //! Wayland has no server-side decoration, so a plain xdg-shell toplevel is a bare rectangle
        //! with no title bar and no close button. libdecor draws one, which is what a desktop wants
        //! and what GGL always does -- but a kiosk or an automotive target does not want a title bar
        //! at all, and neither does anything running full screen.
        //!
        //! An environment variable rather than a WindowSettings field because it is a property of
        //! the machine the program was put on, not of the window the program asked for: the same
        //! binary wants decoration on a developer's desktop and none on the target.
        bool decorationsWanted()
        {
            return environmentValue( "QTLIKESIGNAL_WAYLAND_DECORATIONS" ) != "0";
        }

        //! Maps an evdev button code to the button this library reports.
        //!
        //! wl_pointer passes the kernel's codes through unchanged, so these are the same names the
        //! DRM backend maps -- which is the whole reason both arrive at MouseButton the same way.
        MouseButton buttonOf
            (
            std::uint32_t aCode   //!< The evdev button code.
            )
        {
            switch( aCode )
            {
            case BTN_LEFT:
                return MouseButton::Left;

            case BTN_MIDDLE:
                return MouseButton::Middle;

            case BTN_RIGHT:
                return MouseButton::Right;

            case BTN_SIDE:
                return MouseButton::Extra1;

            case BTN_EXTRA:
                return MouseButton::Extra2;

            default:
                return MouseButton::None;
            }
        }

        //! One wheel detent, in the units wl_pointer reports axis motion in.
        //!
        //! Wayland has no notion of a detent: wl_pointer::axis carries a length in surface
        //! coordinates, and ten of those is what a discrete wheel step has conventionally meant
        //! since Weston. GGL divides by the same ten.
        const double kWaylandUnitsPerDetent = 10.0;

        //! One wheel detent in WheelEvent's units, which are Windows' WHEEL_DELTA.
        const double kWheelDeltaPerDetent = 120.0;
    }

    //! The C callbacks libwayland dispatches through, and the listener tables that name them.
    //!
    //! A struct of statics rather than free functions, so that one friend declaration on the backend
    //! covers all of them. Every one takes the backend as its user data.
    struct WaylandListeners
    {
        //-------------------------------------------------------------------------------------
        // Registry
        //-------------------------------------------------------------------------------------

        //! Binds the globals this backend needs, ignoring the rest.
        static void registryGlobal
            (
            void* aData,             //!< The backend.
            wl_registry* aRegistry,  //!< The registry announcing.
            std::uint32_t aName,     //!< The global's name.
            const char* aInterface,  //!< Its interface name.
            std::uint32_t aVersion   //!< The version the compositor offers.
            )
        {
            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            if( std::strcmp( aInterface, wl_compositor_interface.name ) == 0 )
            {
                internals.mCompositor = static_cast<wl_compositor*>( wl_registry_bind( aRegistry,
                    aName, &wl_compositor_interface, std::min( 3u, aVersion ) ) );
            }
            else if( std::strcmp( aInterface, wl_shm_interface.name ) == 0 )
            {
                internals.mShm = static_cast<wl_shm*>( wl_registry_bind( aRegistry, aName,
                    &wl_shm_interface, 1 ) );
            }
            else if( std::strcmp( aInterface, xdg_wm_base_interface.name ) == 0 )
            {
                internals.mWmBase = static_cast<xdg_wm_base*>( wl_registry_bind( aRegistry, aName,
                    &xdg_wm_base_interface, 1 ) );
                xdg_wm_base_add_listener( internals.mWmBase, &kWmBaseListener, self );
            }
            else if( std::strcmp( aInterface, wl_seat_interface.name ) == 0 )
            {
                internals.mSeat = static_cast<wl_seat*>( wl_registry_bind( aRegistry, aName,
                    &wl_seat_interface, std::min( 4u, aVersion ) ) );
                wl_seat_add_listener( internals.mSeat, &kSeatListener, self );
            }
            else if( std::strcmp( aInterface, ivi_application_interface.name ) == 0 )
            {
                internals.mIvi = static_cast<ivi_application*>( wl_registry_bind( aRegistry, aName,
                    &ivi_application_interface, 1 ) );
            }
        }

        //! A global went away. Nothing here holds one whose loss is survivable, so this is silent.
        static void registryGlobalRemove
            (
            void* aData,
            wl_registry* aRegistry,
            std::uint32_t aName
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aRegistry );
            static_cast<void>( aName );
        }

        //-------------------------------------------------------------------------------------
        // xdg_wm_base
        //-------------------------------------------------------------------------------------

        //! Answers the compositor's liveness check. Not answering gets the client killed.
        static void wmBasePing
            (
            void* aData,
            xdg_wm_base* aWmBase,
            std::uint32_t aSerial
            )
        {
            static_cast<void>( aData );
            xdg_wm_base_pong( aWmBase, aSerial );
        }

        //-------------------------------------------------------------------------------------
        // xdg_surface / xdg_toplevel
        //-------------------------------------------------------------------------------------

        //! Acknowledges a configure. The compositor waits for this before considering us mapped.
        static void xdgSurfaceConfigure
            (
            void* aData,
            xdg_surface* aSurface,
            std::uint32_t aSerial
            )
        {
            xdg_surface_ack_configure( aSurface, aSerial );

            // The surface has just been (re)configured, so whatever is on it is stale. Everywhere
            // else this arrives as an expose; here it is the only thing that resembles one.
            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            if( self->mWindow != nullptr )
            {
                WindowSystemInterface::handleExpose( self->mWindow );
            }
        }

        //! The compositor's size suggestion. Ignored, as GGL ignores it: the window is fixed size.
        static void xdgToplevelConfigure
            (
            void* aData,
            xdg_toplevel* aToplevel,
            std::int32_t aWidth,
            std::int32_t aHeight,
            wl_array* aStates
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aToplevel );
            static_cast<void>( aWidth );
            static_cast<void>( aHeight );
            static_cast<void>( aStates );
        }

        //! The close button, or the compositor asking us to go away.
        static void xdgToplevelClose
            (
            void* aData,
            xdg_toplevel* aToplevel
            )
        {
            static_cast<void>( aToplevel );
            static_cast<PlatformIntegrationWayland*>( aData )->requestClose();
        }

        //-------------------------------------------------------------------------------------
        // ivi_surface
        //-------------------------------------------------------------------------------------

        //! The ivi controller's size hint.
        //!
        //! Deliberately ignored, and GGL's comment explains why better than a shorter one could:
        //! the ivi configure event is only a hint that the client may ignore, and the shell
        //! controller downscales the buffer through its own source-to-destination mapping. Resizing
        //! the surface from it would shrink the rendering while the application still lays out at
        //! its native size, giving a cropped picture rather than a scaled one.
        static void iviSurfaceConfigure
            (
            void* aData,
            ivi_surface* aSurface,
            std::int32_t aWidth,
            std::int32_t aHeight
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aSurface );
            static_cast<void>( aWidth );
            static_cast<void>( aHeight );
        }

        //-------------------------------------------------------------------------------------
        // libdecor
        //-------------------------------------------------------------------------------------

        //! Reports a decoration error. Nothing to do but say so; the window still works undecorated.
        static void decorError
            (
            libdecor* aContext,
            libdecor_error aError,
            const char* aMessage
            )
        {
            static_cast<void>( aContext );
            std::fprintf( stderr, "QtLikeSignalGui: libdecor error %d: %s\n", static_cast<int>( aError ),
                aMessage );
        }

        //! Records that libdecor has finished binding its own globals.
        static void decorReady
            (
            void* aData,
            wl_callback* aCallback,
            std::uint32_t aTime
            )
        {
            static_cast<void>( aTime );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            self->mInternals->mDecorReady = true;

            wl_callback_destroy( aCallback );
            self->mInternals->mDecorReadyCallback = nullptr;
        }

        //! Applies a decoration configure, and reports the resize it implies.
        static void decorFrameConfigure
            (
            libdecor_frame* aFrame,
            libdecor_configuration* aConfiguration,
            void* aData
            )
        {
            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            int width = 0;
            int height = 0;
            if( !libdecor_configuration_get_content_size( aConfiguration, aFrame, &width,
                &height ) )
            {
                width  = internals.mWidth;
                height = internals.mHeight;
            }

            libdecor_state* const state = libdecor_state_new( width, height );
            libdecor_frame_commit( aFrame, state, aConfiguration );
            libdecor_state_free( state );

            self->reportResize( width, height );
        }

        //! The decoration's close button.
        static void decorFrameClose
            (
            libdecor_frame* aFrame,
            void* aData
            )
        {
            static_cast<void>( aFrame );
            static_cast<PlatformIntegrationWayland*>( aData )->requestClose();
        }

        //! libdecor asking us to commit the surface, which is the only place this backend does.
        static void decorFrameCommit
            (
            libdecor_frame* aFrame,
            void* aData
            )
        {
            static_cast<void>( aFrame );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            if( self->mInternals->mSurface != nullptr )
            {
                wl_surface_commit( self->mInternals->mSurface );
            }
        }

        //! A popup was dismissed. This backend has no popups.
        static void decorFrameDismissPopup
            (
            libdecor_frame* aFrame,
            const char* aSeatName,
            void* aData
            )
        {
            static_cast<void>( aFrame );
            static_cast<void>( aSeatName );
            static_cast<void>( aData );
        }

        //-------------------------------------------------------------------------------------
        // Seat
        //-------------------------------------------------------------------------------------

        //! Takes or drops the pointer, keyboard and touch objects as the seat gains or loses them.
        //!
        //! The keyboard is taken even though its keymap is closed unread: the key events themselves
        //! are what this library reports, and they need no keymap. See keyboardKeymap() for what
        //! declining to read it costs.
        static void seatCapabilities
            (
            void* aData,
            wl_seat* aSeat,
            std::uint32_t aCapabilities
            )
        {
            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            const bool hasPointer = ( aCapabilities & WL_SEAT_CAPABILITY_POINTER ) != 0;
            if( hasPointer && internals.mPointer == nullptr )
            {
                internals.mPointer = wl_seat_get_pointer( aSeat );
                wl_pointer_add_listener( internals.mPointer, &kPointerListener, self );
            }
            else if( !hasPointer && internals.mPointer != nullptr )
            {
                wl_pointer_destroy( internals.mPointer );
                internals.mPointer = nullptr;
            }

            const bool hasKeyboard = ( aCapabilities & WL_SEAT_CAPABILITY_KEYBOARD ) != 0;
            if( hasKeyboard && internals.mKeyboard == nullptr )
            {
                internals.mKeyboard = wl_seat_get_keyboard( aSeat );
                wl_keyboard_add_listener( internals.mKeyboard, &kKeyboardListener, self );
            }
            else if( !hasKeyboard && internals.mKeyboard != nullptr )
            {
                wl_keyboard_destroy( internals.mKeyboard );
                internals.mKeyboard = nullptr;
            }

            const bool hasTouch = ( aCapabilities & WL_SEAT_CAPABILITY_TOUCH ) != 0;
            if( hasTouch && internals.mTouch == nullptr )
            {
                internals.mTouch = wl_seat_get_touch( aSeat );
                wl_touch_add_listener( internals.mTouch, &kTouchListener, self );
            }
            else if( !hasTouch && internals.mTouch != nullptr )
            {
                wl_touch_destroy( internals.mTouch );
                internals.mTouch = nullptr;
            }
        }

        //-------------------------------------------------------------------------------------
        // wl_keyboard
        //-------------------------------------------------------------------------------------

        //! The compositor's keymap arrives here, and this backend closes it unread.
        //!
        //! **Deliberate, and the reason there is no text on this platform.** Reading it means
        //! xkbcommon: the file is an XKB keymap, and turning a key code plus a modifier state into
        //! a character is exactly what that library is for. Taking the dependency would buy text
        //! input and a modifier state read straight from the compositor; not taking it costs those
        //! two things and nothing else, because key *identity* is positional and needs no keymap.
        //!
        //! The descriptor is closed either way. Leaving it open leaks one per keymap change, and
        //! compositors send a fresh one whenever the layout is switched.
        static void keyboardKeymap
            (
            void* aData,
            wl_keyboard* aKeyboard,
            std::uint32_t aFormat,
            std::int32_t aFd,
            std::uint32_t aSize
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aKeyboard );
            static_cast<void>( aFormat );
            static_cast<void>( aSize );

            if( aFd >= 0 )
            {
                close( aFd );
            }
        }

        //! Our surface gained keyboard focus.
        //!
        //! The compositor hands over the keys already held, which is the one moment this backend
        //! can learn about a modifier pressed before the window was focused. Rebuilding the set
        //! from that array is what keeps Alt-Tab from leaving Alt stuck on.
        static void keyboardEnter
            (
            void* aData,
            wl_keyboard* aKeyboard,
            std::uint32_t aSerial,
            wl_surface* aSurface,
            wl_array* aKeys
            )
        {
            static_cast<void>( aKeyboard );
            static_cast<void>( aSerial );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            if( aSurface != internals.mSurface )
            {
                return;
            }

            internals.mKeyboardFocus = true;
            internals.mModifiers     = KeyModifiers();

            if( aKeys != nullptr )
            {
                const std::uint32_t* const codes =
                    static_cast<const std::uint32_t*>( aKeys->data );
                const std::size_t count = aKeys->size / sizeof( std::uint32_t );

                for( std::size_t index = 0; index < count; ++index )
                {
                    const KeyModifier modifier = modifierForKey(
                        keyFromEvdevCode( codes[index] ) );
                    if( modifier != KeyModifier::None )
                    {
                        internals.mModifiers |= modifier;
                    }
                }
            }

            WindowSystemInterface::handleFocusChange( self->mWindow, true );
        }

        //! Our surface lost keyboard focus.
        //!
        //! The modifier set is cleared rather than kept: the keys may well be released while
        //! somebody else has focus, and this backend would never hear about it. An empty set is
        //! wrong for as long as a modifier is genuinely still held; a stale one is wrong until the
        //! program is restarted.
        static void keyboardLeave
            (
            void* aData,
            wl_keyboard* aKeyboard,
            std::uint32_t aSerial,
            wl_surface* aSurface
            )
        {
            static_cast<void>( aKeyboard );
            static_cast<void>( aSerial );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            if( aSurface != internals.mSurface )
            {
                return;
            }

            internals.mKeyboardFocus = false;
            internals.mModifiers     = KeyModifiers();

            WindowSystemInterface::handleFocusChange( self->mWindow, false );
        }

        //! One key going down or coming up.
        static void keyboardKey
            (
            void* aData,
            wl_keyboard* aKeyboard,
            std::uint32_t aSerial,
            std::uint32_t aTime,
            std::uint32_t aKey,
            std::uint32_t aState
            )
        {
            static_cast<void>( aKeyboard );
            static_cast<void>( aSerial );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            // aKey is the Linux evdev code as it stands. The protocol's well-known "+ 8" converts
            // it to an XKB scancode, which is a different thing and not what is wanted here -- so
            // it is deliberately not applied.
            KeyEvent event;
            event.mKey         = keyFromEvdevCode( static_cast<unsigned int>( aKey ) );
            event.mNativeCode  = static_cast<unsigned int>( aKey );
            event.mTimestampMs = static_cast<unsigned long>( aTime );

            const bool pressed = ( aState == WL_KEYBOARD_KEY_STATE_PRESSED );

            const KeyModifier own = modifierForKey( event.mKey );
            if( own != KeyModifier::None )
            {
                if( pressed )
                {
                    internals.mModifiers |= own;
                }
                else
                {
                    internals.mModifiers.remove( own );
                }
            }

            event.mModifiers = internals.mModifiers;

            // No repeats are synthesised. wl_keyboard::repeat_info tells a client the rate and
            // delay to repeat at and expects the *client* to run the timer; a library that did that
            // silently would be inventing events the compositor never sent, so this reports what
            // arrived and leaves the policy to whoever wants it.
            if( pressed )
            {
                WindowSystemInterface::handleKeyPressed( self->mWindow, event );
            }
            else
            {
                WindowSystemInterface::handleKeyReleased( self->mWindow, event );
            }
        }

        //! The compositor's view of the modifier state, which this backend cannot read.
        //!
        //! The masks are indices into the keymap sent by keyboardKeymap(), and that keymap was
        //! closed unread -- so there is nothing here to interpret them against. Tracked from the
        //! keys themselves instead; see keyboardKey().
        static void keyboardModifiers
            (
            void* aData,
            wl_keyboard* aKeyboard,
            std::uint32_t aSerial,
            std::uint32_t aDepressed,
            std::uint32_t aLatched,
            std::uint32_t aLocked,
            std::uint32_t aGroup
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aKeyboard );
            static_cast<void>( aSerial );
            static_cast<void>( aDepressed );
            static_cast<void>( aLatched );
            static_cast<void>( aLocked );
            static_cast<void>( aGroup );
        }

        //! How fast the compositor would like repeats. Not acted on; see keyboardKey().
        static void keyboardRepeatInfo
            (
            void* aData,
            wl_keyboard* aKeyboard,
            std::int32_t aRate,
            std::int32_t aDelay
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aKeyboard );
            static_cast<void>( aRate );
            static_cast<void>( aDelay );
        }

        //-------------------------------------------------------------------------------------

                //! The seat's human-readable name. Nothing here uses it.
        static void seatName
            (
            void* aData,
            wl_seat* aSeat,
            const char* aName
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aSeat );
            static_cast<void>( aName );
        }

        //-------------------------------------------------------------------------------------
        // Pointer
        //-------------------------------------------------------------------------------------

        //! The pointer arrived over a surface.
        static void pointerEnter
            (
            void* aData,
            wl_pointer* aPointer,
            std::uint32_t aSerial,
            wl_surface* aSurface,
            wl_fixed_t aX,
            wl_fixed_t aY
            )
        {
            static_cast<void>( aPointer );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            // Null after our own surface has been destroyed, and not ours if the compositor hands
            // us a surface some other part of the process owns.
            if( aSurface == nullptr || aSurface != internals.mSurface )
            {
                return;
            }

            internals.mPointerEnterSerial = aSerial;
            internals.mPointerOver        = true;
            internals.mPointerX           = wl_fixed_to_double( aX );
            internals.mPointerY           = wl_fixed_to_double( aY );

            self->applyCursor();

            WindowSystemInterface::handleMouseEntered( self->mWindow, self->buildMouseEvent(
                MouseButton::None ) );
        }

        //! The pointer left.
        static void pointerLeave
            (
            void* aData,
            wl_pointer* aPointer,
            std::uint32_t aSerial,
            wl_surface* aSurface
            )
        {
            static_cast<void>( aPointer );
            static_cast<void>( aSerial );
            static_cast<void>( aSurface );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            if( !self->mInternals->mPointerOver )
            {
                return;
            }

            self->mInternals->mPointerOver = false;
            WindowSystemInterface::handleMouseLeft( self->mWindow );
        }

        //! The pointer moved.
        static void pointerMotion
            (
            void* aData,
            wl_pointer* aPointer,
            std::uint32_t aTime,
            wl_fixed_t aX,
            wl_fixed_t aY
            )
        {
            static_cast<void>( aPointer );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            internals.mPointerX = wl_fixed_to_double( aX );
            internals.mPointerY = wl_fixed_to_double( aY );

            MouseEvent event = self->buildMouseEvent( MouseButton::None );
            event.mTimestampMs = aTime;
            WindowSystemInterface::handleMouseMoved( self->mWindow, event );
        }

        //! A pointer button changed.
        static void pointerButton
            (
            void* aData,
            wl_pointer* aPointer,
            std::uint32_t aSerial,
            std::uint32_t aTime,
            std::uint32_t aButton,
            std::uint32_t aState
            )
        {
            static_cast<void>( aPointer );
            static_cast<void>( aSerial );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            const MouseButton button = buttonOf( aButton );
            if( button == MouseButton::None )
            {
                // A button this library has no name for. Dropped rather than reported as None,
                // which every receiver would read as a move.
                return;
            }

            const bool pressed = ( aState == WL_POINTER_BUTTON_STATE_PRESSED );

            // Updated before the event is built, so mButtons carries the state *after* the change,
            // which is what MouseEvent documents and what the other backends produce.
            if( pressed )
            {
                internals.mButtons |= button;
            }
            else
            {
                internals.mButtons.remove( button );
            }

            MouseEvent event = self->buildMouseEvent( button );
            event.mTimestampMs = aTime;

            if( pressed )
            {
                WindowSystemInterface::handleMousePressed( self->mWindow, event );
            }
            else
            {
                WindowSystemInterface::handleMouseReleased( self->mWindow, event );
            }
        }

        //! The wheel or a touchpad scrolled.
        static void pointerAxis
            (
            void* aData,
            wl_pointer* aPointer,
            std::uint32_t aTime,
            std::uint32_t aAxis,
            wl_fixed_t aValue
            )
        {
            static_cast<void>( aPointer );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );
            PlatformIntegrationWayland::Internals& internals = *self->mInternals;

            const double detents = wl_fixed_to_double( aValue ) / kWaylandUnitsPerDetent;
            const int delta = static_cast<int>( detents * kWheelDeltaPerDetent );

            WheelEvent wheel;
            wheel.mPos.mX      = static_cast<int>( internals.mPointerX );
            wheel.mPos.mY      = static_cast<int>( internals.mPointerY );
            wheel.mGlobalPos   = wheel.mPos;
            wheel.mButtons     = internals.mButtons;
            wheel.mTimestampMs = aTime;

            if( aAxis == WL_POINTER_AXIS_VERTICAL_SCROLL )
            {
                // Negated. Wayland counts a scroll *towards* the user as positive; WheelEvent
                // counts it as negative, matching Windows and X11 button 4. GGL negates the
                // horizontal axis too, to suit a GLFW-shaped API; this one does not, because
                // WheelEvent and Wayland already agree that positive is to the right.
                wheel.mAngleDeltaY = -delta;
            }
            else if( aAxis == WL_POINTER_AXIS_HORIZONTAL_SCROLL )
            {
                wheel.mAngleDeltaX = delta;
            }

            if( wheel.mAngleDeltaX == 0 && wheel.mAngleDeltaY == 0 )
            {
                return;
            }

            WindowSystemInterface::handleWheel( self->mWindow, wheel );
        }

        //-------------------------------------------------------------------------------------
        // Touch
        //-------------------------------------------------------------------------------------

        //! A finger touched down.
        static void touchDown
            (
            void* aData,
            wl_touch* aTouch,
            std::uint32_t aSerial,
            std::uint32_t aTime,
            wl_surface* aSurface,
            std::int32_t aId,
            wl_fixed_t aX,
            wl_fixed_t aY
            )
        {
            static_cast<void>( aTouch );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );

            if( aSurface == nullptr || aSurface != self->mInternals->mSurface )
            {
                return;
            }

            TouchDownEvent down;
            down.mSerial = aSerial;
            down.mTimeMs = aTime;
            down.mId     = aId;
            down.mX      = wl_fixed_to_double( aX );
            down.mY      = wl_fixed_to_double( aY );

            WindowSystemInterface::handleTouchDown( self->mWindow, down );
        }

        //! A finger lifted.
        static void touchUp
            (
            void* aData,
            wl_touch* aTouch,
            std::uint32_t aSerial,
            std::uint32_t aTime,
            std::int32_t aId
            )
        {
            static_cast<void>( aTouch );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );

            TouchUpEvent up;
            up.mSerial = aSerial;
            up.mTimeMs = aTime;
            up.mId     = aId;

            WindowSystemInterface::handleTouchUp( self->mWindow, up );
        }

        //! A finger moved.
        static void touchMotion
            (
            void* aData,
            wl_touch* aTouch,
            std::uint32_t aTime,
            std::int32_t aId,
            wl_fixed_t aX,
            wl_fixed_t aY
            )
        {
            static_cast<void>( aTouch );

            PlatformIntegrationWayland* const self =
                static_cast<PlatformIntegrationWayland*>( aData );

            TouchMotionEvent motion;
            motion.mTimeMs = aTime;
            motion.mId     = aId;
            motion.mX      = wl_fixed_to_double( aX );
            motion.mY      = wl_fixed_to_double( aY );

            WindowSystemInterface::handleTouchMotion( self->mWindow, motion );
        }

        //! The points reported since the last frame now form a consistent set.
        static void touchFrame
            (
            void* aData,
            wl_touch* aTouch
            )
        {
            static_cast<void>( aTouch );
            WindowSystemInterface::handleTouchFrame(
                static_cast<PlatformIntegrationWayland*>( aData )->mWindow );
        }

        //! The compositor took the sequence away.
        static void touchCancel
            (
            void* aData,
            wl_touch* aTouch
            )
        {
            static_cast<void>( aTouch );
            WindowSystemInterface::handleTouchCancel(
                static_cast<PlatformIntegrationWayland*>( aData )->mWindow );
        }

        static const wl_registry_listener kRegistryListener;
        static const xdg_wm_base_listener kWmBaseListener;
        static const xdg_surface_listener kXdgSurfaceListener;
        static const xdg_toplevel_listener kXdgToplevelListener;
        static const ivi_surface_listener kIviSurfaceListener;
        static const wl_seat_listener kSeatListener;
        static const wl_pointer_listener kPointerListener;
        static const wl_keyboard_listener kKeyboardListener;
        static const wl_touch_listener kTouchListener;
        static const wl_callback_listener kDecorReadyListener;
        static libdecor_interface kDecorInterface;
        static libdecor_frame_interface kDecorFrameInterface;
    };

    const wl_registry_listener WaylandListeners::kRegistryListener =
    {
        &WaylandListeners::registryGlobal,
        &WaylandListeners::registryGlobalRemove
    };

    const xdg_wm_base_listener WaylandListeners::kWmBaseListener =
    {
        &WaylandListeners::wmBasePing
    };

    const xdg_surface_listener WaylandListeners::kXdgSurfaceListener =
    {
        &WaylandListeners::xdgSurfaceConfigure
    };

    const xdg_toplevel_listener WaylandListeners::kXdgToplevelListener =
    {
        &WaylandListeners::xdgToplevelConfigure,
        &WaylandListeners::xdgToplevelClose
    };

    const ivi_surface_listener WaylandListeners::kIviSurfaceListener =
    {
        &WaylandListeners::iviSurfaceConfigure
    };

    const wl_seat_listener WaylandListeners::kSeatListener =
    {
        &WaylandListeners::seatCapabilities,
        &WaylandListeners::seatName
    };

    const wl_pointer_listener WaylandListeners::kPointerListener =
    {
        &WaylandListeners::pointerEnter,
        &WaylandListeners::pointerLeave,
        &WaylandListeners::pointerMotion,
        &WaylandListeners::pointerButton,
        &WaylandListeners::pointerAxis
    };

    const wl_keyboard_listener WaylandListeners::kKeyboardListener =
    {
        &WaylandListeners::keyboardKeymap,
        &WaylandListeners::keyboardEnter,
        &WaylandListeners::keyboardLeave,
        &WaylandListeners::keyboardKey,
        &WaylandListeners::keyboardModifiers,
        &WaylandListeners::keyboardRepeatInfo
    };

    const wl_touch_listener WaylandListeners::kTouchListener =
    {
        &WaylandListeners::touchDown,
        &WaylandListeners::touchUp,
        &WaylandListeners::touchMotion,
        &WaylandListeners::touchFrame,
        &WaylandListeners::touchCancel
    };

    const wl_callback_listener WaylandListeners::kDecorReadyListener =
    {
        &WaylandListeners::decorReady
    };

    //! Only the error callback is named: libdecor's remaining members are reserved slots, and
    //! leaving them out value-initialises them to null, which is what reserved means.
    libdecor_interface WaylandListeners::kDecorInterface =
    {
        &WaylandListeners::decorError
    };

    libdecor_frame_interface WaylandListeners::kDecorFrameInterface =
    {
        &WaylandListeners::decorFrameConfigure,
        &WaylandListeners::decorFrameClose,
        &WaylandListeners::decorFrameCommit,
        &WaylandListeners::decorFrameDismissPopup
    };

    //! Constructs the backend. Nothing is connected until a window is created.
    PlatformIntegrationWayland::PlatformIntegrationWayland()
        : mInternals( new Internals() )
    {
    }

    //! Tears the connection down, in the order the protocol requires.
    PlatformIntegrationWayland::~PlatformIntegrationWayland()
    {
        unregisterConnection();

        Internals& internals = *mInternals;

        // Before anything is destroyed. libdecor is not finished initialising when libdecor_new()
        // returns, and unref-ing it mid-flight is a crash inside libdecor rather than an error it
        // reports -- which is what happened here until this call was added, on the path where a
        // connection is opened and no window is ever created. GGL waits in the same place for the
        // same reason.
        waitForDecorReady();

        if( internals.mDecorReadyCallback != nullptr )
        {
            // Still outstanding, so the wait above gave up rather than being answered. The callback
            // is ours and libdecor will not free it.
            wl_callback_destroy( internals.mDecorReadyCallback );
            internals.mDecorReadyCallback = nullptr;
        }

        if( internals.mCursorTheme != nullptr )
        {
            wl_cursor_theme_destroy( internals.mCursorTheme );
        }
        if( internals.mCursorSurface != nullptr )
        {
            wl_surface_destroy( internals.mCursorSurface );
        }

        // The seat's children before the seat, which is the order the protocol documents.
        if( internals.mPointer != nullptr )
        {
            wl_pointer_destroy( internals.mPointer );
        }
        if( internals.mKeyboard != nullptr )
        {
            wl_keyboard_destroy( internals.mKeyboard );
        }
        if( internals.mTouch != nullptr )
        {
            wl_touch_destroy( internals.mTouch );
        }
        if( internals.mSeat != nullptr )
        {
            wl_seat_destroy( internals.mSeat );
        }

        if( internals.mIvi != nullptr )
        {
            ivi_application_destroy( internals.mIvi );
        }
        if( internals.mDecor != nullptr )
        {
            libdecor_unref( internals.mDecor );
        }
        if( internals.mWmBase != nullptr )
        {
            xdg_wm_base_destroy( internals.mWmBase );
        }
        if( internals.mShm != nullptr )
        {
            wl_shm_destroy( internals.mShm );
        }
        if( internals.mCompositor != nullptr )
        {
            wl_compositor_destroy( internals.mCompositor );
        }
        if( internals.mRegistry != nullptr )
        {
            wl_registry_destroy( internals.mRegistry );
        }

        if( internals.mDisplay != nullptr )
        {
            wl_display_flush( internals.mDisplay );
            wl_display_disconnect( internals.mDisplay );
        }
    }

    //! Gets which window system this backend is.
    PlatformType PlatformIntegrationWayland::type() const
    {
        return PlatformType::Wayland;
    }

    //! Wayland windows are created here.
    bool PlatformIntegrationWayland::canCreateWindows() const
    {
        return true;
    }

    //! There is no adoption path: a wl_surface carries no identity this backend could look up.
    bool PlatformIntegrationWayland::canAdoptWindows() const
    {
        return false;
    }

    //! Gets the compositor connection, opening it if there is not one yet.
    //!
    //! Asking is what connects, for the reason the X11 backend's is: an EGL library has to reach
    //! eglGetPlatformDisplayEXT( EGL_PLATFORM_WAYLAND_EXT, display ) before there is a surface to
    //! hand it, so the connection has to exist before the window does.
    void* PlatformIntegrationWayland::nativeDisplay()
    {
        if( mInternals->mDisplay == nullptr && !connectDisplay() )
        {
            return nullptr;
        }

        return mInternals->mDisplay;
    }

    //! Connects to the compositor and binds the globals this backend needs.
    //!
    //! Two roundtrips, as GGL does: the first brings the registry's announcements, and the second
    //! brings the events those announcements' own listeners generate -- the seat's capabilities
    //! above all, which is what decides whether there is a pointer to listen to.
    //!
    //! @return true if the connection is usable.
    bool PlatformIntegrationWayland::connectDisplay()
    {
        Internals& internals = *mInternals;

        internals.mDisplay = wl_display_connect( nullptr );
        if( internals.mDisplay == nullptr )
        {
            std::fprintf( stderr,
                "QtLikeSignalGui: wl_display_connect() failed; is WAYLAND_DISPLAY set and a compositor "
                "running?\n" );
            return false;
        }

        internals.mRegistry = wl_display_get_registry( internals.mDisplay );
        wl_registry_add_listener( internals.mRegistry, &WaylandListeners::kRegistryListener, this );

        wl_display_roundtrip( internals.mDisplay );
        wl_display_roundtrip( internals.mDisplay );

        if( internals.mCompositor == nullptr )
        {
            std::fprintf( stderr, "QtLikeSignalGui: the compositor offers no wl_compositor\n" );
            return false;
        }

        if( internals.mWmBase == nullptr && internals.mIvi == nullptr )
        {
            std::fprintf( stderr,
                "QtLikeSignalGui: the compositor offers neither xdg-shell nor ivi-shell; at least one "
                "is needed to give a surface a role\n" );
            return false;
        }

        if( internals.mWmBase != nullptr && decorationsWanted() )
        {
            // libdecor draws the title bar and the close button. Wayland has no server-side
            // decoration to fall back on, so without this an xdg-shell window is a bare rectangle
            // the user cannot close. It depends on xdg_wm_base, which is why it is set up only
            // here and not on the ivi path -- an ivi surface has no decoration by design.
            internals.mDecor = libdecor_new( internals.mDisplay,
                &WaylandListeners::kDecorInterface );

            if( internals.mDecor != nullptr )
            {
                libdecor_dispatch( internals.mDecor, 0 );

                internals.mDecorReadyCallback = wl_display_sync( internals.mDisplay );
                wl_callback_add_listener( internals.mDecorReadyCallback,
                    &WaylandListeners::kDecorReadyListener, this );
            }
        }

        if( internals.mShm != nullptr )
        {
            // 24 pixels is the size every cursor theme ships, and the one GGL asks for. Without a
            // theme the pointer is simply invisible over our surface, because a Wayland client is
            // responsible for drawing its own.
            internals.mCursorTheme = wl_cursor_theme_load( nullptr, 24, internals.mShm );
            internals.mCursorSurface = wl_compositor_create_surface( internals.mCompositor );
        }

        return true;
    }

    //! Creates the surface and gives it a role on whichever shell is available.
    //!
    //! @return the new Window, or nullptr if the surface could not be created.
    Window* PlatformIntegrationWayland::createWindow
        (
        const WindowSettings& aSettings   //!< Requested size, title, app id and ivi id.
        )
    {
        if( mWindow != nullptr )
        {
            std::fprintf( stderr,
                "QtLikeSignalGui: the wayland backend supports one window, and one already exists\n" );
            return nullptr;
        }

        if( mInternals->mDisplay == nullptr && !connectDisplay() )
        {
            return nullptr;
        }

        Internals& internals = *mInternals;
        internals.mWidth  = std::max( 1, aSettings.mWidth );
        internals.mHeight = std::max( 1, aSettings.mHeight );
        internals.mAppId  = aSettings.mAppId;
        internals.mTitle  = aSettings.mTitle;

        internals.mSurface = wl_compositor_create_surface( internals.mCompositor );
        if( internals.mSurface == nullptr )
        {
            std::fprintf( stderr, "QtLikeSignalGui: wl_compositor_create_surface() failed\n" );
            return nullptr;
        }

        if( !createShellObjects( aSettings ) )
        {
            wl_surface_destroy( internals.mSurface );
            internals.mSurface = nullptr;
            return nullptr;
        }

        if( !registerConnection() )
        {
            return nullptr;
        }

        NativeWindow native;
        native.mDisplay = internals.mDisplay;
        native.mSurface = internals.mSurface;
        native.mWidth   = internals.mWidth;
        native.mHeight  = internals.mHeight;

        mWindow = newWindow( this, native, internals.mWidth, internals.mHeight );
        return mWindow;
    }

    //! Gives the surface a role: an ivi surface when one was asked for, an xdg toplevel otherwise.
    //!
    //! @return true if the surface has a role.
    bool PlatformIntegrationWayland::createShellObjects
        (
        const WindowSettings& aSettings   //!< The requested settings.
        )
    {
        Internals& internals = *mInternals;

        // The environment is consulted only when the caller left the id at zero, so that one binary
        // runs on a desktop and on a head unit without being rebuilt. GGL reads the same variable.
        int iviId = aSettings.mIviId;
        if( iviId == 0 )
        {
            const std::string fromEnvironment = environmentValue( "WAYLAND_IVI_ID" );
            if( !fromEnvironment.empty() )
            {
                iviId = std::atoi( fromEnvironment.c_str() );
            }
        }

        if( internals.mIvi != nullptr && iviId != 0 )
        {
            internals.mIviSurface = ivi_application_surface_create( internals.mIvi,
                static_cast<std::uint32_t>( iviId ), internals.mSurface );

            if( internals.mIviSurface == nullptr )
            {
                std::fprintf( stderr, "QtLikeSignalGui: ivi_application_surface_create() failed\n" );
                return false;
            }

            ivi_surface_add_listener( internals.mIviSurface,
                &WaylandListeners::kIviSurfaceListener, this );

            wl_surface_commit( internals.mSurface );
            wl_display_roundtrip( internals.mDisplay );
            return true;
        }

        if( internals.mDecor != nullptr )
        {
            // libdecor makes the xdg objects itself, so this path must not also make them, and it
            // cannot decorate anything until its own globals have arrived.
            waitForDecorReady();

            internals.mDecorFrame = libdecor_decorate( internals.mDecor, internals.mSurface,
                &WaylandListeners::kDecorFrameInterface, this );

            if( internals.mDecorFrame != nullptr )
            {
                libdecor_state* const state = libdecor_state_new( internals.mWidth,
                    internals.mHeight );
                libdecor_frame_commit( internals.mDecorFrame, state, nullptr );
                libdecor_state_free( state );

                if( !internals.mAppId.empty() )
                {
                    libdecor_frame_set_app_id( internals.mDecorFrame, internals.mAppId.c_str() );
                }
                libdecor_frame_set_title( internals.mDecorFrame, internals.mTitle.c_str() );

                // Fixed size, as GGL fixes it. The surface's size is the renderer's to choose, and
                // a compositor-driven resize would change it underneath a GL context sized for the
                // old one.
                libdecor_frame_set_min_content_size( internals.mDecorFrame, internals.mWidth,
                    internals.mHeight );
                libdecor_frame_set_max_content_size( internals.mDecorFrame, internals.mWidth,
                    internals.mHeight );
                libdecor_frame_unset_capabilities( internals.mDecorFrame, LIBDECOR_ACTION_RESIZE );
                libdecor_frame_unset_capabilities( internals.mDecorFrame,
                    LIBDECOR_ACTION_FULLSCREEN );
                libdecor_frame_unset_capabilities( internals.mDecorFrame,
                    LIBDECOR_ACTION_MINIMIZE );

                libdecor_frame_map( internals.mDecorFrame );
                wl_display_roundtrip( internals.mDisplay );
                return true;
            }

            std::fprintf( stderr,
                "QtLikeSignalGui: libdecor_decorate() failed; falling back to an undecorated window\n" );
        }

        if( internals.mWmBase == nullptr )
        {
            std::fprintf( stderr, "QtLikeSignalGui: no shell is available for this surface\n" );
            return false;
        }

        internals.mXdgSurface = xdg_wm_base_get_xdg_surface( internals.mWmBase,
            internals.mSurface );
        if( internals.mXdgSurface == nullptr )
        {
            std::fprintf( stderr, "QtLikeSignalGui: xdg_wm_base_get_xdg_surface() failed\n" );
            return false;
        }

        xdg_surface_add_listener( internals.mXdgSurface, &WaylandListeners::kXdgSurfaceListener,
            this );

        internals.mXdgToplevel = xdg_surface_get_toplevel( internals.mXdgSurface );
        if( internals.mXdgToplevel == nullptr )
        {
            std::fprintf( stderr, "QtLikeSignalGui: xdg_surface_get_toplevel() failed\n" );
            return false;
        }

        xdg_toplevel_add_listener( internals.mXdgToplevel,
            &WaylandListeners::kXdgToplevelListener, this );

        if( !internals.mAppId.empty() )
        {
            xdg_toplevel_set_app_id( internals.mXdgToplevel, internals.mAppId.c_str() );
        }
        xdg_toplevel_set_title( internals.mXdgToplevel, internals.mTitle.c_str() );

        xdg_toplevel_set_min_size( internals.mXdgToplevel, internals.mWidth, internals.mHeight );
        xdg_toplevel_set_max_size( internals.mXdgToplevel, internals.mWidth, internals.mHeight );

        wl_surface_commit( internals.mSurface );
        wl_display_roundtrip( internals.mDisplay );
        return true;
    }

    //! Waits until libdecor has finished binding its own globals and loading its plugin.
    //!
    //! libdecor_new() returns before either has happened, and until they have, the context can
    //! neither decorate a surface nor be torn down: libdecor_decorate() gets no decoration, and
    //! libdecor_unref() crashes. The sync callback set up in connectDisplay() is what reports that
    //! it is safe, and this is the wait for it.
    //!
    //! Bounded, unlike GGL's, which loops until ready. A destructor is one of the two callers, and
    //! a compositor that never answers would otherwise hang a program on its way out -- a worse
    //! failure than an undecorated window. Each pass blocks in a round trip rather than spinning,
    //! so the bound is on server replies, not on iterations of a busy loop.
    void PlatformIntegrationWayland::waitForDecorReady()
    {
        Internals& internals = *mInternals;

        if( internals.mDecor == nullptr || internals.mDisplay == nullptr )
        {
            return;
        }

        for( int attempt = 0; attempt < 64 && !internals.mDecorReady; ++attempt )
        {
            if( wl_display_roundtrip( internals.mDisplay ) < 0 )
            {
                break;
            }
        }
    }

    //! Sets the pointer image, which on Wayland is the client's job.
    void PlatformIntegrationWayland::applyCursor()
    {
        Internals& internals = *mInternals;

        if( internals.mPointer == nullptr || internals.mCursorTheme == nullptr
            || internals.mCursorSurface == nullptr )
        {
            return;
        }

        wl_cursor* const cursor = wl_cursor_theme_get_cursor( internals.mCursorTheme, "left_ptr" );
        if( cursor == nullptr || cursor->image_count == 0 )
        {
            return;
        }

        wl_cursor_image* const image = cursor->images[0];
        wl_buffer* const buffer = wl_cursor_image_get_buffer( image );
        if( buffer == nullptr )
        {
            return;
        }

        // Only the first frame, unlike GGL, which drives an animated cursor from a timerfd. The
        // pointer this library needs is the arrow, which has one frame, and an animation would mean
        // a second descriptor in the loop for a cosmetic detail.
        wl_pointer_set_cursor( internals.mPointer, internals.mPointerEnterSerial,
            internals.mCursorSurface, static_cast<std::int32_t>( image->hotspot_x ),
            static_cast<std::int32_t>( image->hotspot_y ) );
        wl_surface_set_buffer_scale( internals.mCursorSurface, 1 );
        wl_surface_attach( internals.mCursorSurface, buffer, 0, 0 );
        wl_surface_damage( internals.mCursorSurface, 0, 0,
            static_cast<std::int32_t>( image->width ), static_cast<std::int32_t>( image->height ) );
        wl_surface_commit( internals.mCursorSurface );
    }

    //! Builds a MouseEvent from the pointer state this backend keeps.
    //!
    //! Wayland reports a position only when it changes and a button only when it changes, so every
    //! event has to be assembled from what was last said rather than read whole out of the event --
    //! which is why the position and the held-button set are members.
    MouseEvent PlatformIntegrationWayland::buildMouseEvent
        (
        MouseButton aChanged   //!< The button that changed, or None.
        ) const
    {
        const Internals& internals = *mInternals;

        MouseEvent event;
        event.mPos.mX  = static_cast<int>( internals.mPointerX );
        event.mPos.mY  = static_cast<int>( internals.mPointerY );

        // The same numbers. A Wayland client is never told where its surface is on screen, so there
        // is no global position to report and pretending otherwise would invent one.
        event.mGlobalPos = event.mPos;
        event.mButton    = aChanged;
        event.mButtons   = internals.mButtons;
        return event;
    }

    //! Puts the compositor socket in the loop's poll set.
    //!
    //! @return true if the descriptor is being polled.
    bool PlatformIntegrationWayland::registerConnection()
    {
        if( mConnectionFd >= 0 )
        {
            return true;
        }

        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher = currentLinuxDispatcher();
        if( !dispatcher )
        {
            std::fprintf( stderr,
                "QtLikeSignalGui: this thread is not running EventDispatcherLinux, so the compositor "
                "connection cannot join the event loop; construct the GuiApplication on the thread "
                "that will call exec()\n" );
            return false;
        }

        mConnectionFd = wl_display_get_fd( mInternals->mDisplay );
        mPollMask     = POLLIN;

        if( !dispatcher->registerEventSource( mConnectionFd, mPollMask,
            [this]( short aEvents )
            {
                pumpDisplay( aEvents );
            } ) )
        {
            std::fprintf( stderr, "QtLikeSignalGui: registerEventSource( %d ) was refused\n",
                mConnectionFd );
            mConnectionFd = -1;
            return false;
        }

        // Everything above talked to the compositor, and its replies may have brought events along
        // that are already in libwayland's queue with an empty socket behind them. poll() would
        // have nothing to report and that first batch would wait for whatever happened next.
        pumpDisplay( 0 );
        return true;
    }

    //! Takes the connection back out of the poll set. Safe when it was never in it.
    void PlatformIntegrationWayland::unregisterConnection()
    {
        if( mConnectionFd < 0 )
        {
            return;
        }

        // Unregistered from the loop's own thread, so EventDispatcherLinux's contract makes the
        // call synchronous: the callback will not run again, not even for a readiness the current
        // poll() round has already observed.
        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher = currentLinuxDispatcher();
        if( dispatcher )
        {
            dispatcher->unregisterEventSource( mConnectionFd );
        }

        mConnectionFd = -1;
        mPollMask     = 0;
    }

    //! Reads whatever the compositor has sent and runs the listeners.
    //!
    //! @p aEvents is 0 for the priming call from registerConnection(), which dispatches what is
    //! already queued without reading the socket.
    void PlatformIntegrationWayland::pumpDisplay
        (
        short aEvents   //!< poll(2) revents for the connection, or 0 to dispatch without reading.
        )
    {
        wl_display* const display = mInternals->mDisplay;
        if( display == nullptr )
        {
            return;
        }

        if( ( aEvents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
        {
            std::fprintf( stderr,
                "QtLikeSignalGui: the compositor connection dropped (revents 0x%x); quitting\n",
                static_cast<unsigned int>( aEvents ) );
            unregisterConnection();
            QtLikeSignal::CoreApplication::quit();
            return;
        }

        if( ( aEvents & POLLIN ) != 0 )
        {
            while( wl_display_prepare_read( display ) != 0 )
            {
                if( wl_display_dispatch_pending( display ) < 0 )
                {
                    std::fprintf( stderr, "QtLikeSignalGui: wl_display_dispatch_pending() failed\n" );
                    return;
                }
            }

            // poll() said readable, so this does not block. libwayland cancels the announced read
            // itself if it fails, so there is nothing to undo here.
            if( wl_display_read_events( display ) < 0 )
            {
                std::fprintf( stderr, "QtLikeSignalGui: wl_display_read_events() failed\n" );
                return;
            }
        }

        // Runs the listeners, which is where every WindowSystemInterface call in this file happens.
        if( wl_display_dispatch_pending( display ) < 0 )
        {
            std::fprintf( stderr, "QtLikeSignalGui: wl_display_dispatch_pending() failed\n" );
            return;
        }

        flushOutgoing();
    }

    //! Pushes what the listeners asked for out to the compositor.
    void PlatformIntegrationWayland::flushOutgoing()
    {
        wl_display* const display = mInternals->mDisplay;
        if( display == nullptr )
        {
            return;
        }

        if( wl_display_flush( display ) >= 0 )
        {
            setPollMask( POLLIN );
            return;
        }

        if( errno == EAGAIN )
        {
            // The compositor is not draining its end and the socket's send buffer is full. Going
            // back to sleep on POLLIN alone is the classic Wayland client hang: nothing more will
            // arrive until the compositor has read what was already sent, so the wait would never
            // end. Ask to be woken when the socket becomes writable instead, and drop back to
            // POLLIN as soon as a flush completes.
            setPollMask( POLLIN | POLLOUT );
            return;
        }

        std::fprintf( stderr, "QtLikeSignalGui: wl_display_flush() failed (%d); quitting\n", errno );
        unregisterConnection();
        QtLikeSignal::CoreApplication::quit();
    }

    //! Changes what the loop waits on for the connection.
    void PlatformIntegrationWayland::setPollMask
        (
        short aEvents   //!< The mask to wait on from now on.
        )
    {
        if( aEvents == mPollMask || mConnectionFd < 0 )
        {
            return;
        }

        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher = currentLinuxDispatcher();
        if( !dispatcher )
        {
            return;
        }

        // Re-registering the same descriptor replaces its mask and callback, and wakes a loop
        // already blocked on the old set so it rebuilds immediately. That is documented behaviour
        // of registerEventSource(), and it is why no separate "change the mask" call is needed.
        dispatcher->registerEventSource( mConnectionFd, aEvents,
            [this]( short aReady )
            {
                pumpDisplay( aReady );
            } );
        mPollMask = aEvents;
    }

    //! Records a new size and reports it, if it actually changed.
    void PlatformIntegrationWayland::reportResize
        (
        int aWidth,    //!< New width in pixels.
        int aHeight    //!< New height in pixels.
        )
    {
        Internals& internals = *mInternals;

        const int width  = std::max( 1, aWidth );
        const int height = std::max( 1, aHeight );

        if( width == internals.mWidth && height == internals.mHeight )
        {
            return;
        }

        internals.mWidth  = width;
        internals.mHeight = height;

        if( mWindow != nullptr )
        {
            WindowSystemInterface::handleResize( mWindow, width, height );
        }
    }

    //! Reports that the user asked to close the window.
    void PlatformIntegrationWayland::requestClose()
    {
        if( mWindow != nullptr )
        {
            WindowSystemInterface::handleCloseRequest( mWindow );
        }
    }

    //! Destroys the surface and its role. The connection stays open until this backend does not.
    void PlatformIntegrationWayland::releaseNativeWindow
        (
        Window* aWindow   //!< The window being destroyed.
        )
    {
        if( aWindow != mWindow )
        {
            assignNative( aWindow, NativeWindow() );
            return;
        }

        Internals& internals = *mInternals;

        if( internals.mDecorFrame != nullptr )
        {
            libdecor_frame_unref( internals.mDecorFrame );
            internals.mDecorFrame = nullptr;
        }
        if( internals.mXdgToplevel != nullptr )
        {
            xdg_toplevel_destroy( internals.mXdgToplevel );
            internals.mXdgToplevel = nullptr;
        }
        if( internals.mXdgSurface != nullptr )
        {
            xdg_surface_destroy( internals.mXdgSurface );
            internals.mXdgSurface = nullptr;
        }
        if( internals.mIviSurface != nullptr )
        {
            ivi_surface_destroy( internals.mIviSurface );
            internals.mIviSurface = nullptr;
        }
        if( internals.mSurface != nullptr )
        {
            wl_surface_destroy( internals.mSurface );
            internals.mSurface = nullptr;
        }

        if( internals.mDisplay != nullptr )
        {
            wl_display_flush( internals.mDisplay );
        }

        mWindow        = nullptr;
        mUpdatePending = false;

        assignNative( aWindow, NativeWindow() );
    }

    //! Sets the window title, on whichever shell gave the surface its role.
    void PlatformIntegrationWayland::setWindowTitle
        (
        Window* aWindow,           //!< Window to retitle.
        const std::string& aTitle  //!< New title, in UTF-8, which is what Wayland wants.
        )
    {
        if( aWindow != mWindow )
        {
            return;
        }

        Internals& internals = *mInternals;
        internals.mTitle = aTitle;

        if( internals.mDecorFrame != nullptr )
        {
            libdecor_frame_set_title( internals.mDecorFrame, aTitle.c_str() );
        }
        else if( internals.mXdgToplevel != nullptr )
        {
            xdg_toplevel_set_title( internals.mXdgToplevel, aTitle.c_str() );
        }

        // ivi-shell has no title: a surface there is identified to the controller by its id, and
        // there is no decoration to write a name into.
        flushOutgoing();
    }

    //! Records the flag. A Wayland surface is shown by the renderer, not by this library.
    //!
    //! There is nothing to map or unmap: a surface becomes visible when a buffer is attached and
    //! committed, and the buffer belongs to whatever draws through nativeHandle(). So the window
    //! appears on the renderer's first frame rather than on this call, and isVisible() answers what
    //! the application asked for rather than what is on screen.
    void PlatformIntegrationWayland::setWindowVisible
        (
        Window* aWindow,   //!< Window whose flag to set.
        bool aVisible      //!< True once it is considered shown.
        )
    {
        assignVisible( aWindow, aVisible );
    }

    //! Sets the size the surface reports, and tells the shell the new fixed size.
    //!
    //! The surface itself has no size on Wayland -- the buffer the renderer attaches decides that --
    //! so this records the figure, updates the size the shell will hold the window to, and reports
    //! the change. What actually appears follows on the renderer's next frame.
    void PlatformIntegrationWayland::setClientSize
        (
        Window* aWindow,   //!< Window to resize.
        int aWidth,        //!< Desired width in pixels.
        int aHeight        //!< Desired height in pixels.
        )
    {
        if( aWindow != mWindow )
        {
            return;
        }

        Internals& internals = *mInternals;

        if( internals.mDecorFrame != nullptr )
        {
            libdecor_frame_set_min_content_size( internals.mDecorFrame, aWidth, aHeight );
            libdecor_frame_set_max_content_size( internals.mDecorFrame, aWidth, aHeight );
        }
        else if( internals.mXdgToplevel != nullptr )
        {
            xdg_toplevel_set_min_size( internals.mXdgToplevel, aWidth, aHeight );
            xdg_toplevel_set_max_size( internals.mXdgToplevel, aWidth, aHeight );
        }

        reportResize( aWidth, aHeight );
        flushOutgoing();
    }

    //! Asks for a repaint by posting one onto the loop.
    //!
    //! Wayland has no expose. The idiomatic way to be told when to draw is a wl_surface frame
    //! callback, but that only fires after a commit, and this backend never commits -- the renderer
    //! does, inside eglSwapBuffers. So the repaint is posted, exactly as the DRM backend posts one,
    //! and arrives from inside a dispatch pass rather than from inside this call.
    void PlatformIntegrationWayland::requestUpdate
        (
        Window* aWindow   //!< Window to repaint.
        )
    {
        if( aWindow != mWindow )
        {
            return;
        }

        // Before the early return for an update already pending, and unconditionally. A caller that
        // drew straight away and then asked for the next frame has a commit sitting unsent in
        // libwayland's output buffer, and this is the first moment anything in this library learns
        // that it might. Flushing an empty buffer costs nothing.
        flushOutgoing();

        if( mUpdatePending )
        {
            return;
        }

        mUpdatePending = true;

        std::weak_ptr<int> life = mLifeToken;

        if( !QtLikeSignal::CoreApplication::post( [this, life]()
            {
                if( life.expired() )
                {
                    return;
                }

                mUpdatePending = false;

                if( mWindow != nullptr )
                {
                    WindowSystemInterface::handleExpose( mWindow );

                    // The renderer has just drawn, and on Wayland drawing means requests queued in
                    // libwayland's output buffer -- wl_surface_attach, damage and commit. Nothing
                    // writes them to the socket by itself.
                    //
                    // Every other path out of this backend already ends in a flush because it runs
                    // inside pumpDisplay(), which flushes last. This one does not: it is a task on
                    // the loop, reached because the application asked to draw rather than because
                    // the compositor said anything. Without this the first frame is marshalled and
                    // never sent, the surface is never mapped, and the window simply never appears
                    // -- with no error anywhere, because the requests are perfectly valid and just
                    // sitting in a buffer.
                    flushOutgoing();
                }
            } ) )
        {
            mUpdatePending = false;
        }
    }
}
