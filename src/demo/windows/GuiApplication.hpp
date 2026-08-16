// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalDemo::GuiApplication -- a CoreApplication that owns a real Win32 window and turns
//! its messages into QtLikeSignal signals.

#ifndef QT_LIKE_SIGNAL_DEMO_GUIAPPLICATION_HPP
#define QT_LIKE_SIGNAL_DEMO_GUIAPPLICATION_HPP

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Signal.hpp"

#include "MouseEvent.hpp"

// For TCHAR only. The window handle below is still a void* so that <windows.h> stays out of this
// header; <tchar.h> is the small CRT header that defines the character type the generic-text Win32
// entry points take, and nothing else this header needs.
#include <tchar.h>

namespace QtLikeSignalDemo
{
    //! An application with a window: CoreApplication plus a Win32 window whose messages become
    //! signals.
    //!
    //! This is the demo's answer to Qt's QGuiApplication, and it is deliberately the same shape.
    //! QGuiApplication derives from QCoreApplication and adds the window system: it owns the
    //! platform integration, and native events arrive through it and are delivered as Qt events.
    //! Here the platform integration is one window procedure, and the delivery is three signals.
    //!
    //! **What this demonstrates, and why it needs no message loop of its own.** There is no
    //! `while( GetMessage( ... ) )` anywhere in this program. exec() runs the inherited
    //! QtLikeSignal loop, and it is EventDispatcherWin32::processPlatformEvents() -- called from
    //! EventDispatcherDefault::processEvents(), on every pass -- that does the PeekMessage,
    //! TranslateMessage and DispatchMessage. So every mouse event visible in the window has
    //! travelled through this library's dispatcher. If the dispatcher stopped servicing OS
    //! messages, the window would freeze and nothing else would change.
    //!
    //! Two things follow from that, and both are visible in the running program:
    //!
    //! - **Timers and OS messages share one loop.** The uptime counter is a QtLikeSignal::Timer,
    //!   not a WM_TIMER, and it keeps ticking while the mouse is moving. One primitive multiplexes
    //!   both, which is mission stage 5's requirement and the reason waitForEvents() blocks in
    //!   MsgWaitForMultipleObjectsEx rather than in a condition variable.
    //! - **A foreign message loop does not break it.** Press M and the window procedure opens a
    //!   MessageBox, which runs its own message loop and drains this thread's queue -- including
    //!   any wakeup this dispatcher had posted. The application still works afterwards, because
    //!   the dispatcher's window procedure re-arms the wake flag when that message is dispatched
    //!   by whoever dispatches it. That is R34 in history/OPEN-RISKS-20260816.md, and before it
    //!   was fixed this demo would have gone permanently deaf at that point.
    //!
    //! Must be constructed on the thread that will call exec(), like CoreApplication itself, and
    //! for the extra reason that a window belongs to the thread that creates it.
    class GuiApplication : public QtLikeSignal::CoreApplication
    {
    public:
        GuiApplication();

        virtual ~GuiApplication() override;

        bool createWindow
            (
            const TCHAR* aTitle,
            int aClientWidth,
            int aClientHeight
            );

        void update();

        void* nativeWindowHandle() const;

        QtLikeSignal::SignalView<MouseEvent>& getMousePressed() const;

        QtLikeSignal::SignalView<MouseEvent>& getMouseReleased() const;

        QtLikeSignal::SignalView<MouseEvent>& getMouseMoved() const;

        QtLikeSignal::SignalView<void*>& getPaintRequested() const;

    private:
        //! The window, or nullptr before createWindow() succeeds.
        //!
        //! Typed void* rather than HWND so this header does not pull <windows.h>, and its macros,
        //! into every translation unit that includes it. EventDispatcherWin32 keeps its own message
        //! window the same way, for the same reason.
        void* mWindow { nullptr };

        //! Emitted when a mouse button goes down, with the button and position.
        QtLikeSignal::Signal<MouseEvent> mMousePressed;

        //! Emitted when a mouse button comes up, with the button and position.
        QtLikeSignal::Signal<MouseEvent> mMouseReleased;

        //! Emitted when the mouse moves over the client area. Button is None.
        QtLikeSignal::Signal<MouseEvent> mMouseMoved;

        //! Emitted from WM_PAINT, carrying the HDC to draw on as a void*.
        //!
        //! void* for the same reason mWindow is: a slot casts it back to HDC, which keeps Windows
        //! out of this header. The device context is only valid for the duration of the emission,
        //! because it belongs to the BeginPaint/EndPaint pair around it.
        QtLikeSignal::Signal<void*> mPaintRequested;
    };
}

#endif // QT_LIKE_SIGNAL_DEMO_GUIAPPLICATION_HPP
