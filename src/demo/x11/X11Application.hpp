// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalDemo::X11Application -- a CoreApplication that owns a real X11 window and turns the
//! events arriving on its display connection into QtLikeSignal signals.

#ifndef QT_LIKE_SIGNAL_DEMO_X11APPLICATION_HPP
#define QT_LIKE_SIGNAL_DEMO_X11APPLICATION_HPP

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Signal.hpp"

#include "MouseEvent.hpp"

#include <atomic>

namespace QtLikeSignalDemo
{
    //! An application with a window: CoreApplication plus an X11 display connection whose events
    //! become signals.
    //!
    //! The X11 counterpart of the Win32 demo's GuiApplication, and deliberately the same shape --
    //! Qt's QGuiApplication over QCoreApplication, with the platform integration in the derived
    //! class. What differs is *how* the platform gets into the loop, and that difference is the
    //! whole point of this demo.
    //!
    //! **On Windows the dispatcher pumps; on Linux the application registers.** Win32 has one
    //! per-thread message queue that the OS owns, so EventDispatcherWin32 calls PeekMessage on
    //! every pass and no application has to ask for it. X11 has no such thing: a display connection
    //! is a socket, and it is the *program* that must tell the loop about it. That is what
    //! EventDispatcherLinux::registerEventSource() is for. createWindow() hands it
    //! XConnectionNumber( display ) and a callback, and from then on the descriptor is one more
    //! entry in the poll(2) set the loop already blocks in:
    //!
    //! @code
    //!   poll( [ eventfd, X11 socket ], timeout-until-next-timer )
    //!          ^^^^^^^^  ^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^
    //!          our own   the server's  the timers
    //!           events      events
    //! @endcode
    //!
    //! One poll(), three kinds of work, no helper thread and no polling interval. A queued signal
    //! emitted on a worker thread writes the eventfd; the X server writing to the socket makes the
    //! other descriptor readable; whichever happens first ends the same wait. When nothing at all
    //! is scheduled the timeout is -1 and the process uses no CPU, which is mission stage 5's
    //! "100% cpu-spin is not allowed".
    //!
    //! **Draining is the application's job, and it is not one event per readiness.** poll() reports
    //! that the *socket* has bytes, but Xlib parses those bytes into events held in a queue in this
    //! process. Reading one event per readiness leaves the rest of them in that queue with an empty
    //! socket behind them, and the loop then blocks with work already in hand. pumpDisplay()
    //! carries the reasoning and the loop that avoids it.
    //!
    //! Must be constructed on the thread that will call exec(), like CoreApplication itself. The
    //! display connection is used from that thread only, which is why no XInitThreads() call is
    //! needed here.
    class X11Application : public QtLikeSignal::CoreApplication
    {
    public:
        X11Application();

        X11Application
            (
            int aArgc,
            char** aArgv
            );

        virtual ~X11Application() override;

        bool createWindow
            (
            const char* aTitle,
            int aWidth,
            int aHeight
            );

        void update();

        //! Gets the current window width in pixels, tracked from ConfigureNotify.
        int width() const
        {
            return mWidth;
        }

        //! Gets the current window height in pixels, tracked from ConfigureNotify.
        int height() const
        {
            return mHeight;
        }

        void* nativeDisplay() const;

        unsigned long nativeWindow() const;

        void* nativeGraphicsContext() const;

        QtLikeSignal::SignalView<MouseEvent>& getMousePressed() const;

        QtLikeSignal::SignalView<MouseEvent>& getMouseReleased() const;

        QtLikeSignal::SignalView<MouseEvent>& getMouseMoved() const;

        QtLikeSignal::SignalView<unsigned long>& getKeyPressed() const;

        QtLikeSignal::SignalView<unsigned long>& getPaintRequested() const;

    private:
        void pumpDisplay
            (
            short aEvents
            );

        void dispatchNativeEvent
            (
            void* aEvent
            );

        void paintNow();

        void resizeBackBuffer
            (
            int aWidth,
            int aHeight
            );

        //! The display connection, or nullptr before createWindow() succeeds.
        //!
        //! Typed void* rather than Display* so this header does not pull <X11/Xlib.h>, and its
        //! macros, into every translation unit that includes it. Xlib is worse than <windows.h>
        //! here: it defines Bool, Status, Screen and a plain `None`, all unqualified. The Win32
        //! demo keeps its HWND behind a void* for the same reason.
        void* mDisplay { nullptr };

        //! The window, or 0 before createWindow() succeeds. An X resource id, not a pointer.
        unsigned long mWindow { 0 };

        //! Off-screen pixmap the paint signal draws into, blitted to the window afterwards.
        //!
        //! Without it the window flickers under a stream of MotionNotify: every repaint would clear
        //! the window and then draw over it, and the cleared state is visible.
        unsigned long mBackBuffer { 0 };

        //! Graphics context used for the blit and handed to the paint slot.
        void* mGraphicsContext { nullptr };

        //! The WM_DELETE_WINDOW atom, so the close button arrives as a ClientMessage rather than
        //! as the server killing the connection underneath us.
        unsigned long mDeleteWindowAtom { 0 };

        //! The connection's file descriptor while it is registered with the dispatcher, else -1.
        //!
        //! Kept so the destructor can unregister exactly what it registered, without needing a live
        //! display to ask for the number again.
        int mConnectionFd { -1 };

        int mWidth { 0 };    //!< Current window width in pixels.
        int mHeight { 0 };   //!< Current window height in pixels.

        //! True while a repaint has been posted to our own event queue and not yet run.
        //!
        //! update() may be called many times between two passes -- once per MotionNotify in a drag,
        //! plus once from the timer -- and each one would otherwise post a repaint of its own. This
        //! collapses them into a single paint per pass, which is what QWidget::update() does.
        //!
        //! Atomic only because it is an exchange; the demo touches it from the loop's thread alone.
        std::atomic<bool> mRepaintPosted { false };

        //! Emitted when a mouse button goes down, with the button and position.
        QtLikeSignal::Signal<MouseEvent> mMousePressed;

        //! Emitted when a mouse button comes up, with the button and position.
        QtLikeSignal::Signal<MouseEvent> mMouseReleased;

        //! Emitted when the pointer moves over the window. Button is None.
        QtLikeSignal::Signal<MouseEvent> mMouseMoved;

        //! Emitted on KeyPress, carrying the KeySym the keycode mapped to.
        //!
        //! unsigned long rather than KeySym for the same reason mWindow is: KeySym is an XID, and
        //! naming it would mean including Xlib here.
        QtLikeSignal::Signal<unsigned long> mKeyPressed;

        //! Emitted when the window needs redrawing, carrying the Drawable to draw into.
        //!
        //! That drawable is the back buffer, not the window: the slot draws off-screen and this
        //! class copies the result over in one operation. The id is only valid for the duration of
        //! the emission, since a resize replaces the pixmap.
        QtLikeSignal::Signal<unsigned long> mPaintRequested;
    };
}

#endif // QT_LIKE_SIGNAL_DEMO_X11APPLICATION_HPP
