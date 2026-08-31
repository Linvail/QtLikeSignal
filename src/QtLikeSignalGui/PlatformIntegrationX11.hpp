// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::PlatformIntegrationX11 -- the X11 backend.

#ifndef QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONX11_HPP
#define QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONX11_HPP

#include "QtLikeSignalGui/PlatformIntegration.hpp"

#include <vector>

namespace QtLikeSignalGui
{
    //! The X11 backend: it creates windows or adopts ones somebody else made, and reads the
    //! connection itself.
    //!
    //! **Both ways in, and they differ in exactly one thing: who destroys the window.** A created
    //! window is this backend's, and is destroyed with its Window. An adopted one belongs to the
    //! external library that made it -- along with the GLX context still bound to it -- so releasing
    //! it only undoes the event selection and leaves the window standing. Everything else is the
    //! same: the same signals, the same drain, the same place in GuiApplication::windows().
    //!
    //! **The visual is X11's pixel format, and it is chosen once.** A window's visual is fixed at
    //! creation, and glXCreateWindow or eglCreateWindowSurface fails with BadMatch when it does not
    //! match the config the context was made for. So a GL program takes the connection from
    //! nativeDisplay(), lets its library choose a config on it, and passes that config's visual to
    //! createWindow() through WindowSettings::mVisualId. Nothing here ever creates a context or
    //! chooses a config itself, for the reason the Win32 backend never calls SetPixelFormat: doing
    //! it would spend the one chance the library needs.
    //!
    //! **The connection may be ours or theirs.** Creating a window opens one if there is not already
    //! one; adopting uses the one handed in. Whichever it is, only that one is registered with the
    //! loop, and only a connection this backend opened is closed by it.
    //!
    //! **Joining the loop is the whole difference from Win32.** Windows gives every thread a message
    //! queue whether it asks or not, so the Win32 backend needs no loop code at all. X11 has no such
    //! thing: a display connection is a socket, and it is the program that must tell the loop about
    //! it. EventDispatcherLinux::registerEventSource() takes XConnectionNumber( display ) and a
    //! callback, and from then on the descriptor is one more entry in the poll(2) set the loop
    //! already blocks in:
    //!
    //! @code
    //!   poll( [ eventfd, X11 socket ], timeout-until-next-timer )
    //!          ^^^^^^^^  ^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^
    //!          our own   the server's  the timers
    //!           events      events
    //! @endcode
    //!
    //! One poll(), three kinds of work, no helper thread and no polling interval. When nothing is
    //! scheduled the timeout is -1 and the process uses no CPU, which is the mission's "100% cpu-spin
    //! is not allowed".
    //!
    //! **Draining is not one event per readiness.** poll() reports that the *socket* has bytes, but
    //! Xlib parses those bytes into events held in a queue inside this process. Reading one event
    //! per readiness leaves the rest in that queue with an empty socket behind them, and the loop
    //! then blocks with work already in hand -- the window appears frozen until something unrelated
    //! wakes it. pumpDisplay() carries the loop that avoids that, and the residual check for the
    //! events a round trip inside a slot can pull off the socket invisibly.
    //!
    //! **One connection.** Every adopted window must live on the same Display*, because exactly one
    //! descriptor is registered with the loop. A second connection would need a second registration
    //! and a second drain; nothing needs that, and adopting a window from a different display is
    //! refused with a diagnostic rather than silently going deaf.
    //!
    //! **Keyboard works, text included.** XLookupString answers both halves of the question from
    //! one call -- the keysym that identifies the key and the bytes the layout produces for it --
    //! so this backend fills in KeyEvent::mText as well as mKey, which the two Linux backends
    //! cannot. Text is Latin-1 only; anything beyond that needs an input method.
    //!
    //! **No touch and no menu bar.** Touch needs XInput2, and a menu bar on X11 is drawn by the
    //! client rather than by the window system. Both are documented reaches rather than stubs; see
    //! Window's signals.
    //!
    //! Used only from the thread running the loop, like the connection it reads.
    class PlatformIntegrationX11 : public PlatformIntegration
    {
    public:
        static bool isAvailable();

        PlatformIntegrationX11();

        virtual ~PlatformIntegrationX11() override;

        virtual PlatformType type() const override;

        virtual bool canCreateWindows() const override;

        virtual bool canAdoptWindows() const override;

        virtual Window* createWindow
            (
            const WindowSettings& aSettings
            ) override;

        virtual Window* adoptWindow
            (
            const NativeWindow& aNative
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
        //! One window this backend is listening to, and the state it keeps for it.
        struct Tracked
        {
            Window* mWindow;             //!< The Window fronting it. Not owned.
            unsigned long mWindowId;     //!< Its X11 resource id, for routing incoming events.

            //! True while a synthetic Expose has been sent and not yet arrived.
            //!
            //! requestUpdate() may be called many times between two passes -- once per MotionNotify
            //! in a drag, plus once from a timer -- and each one would otherwise put another Expose
            //! on the wire. This collapses them into one repaint per pass, which is what
            //! QWindow::requestUpdate() does.
            bool mUpdatePending;

            //! True if this backend created the window, and must therefore destroy it.
            //!
            //! The one thing that differs between a created window and an adopted one, and the
            //! reason releaseNativeWindow() cannot simply do the same thing to both.
            bool mCreated;

            //! The colormap created alongside the window, or 0 when there is none to free.
            //!
            //! A non-default visual needs a colormap of its own -- XCreateWindow returns BadMatch
            //! without one -- and it is not freed with the window, so it is remembered here.
            unsigned long mColormap;
        };

        bool ensureOwnDisplay();

        bool prepareConnection();

        void unregisterConnection();

        void pumpDisplay
            (
            short aEvents
            );

        void dispatchNativeEvent
            (
            const void* aEvent
            );

        Tracked* windowFor
            (
            unsigned long aWindowId
            );

        //! The connection, or null before there is one. See mOwnsDisplay for who closes it.
        void* mDisplay { nullptr };

        //! True if this backend opened the connection and must therefore close it.
        //!
        //! False for one handed in by adoptWindow(), which the external library opened and will
        //! close. Closing somebody else's Display would take down every window on it, including the
        //! ones this library never touched.
        bool mOwnsDisplay { false };

        //! The connection's descriptor while it is registered with the dispatcher, else -1.
        //!
        //! Kept so the destructor can unregister exactly what it registered, without needing a live
        //! display to ask for the number again.
        int mConnectionFd { -1 };

        //! The WM_DELETE_WINDOW atom, so the close button arrives as a ClientMessage rather than as
        //! the server killing the connection underneath us.
        unsigned long mDeleteWindowAtom { 0 };

        //! The WM_PROTOCOLS atom, which is what marks such a ClientMessage as one of ours.
        unsigned long mProtocolsAtom { 0 };

        //! Every window this backend is listening to. The windows themselves belong to the
        //! GuiApplication.
        std::vector<Tracked> mWindows;
    };
}

#endif // QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONX11_HPP
