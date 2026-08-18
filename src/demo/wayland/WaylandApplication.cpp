// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! WaylandApplication implementation: the compositor connection, its registration with the loop,
//! the read sequence libwayland requires, and the listeners that turn protocol events into signals.

#include "WaylandApplication.hpp"

#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

namespace QtLikeSignalDemo
{
    //! Every Wayland handle the demo owns.
    //!
    //! One struct rather than a dozen void* members in the header; see the declaration.
    struct WaylandApplication::Internals
    {
        wl_display* mDisplay { nullptr };         //!< The connection itself.
        wl_registry* mRegistry { nullptr };       //!< Global advertiser, held for its listener.
        wl_compositor* mCompositor { nullptr };   //!< Makes surfaces.
        wl_shm* mShm { nullptr };                 //!< Makes shared-memory buffer pools.
        wl_seat* mSeat { nullptr };               //!< The input group this demo listens to.
        xdg_wm_base* mWmBase { nullptr };         //!< Shell that turns a surface into a window.
        wl_surface* mSurface { nullptr };         //!< The surface itself.
        xdg_surface* mXdgSurface { nullptr };     //!< Shell role wrapper around mSurface.
        xdg_toplevel* mToplevel { nullptr };      //!< Window role: title, close button, size.
        wl_pointer* mPointer { nullptr };         //!< Pointer, if the seat has one.
        wl_keyboard* mKeyboard { nullptr };       //!< Keyboard, if the seat has one.
        wl_buffer* mBuffer { nullptr };           //!< The one buffer, filled once and never redrawn.

        void* mPixels { nullptr };                //!< mmap of the buffer's shared memory.
        size_t mPixelBytes { 0 };                 //!< Length of that mapping, for munmap().

        //! True once the buffer has been attached and committed.
        //!
        //! The attach happens in the first xdg_surface configure, which is the earliest point the
        //! protocol allows; every later configure only has to acknowledge.
        bool mBufferAttached { false };
    };

    namespace
    {
        //! The colour the one buffer is filled with, as XRGB8888.
        //!
        //! There is exactly one fill, at startup, because a Wayland surface is not mapped until a
        //! buffer is attached to it. Nothing draws after that: this demo reports what it receives on
        //! the console and does not paint.
        const uint32_t kFillColour = 0x00181a20u;

        //! Gets the running thread's dispatcher as an EventDispatcherLinux, or null if it is not
        //! one.
        std::shared_ptr<QtLikeSignal::EventDispatcherLinux> currentLinuxDispatcher()
        {
            QtLikeSignal::Thread* current = QtLikeSignal::Thread::currentThread();
            if( current == nullptr )
            {
                return nullptr;
            }
            return std::dynamic_pointer_cast<QtLikeSignal::EventDispatcherLinux>(
                current->eventDispatcher() );
        }

        //! Maps an evdev button code to the button the demo reports.
        //!
        //! Wayland does not number buttons itself; it passes the kernel's evdev codes straight
        //! through, which is why this reads linux/input-event-codes.h rather than a protocol header.
        MouseEvent::Button buttonOf
            (
            uint32_t aButton   //!< An evdev BTN_ code.
            )
        {
            switch( aButton )
            {
            case BTN_LEFT:
                return MouseEvent::Button::Left;

            case BTN_MIDDLE:
                return MouseEvent::Button::Middle;

            case BTN_RIGHT:
                return MouseEvent::Button::Right;

            default:
                return MouseEvent::Button::None;
            }
        }

        //! Creates an anonymous file of @p aSize bytes to share with the compositor.
        //!
        //! memfd rather than a file in XDG_RUNTIME_DIR: there is no path to collide on, nothing to
        //! unlink, and MFD_CLOEXEC keeps it out of any process this one might exec. Returns -1 on
        //! failure.
        int createSharedFile
            (
            size_t aSize   //!< Bytes the file should hold.
            )
        {
            const int fd = ::memfd_create( "qtlikesignal-demo", MFD_CLOEXEC );
            if( fd < 0 )
            {
                return -1;
            }

            if( ::ftruncate( fd, static_cast<off_t>( aSize ) ) < 0 )
            {
                ::close( fd );
                return -1;
            }
            return fd;
        }
    }

    //! Every Wayland listener callback, as the C protocol bindings need them.
    //!
    //! libwayland dispatches through plain function pointers with a void* user pointer, so these
    //! cannot be members of WaylandApplication without dragging every wl_ type into its header.
    //! They are static members of this struct instead, which WaylandApplication befriends, so they
    //! can emit its signals directly rather than through a layer of forwarding methods.
    //!
    //! Every one of them receives the application as @p aData, set when the listener was added.
    struct WaylandListeners
    {
        //! Binds the globals the demo needs as the compositor advertises them.
        static void global
            (
            void* aData,             //!< The WaylandApplication.
            wl_registry* aRegistry,  //!< Registry the global came from.
            uint32_t aName,          //!< The global's id, for binding.
            const char* aInterface,  //!< Its interface name.
            uint32_t aVersion        //!< The highest version the compositor supports.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );
            WaylandApplication::Internals& internals = *app->mInternals;

            if( std::strcmp( aInterface, wl_compositor_interface.name ) == 0 )
            {
                internals.mCompositor = static_cast<wl_compositor*>(
                    wl_registry_bind( aRegistry, aName, &wl_compositor_interface, 1 ) );
            }
            else if( std::strcmp( aInterface, wl_shm_interface.name ) == 0 )
            {
                internals.mShm = static_cast<wl_shm*>(
                    wl_registry_bind( aRegistry, aName, &wl_shm_interface, 1 ) );
            }
            else if( std::strcmp( aInterface, wl_seat_interface.name ) == 0 )
            {
                // Capped at 4 on purpose. Seat 5 introduced wl_pointer.frame and the axis detail
                // events, and a listener is only obliged to handle what the version it bound can
                // send. Binding low keeps the set of events this demo must answer exactly the set
                // it wants to print.
                const uint32_t version = std::min( aVersion, 4u );
                internals.mSeat = static_cast<wl_seat*>(
                    wl_registry_bind( aRegistry, aName, &wl_seat_interface, version ) );
                wl_seat_add_listener( internals.mSeat, &kSeatListener, app );
            }
            else if( std::strcmp( aInterface, xdg_wm_base_interface.name ) == 0 )
            {
                internals.mWmBase = static_cast<xdg_wm_base*>(
                    wl_registry_bind( aRegistry, aName, &xdg_wm_base_interface, 1 ) );
                xdg_wm_base_add_listener( internals.mWmBase, &kWmBaseListener, app );
            }
        }

        //! A global went away. Nothing this demo binds is ever removed while it runs.
        static void globalRemove
            (
            void*,        //!< The WaylandApplication. Unused.
            wl_registry*, //!< Registry. Unused.
            uint32_t      //!< The global's id. Unused.
            )
        {
        }

        //! Answers the compositor's liveness check.
        //!
        //! Not optional: a client that does not pong is declared unresponsive. The pong is a
        //! request, so it only reaches the compositor when pumpDisplay() flushes at the end of the
        //! pass -- which is the same flush that carries everything else.
        static void ping
            (
            void* aData,        //!< The WaylandApplication.
            xdg_wm_base* aBase, //!< The shell that pinged.
            uint32_t aSerial    //!< Serial to echo back.
            )
        {
            ( void )aData;
            xdg_wm_base_pong( aBase, aSerial );
        }

        //! Acknowledges a configure, and on the first one attaches the buffer that maps the window.
        static void surfaceConfigure
            (
            void* aData,             //!< The WaylandApplication.
            xdg_surface* aSurface,   //!< The surface being configured.
            uint32_t aSerial         //!< Serial to acknowledge.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );
            WaylandApplication::Internals& internals = *app->mInternals;

            xdg_surface_ack_configure( aSurface, aSerial );

            if( !internals.mBufferAttached && internals.mBuffer != nullptr )
            {
                // The earliest point the protocol allows a buffer to be attached, and the only
                // point this demo ever attaches one.
                wl_surface_attach( internals.mSurface, internals.mBuffer, 0, 0 );
                wl_surface_damage( internals.mSurface, 0, 0, app->mWidth, app->mHeight );
                wl_surface_commit( internals.mSurface );
                internals.mBufferAttached = true;
            }
        }

        //! Reports the size the compositor suggests. The buffer is never resized to match.
        static void toplevelConfigure
            (
            void* aData,       //!< The WaylandApplication.
            xdg_toplevel*,     //!< The toplevel. Unused.
            int32_t aWidth,    //!< Suggested width, or 0 for "you choose".
            int32_t aHeight,   //!< Suggested height, or 0 for "you choose".
            wl_array*          //!< Window states. Unused.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );
            app->mSurfaceConfigured.emit( aWidth, aHeight );
        }

        //! The close button, or the compositor asking the window to go away.
        static void toplevelClose
            (
            void*,         //!< The WaylandApplication. Unused.
            xdg_toplevel*  //!< The toplevel. Unused.
            )
        {
            // quit(), for the same reason the other two demos end their loops that way:
            // Thread::exec() runs until its own exit flag is set and never reads what
            // processEvents() returned, so nothing a platform event can return will stop it. R22 in
            // history/OPEN-RISKS-20260816.md records that divergence from Qt.
            QtLikeSignal::CoreApplication::quit();
        }

        //! Takes the pointer and the keyboard as the seat announces them.
        static void seatCapabilities
            (
            void* aData,             //!< The WaylandApplication.
            wl_seat* aSeat,          //!< The seat.
            uint32_t aCapabilities   //!< WL_SEAT_CAPABILITY_ bitmask.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );
            WaylandApplication::Internals& internals = *app->mInternals;

            const bool hasPointer = ( aCapabilities & WL_SEAT_CAPABILITY_POINTER ) != 0;
            if( hasPointer && internals.mPointer == nullptr )
            {
                internals.mPointer = wl_seat_get_pointer( aSeat );
                wl_pointer_add_listener( internals.mPointer, &kPointerListener, app );
            }

            const bool hasKeyboard = ( aCapabilities & WL_SEAT_CAPABILITY_KEYBOARD ) != 0;
            if( hasKeyboard && internals.mKeyboard == nullptr )
            {
                internals.mKeyboard = wl_seat_get_keyboard( aSeat );
                wl_keyboard_add_listener( internals.mKeyboard, &kKeyboardListener, app );
            }
        }

        //! The seat's human-readable name. Not used.
        static void seatName
            (
            void*,       //!< The WaylandApplication. Unused.
            wl_seat*,    //!< The seat. Unused.
            const char*  //!< Its name. Unused.
            )
        {
        }

        //! The pointer came over the surface, with its position at that moment.
        static void pointerEnter
            (
            void* aData,     //!< The WaylandApplication.
            wl_pointer*,     //!< The pointer. Unused.
            uint32_t,        //!< Serial. Unused.
            wl_surface*,     //!< The surface entered. Only one exists here.
            wl_fixed_t aX,   //!< Surface-local x, 24.8 fixed point.
            wl_fixed_t aY    //!< Surface-local y, 24.8 fixed point.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );

            MouseEvent where;
            where.mX      = wl_fixed_to_int( aX );
            where.mY      = wl_fixed_to_int( aY );
            where.mButton = MouseEvent::Button::None;

            app->mPointerEntered.emit();
            app->mMouseMoved.emit( where );
        }

        //! The pointer left the surface. No position: there is none to report.
        static void pointerLeave
            (
            void* aData,  //!< The WaylandApplication.
            wl_pointer*,  //!< The pointer. Unused.
            uint32_t,     //!< Serial. Unused.
            wl_surface*   //!< The surface left. Unused.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );
            app->mPointerLeft.emit();
        }

        //! The pointer moved within the surface.
        static void pointerMotion
            (
            void* aData,     //!< The WaylandApplication.
            wl_pointer*,     //!< The pointer. Unused.
            uint32_t,        //!< Event time in milliseconds. Unused.
            wl_fixed_t aX,   //!< Surface-local x, 24.8 fixed point.
            wl_fixed_t aY    //!< Surface-local y, 24.8 fixed point.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );

            MouseEvent moved;
            moved.mX      = wl_fixed_to_int( aX );
            moved.mY      = wl_fixed_to_int( aY );
            moved.mButton = MouseEvent::Button::None;
            app->mMouseMoved.emit( moved );
        }

        //! A pointer button changed state.
        //!
        //! Wayland reports the position separately, in motion and enter, so the last one seen is
        //! what a press is at. This demo forwards it as it stands rather than caching, and the
        //! console output carries the position from the preceding motion.
        static void pointerButton
            (
            void* aData,       //!< The WaylandApplication.
            wl_pointer*,       //!< The pointer. Unused.
            uint32_t,          //!< Serial. Unused.
            uint32_t,          //!< Event time in milliseconds. Unused.
            uint32_t aButton,  //!< evdev BTN_ code.
            uint32_t aState    //!< WL_POINTER_BUTTON_STATE_PRESSED or _RELEASED.
            )
        {
            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );

            MouseEvent event;
            event.mButton = buttonOf( aButton );

            if( aState == WL_POINTER_BUTTON_STATE_PRESSED )
            {
                app->mMousePressed.emit( event );
            }
            else
            {
                app->mMouseReleased.emit( event );
            }
        }

        //! The wheel. Deliberately ignored: this demo has no signal for scrolling, and inventing
        //! one would say nothing about the loop that the button and motion signals do not.
        //!
        //! The callback still has to exist. The seat is bound at a version that can send axis
        //! events, and libwayland calls straight through the table with no null check.
        static void pointerAxis
            (
            void*,        //!< The WaylandApplication. Unused.
            wl_pointer*,  //!< The pointer. Unused.
            uint32_t,     //!< Event time. Unused.
            uint32_t,     //!< Axis. Unused.
            wl_fixed_t    //!< Distance. Unused.
            )
        {
        }

        //! The keymap, which this demo does not use.
        //!
        //! The compositor sends it as a file descriptor, and a client that ignores it still has to
        //! close it -- one leaked descriptor per keymap change otherwise. Reporting raw evdev
        //! keycodes is the deliberate choice here: translating them needs libxkbcommon, and there
        //! is nothing in this demo to type into.
        static void keyboardKeymap
            (
            void*,      //!< The WaylandApplication. Unused.
            wl_keyboard*, //!< The keyboard. Unused.
            uint32_t,   //!< Keymap format. Unused.
            int32_t aFd, //!< Descriptor holding the keymap. Closed here.
            uint32_t    //!< Its size. Unused.
            )
        {
            ::close( aFd );
        }

        //! Keyboard focus arrived. Not reported.
        static void keyboardEnter
            (
            void*,        //!< The WaylandApplication. Unused.
            wl_keyboard*, //!< The keyboard. Unused.
            uint32_t,     //!< Serial. Unused.
            wl_surface*,  //!< The focused surface. Unused.
            wl_array*     //!< Keys already held. Unused.
            )
        {
        }

        //! Keyboard focus left. Not reported.
        static void keyboardLeave
            (
            void*,        //!< The WaylandApplication. Unused.
            wl_keyboard*, //!< The keyboard. Unused.
            uint32_t,     //!< Serial. Unused.
            wl_surface*   //!< The surface that lost focus. Unused.
            )
        {
        }

        //! A key changed state. Presses are emitted; Escape also ends the loop.
        static void keyboardKey
            (
            void* aData,     //!< The WaylandApplication.
            wl_keyboard*,    //!< The keyboard. Unused.
            uint32_t,        //!< Serial. Unused.
            uint32_t,        //!< Event time. Unused.
            uint32_t aKey,   //!< Raw evdev keycode.
            uint32_t aState  //!< WL_KEYBOARD_KEY_STATE_PRESSED or _RELEASED.
            )
        {
            if( aState != WL_KEYBOARD_KEY_STATE_PRESSED )
            {
                return;
            }

            WaylandApplication* const app = static_cast<WaylandApplication*>( aData );
            app->mKeyPressed.emit( aKey );

            if( aKey == KEY_ESC )
            {
                QtLikeSignal::CoreApplication::quit();
            }
        }

        //! Modifier state. Not tracked: without a keymap there is nothing to apply it to.
        static void keyboardModifiers
            (
            void*,        //!< The WaylandApplication. Unused.
            wl_keyboard*, //!< The keyboard. Unused.
            uint32_t,     //!< Serial. Unused.
            uint32_t,     //!< Depressed modifiers. Unused.
            uint32_t,     //!< Latched modifiers. Unused.
            uint32_t,     //!< Locked modifiers. Unused.
            uint32_t      //!< Effective group. Unused.
            )
        {
        }

        //! Key repeat rate and delay. This demo does not repeat.
        static void keyboardRepeatInfo
            (
            void*,        //!< The WaylandApplication. Unused.
            wl_keyboard*, //!< The keyboard. Unused.
            int32_t,      //!< Repeats per second. Unused.
            int32_t       //!< Delay before repeating. Unused.
            )
        {
        }

        // The listener tables below are initialised positionally, in the order libwayland declares
        // the members, because C++17 has no designated initialisers -- so the name of each member
        // is in the comment beside it rather than in the code. Any member left out is
        // value-initialised to nullptr, and libwayland would call through it; that is safe here only
        // because every global is bound at a version low enough that the compositor cannot send the
        // events those members would serve.

        static const wl_registry_listener kRegistryListener;   //!< global, global_remove.
        static const xdg_wm_base_listener kWmBaseListener;     //!< ping.
        static const xdg_surface_listener kXdgSurfaceListener; //!< configure.
        static const xdg_toplevel_listener kToplevelListener;  //!< configure, close.
        static const wl_seat_listener kSeatListener;           //!< capabilities, name.
        static const wl_pointer_listener kPointerListener;     //!< enter, leave, motion, button, axis.
        static const wl_keyboard_listener kKeyboardListener;   //!< keymap, enter, leave, key, ...
    };

    //! Registry events: a global appeared, a global went away.
    const wl_registry_listener WaylandListeners::kRegistryListener =
    {
        WaylandListeners::global,
        WaylandListeners::globalRemove
    };

    //! Shell events: the liveness ping.
    const xdg_wm_base_listener WaylandListeners::kWmBaseListener =
    {
        WaylandListeners::ping
    };

    //! Shell surface events: configure.
    const xdg_surface_listener WaylandListeners::kXdgSurfaceListener =
    {
        WaylandListeners::surfaceConfigure
    };

    //! Toplevel events: configure and close.
    const xdg_toplevel_listener WaylandListeners::kToplevelListener =
    {
        WaylandListeners::toplevelConfigure,
        WaylandListeners::toplevelClose
    };

    //! Seat events: capabilities and name.
    const wl_seat_listener WaylandListeners::kSeatListener =
    {
        WaylandListeners::seatCapabilities,
        WaylandListeners::seatName
    };

    //! Pointer events: enter, leave, motion, button, axis.
    //!
    //! The members after axis -- frame and the axis detail events -- exist in libwayland's struct
    //! but are version 5 and above, and the seat is bound at 4, so they are left null.
    const wl_pointer_listener WaylandListeners::kPointerListener =
    {
        WaylandListeners::pointerEnter,
        WaylandListeners::pointerLeave,
        WaylandListeners::pointerMotion,
        WaylandListeners::pointerButton,
        WaylandListeners::pointerAxis
    };

    //! Keyboard events: keymap, enter, leave, key, modifiers, repeat_info.
    const wl_keyboard_listener WaylandListeners::kKeyboardListener =
    {
        WaylandListeners::keyboardKeymap,
        WaylandListeners::keyboardEnter,
        WaylandListeners::keyboardLeave,
        WaylandListeners::keyboardKey,
        WaylandListeners::keyboardModifiers,
        WaylandListeners::keyboardRepeatInfo
    };

    //! Constructs the application. The connection is opened separately, by createWindow().
    WaylandApplication::WaylandApplication()
        : QtLikeSignal::CoreApplication()
        , mInternals( new Internals() )
    {
    }

    //! Constructs the application with the program's command line, reachable through arguments().
    WaylandApplication::WaylandApplication
        (
        int aArgc,     //!< Argument count, as main() received it.
        char** aArgv   //!< Argument values, as main() received them.
        )
        : QtLikeSignal::CoreApplication( aArgc, aArgv )
        , mInternals( new Internals() )
    {
    }

    //! Unregisters the connection from the loop and tears everything down.
    WaylandApplication::~WaylandApplication()
    {
        // Unregistered first, and from this thread -- the loop's own thread -- so
        // EventDispatcherLinux's contract makes it synchronous: the callback cannot run again, not
        // even for a readiness the current poll() round has already observed.
        if( mConnectionFd >= 0 )
        {
            const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher
                = currentLinuxDispatcher();
            if( dispatcher )
            {
                dispatcher->unregisterEventSource( mConnectionFd );
            }
            mConnectionFd = -1;
        }

        Internals& internals = *mInternals;

        if( internals.mBuffer != nullptr )
        {
            wl_buffer_destroy( internals.mBuffer );
        }
        if( internals.mPixels != nullptr )
        {
            ::munmap( internals.mPixels, internals.mPixelBytes );
        }
        if( internals.mToplevel != nullptr )
        {
            xdg_toplevel_destroy( internals.mToplevel );
        }
        if( internals.mXdgSurface != nullptr )
        {
            xdg_surface_destroy( internals.mXdgSurface );
        }
        if( internals.mSurface != nullptr )
        {
            wl_surface_destroy( internals.mSurface );
        }
        // release, not destroy, from version 3 on. Both free the proxy locally, but only release
        // tells the compositor the object is gone; destroy leaves it holding a resource for the
        // life of the connection. The version test is what keeps this correct against a compositor
        // that only offers an older seat, where sending release would be a protocol error.
        if( internals.mPointer != nullptr )
        {
            if( wl_pointer_get_version( internals.mPointer ) >= WL_POINTER_RELEASE_SINCE_VERSION )
            {
                wl_pointer_release( internals.mPointer );
            }
            else
            {
                wl_pointer_destroy( internals.mPointer );
            }
        }
        if( internals.mKeyboard != nullptr )
        {
            if( wl_keyboard_get_version( internals.mKeyboard )
                >= WL_KEYBOARD_RELEASE_SINCE_VERSION )
            {
                wl_keyboard_release( internals.mKeyboard );
            }
            else
            {
                wl_keyboard_destroy( internals.mKeyboard );
            }
        }
        if( internals.mSeat != nullptr )
        {
            wl_seat_destroy( internals.mSeat );
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
            wl_display_disconnect( internals.mDisplay );
            internals.mDisplay = nullptr;
        }
    }

    //! Connects to the compositor, creates and maps the window, and joins the connection to the
    //! event loop.
    //!
    //! The last step is the one that matters: wl_display_get_fd() is handed to
    //! EventDispatcherLinux::registerEventSource(), after which the socket is polled together with
    //! the dispatcher's own wakeup descriptor and its timer deadline. Nothing in this program calls
    //! wl_display_dispatch() from a loop of its own.
    //! @return true if the window exists and the connection is registered.
    bool WaylandApplication::createWindow
        (
        const char* aTitle,   //!< Window title, as the compositor will show it.
        int aWidth,           //!< Surface width in pixels.
        int aHeight           //!< Surface height in pixels.
        )
    {
        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher
            = currentLinuxDispatcher();
        if( !dispatcher )
        {
            std::fprintf( stderr,
                "WaylandApplication: this thread is not running EventDispatcherLinux, so the "
                "compositor connection has no poll() set to join. Construct the application "
                "first.\n" );
            return false;
        }

        Internals& internals = *mInternals;

        internals.mDisplay = wl_display_connect( nullptr );
        if( internals.mDisplay == nullptr )
        {
            const char* const wanted = std::getenv( "WAYLAND_DISPLAY" );
            std::fprintf( stderr,
                "WaylandApplication: wl_display_connect() failed (WAYLAND_DISPLAY=%s, "
                "XDG_RUNTIME_DIR=%s)\n",
                wanted != nullptr ? wanted : "<unset>",
                std::getenv( "XDG_RUNTIME_DIR" ) != nullptr
                    ? std::getenv( "XDG_RUNTIME_DIR" ) : "<unset>" );
            return false;
        }

        mWidth  = aWidth;
        mHeight = aHeight;

        internals.mRegistry = wl_display_get_registry( internals.mDisplay );
        wl_registry_add_listener( internals.mRegistry, &WaylandListeners::kRegistryListener, this );

        // Two round trips, and both are needed. The first collects the globals the compositor
        // advertises; the second collects what those globals then announce about themselves, which
        // is where the seat's capabilities -- and therefore the pointer and keyboard -- arrive.
        // These block, but they run before the loop starts, which is the only place in this program
        // where blocking on the connection is allowed.
        wl_display_roundtrip( internals.mDisplay );
        wl_display_roundtrip( internals.mDisplay );

        if( internals.mCompositor == nullptr || internals.mShm == nullptr
            || internals.mWmBase == nullptr )
        {
            std::fprintf( stderr,
                "WaylandApplication: the compositor is missing something this demo needs "
                "(wl_compositor %s, wl_shm %s, xdg_wm_base %s)\n",
                internals.mCompositor != nullptr ? "ok" : "absent",
                internals.mShm != nullptr ? "ok" : "absent",
                internals.mWmBase != nullptr ? "ok" : "absent" );
            return false;
        }

        if( !createBuffer() )
        {
            return false;
        }

        internals.mSurface = wl_compositor_create_surface( internals.mCompositor );
        internals.mXdgSurface = xdg_wm_base_get_xdg_surface( internals.mWmBase,
            internals.mSurface );
        xdg_surface_add_listener( internals.mXdgSurface, &WaylandListeners::kXdgSurfaceListener,
            this );

        internals.mToplevel = xdg_surface_get_toplevel( internals.mXdgSurface );
        xdg_toplevel_add_listener( internals.mToplevel, &WaylandListeners::kToplevelListener,
            this );
        xdg_toplevel_set_title( internals.mToplevel, aTitle );
        xdg_toplevel_set_app_id( internals.mToplevel, "qtlikesignal.demo.wayland" );

        // A commit with no buffer, which is how a client asks to be configured. The compositor
        // replies with an xdg_surface configure, and the listener attaches the buffer then -- doing
        // it before the configure is a protocol error, not merely early.
        wl_surface_commit( internals.mSurface );
        wl_display_roundtrip( internals.mDisplay );

        mConnectionFd = wl_display_get_fd( internals.mDisplay );
        mPollMask     = POLLIN;
        if( !dispatcher->registerEventSource( mConnectionFd, mPollMask,
            [this]( short aEvents )
            {
                pumpDisplay( aEvents );
            } ) )
        {
            std::fprintf( stderr, "WaylandApplication: registerEventSource( %d ) was refused\n",
                mConnectionFd );
            mConnectionFd = -1;
            return false;
        }

        // One pass before the loop ever blocks. The round trips above read the socket, so events
        // may already be queued with nothing left on the wire for poll() to report. Passing 0 for
        // the mask skips the read and only dispatches what is already in hand, then flushes.
        pumpDisplay( 0 );
        return true;
    }

    //! Creates the one shared-memory buffer and fills it with a flat colour.
    //!
    //! This exists because a Wayland surface is not mapped, and therefore receives no input, until
    //! a buffer is attached to it. It is filled once and never touched again: this demo prints what
    //! it receives rather than drawing it.
    //! @return true if the buffer is ready to attach.
    bool WaylandApplication::createBuffer()
    {
        Internals& internals = *mInternals;

        const int stride = mWidth * 4;
        const size_t bytes = static_cast<size_t>( stride ) * static_cast<size_t>( mHeight );

        const int fd = createSharedFile( bytes );
        if( fd < 0 )
        {
            std::fprintf( stderr, "WaylandApplication: could not create a %zu byte buffer (%d)\n",
                bytes, errno );
            return false;
        }

        void* const pixels = ::mmap( nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
        if( pixels == MAP_FAILED )
        {
            std::fprintf( stderr, "WaylandApplication: mmap() of the buffer failed (%d)\n", errno );
            ::close( fd );
            return false;
        }
        internals.mPixels     = pixels;
        internals.mPixelBytes = bytes;

        uint32_t* const words = static_cast<uint32_t*>( pixels );
        std::fill( words, words + ( bytes / sizeof( uint32_t ) ), kFillColour );

        wl_shm_pool* const pool = wl_shm_create_pool( internals.mShm, fd, static_cast<int32_t>(
            bytes ) );
        internals.mBuffer = wl_shm_pool_create_buffer( pool, 0, mWidth, mHeight, stride,
            WL_SHM_FORMAT_XRGB8888 );

        // The pool and the descriptor have both done their job: the buffer holds its own reference
        // to the mapping, and keeping either open would only leak.
        wl_shm_pool_destroy( pool );
        ::close( fd );

        return internals.mBuffer != nullptr;
    }

    //! Reads and dispatches whatever the compositor has sent, then flushes what the handlers said
    //! back.
    //!
    //! Called by the dispatcher, on the dispatcher's thread, whenever poll() reports the connection
    //! ready -- and once from createWindow() with a mask of 0, which dispatches what is already
    //! queued without touching the socket.
    //!
    //! **The three-step read is the API's contract.** libwayland lets several threads share one
    //! connection, so a thread must announce a read before performing it. wl_display_prepare_read()
    //! refuses while the default queue still holds events -- reading more of them before those are
    //! dispatched is what deadlocks a second thread waiting on the queue -- so the loop empties the
    //! queue first and then announces. Only then may the socket be read. Calling
    //! wl_display_dispatch() here instead would be shorter and would block, which is precisely what
    //! a dispatcher-integrated client must not do.
    void WaylandApplication::pumpDisplay
        (
        short aEvents   //!< poll(2) revents for the connection, or 0 to dispatch without reading.
        )
    {
        Internals& internals = *mInternals;
        wl_display* const display = internals.mDisplay;
        if( display == nullptr )
        {
            return;
        }

        if( ( aEvents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
        {
            std::fprintf( stderr,
                "WaylandApplication: the compositor connection dropped (revents 0x%x); quitting\n",
                static_cast<unsigned int>( aEvents ) );
            QtLikeSignal::CoreApplication::quit();
            return;
        }

        if( ( aEvents & POLLIN ) != 0 )
        {
            while( wl_display_prepare_read( display ) != 0 )
            {
                if( wl_display_dispatch_pending( display ) < 0 )
                {
                    reportFatal( "wl_display_dispatch_pending" );
                    return;
                }
            }

            // poll() said readable, so this does not block. libwayland cancels the announced read
            // itself if this fails, so there is nothing to undo here.
            if( wl_display_read_events( display ) < 0 )
            {
                reportFatal( "wl_display_read_events" );
                return;
            }
        }

        // Runs the listeners, which is where every signal in this class is emitted.
        if( wl_display_dispatch_pending( display ) < 0 )
        {
            reportFatal( "wl_display_dispatch_pending" );
            return;
        }

        flushOutgoing();
    }

    //! Pushes queued requests to the compositor, asking to be told about writability if it stalls.
    //!
    //! Every request made in this pass -- the pong that keeps the client alive, the acks, the
    //! commits -- is sitting in libwayland's output buffer until this runs, and nothing else is
    //! about to flush it.
    void WaylandApplication::flushOutgoing()
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

        std::fprintf( stderr, "WaylandApplication: wl_display_flush() failed (%d); quitting\n",
            errno );
        QtLikeSignal::CoreApplication::quit();
    }

    //! Changes the poll(2) mask the connection is registered with, if it is not already that.
    void WaylandApplication::setPollMask
        (
        short aEvents   //!< The mask to wait on from now on.
        )
    {
        if( aEvents == mPollMask || mConnectionFd < 0 )
        {
            return;
        }

        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher
            = currentLinuxDispatcher();
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

    //! Reports a protocol-level failure and ends the loop.
    //!
    //! wl_display_get_error() reports errno for a transport failure and a protocol error code
    //! otherwise; both are worth printing, because "the compositor closed the connection" and "this
    //! client sent something invalid" look identical from the call site.
    void WaylandApplication::reportFatal
        (
        const char* aWhere   //!< The call that failed.
        )
    {
        const int error = wl_display_get_error( mInternals->mDisplay );
        std::fprintf( stderr, "WaylandApplication: %s failed (display error %d, errno %d); "
            "quitting\n", aWhere, error, errno );
        QtLikeSignal::CoreApplication::quit();
    }

    //! Reports whether the seat gave this client a pointer.
    //!
    //! Worth asking rather than assuming: a seat announces its capabilities after the connection is
    //! up, and a compositor with no pointer attached will simply never send one. "No pointer
    //! events" then means "there is no pointer", not "the loop is broken".
    bool WaylandApplication::hasPointer() const
    {
        return mInternals->mPointer != nullptr;
    }

    //! Reports whether the seat gave this client a keyboard, for the same reason.
    bool WaylandApplication::hasKeyboard() const
    {
        return mInternals->mKeyboard != nullptr;
    }

    //! Gets the pointer-press signal.
    QtLikeSignal::SignalView<MouseEvent>& WaylandApplication::getMousePressed() const
    {
        return mMousePressed.view();
    }

    //! Gets the pointer-release signal.
    QtLikeSignal::SignalView<MouseEvent>& WaylandApplication::getMouseReleased() const
    {
        return mMouseReleased.view();
    }

    //! Gets the pointer-motion signal.
    QtLikeSignal::SignalView<MouseEvent>& WaylandApplication::getMouseMoved() const
    {
        return mMouseMoved.view();
    }

    //! Gets the pointer-enter signal.
    QtLikeSignal::SignalView<>& WaylandApplication::getPointerEntered() const
    {
        return mPointerEntered.view();
    }

    //! Gets the pointer-leave signal.
    QtLikeSignal::SignalView<>& WaylandApplication::getPointerLeft() const
    {
        return mPointerLeft.view();
    }

    //! Gets the key-press signal, which carries the raw evdev keycode.
    QtLikeSignal::SignalView<unsigned int>& WaylandApplication::getKeyPressed() const
    {
        return mKeyPressed.view();
    }

    //! Gets the configure signal, which carries the size the compositor suggested.
    QtLikeSignal::SignalView<int, int>& WaylandApplication::getSurfaceConfigured() const
    {
        return mSurfaceConfigured.view();
    }
}
