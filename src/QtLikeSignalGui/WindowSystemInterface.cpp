// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! WindowSystemInterface implementation: global input state, then straight to the window's signals.

#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include "QtLikeSignalGui/GuiApplication.hpp"
#include "QtLikeSignalGui/Window.hpp"

namespace QtLikeSignalGui
{
    MouseButtons WindowSystemInterface::sMouseButtons {};
    Window* WindowSystemInterface::sFocusWindow { nullptr };
    KeyModifiers WindowSystemInterface::sKeyModifiers {};
    Window* WindowSystemInterface::sTouchFocusWindow { nullptr };

    //! Reports a mouse button going down.
    void WindowSystemInterface::handleMousePressed
        (
        Window* aWindow,           //!< Window the press landed on. Ignored if null.
        const MouseEvent& aEvent   //!< The press, already in this library's terms.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        sMouseButtons = aEvent.mButtons;
        aWindow->mMousePressed.emit( aEvent );
    }

    //! Reports a mouse button coming up.
    void WindowSystemInterface::handleMouseReleased
        (
        Window* aWindow,           //!< Window the press that started this landed on.
        const MouseEvent& aEvent   //!< The release.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        sMouseButtons = aEvent.mButtons;
        aWindow->mMouseReleased.emit( aEvent );
    }

    //! Reports the mouse moving.
    void WindowSystemInterface::handleMouseMoved
        (
        Window* aWindow,           //!< Window the move is relative to.
        const MouseEvent& aEvent   //!< The move; mButton is None, mButtons says if it is a drag.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        sMouseButtons = aEvent.mButtons;
        aWindow->mMouseMoved.emit( aEvent );
    }

    //! Reports the mouse entering a window's client area.
    void WindowSystemInterface::handleMouseEntered
        (
        Window* aWindow,           //!< Window entered.
        const MouseEvent& aEvent   //!< Where it entered.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mMouseEntered.emit( aEvent );
    }

    //! Reports the mouse leaving a window's client area.
    void WindowSystemInterface::handleMouseLeft
        (
        Window* aWindow   //!< Window left.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mMouseLeft.emit();
    }

    //! Reports a wheel turn.
    void WindowSystemInterface::handleWheel
        (
        Window* aWindow,           //!< Window under the pointer.
        const WheelEvent& aEvent   //!< The rotation.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mWheel.emit( aEvent );
    }

    //! Reports a finger touching down, and makes @p aWindow the touch focus.
    //!
    //! The focus is what routes the later frame and cancel, which the protocol reports against the
    //! seat rather than against any surface.
    void WindowSystemInterface::handleTouchDown
        (
        Window* aWindow,               //!< Window the finger came down on.
        const TouchDownEvent& aEvent   //!< The touch, straight from wl_touch::down.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        sTouchFocusWindow = aWindow;
        aWindow->mTouchDown.emit( aEvent );
    }

    //! Reports a finger lifting.
    void WindowSystemInterface::handleTouchUp
        (
        Window* aWindow,             //!< Window holding the touch focus.
        const TouchUpEvent& aEvent   //!< The release, straight from wl_touch::up.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mTouchUp.emit( aEvent );
    }

    //! Reports a finger moving.
    void WindowSystemInterface::handleTouchMotion
        (
        Window* aWindow,                 //!< Window holding the touch focus.
        const TouchMotionEvent& aEvent   //!< The motion, straight from wl_touch::motion.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mTouchMotion.emit( aEvent );
    }

    //! Reports that the touch points just delivered now form a consistent set.
    //!
    //! Callers that have no window to name -- which is every Wayland caller, since wl_touch::frame
    //! carries no surface -- leave @p aWindow null and the touch focus is used.
    void WindowSystemInterface::handleTouchFrame
        (
        Window* aWindow   //!< Window to notify, or null for the touch-focus window.
        )
    {
        Window* const target = ( aWindow != nullptr ) ? aWindow : sTouchFocusWindow;
        if( target == nullptr )
        {
            return;
        }

        target->mTouchFrame.emit();
    }

    //! Reports that the compositor has taken the touch sequence away.
    void WindowSystemInterface::handleTouchCancel
        (
        Window* aWindow   //!< Window to notify, or null for the touch-focus window.
        )
    {
        Window* const target = ( aWindow != nullptr ) ? aWindow : sTouchFocusWindow;

        // Cleared before emitting, not after: the sequence is over either way, and a slot that
        // destroys the window during the emission must not leave a pointer to it behind here.
        sTouchFocusWindow = nullptr;

        if( target != nullptr )
        {
            target->mTouchCancel.emit();
        }
    }

    //! Reports a new client-area size, and records it on the window.
    void WindowSystemInterface::handleResize
        (
        Window* aWindow,   //!< Window resized.
        int aWidth,        //!< New client-area width in pixels.
        int aHeight        //!< New client-area height in pixels.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        // Recorded before the signal, so that a slot asking the window for its size during the
        // emission gets the new one rather than the one it is being told about.
        aWindow->mWidth  = aWidth;
        aWindow->mHeight = aHeight;
        aWindow->mResized.emit( aWidth, aHeight );
    }

    //! Reports that the window needs redrawing.
    void WindowSystemInterface::handleExpose
        (
        Window* aWindow   //!< Window to repaint.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mExposed.emit();
    }

    //! Reports that the user asked to close the window.
    //!
    //! Emits the signal first, so a slot sees the request before any automatic reaction to it, then
    //! lets the application apply its quit-on-last-window-closed policy.
    //!
    //! The lifetime token is not belt-and-braces. Deleting the window from the slot that hears about
    //! its close is an entirely reasonable thing for an application to do, and the policy step below
    //! would then be handed a freed Window. Asking the token afterwards is the library's own way of
    //! surviving that, and it costs a shared-pointer read on a path that runs once per close.
    void WindowSystemInterface::handleCloseRequest
        (
        Window* aWindow   //!< Window whose close was requested.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        const QtLikeSignal::ObjectLife life = aWindow->objectLife();

        aWindow->mCloseRequested.emit();

        if( life.expired() )
        {
            return;
        }

        GuiApplication* const application = GuiApplication::instance();
        if( application != nullptr )
        {
            application->handleCloseRequested( aWindow );
        }
    }

    //! Reports a window gaining or losing keyboard focus.
    void WindowSystemInterface::handleFocusChange
        (
        Window* aWindow,   //!< Window whose focus changed.
        bool aGained       //!< True if it gained focus, false if it lost it.
        )
    {
        if( aWindow == nullptr )
        {
            return;
        }

        if( aGained )
        {
            sFocusWindow = aWindow;
        }
        else if( sFocusWindow == aWindow )
        {
            sFocusWindow = nullptr;
        }

        aWindow->mFocusChanged.emit( aGained );
    }

    //! Drops every reference to a window that is being destroyed.
    //!
    //! Called from ~Window(). Without it, focusWindow() and the touch focus would keep handing out a
    //! pointer to freed memory -- and the touch focus in particular is dereferenced by a later
    //! frame or cancel that names no window of its own.
    void WindowSystemInterface::handleWindowDestroyed
        (
        Window* aWindow   //!< The window going away.
        )
    {
        if( sFocusWindow == aWindow )
        {
            sFocusWindow = nullptr;
        }

        if( sTouchFocusWindow == aWindow )
        {
            sTouchFocusWindow = nullptr;
        }
    }

    //! Delivers a key press, and records the modifiers it carried.
    //!
    //! **The modifier set comes from the backend, not from here.** Each platform already knows
    //! which modifiers were active when the key changed -- Win32 from the key state, X11 from the
    //! event's state mask, Wayland and DRM from tracking the modifier keys themselves -- and
    //! recomputing it here from press and release counts would be a second, worse answer that
    //! disagreed with the first whenever focus changed while a modifier was held.
    //!
    //! What is recorded is the answer, so keyModifiers() can be asked outside an event.
    void WindowSystemInterface::handleKeyPressed
        (
        Window* aWindow,         //!< Window with keyboard focus.
        const KeyEvent& aEvent   //!< The key.
        )
    {
        sKeyModifiers = aEvent.mModifiers;

        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mKeyPressed.emit( aEvent );
    }

    //! Delivers a key release, and records the modifiers it carried.
    void WindowSystemInterface::handleKeyReleased
        (
        Window* aWindow,         //!< Window with keyboard focus.
        const KeyEvent& aEvent   //!< The key.
        )
    {
        sKeyModifiers = aEvent.mModifiers;

        if( aWindow == nullptr )
        {
            return;
        }

        aWindow->mKeyReleased.emit( aEvent );
    }

    //! Gets every keyboard modifier currently active.
    KeyModifiers WindowSystemInterface::keyModifiers()
    {
        return sKeyModifiers;
    }

    //! Gets every mouse button currently held.
    MouseButtons WindowSystemInterface::mouseButtons()
    {
        return sMouseButtons;
    }

    //! Gets the window with keyboard focus, or nullptr.
    Window* WindowSystemInterface::focusWindow()
    {
        return sFocusWindow;
    }
}
