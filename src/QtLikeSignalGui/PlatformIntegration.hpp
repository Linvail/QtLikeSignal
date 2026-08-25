// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::PlatformIntegration -- the seam every window system attaches through.

#ifndef QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATION_HPP
#define QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATION_HPP

#include "QtLikeSignalGui/PlatformType.hpp"
#include "QtLikeSignalGui/Window.hpp"

#include <memory>
#include <string>
#include <vector>

namespace QtLikeSignalGui
{
    //! One window system, and everything QtLikeSignalGui needs from it.
    //!
    //! The counterpart of Qt's QPlatformIntegration, minus everything this library has no use for:
    //! no font database, no clipboard, no drag and drop, no backing store. Windows are created (or
    //! adopted), destroyed, retitled, shown and invalidated; native input is reported through
    //! WindowSystemInterface. That is the whole contract.
    //!
    //! **Backends are linked in, not loaded.** Qt resolves a plugin name to a shared library at
    //! startup; this factory resolves it to a statically linked class. Loading buys nothing here --
    //! there is no third-party backend to discover -- and it would trade a link error for a runtime
    //! one, which the mission's preference for stability over feature richness argues against.
    //!
    //! **Backends come in two shapes, and both are legitimate:**
    //!
    //! - A backend that *creates* windows and translates native events itself. Win32 is one: it
    //!   registers a window class, owns the window procedure, and calls WindowSystemInterface from
    //!   inside it. canCreateWindows() is true.
    //! - A backend that *adopts* a window some other library created, and may not translate input at
    //!   all. This is where X11 and Wayland will attach: on X11 an external library creates the
    //!   window and hands over the Display*, and on Wayland it also owns the seat and reports input
    //!   through its own signals, leaving this library only the loop integration.
    //!   canCreateWindows() is false, and an adoption entry point arrives with those backends.
    //!
    //! Nothing here is thread-safe. A PlatformIntegration is used only from the thread that runs the
    //! event loop, which is the thread that created the GuiApplication.
    class PlatformIntegration
    {
    public:
        static PlatformType choosePlatform
            (
            const std::vector<std::string>& aArgs
            );

        static std::unique_ptr<PlatformIntegration> create
            (
            PlatformType aPlatform
            );

        virtual ~PlatformIntegration() = default;

        //! Gets which window system this backend is.
        virtual PlatformType type() const = 0;

        //! Returns true if this backend can create windows of its own.
        //!
        //! False for a backend that can only adopt one an external library created. An honest query
        //! rather than a createWindow() that returns null for reasons the caller cannot distinguish;
        //! Qt asks the same question as hasCapability( ForeignWindows ).
        virtual bool canCreateWindows() const = 0;

        //! Returns true if this backend can take over a window created outside it.
        //!
        //! The mirror of canCreateWindows(), and the two are not exclusive in principle even though
        //! no backend here does both today.
        virtual bool canAdoptWindows() const = 0;

        virtual Window* createWindow
            (
            const WindowSettings& aSettings
            );

        virtual Window* adoptWindow
            (
            const NativeWindow& aNative
            );

        virtual void* nativeDisplay();

        //! Lets go of the native window behind @p aWindow, leaving the Window object itself alone.
        //!
        //! **Destroy it only if this backend created it.** A backend that adopted a window an
        //! external library made must unhook its event selection and leave the window standing:
        //! destroying it would take it away from the code that owns it, and on X11 that code is also
        //! still holding a GLX context bound to it.
        //!
        //! Called from ~Window(). Must clear the window's native fields, and must tolerate being
        //! called for a window whose native side has already gone away.
        virtual void releaseNativeWindow
            (
            Window* aWindow
            ) = 0;

        //! Sets the window caption. @p aTitle is UTF-8.
        virtual void setWindowTitle
            (
            Window* aWindow,
            const std::string& aTitle
            ) = 0;

        //! Shows or hides the window.
        virtual void setWindowVisible
            (
            Window* aWindow,
            bool aVisible
            ) = 0;

        //! Resizes the window so its client area is exactly @p aWidth by @p aHeight.
        //!
        //! The client area is what a renderer draws on, and every backend has to be able to produce
        //! a requested one -- so this is required rather than optional. Both arguments are positive;
        //! Window::setClientSize() rejects anything else before it gets here.
        virtual void setClientSize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            ) = 0;

        virtual void setMenu
            (
            Window* aWindow,
            void* aMenuHandle
            );

        //! Marks the window as needing a repaint, so an expose arrives through the loop.
        //!
        //! Must return immediately rather than painting: the redraw is a message like any other, and
        //! painting here would run a slot from outside a dispatch pass.
        virtual void requestUpdate
            (
            Window* aWindow
            ) = 0;

    protected:
        static Window* newWindow
            (
            PlatformIntegration* aIntegration,
            const NativeWindow& aNative,
            int aWidth,
            int aHeight
            );

        static void assignNative
            (
            Window* aWindow,
            const NativeWindow& aNative
            );

        static void assignSize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            );

        static void assignVisible
            (
            Window* aWindow,
            bool aVisible
            );
    };
}

#endif // QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATION_HPP
