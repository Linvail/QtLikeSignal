// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::PlatformIntegrationWayland -- the Wayland backend.

#ifndef QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONWAYLAND_HPP
#define QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONWAYLAND_HPP

#include "QtLikeSignalGui/PlatformIntegration.hpp"

#include <memory>

namespace QtLikeSignalGui
{
    //! The Wayland backend: it creates a surface, drives the compositor connection, and reports the
    //! seat's input.
    //!
    //! Ported from GGL's Wayland library, which does the same job behind a C API and boost signals.
    //! The protocol work is the same work -- registry binding, xdg-shell or ivi-shell, a wl_seat
    //! with pointer and touch listeners -- and the differences are all at the edges, where this has
    //! to fit a library that already owns an event loop and already has a way to report input.
    //!
    //! **Two shells, chosen the way GGL chooses.** ivi-shell when an ivi id is given and the
    //! compositor offers ivi_application, which is the automotive case: no window manager, no
    //! decoration, and a surface identified to the controller by number. xdg-shell otherwise, which
    //! is every desktop compositor, with libdecor drawing the title bar and close button since
    //! Wayland has no server-side decoration to fall back on.
    //!
    //! **The loop is this library's, not the connection's.** GGL's HandleEvents() blocks in a poll()
    //! of its own until something arrives, which is exactly what a program with one event loop
    //! cannot afford: the timers and the queued signals would stop while it waited. Here the
    //! compositor socket is registered with EventDispatcherLinux and the read is driven from the
    //! outside, so one poll() waits on the socket, the dispatcher's eventfd and the timer deadline
    //! together:
    //!
    //! @code
    //!   poll( [ eventfd, wayland socket ], timeout-until-next-timer )
    //!          ^^^^^^^^  ^^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^^^
    //!          our own    the compositor    the timers
    //!           events       events
    //! @endcode
    //!
    //! **Reading is a three-step sequence, not one call**, because libwayland lets several threads
    //! share a connection:
    //!
    //! @code
    //!   while( wl_display_prepare_read( d ) != 0 )   // fails while the queue is non-empty
    //!       wl_display_dispatch_pending( d );        // ... so empty it first
    //!   wl_display_read_events( d );                 // now read the socket
    //!   wl_display_dispatch_pending( d );            // and run the listeners
    //! @endcode
    //!
    //! Announcing the read before doing it is what stops two threads both blocking on the same
    //! socket while one holds events the other waits for. This backend is single threaded on the
    //! connection, but the sequence is the API's contract rather than an optimisation, and getting
    //! it wrong is the classic Wayland client hang.
    //!
    //! **Writing can block too.** wl_display_flush() fails with EAGAIN when the compositor is not
    //! draining its end, and going back to sleep on POLLIN alone would then wait for something that
    //! cannot arrive until we have written. The descriptor's poll mask switches to POLLIN|POLLOUT
    //! until a flush completes, which EventDispatcherLinux supports by re-registering.
    //!
    //! **No EGL here, and no buffer.** The external rendering library takes nativeDisplay() and
    //! nativeHandle() -- the wl_display and the wl_surface -- and builds its own wl_egl_window and
    //! EGLSurface from them, exactly as GGL's PA layer does. A Wayland surface is not mapped until
    //! a buffer is attached, and the buffer is that library's, so this backend never attaches one
    //! and a window becomes visible when the renderer presents its first frame rather than when
    //! show() is called.
    //!
    //! **Keyboard reports key identities but no text.** wl_keyboard sends evdev codes, which name
    //! a place on the keyboard, and the keymap that would turn one into a character is an XKB file
    //! this backend closes unread -- reading it means depending on xkbcommon. So KeyEvent::mKey is
    //! filled in and KeyEvent::mText is left empty.
    //!
    //! Touch, which is a documented gap on X11 and Win32, works fully here -- the wl_touch protocol
    //! is what Window's touch signals were shaped around in the first place.
    //!
    //! Used only from the thread running the loop, like the connection it reads.
    class PlatformIntegrationWayland : public PlatformIntegration
    {
    public:
        static bool isAvailable();

        PlatformIntegrationWayland();

        virtual ~PlatformIntegrationWayland() override;

        virtual PlatformType type() const override;

        virtual bool canCreateWindows() const override;

        virtual bool canAdoptWindows() const override;

        virtual Window* createWindow
            (
            const WindowSettings& aSettings
            ) override;

        virtual void* nativeDisplay() override;

        virtual void releaseNativeWindow
            (
            Window* aWindow
            ) override;

        virtual void setWindowTitle
            (
            Window* aWindow,
            const std::string& aTitle
            ) override;

        virtual void setWindowVisible
            (
            Window* aWindow,
            bool aVisible
            ) override;

        virtual void setClientSize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            ) override;

        virtual void requestUpdate
            (
            Window* aWindow
            ) override;

    private:
        //! Grants the Wayland listener callbacks access to everything they report against.
        //!
        //! libwayland dispatches through C function pointers, so the callbacks cannot be members of
        //! this class without putting wl_ types in this header. They are static members of a struct
        //! defined in the .cpp instead, and this friendship is what lets them reach the internals.
        //! One declaration rather than three dozen forwarding methods.
        friend struct WaylandListeners;

        bool connectDisplay();

        bool createShellObjects
            (
            const WindowSettings& aSettings
            );

        bool registerConnection();

        void unregisterConnection();

        void pumpDisplay
            (
            short aEvents
            );

        void flushOutgoing();

        void setPollMask
            (
            short aEvents
            );

        void waitForDecorReady();

        void applyCursor();

        MouseEvent buildMouseEvent
            (
            MouseButton aChanged
            ) const;

        void reportResize
            (
            int aWidth,
            int aHeight
            );

        void requestClose();

        //! Every Wayland handle this backend owns. Defined in the .cpp.
        //!
        //! A pimpl rather than a row of void* members: Wayland's client API is two dozen distinct
        //! object types, and spelling each one void* in this header would trade the include for a
        //! pile of casts at every use. Nothing outside the .cpp needs to know what is in here.
        struct Internals;
        std::unique_ptr<Internals> mInternals;

        //! The connection's descriptor while it is registered with the dispatcher, else -1.
        int mConnectionFd { -1 };

        //! The poll(2) mask the connection is currently registered with.
        //!
        //! Tracked so flushOutgoing() only re-registers when the mask has to change, rather than on
        //! every pass.
        short mPollMask { 0 };

        //! The one window, or null. Not owned; the GuiApplication owns it.
        //!
        //! One, like GGL: a program built on this draws a single surface, and a second would need
        //! the pointer and touch focus to be tracked per surface rather than for the connection.
        Window* mWindow { nullptr };

        //! True while a repaint has been posted to the loop and not yet run. See requestUpdate().
        bool mUpdatePending { false };

        //! Kept alive exactly as long as this backend is, so a posted repaint can ask whether it is.
        //!
        //! The same guard the DRM backend uses, and for the same reason: requestUpdate() puts a task
        //! on the loop that touches this object when it runs, and the task outlives the backend if
        //! the application is torn down in between.
        std::shared_ptr<int> mLifeToken { std::make_shared<int>( 0 ) };
    };
}

#endif // QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONWAYLAND_HPP
