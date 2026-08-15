// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Linux event dispatcher: blocks in poll() on an eventfd.

#ifndef QT_LIKE_SIGNAL_EVENTDISPATCHERLINUX_HPP
#define QT_LIKE_SIGNAL_EVENTDISPATCHERLINUX_HPP

#include "QtLikeSignal/EventDispatcherDefault.hpp"

#include <atomic>
#include <functional>
#include <vector>

namespace QtLikeSignal
{
    //! Linux event dispatcher: blocks in poll() over the OS's file descriptors and our own wakeup.
    //!
    //! This is how a thread running our event loop also receives platform messages. Sources such as
    //! Wayland and X11 hand out a file descriptor that becomes readable when there is something to
    //! read; register it with registerEventSource() and the loop will poll() it alongside everything
    //! else, calling back when it is ready.
    //!
    //! Deliberately **no helper thread**. It is tempting to have a second thread block on the
    //! platform descriptor and wake the main one, but poll() already waits on any number of
    //! descriptors at once, so the platform FD and our internal wakeup FD are simply two entries in
    //! the same call. A helper thread would add a hand-off, a second lifetime to manage, and a race
    //! between "FD became ready" and "main thread went back to sleep", buying nothing. Qt reaches
    //! the same conclusion: QEventDispatcherUNIX polls its QThreadPipe together with the registered
    //! descriptors on one thread.
    //!
    //! The wakeup FD is an eventfd, matching Qt's QThreadPipe. It is what turns "somebody posted an
    //! event" into something poll() can observe, so the loop blocks with no timeout and no polling
    //! when nothing is scheduled -- there is no CPU spin at idle.
    //!
    //! All public methods are thread-safe.
    class EventDispatcherLinux : public EventDispatcherDefault
    {
    public:
        EventDispatcherLinux();

        virtual ~EventDispatcherLinux() override;

        //! Invoked when a registered descriptor is ready; receives the poll(2) revents bitmask.
        using EventSourceCallback = std::function<void( short aEvents )>;

        bool registerEventSource
            (
            int aFd,
            short aEvents,
            EventSourceCallback aCallback
            );

        bool unregisterEventSource
            (
            int aFd
            );

    protected:
        virtual void waitForEvents
            (
            std::unique_lock<std::mutex>& aLock,
            int aTimeoutMs
            ) override;

        virtual void wakeWaiter() override;

    private:
        //! One registered platform descriptor and what to call when it is ready.
        struct EventSource
        {
            int mFd;                          //!< The descriptor to poll.
            short mEvents;                    //!< poll(2) event mask to wait for.
            EventSourceCallback mCallback;    //!< Invoked with the revents when ready.

            //! Distinguishes this registration from any later one reusing the same descriptor.
            //!
            //! A descriptor number is not an identity: unregister it, close it, and the next open()
            //! may hand the same number straight back. Without this, a poll() round that began
            //! before all that could deliver its stale readiness to whatever now owns the number.
            unsigned long long mGeneration;
        };

        //! Looks up a still-registered source by descriptor and generation, and copies its callback.
        //!
        //! Returns an empty callback if that exact registration is gone, which is the check that
        //! makes unregisterEventSource() take effect immediately rather than one poll() round later.
        EventSourceCallback callbackIfStillRegistered
            (
            int aFd,
            unsigned long long aGeneration
            );

        void drainWakeFd();

        //! eventfd written by wakeWaiter() and polled by waitForEvents(); -1 if creation failed.
        int mWakeFd { -1 };

        //! True once wakeWaiter() has written to mWakeFd and before the loop has drained it.
        //!
        //! Collapses a burst of posts into a single write. The eventfd counter would coalesce them
        //! anyway, but this avoids the syscall entirely, which matters because wakeWaiter() runs on
        //! every single post. Qt guards its pipe write the same way.
        std::atomic<bool> mWakePending { false };

        //! Registered platform descriptors. Guarded by the inherited mMutex.
        std::vector<EventSource> mSources;

        //! Stamped into each registration and never reused. Guarded by the inherited mMutex.
        unsigned long long mNextGeneration { 1 };
    };
}

#endif // QT_LIKE_SIGNAL_EVENTDISPATCHERLINUX_HPP
