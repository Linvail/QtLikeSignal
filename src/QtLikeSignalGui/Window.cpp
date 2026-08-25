// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Window implementation: everything a Window does, it asks its PlatformIntegration to do.

#include "QtLikeSignalGui/Window.hpp"

#include "QtLikeSignalGui/PlatformIntegration.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

namespace QtLikeSignalGui
{
    //! Constructs a Window fronting an already-created native window.
    //!
    //! Private; PlatformIntegration::newWindow() is the only caller. The native handle is a
    //! constructor argument rather than something set afterwards because there is no useful state in
    //! which a Window exists without one.
    Window::Window
        (
        PlatformIntegration* aIntegration,   //!< Backend that made the native window. Not owned.
        const NativeWindow& aNative,         //!< The native window this object fronts.
        int aWidth,                          //!< Initial client-area width in pixels.
        int aHeight                          //!< Initial client-area height in pixels.
        )
        : QtLikeSignal::Object()
        , mIntegration( aIntegration )
        , mNative( aNative )
        , mWidth( aWidth )
        , mHeight( aHeight )
    {
    }

    //! Destroys the window, releasing the native window behind it.
    //!
    //! **What "releasing" means depends on who made it.** A backend that created the native window
    //! destroys it here, which is Win32. A backend that only adopted one an external library
    //! created stops listening to it and leaves it standing, which is X11 -- destroying a window
    //! this library never made would pull it out from under whoever did.
    //!
    //! The order matters. The global input state is cleared first, so that a focus or touch-focus
    //! pointer aimed at this window cannot outlive it -- WindowSystemInterface would otherwise hand
    //! a freed Window to the next frame or cancel that arrived. Only then is the native side let go,
    //! which on Win32 detaches the window procedure before calling DestroyWindow so that no message
    //! can reach a half-destroyed object.
    Window::~Window()
    {
        WindowSystemInterface::handleWindowDestroyed( this );

        if( hasNative() )
        {
            mIntegration->releaseNativeWindow( this );
        }
    }

    //! Shows the window. Does nothing if the native window has already gone away.
    void Window::show()
    {
        if( hasNative() )
        {
            mIntegration->setWindowVisible( this, true );
        }
    }

    //! Hides the window without destroying it.
    void Window::hide()
    {
        if( hasNative() )
        {
            mIntegration->setWindowVisible( this, false );
        }
    }

    //! Sets the window caption.
    void Window::setTitle
        (
        const std::string& aTitle   //!< New caption, in UTF-8.
        )
    {
        if( hasNative() )
        {
            mIntegration->setWindowTitle( this, aTitle );
        }
    }

    //! Resizes the window so that its **client area** is exactly @p aWidth by @p aHeight.
    //!
    //! The size that matters to a renderer is the drawable one, and it is not the size the window
    //! was given: the frame, the caption and any menu bar all sit outside it. This sets the drawable
    //! size and lets the outer window land wherever it must to produce it.
    //!
    //! Measured from the live window rather than calculated from its style, which is what makes it
    //! correct in the cases a calculation is not: a menu bar that has wrapped onto a second line, a
    //! frame scaled by the display's DPI, a window the shell has clamped.
    //!
    //! A resize signal follows if the size actually changed, exactly as though the user had dragged
    //! the border -- so a renderer connected to it needs no special case for this call.
    void Window::setClientSize
        (
        int aWidth,   //!< Desired client-area width in pixels. Ignored if not positive.
        int aHeight   //!< Desired client-area height in pixels. Ignored if not positive.
        )
    {
        if( hasNative() && aWidth > 0 && aHeight > 0 )
        {
            mIntegration->setClientSize( this, aWidth, aHeight );
        }
    }

    //! Attaches a menu bar, keeping the client area the size it already was. **Win32 only.**
    //!
    //! @p aMenuHandle is an HMENU on Win32, passed as void* for the reason nativeHandle() is one --
    //! so this header stays free of <windows.h>. Pass nullptr to remove the menu.
    //!
    //! **This exists because the raw SetMenu() would shrink the drawable.** A menu bar lives outside
    //! the client area, so attaching one to a 1280x800 window leaves roughly 1280x780 to render
    //! into, and a GL context sized from what was asked for would be wrong from its first frame.
    //! This measures the client area before attaching the menu and puts it back afterwards, so the
    //! surface the renderer owns is unchanged and the window grows instead.
    //!
    //! Building the menu is the caller's: this library has no menu model, no actions and no
    //! shortcuts, and inventing one would be a far larger thing than the window it hangs off.
    //! Construct it with CreateMenu()/AppendMenu() and hand the result over; ownership passes to the
    //! window, which destroys it with itself.
    //!
    //! Does nothing on a backend with no menu bars to attach. Wayland has no server-side menus at
    //! all, and on X11 a menu bar is drawn by the client rather than by the window system.
    void Window::setMenu
        (
        void* aMenuHandle   //!< HMENU on Win32; nullptr to remove the menu.
        )
    {
        if( hasNative() )
        {
            mIntegration->setMenu( this, aMenuHandle );
        }
    }

    //! Asks for a repaint. The expose signal arrives through the loop, like every other event.
    //!
    //! Named as Qt6 names it on QWindow, and doing what Qt's does: mark the window dirty and return
    //! immediately rather than painting here. Calling it from inside an exposed slot is the normal way
    //! to drive continuous rendering, and is safe: the backend invalidates after having validated
    //! the region that triggered the current paint, so the request is not swallowed.
    void Window::requestUpdate()
    {
        if( hasNative() )
        {
            mIntegration->requestUpdate( this );
        }
    }
}
