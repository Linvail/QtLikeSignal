// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalDemo::WaylandApplication -- a CoreApplication that owns a real Wayland surface and
//! turns the events arriving on its compositor connection into QtLikeSignal signals.

#ifndef QT_LIKE_SIGNAL_DEMO_WAYLANDAPPLICATION_HPP
#define QT_LIKE_SIGNAL_DEMO_WAYLANDAPPLICATION_HPP

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Signal.hpp"

#include "MouseEvent.hpp"

#include <memory>

namespace QtLikeSignalDemo
{
    //! An application with a window: CoreApplication plus a Wayland connection whose events become
    //! signals.
    //!
    //! The third of the demos, and the one that shows the Linux platform seam is not a one-off for
    //! X11. Wayland joins the loop exactly the way X11 does -- wl_display_get_fd() handed to
    //! EventDispatcherLinux::registerEventSource(), after which one poll() waits on the compositor
    //! socket, the dispatcher's eventfd and the timer deadline together. Nothing in this program
    //! calls wl_display_dispatch() in a loop of its own.
    //!
    //! **What Wayland asks for that X11 does not.** Reading the socket is a three-step sequence
    //! rather than one call, because libwayland lets several threads share one connection:
    //!
    //! @code
    //!   while( wl_display_prepare_read( d ) != 0 )   // fails while the queue is non-empty
    //!       wl_display_dispatch_pending( d );        // ... so empty it first
    //!   wl_display_read_events( d );                 // now read the socket
    //!   wl_display_dispatch_pending( d );            // and run the handlers
    //! @endcode
    //!
    //! Announcing the read before doing it is what stops two threads both blocking on the same
    //! socket while one of them holds events the other is waiting for. This demo is single
    //! threaded on the connection, but the sequence is the API's contract, not an optional
    //! optimisation, and getting it wrong is the classic Wayland client hang. pumpDisplay() carries
    //! the detail.
    //!
    //! **Writing can block too, and that is what the poll mask is for.** wl_display_flush() fails
    //! with EAGAIN when the compositor is not draining its end fast enough. The answer is to wait
    //! for the socket to become *writable*, which means the descriptor's poll mask has to change
    //! from POLLIN to POLLIN|POLLOUT and back. EventDispatcherLinux supports exactly that:
    //! re-registering a descriptor replaces its mask and wakes a loop already blocked on the old
    //! one. flushOutgoing() is that, and it is why this demo registers a mask it later changes.
    //!
    //! **There is deliberately no drawing.** Everything received is printed to the console. A
    //! surface will not be mapped until a buffer is attached, so there is one shared-memory buffer
    //! filled with a single flat colour once at startup, and nothing ever draws into it again.
    //!
    //! Must be constructed on the thread that will call exec(), like CoreApplication itself. The
    //! connection is used from that thread only.
    class WaylandApplication : public QtLikeSignal::CoreApplication
    {
    public:
        WaylandApplication();

        WaylandApplication
            (
            int aArgc,
            char** aArgv
            );

        virtual ~WaylandApplication() override;

        bool createWindow
            (
            const char* aTitle,
            int aWidth,
            int aHeight
            );

        //! Gets the surface width in pixels, as the buffer was created.
        int width() const
        {
            return mWidth;
        }

        //! Gets the surface height in pixels, as the buffer was created.
        int height() const
        {
            return mHeight;
        }

        bool hasPointer() const;

        bool hasKeyboard() const;

        QtLikeSignal::SignalView<MouseEvent>& getMousePressed() const;

        QtLikeSignal::SignalView<MouseEvent>& getMouseReleased() const;

        QtLikeSignal::SignalView<MouseEvent>& getMouseMoved() const;

        QtLikeSignal::SignalView<>& getPointerEntered() const;

        QtLikeSignal::SignalView<>& getPointerLeft() const;

        QtLikeSignal::SignalView<unsigned int>& getKeyPressed() const;

        QtLikeSignal::SignalView<int, int>& getSurfaceConfigured() const;

    private:
        void pumpDisplay
            (
            short aEvents
            );

        void flushOutgoing();

        void setPollMask
            (
            short aEvents
            );

        bool createBuffer();

        void reportFatal
            (
            const char* aWhere
            );

        //! Grants the Wayland listener callbacks access to the signals and handles they serve.
        //!
        //! libwayland dispatches through C function pointers, so the callbacks cannot be members of
        //! this class without putting wl_ types in this header. They are static members of a struct
        //! defined in the .cpp instead, and this friendship is what lets them emit. One declaration
        //! rather than a dozen forwarding methods.
        friend struct WaylandListeners;

        //! Every Wayland handle this demo owns. Defined in the .cpp.
        //!
        //! A pimpl rather than a row of void* members: Wayland's client API is a dozen distinct
        //! object types, and spelling each one void* in this header would trade the include for a
        //! pile of casts at every use. Nothing outside the .cpp needs to know what is in here.
        struct Internals;
        std::unique_ptr<Internals> mInternals;

        //! The connection's file descriptor while it is registered with the dispatcher, else -1.
        int mConnectionFd { -1 };

        //! The poll(2) mask the connection is currently registered with.
        //!
        //! Tracked so flushOutgoing() only re-registers when the mask actually has to change, and
        //! not on every pass.
        short mPollMask { 0 };

        int mWidth { 0 };    //!< Surface width in pixels.
        int mHeight { 0 };   //!< Surface height in pixels.

        //! Emitted when a pointer button goes down, with the button and position.
        QtLikeSignal::Signal<MouseEvent> mMousePressed;

        //! Emitted when a pointer button comes up, with the button and position.
        QtLikeSignal::Signal<MouseEvent> mMouseReleased;

        //! Emitted when the pointer moves over the surface. Button is None.
        QtLikeSignal::Signal<MouseEvent> mMouseMoved;

        //! Emitted when the pointer enters the surface.
        //!
        //! Wayland has no equivalent of asking where the pointer is: a client only knows once the
        //! compositor tells it, and only while the pointer is over its own surface. Enter and leave
        //! are how that starts and stops, which is worth seeing in the output.
        QtLikeSignal::Signal<> mPointerEntered;

        //! Emitted when the pointer leaves the surface.
        QtLikeSignal::Signal<> mPointerLeft;

        //! Emitted on a key press, carrying the raw evdev keycode.
        //!
        //! Not a character and not a keysym. Translating one into the other needs the XKB keymap
        //! the compositor sends, and this demo has nothing to type into, so it reports what the
        //! protocol gave it.
        QtLikeSignal::Signal<unsigned int> mKeyPressed;

        //! Emitted when the compositor configures the surface, with the size it suggested.
        //!
        //! Zero for either dimension means "you choose", which is what arrives for the first
        //! configure of an ordinary window.
        QtLikeSignal::Signal<int, int> mSurfaceConfigured;
    };
}

#endif // QT_LIKE_SIGNAL_DEMO_WAYLANDAPPLICATION_HPP
