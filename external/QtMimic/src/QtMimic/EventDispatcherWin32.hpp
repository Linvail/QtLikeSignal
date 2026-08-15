//! @file
//!
//! Win32 event dispatcher: blocks on the thread message queue.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_EVENTDISPATCHERWIN32_HPP
#define QT_MIMIC_EVENTDISPATCHERWIN32_HPP

#include "QtMimic/EventDispatcherDefault.hpp"

namespace QtMimic
{
    //! Win32 event dispatcher: blocks on the thread's message queue, so OS messages and our own
    //! events arrive through one primitive.
    //!
    //! Windows delivers messages to the thread that created the window, and there is no way to wait
    //! on a message queue and a condition variable at the same time. Qt's answer, reproduced here,
    //! is to stop needing two primitives: the dispatcher creates a hidden **message-only window** on
    //! its own thread, wakeWaiter() posts a private message to it, and the loop blocks in
    //! MsgWaitForMultipleObjectsEx(), which returns for *any* message. A posted event and a mouse
    //! move then wake the loop by exactly the same mechanism.
    //!
    //! This is why no helper thread is needed on Windows either, despite messages being
    //! thread-affine -- the affinity is satisfied by creating the window on the loop's own thread.
    //!
    //! Timers are **not** handed to SetTimer(). The inherited timer list already computes the next
    //! deadline, and it is passed to MsgWaitForMultipleObjectsEx() as its timeout, so both platforms
    //! keep identical timer behaviour instead of inheriting Windows' own timer granularity and
    //! WM_TIMER coalescing rules.
    //!
    //! Must be constructed on the thread that will run the loop; the message-only window belongs to
    //! whichever thread creates it. Both construction sites satisfy this: Thread::threadBody() runs
    //! on the new worker thread, and CoreApplication constructs its dispatcher on the main thread.
    //!
    //! All public methods are thread-safe.
    class EventDispatcherWin32 : public EventDispatcherDefault
    {
    public:
        EventDispatcherWin32();

        virtual ~EventDispatcherWin32() override;

    protected:
        virtual void waitForEvents
            (
            std::unique_lock<std::mutex>& aLock,
            int aTimeoutMs
            ) override;

        virtual void wakeWaiter() override;

        virtual void processPlatformEvents() override;

    private:
        //! The hidden message-only window, or nullptr if it could not be created.
        //!
        //! Typed void* rather than HWND so this header does not pull <windows.h> -- and its macros --
        //! into every translation unit that includes it. Thread.h keeps its native handle the same
        //! way for the same reason.
        void* mMessageWindow { nullptr };

        //! True once wakeWaiter() has posted a wakeup that the loop has not yet consumed.
        //!
        //! Collapses a burst of posts into one message. Without it a busy sender could flood the
        //! thread's message queue, which is a fixed-size resource on Windows.
        std::atomic<bool> mWakePending { false };
    };
}

#endif // QT_MIMIC_EVENTDISPATCHERWIN32_HPP
