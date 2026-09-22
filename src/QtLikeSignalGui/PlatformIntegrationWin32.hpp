// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::PlatformIntegrationWin32 -- the Win32 backend.

#ifndef QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONWIN32_HPP
#define QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONWIN32_HPP

#include "QtLikeSignalGui/PlatformIntegration.hpp"

namespace QtLikeSignalGui
{
    //! The Windows backend: it registers a window class, creates windows, and owns their procedure.
    //!
    //! A full backend rather than an adopting one. It creates the HWND itself and translates every
    //! mouse message into a WindowSystemInterface call from inside its own window procedure, which
    //! makes it the only backend that needs nothing from an external library to work.
    //!
    //! **It does not touch OpenGL, and the window is built so that someone else can.** The class is
    //! registered CS_OWNDC and the window styled WS_CLIPCHILDREN | WS_CLIPSIBLINGS, which is what
    //! SetPixelFormat requires of a window it is called on; the background brush is null so nothing
    //! flood-fills the client area before a frame is drawn; and SetPixelFormat is never called
    //! here, because it succeeds exactly once per window and calling it would take that one chance
    //! away from the library that actually creates the context. Hand Window::nativeHandle() to that
    //! library and the whole WGL sequence is available to it.
    //!
    //! **There is no message pump here.** EventDispatcherWin32::processPlatformEvents() is what
    //! calls PeekMessage, TranslateMessage and DispatchMessage, on every pass of the loop
    //! CoreApplication::exec() runs, so the procedure below is reached through this library's own
    //! dispatcher. Nothing in this backend has to be started or stopped to join the loop, which is
    //! why it has no initialize() to call -- Win32 gives every thread a message queue whether it
    //! asks for one or not, unlike the X11 and Wayland connections that must be registered.
    class PlatformIntegrationWin32 : public PlatformIntegration
    {
    public:
        PlatformIntegrationWin32();

        ~PlatformIntegrationWin32() override;

        PlatformType type() const override;

        bool canCreateWindows() const override;

        bool canAdoptWindows() const override;

        Window* createWindow
            (
            const WindowSettings& aSettings
            ) override;

        void releaseNativeWindow
            (
            Window* aWindow
            ) override;

        void setWindowTitle
            (
            Window* aWindow,
            const std::string& aTitle
            ) override;

        void setWindowVisible
            (
            Window* aWindow,
            bool aVisible
            ) override;

        void setClientSize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            ) override;

        void setMenu
            (
            Window* aWindow,
            void* aMenuHandle
            ) override;

        void requestUpdate
            (
            Window* aWindow
            ) override;

    private:
        bool ensureWindowClass();

        //! True once RegisterClass() has succeeded, so it is attempted once rather than per window.
        bool mClassRegistered { false };
    };
}

#endif // QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONWIN32_HPP
