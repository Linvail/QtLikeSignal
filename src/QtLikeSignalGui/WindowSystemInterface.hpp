// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::WindowSystemInterface -- the one funnel every window system reports input through.

#ifndef QT_LIKE_SIGNAL_GUI_WINDOWSYSTEMINTERFACE_HPP
#define QT_LIKE_SIGNAL_GUI_WINDOWSYSTEMINTERFACE_HPP

#include "QtLikeSignalGui/InputEvents.hpp"

namespace QtLikeSignalGui
{
    class Window;

    //! The single entry point by which native input becomes a Window signal.
    //!
    //! Modelled on Qt's QWindowSystemInterface, and public for the same reason Qt keeps its
    //! semi-public: **on two of the three platforms, the producer is not this library.** The Win32
    //! backend calls these from inside its own window procedure, but on Wayland the external library
    //! owns the seat and reports touch and pointer input through its own signals, so the few lines
    //! of glue that forward those live in the application. Making this funnel public is what lets
    //! that work without QtLikeSignalGui ever linking libwayland, boost, or the external library itself.
    //!
    //! **Delivery is synchronous.** Qt queues here and flushes later, because a native event can
    //! reach it on a stack where running application code would be unsafe. This library has no such
    //! case: the Win32 window procedure runs inside EventDispatcherWin32::processPlatformEvents(),
    //! and a Linux event-source callback runs inside EventDispatcherLinux::processEvents(), so every
    //! caller is already inside a dispatch pass. A queue would add a hop and no safety.
    //!
    //! **There is deliberately no event compression.** Windows already coalesces the two floods that
    //! matter -- it synthesises at most one pending WM_MOUSEMOVE and merges the update region into a
    //! single WM_PAINT -- so a compression layer here would be inert on the only platform that
    //! currently produces events. X11 does need one, since MotionNotify and Expose genuinely pile up
    //! on the connection, and it belongs with that backend when it lands rather than being written
    //! speculatively now.
    //!
    //! Every function must be called from the thread running the event loop; the global state below
    //! is plain, not atomic, for that reason.
    class WindowSystemInterface
    {
    public:
        static void handleMousePressed
            (
            Window* aWindow,
            const MouseEvent& aEvent
            );

        static void handleMouseReleased
            (
            Window* aWindow,
            const MouseEvent& aEvent
            );

        static void handleMouseMoved
            (
            Window* aWindow,
            const MouseEvent& aEvent
            );

        static void handleMouseEntered
            (
            Window* aWindow,
            const MouseEvent& aEvent
            );

        static void handleMouseLeft
            (
            Window* aWindow
            );

        static void handleWheel
            (
            Window* aWindow,
            const WheelEvent& aEvent
            );

        static void handleKeyPressed
            (
            Window* aWindow,
            const KeyEvent& aEvent
            );

        static void handleKeyReleased
            (
            Window* aWindow,
            const KeyEvent& aEvent
            );

        static void handleTouchDown
            (
            Window* aWindow,
            const TouchDownEvent& aEvent
            );

        static void handleTouchUp
            (
            Window* aWindow,
            const TouchUpEvent& aEvent
            );

        static void handleTouchMotion
            (
            Window* aWindow,
            const TouchMotionEvent& aEvent
            );

        static void handleTouchFrame
            (
            Window* aWindow = nullptr
            );

        static void handleTouchCancel
            (
            Window* aWindow = nullptr
            );

        static void handleResize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            );

        static void handleExpose
            (
            Window* aWindow
            );

        static void handleCloseRequest
            (
            Window* aWindow
            );

        static void handleFocusChange
            (
            Window* aWindow,
            bool aGained
            );

        static void handleWindowDestroyed
            (
            Window* aWindow
            );

        //! Gets every mouse button currently held, as of the last event delivered.
        //!
        //! Maintained here rather than asked of the OS, so the answer matches the event stream the
        //! application has actually seen rather than the state a moment later.
        static MouseButtons mouseButtons();

        //! Gets the window with keyboard focus, or nullptr if none of ours has it.
        static Window* focusWindow();

        //! Gets every keyboard modifier currently active, as of the last key event delivered.
        //!
        //! Maintained here for the same reason mouseButtons() is: it answers what the application
        //! has been told, which is what its own logic was written against, rather than what the
        //! hardware happens to be doing at the moment somebody asks.
        static KeyModifiers keyModifiers();

    private:
        //! Buttons held, as of the last press or release delivered.
        static MouseButtons sMouseButtons;

        //! Modifiers active, as of the last key event delivered.
        static KeyModifiers sKeyModifiers;

        //! The window that most recently gained focus, cleared when it loses it or is destroyed.
        static Window* sFocusWindow;

        //! The window that received the most recent touch down.
        //!
        //! wl_touch reports frame and cancel against the seat, not against a surface, so there is no
        //! window on those two events to route by. Qt tracks the same thing as Touch::mFocus. A
        //! frame signal that did not say whose points had settled would not be usable.
        static Window* sTouchFocusWindow;
    };
}

#endif // QT_LIKE_SIGNAL_GUI_WINDOWSYSTEMINTERFACE_HPP
