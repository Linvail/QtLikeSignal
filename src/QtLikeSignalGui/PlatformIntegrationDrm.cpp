// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The DRM backend: opening libinput, joining the loop, and turning evdev input into
//! WindowSystemInterface calls.

#include "QtLikeSignalGui/PlatformIntegrationDrm.hpp"

#include "QtLikeSignalGui/Window.hpp"
#include "QtLikeSignalGui/KeyTranslation.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Thread.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignalGui/LogCategories.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>

namespace QtLikeSignalGui
{
    //! Opens one input device for libinput.
    //!
    //! extern "C" because it is handed to a C library as a function pointer, and a C++-linkage
    //! pointer is not the same type as the C-linkage one the struct declares. Every toolchain
    //! accepts the mismatch, which is exactly why it is worth spelling correctly rather than
    //! relying on that.
    //!
    //! Plain open(), with no privilege escalation of any kind. /dev/input/event* is not world
    //! readable, so this succeeds only for a process that already has the access -- root, a member
    //! of the `input` group, or one handed the node by logind. A program that needs to drop
    //! privileges keeps its own seat manager and passes the devices in through
    //! QTLIKESIGNAL_INPUT_DEVICES; that decision is not this library's to make.
    //!
    //! @return the descriptor, or a negative errno.
    extern "C" int qtLikeSignalGuiOpenRestricted
        (
        const char* aPath,   //!< Device node to open.
        int aFlags,          //!< Flags libinput wants; passed through unchanged.
        void* aUserData      //!< Unused.
        )
    {
        static_cast<void>( aUserData );

        const int descriptor = open( aPath, aFlags );
        return ( descriptor < 0 ) ? -errno : descriptor;
    }

    //! Closes an input device for libinput. See qtLikeSignalGuiOpenRestricted().
    extern "C" void qtLikeSignalGuiCloseRestricted
        (
        int aFd,          //!< Descriptor to close.
        void* aUserData   //!< Unused.
        )
    {
        static_cast<void>( aUserData );
        close( aFd );
    }

    namespace
    {
        //! The callbacks libinput uses to reach the device nodes.
        const libinput_interface kInterface =
        {
            &qtLikeSignalGuiOpenRestricted,
            &qtLikeSignalGuiCloseRestricted
        };

        //! Gets the running thread's dispatcher as an EventDispatcherLinux, or null if it is not
        //! one.
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

        //! Splits a colon-separated list, the way PATH is written.
        std::vector<std::string> splitPaths
            (
            const std::string& aList   //!< Colon-separated device paths.
            )
        {
            std::vector<std::string> paths;
            std::string::size_type start = 0;

            while( start <= aList.size() )
            {
                const std::string::size_type end = aList.find( ':', start );
                const std::string piece = ( end == std::string::npos )
                    ? aList.substr( start )
                    : aList.substr( start, end - start );

                if( !piece.empty() )
                {
                    paths.push_back( piece );
                }

                if( end == std::string::npos )
                {
                    break;
                }
                start = end + 1;
            }

            return paths;
        }

        //! Maps an evdev button code to the button this library reports.
        //!
        //! These are the raw kernel codes from <linux/input-event-codes.h>, which is what libinput
        //! passes through unchanged. BTN_SIDE and BTN_EXTRA are the two thumb buttons that Windows
        //! calls XBUTTON1 and XBUTTON2 and X11 numbers 8 and 9.
        MouseButton buttonOf
            (
            unsigned int aCode   //!< The evdev button code.
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

        //! Clamps a coordinate into [0, aLimit - 1], the pixels that actually exist.
        double clampToDrawable
            (
            double aValue,   //!< The coordinate.
            int aLimit       //!< The drawable's extent in that axis, in pixels.
            )
        {
            if( aValue < 0.0 )
            {
                return 0.0;
            }

            const double highest = static_cast<double>( aLimit > 0 ? aLimit - 1 : 0 );
            return ( aValue > highest ) ? highest : aValue;
        }
    }

    //! Constructs the backend. libinput is opened when the first window is adopted.
    PlatformIntegrationDrm::PlatformIntegrationDrm()
    {
    }

    //! Closes the input devices.
    PlatformIntegrationDrm::~PlatformIntegrationDrm()
    {
        closeInput();
    }

    //! Gets which window system this backend is.
    PlatformType PlatformIntegrationDrm::type() const
    {
        return PlatformType::Drm;
    }

    //! There is no window system here, so there is nothing to create a window with.
    bool PlatformIntegrationDrm::canCreateWindows() const
    {
        return false;
    }

    //! The external library sets the mode and makes the surface; this backend adopts the result.
    bool PlatformIntegrationDrm::canAdoptWindows() const
    {
        return true;
    }

    //! Takes charge of input for a DRM surface the external library has already set up.
    //!
    //! The size is required and cannot be inferred: there is no window system to ask, and the mode
    //! is known only to whoever set it. Everything this backend does with coordinates -- clamping
    //! the pointer, transforming a touch point -- is relative to it.
    //!
    //! @return the new Window, owned by the caller, or nullptr if the size was missing, a surface
    //!         was already adopted, or the input devices could not be opened.
    Window* PlatformIntegrationDrm::adoptWindow
        (
        const NativeWindow& aNative   //!< The surface's size, and optionally an opaque handle.
        )
    {
        if( aNative.mWidth <= 0 || aNative.mHeight <= 0 )
        {
            qCWarning( gLogGuiDrm )
                << "QtLikeSignalGui: adoptWindow on DRM needs NativeWindow::mWidth and mHeight;"
                << "there is no window system to ask for the scanout size";
            return nullptr;
        }

        if( mWindow != nullptr )
        {
            // One scanout, one surface. A second would have no way to receive anything: with no
            // window system there is nothing in an input event that says which surface it meant.
            qCWarning( gLogGuiDrm )
                <<
                "QtLikeSignalGui: the drm backend supports one surface, and one is already adopted";
            return nullptr;
        }

        if( mInput == nullptr && !openInput() )
        {
            return nullptr;
        }

        Window* const window = newWindow( this, aNative, aNative.mWidth, aNative.mHeight );
        mWindow = window;

        // Centred, because there is no previous position to restore and no window manager with an
        // opinion. A pointer that started in a corner would look like a bug on first movement.
        mPointerX = static_cast<double>( aNative.mWidth ) / 2.0;
        mPointerY = static_cast<double>( aNative.mHeight ) / 2.0;

        // Nothing else will ever say this: with no window manager there is no focus to gain or
        // lose, and a program asking GuiApplication::focusWindow() should get the one surface there
        // is rather than a null that means something different here than it does everywhere else.
        WindowSystemInterface::handleFocusChange( window, true );

        return window;
    }

    //! Stops reporting input for the surface. It belongs to the library that made it.
    void PlatformIntegrationDrm::releaseNativeWindow
        (
        Window* aWindow   //!< The window being destroyed.
        )
    {
        if( mWindow == aWindow )
        {
            mWindow        = nullptr;
            mUpdatePending = false;
            mButtons       = MouseButtons();
        }

        assignNative( aWindow, NativeWindow() );
    }

    //! Does nothing: there is no window manager, so there is no title bar to write into.
    void PlatformIntegrationDrm::setWindowTitle
        (
        Window* aWindow,           //!< Ignored.
        const std::string& aTitle  //!< Ignored.
        )
    {
        static_cast<void>( aWindow );
        static_cast<void>( aTitle );
    }

    //! Records the flag and nothing else: what reaches the screen is the external library's
    //! scanout.
    //!
    //! Not silently ignored, because isVisible() is still a question a program can reasonably ask
    //! and answer for itself -- it just does not mean the display turns off.
    void PlatformIntegrationDrm::setWindowVisible
        (
        Window* aWindow,   //!< Window whose flag to set.
        bool aVisible      //!< True once it is considered shown.
        )
    {
        assignVisible( aWindow, aVisible );
    }

    //! Refused: the drawable's size is the display mode, and the mode is not this library's to set.
    void PlatformIntegrationDrm::setClientSize
        (
        Window* aWindow,   //!< Ignored.
        int aWidth,        //!< Ignored.
        int aHeight        //!< Ignored.
        )
    {
        static_cast<void>( aWindow );
        static_cast<void>( aWidth );
        static_cast<void>( aHeight );

        qCWarning( gLogGuiDrm )
            <<
            "QtLikeSignalGui: setClientSize() has no meaning on drm; the size is the display mode,"
            << "which belongs to whoever set it";
    }

    //! Asks for a repaint by posting one onto the loop.
    //!
    //! Nothing else can ask. On X11 the server sends an Expose and on Win32 the OS synthesises a
    //! WM_PAINT, but a DRM program has no compositor to notice that anything was uncovered -- it
    //! renders because it decided to. Posting keeps the promise the other backends make: the expose
    //! signal arrives from inside a dispatch pass, never from inside requestUpdate() itself, so a
    //! renderer driving continuous frames from its own expose slot does not recurse.
    //!
    //! Collapsed to one per pass by the pending flag, for the reason the X11 backend collapses its
    //! synthetic Expose.
    void PlatformIntegrationDrm::requestUpdate
        (
        Window* aWindow   //!< Window to repaint.
        )
    {
        if( aWindow != mWindow || mUpdatePending )
        {
            return;
        }

        mUpdatePending = true;

        // Weak, not a bare this. The task can outlive this backend -- the application may be torn
        // down between the post and the pass that would have run it -- and touching a destroyed
        // integration is the sort of shutdown crash that only ever shows up in the field.
        std::weak_ptr<int> life = mLifeToken;

        if( !QtLikeSignal::CoreApplication::post( [this, life]()
            {
                if( life.expired() )
                {
                    return;
                }

                mUpdatePending = false;

                // Re-read rather than captured: the window may have been destroyed between the post
                // and this running, and a captured pointer would then be dangling. The member is
                // cleared by releaseNativeWindow(), so reading it here is the check.
                if( mWindow != nullptr )
                {
                    WindowSystemInterface::handleExpose( mWindow );
                }
            } ) )
        {
            // The loop is gone -- the application is shutting down -- so the repaint will never
            // run. Cleared so a later request is not refused as a duplicate of one that is not
            // coming.
            mUpdatePending = false;
        }
    }

    //! Creates the libinput context and puts its descriptor in the loop's poll set.
    //!
    //! @return true if input is being read.
    bool PlatformIntegrationDrm::openInput()
    {
        const std::string explicitDevices = environmentValue( "QTLIKESIGNAL_INPUT_DEVICES" );

        if( !explicitDevices.empty() )
        {
            // The path interface: exactly the devices named, and no udev at all. What a container
            // or a test rig needs, since udev's device database is not there to enumerate.
            libinput* const input = libinput_path_create_context( &kInterface, nullptr );
            if( input == nullptr )
            {
                qCWarning( gLogGuiDrm )
                    << "QtLikeSignalGui: libinput_path_create_context() failed";
                return false;
            }

            int added = 0;
            const std::vector<std::string> paths = splitPaths( explicitDevices );
            for( const std::string& path : paths )
            {
                if( libinput_path_add_device( input, path.c_str() ) != nullptr )
                {
                    ++added;
                }
                else
                {
                    qCWarning( gLogGuiDrm )
                        << "QtLikeSignalGui: could not open input device" << path;
                }
            }

            if( added == 0 )
            {
                qCWarning( gLogGuiDrm )
                    << "QtLikeSignalGui: QTLIKESIGNAL_INPUT_DEVICES named no device"
                    << "that could be opened";
                libinput_unref( input );
                return false;
            }

            mInput = input;
        }
        else
        {
            udev* const udevContext = udev_new();
            if( udevContext == nullptr )
            {
                qCWarning( gLogGuiDrm ) << "QtLikeSignalGui: udev_new() failed";
                return false;
            }

            libinput* const input = libinput_udev_create_context( &kInterface, nullptr,
                udevContext );
            if( input == nullptr )
            {
                qCWarning( gLogGuiDrm )
                    << "QtLikeSignalGui: libinput_udev_create_context() failed";
                udev_unref( udevContext );
                return false;
            }

            const std::string seat = environmentValue( "QTLIKESIGNAL_SEAT" );
            const char* const seatName = seat.empty() ? "seat0" : seat.c_str();

            if( libinput_udev_assign_seat( input, seatName ) != 0 )
            {
                qCCritical( gLogGuiDrm )
                    << "QtLikeSignalGui: libinput_udev_assign_seat() failed; the process needs read"
                    << "access to /dev/input/event* (root, the input group, or logind). Seat"
                    << seatName;
                libinput_unref( input );
                udev_unref( udevContext );
                return false;
            }

            mUdev  = udevContext;
            mInput = input;
        }

        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher =
            currentLinuxDispatcher();
        if( !dispatcher )
        {
            qCWarning( gLogGuiDrm )
                << "QtLikeSignalGui: this thread is not running EventDispatcherLinux, so the input"
                << "devices cannot join the event loop; construct the GuiApplication on the thread"
                << "that will call exec()";
            closeInput();
            return false;
        }

        mInputFd = libinput_get_fd( static_cast<libinput*>( mInput ) );

        if( !dispatcher->registerEventSource( mInputFd, POLLIN,
            [this]( short aEvents )
            {
                pumpInput( aEvents );
            } ) )
        {
            qCWarning( gLogGuiDrm )
                << "QtLikeSignalGui: registerEventSource() was refused for the libinput descriptor"
                << mInputFd;
            mInputFd = -1;
            closeInput();
            return false;
        }

        // One drain before the loop ever blocks. Assigning the seat enumerates the devices, and the
        // DEVICE_ADDED events for them are queued already -- with nothing left on the descriptor
        // for poll() to report. Left there they would sit until the next real input arrived.
        pumpInput( POLLIN );
        return true;
    }

    //! Unregisters the descriptor and releases libinput and udev.
    void PlatformIntegrationDrm::closeInput()
    {
        if( mInputFd >= 0 )
        {
            // Unregistered from the loop's own thread, so EventDispatcherLinux's contract makes the
            // call synchronous: the callback will not run again, not even for a readiness the
            // current poll() round has already observed. That is what makes it safe to free
            // everything it touches immediately afterwards.
            const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher
                = currentLinuxDispatcher();
            if( dispatcher )
            {
                dispatcher->unregisterEventSource( mInputFd );
            }
            mInputFd = -1;
        }

        if( mInput != nullptr )
        {
            libinput_unref( static_cast<libinput*>( mInput ) );
            mInput = nullptr;
        }

        if( mUdev != nullptr )
        {
            udev_unref( static_cast<udev*>( mUdev ) );
            mUdev = nullptr;
        }
    }

    //! Drains every event libinput has and delivers each one.
    //!
    //! Called by the dispatcher, on the dispatcher's thread, whenever poll() reports the descriptor
    //! ready -- and once from openInput() before the loop starts.
    //!
    //! **libinput_dispatch() first, then the loop.** The descriptor becoming readable means bytes
    //! arrived from the kernel, not that an event is available: libinput_dispatch() is what reads
    //! them and turns them into events, and libinput_get_event() then hands them out one at a time
    //! until it returns null. Calling get_event without dispatch reads nothing at all, and reading
    //! one event per readiness would leave the rest queued behind an empty descriptor -- the same
    //! trap the X11 drain avoids, arrived at from a different direction.
    void PlatformIntegrationDrm::pumpInput
        (
        short aEvents   //!< poll(2) revents for the descriptor.
        )
    {
        if( mInput == nullptr )
        {
            return;
        }

        if( ( aEvents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
        {
            qCCritical( gLogGuiDrm )
                << "QtLikeSignalGui: the libinput descriptor failed, so input has stopped; revents"
                << QtLikeSignal::logHex( static_cast<unsigned int>( aEvents ) );

            // Input stops; the program does not. Unlike a lost X11 connection there is still a
            // display being scanned out and a renderer drawing to it, so ending the loop here would
            // destroy something that is still working because something else broke.
            closeInput();
            return;
        }

        libinput* const input = static_cast<libinput*>( mInput );

        if( libinput_dispatch( input ) != 0 )
        {
            qCWarning( gLogGuiDrm ) << "QtLikeSignalGui: libinput_dispatch() failed";
            return;
        }

        while( libinput_event* const event = libinput_get_event( input ) )
        {
            dispatchNativeEvent( event );

            // Destroyed here, unconditionally, whatever the handler did with it. libinput hands out
            // a reference per event and leaks it otherwise.
            libinput_event_destroy( event );
        }
    }

    //! Translates one libinput event and reports it through WindowSystemInterface.
    void PlatformIntegrationDrm::dispatchNativeEvent
        (
        void* aEvent   //!< The libinput_event just taken off the queue.
        )
    {
        if( mWindow == nullptr )
        {
            return;
        }

        libinput_event* const event = static_cast<libinput_event*>( aEvent );

        switch( libinput_event_get_type( event ) )
        {
        case LIBINPUT_EVENT_POINTER_MOTION:
            handlePointerMotion( event, false );
            return;

        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            handlePointerMotion( event, true );
            return;

        case LIBINPUT_EVENT_POINTER_BUTTON:
            handlePointerButton( event );
            return;

        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
            handlePointerScroll( event );
            return;

        case LIBINPUT_EVENT_KEYBOARD_KEY:
            handleKeyboardKey( event );
            return;

        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_UP:
        case LIBINPUT_EVENT_TOUCH_MOTION:
            handleTouch( event, static_cast<int>( libinput_event_get_type( event ) ) );
            return;

        case LIBINPUT_EVENT_TOUCH_FRAME:
            WindowSystemInterface::handleTouchFrame( mWindow );
            return;

        case LIBINPUT_EVENT_TOUCH_CANCEL:
            WindowSystemInterface::handleTouchCancel( mWindow );
            return;

        default:
            // Device arrivals and departures, tablet, gestures and switches all land here.
            // Ignored rather than logged: a running system produces these constantly, and a library
            // that printed each one would be unusable.
            return;
        }
    }

    //! Reports pointer movement, keeping the position this backend has to maintain itself.
    void PlatformIntegrationDrm::handlePointerMotion
        (
        void* aEvent,     //!< The libinput_event.
        bool aAbsolute    //!< True for a touchscreen or tablet, which report where rather than how far.
        )
    {
        libinput_event_pointer* const pointer = libinput_event_get_pointer_event(
            static_cast<libinput_event*>( aEvent ) );

        if( aAbsolute )
        {
            movePointerTo(
                libinput_event_pointer_get_absolute_x_transformed( pointer, mWindow->width() ),
                libinput_event_pointer_get_absolute_y_transformed( pointer, mWindow->height() ) );
        }
        else
        {
            // get_dx(), not get_dx_unaccelerated(): the accelerated figure is the one libinput has
            // applied its pointer-acceleration profile to, which is what makes a mouse feel the way
            // it does everywhere else on the machine. The unaccelerated value is for a program
            // doing its own acceleration, which this is not.
            movePointerTo(
                mPointerX + libinput_event_pointer_get_dx( pointer ),
                mPointerY + libinput_event_pointer_get_dy( pointer ) );
        }

        MouseEvent mouse;
        mouse.mPos.mX      = static_cast<int>( mPointerX );
        mouse.mPos.mY      = static_cast<int>( mPointerY );

        // The same numbers: with one full-screen surface and no window manager, the drawable's
        // coordinate space and the screen's are the same space.
        mouse.mGlobalPos   = mouse.mPos;
        mouse.mButton      = MouseButton::None;
        mouse.mButtons     = mButtons;
        mouse.mTimestampMs = static_cast<unsigned long>(
            libinput_event_pointer_get_time( pointer ) );

        WindowSystemInterface::handleMouseMoved( mWindow, mouse );
    }

    //! Reports a button going down or coming up, maintaining the held set as it goes.
    //! Translates one libinput key event and delivers it.
    //!
    //! **No text, and that is a dependency decision rather than an oversight.** libinput reports
    //! evdev codes, which name a place on the keyboard; turning one into a character needs the
    //! user's keymap, which means xkbcommon. This backend exists for embedded targets where the
    //! layout is fixed and known, and a program there can map the handful of keys it cares about
    //! from KeyEvent::mKey. A text field would want the dependency; nothing here does yet.
    //!
    //! Modifiers are accumulated from the modifier keys themselves, for the same reason.
    void PlatformIntegrationDrm::handleKeyboardKey
        (
        void* aEvent   //!< The libinput_event.
        )
    {
        libinput_event_keyboard* const keyboard = libinput_event_get_keyboard_event(
            static_cast<libinput_event*>( aEvent ) );
        if( keyboard == nullptr )
        {
            return;
        }

        const unsigned int code = libinput_event_keyboard_get_key( keyboard );
        const bool pressed = ( libinput_event_keyboard_get_key_state( keyboard )
            == LIBINPUT_KEY_STATE_PRESSED );

        KeyEvent event;
        event.mKey         = keyFromEvdevCode( code );
        event.mNativeCode  = code;
        event.mTimestampMs = static_cast<unsigned long>(
            libinput_event_keyboard_get_time( keyboard ) );

        // The set is updated *before* the event is filled in for a press and after for a release,
        // so that a modifier's own press reports itself as held and its release reports itself as
        // not -- the same before/after rule the mouse buttons follow.
        const KeyModifier own = modifierForKey( event.mKey );
        if( own != KeyModifier::None )
        {
            if( pressed )
            {
                mModifiers |= own;
            }
            else
            {
                mModifiers.remove( own );
            }
        }

        event.mModifiers = mModifiers;

        // libinput does not repeat: it reports the hardware, and repeating is a policy the
        // compositor or toolkit above it applies. So mAutoRepeat stays false here, always, rather
        // than this backend inventing a rate nobody asked for.
        if( pressed )
        {
            WindowSystemInterface::handleKeyPressed( mWindow, event );
        }
        else
        {
            WindowSystemInterface::handleKeyReleased( mWindow, event );
        }
    }

    void PlatformIntegrationDrm::handlePointerButton
        (
        void* aEvent   //!< The libinput_event.
        )
    {
        libinput_event_pointer* const pointer = libinput_event_get_pointer_event(
            static_cast<libinput_event*>( aEvent ) );

        const MouseButton button = buttonOf( libinput_event_pointer_get_button( pointer ) );
        if( button == MouseButton::None )
        {
            // A button this library has no name for -- the extra keys on a gaming mouse, say.
            // Dropped rather than reported as None, which every receiver would read as "a move".
            return;
        }

        const bool pressed = libinput_event_pointer_get_button_state( pointer )
            == LIBINPUT_BUTTON_STATE_PRESSED;

        // Updated before the event is built, so mButtons carries the state *after* the change --
        // what MouseEvent documents, and what both other backends produce.
        if( pressed )
        {
            mButtons |= button;
        }
        else
        {
            mButtons.remove( button );
        }

        MouseEvent mouse;
        mouse.mPos.mX      = static_cast<int>( mPointerX );
        mouse.mPos.mY      = static_cast<int>( mPointerY );
        mouse.mGlobalPos   = mouse.mPos;
        mouse.mButton      = button;
        mouse.mButtons     = mButtons;
        mouse.mTimestampMs = static_cast<unsigned long>(
            libinput_event_pointer_get_time( pointer ) );

        if( pressed )
        {
            WindowSystemInterface::handleMousePressed( mWindow, mouse );
        }
        else
        {
            WindowSystemInterface::handleMouseReleased( mWindow, mouse );
        }
    }

    //! Reports a scroll.
    void PlatformIntegrationDrm::handlePointerScroll
        (
        void* aEvent   //!< The libinput_event.
        )
    {
        libinput_event_pointer* const pointer = libinput_event_get_pointer_event(
            static_cast<libinput_event*>( aEvent ) );

        WheelEvent wheel;
        wheel.mPos.mX    = static_cast<int>( mPointerX );
        wheel.mPos.mY    = static_cast<int>( mPointerY );
        wheel.mGlobalPos = wheel.mPos;

        // v120, which is libinput's own name for the unit Windows calls WHEEL_DELTA: one detent is
        // 120, and a high-resolution wheel or a touchpad reports a fraction of it. WheelEvent uses
        // the same unit, so this is a copy rather than a conversion.
        //
        // Negated on the vertical axis, and only there. libinput counts a scroll *towards* the user
        // as positive; WheelEvent counts it as negative, matching Windows and X11 button 4. The
        // horizontal axis already agrees -- positive is to the right in both.
        if( libinput_event_pointer_has_axis( pointer,
            LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) != 0 )
        {
            wheel.mAngleDeltaY = -static_cast<int>( libinput_event_pointer_get_scroll_value_v120(
                pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ) );
        }

        if( libinput_event_pointer_has_axis( pointer,
            LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) != 0 )
        {
            wheel.mAngleDeltaX = static_cast<int>( libinput_event_pointer_get_scroll_value_v120(
                pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL ) );
        }

        if( wheel.mAngleDeltaX == 0 && wheel.mAngleDeltaY == 0 )
        {
            // A scroll-stop event, which libinput sends with both axes at zero to say a finger has
            // left the touchpad. Nothing downstream has a use for it, and delivering it as a scroll
            // of nothing would make every accumulator emit a no-op.
            return;
        }

        wheel.mButtons     = mButtons;
        wheel.mTimestampMs = static_cast<unsigned long>(
            libinput_event_pointer_get_time( pointer ) );

        WindowSystemInterface::handleWheel( mWindow, wheel );
    }

    //! Reports a touch point going down, moving, or lifting.
    //!
    //! The one backend where touch actually arrives. libinput reports down, up, motion, frame and
    //! cancel, which is the vocabulary wl_touch uses -- so the five signals Window declares for
    //! Wayland are served here without translation.
    void PlatformIntegrationDrm::handleTouch
        (
        void* aEvent,   //!< The libinput_event.
        int aType       //!< Its libinput_event_type, already read by the caller.
        )
    {
        libinput_event_touch* const touch = libinput_event_get_touch_event(
            static_cast<libinput_event*>( aEvent ) );

        // The seat slot, not the device slot: it is unique across every touch device on the seat,
        // so two touchscreens cannot hand out the same finger id for different fingers.
        const int slot = libinput_event_touch_get_seat_slot( touch );
        const unsigned int time = libinput_event_touch_get_time( touch );

        switch( aType )
        {
        case LIBINPUT_EVENT_TOUCH_DOWN:
        {
            TouchDownEvent down;

            // Zero, and honestly so: the serial exists to quote back to a Wayland compositor when
            // asking for a move or a grab, and there is no compositor here to quote it to.
            down.mSerial = 0;
            down.mTimeMs = time;
            down.mId     = slot;
            down.mX      = libinput_event_touch_get_x_transformed( touch, mWindow->width() );
            down.mY      = libinput_event_touch_get_y_transformed( touch, mWindow->height() );

            WindowSystemInterface::handleTouchDown( mWindow, down );
            return;
        }

        case LIBINPUT_EVENT_TOUCH_MOTION:
        {
            TouchMotionEvent motion;
            motion.mTimeMs = time;
            motion.mId     = slot;
            motion.mX      = libinput_event_touch_get_x_transformed( touch, mWindow->width() );
            motion.mY      = libinput_event_touch_get_y_transformed( touch, mWindow->height() );

            WindowSystemInterface::handleTouchMotion( mWindow, motion );
            return;
        }

        case LIBINPUT_EVENT_TOUCH_UP:
        {
            // No position, matching wl_touch::up and for the same reason: the protocol does not
            // carry one, and inventing the last known point here would make a value that looks
            // measured but is not.
            TouchUpEvent up;
            up.mSerial = 0;
            up.mTimeMs = time;
            up.mId     = slot;

            WindowSystemInterface::handleTouchUp( mWindow, up );
            return;
        }

        default:
            return;
        }
    }

    //! Moves the pointer to a position, clamped to the drawable.
    //!
    //! Clamping is the whole of what a window system would otherwise do here. Without it a relative
    //! mouse walks off the display and never comes back: the position keeps accumulating, so the
    //! user has to push the same distance in the other direction before anything moves on screen.
    void PlatformIntegrationDrm::movePointerTo
        (
        double aX,   //!< Desired X in drawable coordinates.
        double aY    //!< Desired Y in drawable coordinates.
        )
    {
        mPointerX = clampToDrawable( aX, mWindow->width() );
        mPointerY = clampToDrawable( aY, mWindow->height() );
    }
}
