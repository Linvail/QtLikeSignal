// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Cross-platform event dispatcher: queue, timers, condition-variable wait.

#ifndef QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_HPP
#define QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_HPP

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace QtLikeSignal
{
    class Event;
    class Object;

    //! Default cross-platform concrete implementation of AbstractEventDispatcher.
    //!
    //! All public methods are thread-safe and can be invoked safely across threads.
    class EventDispatcherDefault : public AbstractEventDispatcher
    {
    public:
        EventDispatcherDefault();

        virtual ~EventDispatcherDefault() override;

        virtual bool processEvents() override;

        virtual void wakeUp() override;

        virtual void interrupt() override;

        virtual void close() override;

        virtual void processDeferredDeletes() override;

        virtual void setWakeCallback
            (
            std::function<void()> aCallback
            ) override;

    protected:
        // Mirrors the access level AbstractEventDispatcher gives these. The base class's access
        // already governs every call made through the AbstractEventDispatcher* that
        // Thread::eventDispatcher() hands out, so this is belt-and-suspenders -- it closes the
        // remaining gap for a caller holding a EventDispatcherDefault* directly.
        virtual void registerTimer
            (
            int aTimerId,
            int aInterval,
            Object* aObject
            ) override;

        virtual bool unregisterTimer
            (
            int aTimerId
            ) override;

        virtual bool postEvent
            (
            Object* aReceiver,
            Event* aEvent
            ) override;

        virtual void removeEventsForReceiver
            (
            Object* aReceiver
            ) override;

        virtual std::vector<TimerRegistration> takeTimersForReceiver
            (
            Object* aReceiver
            ) override;

        virtual std::vector<Event*> takeEventsForReceiver
            (
            Object* aReceiver
            ) override;

        // The three hooks below are the whole platform seam. Everything else -- the event queue,
        // the timer list, the mutex that guards them, and the dispatch loop -- stays here and is
        // shared, so a platform dispatcher only has to answer three questions: how do we block,
        // how does someone else un-block us, and how do we drain the OS's own event source.

        //! Blocks until there is work to do or @p aTimeoutMs elapses (-1 to block indefinitely).
        //!
        //! Called with @p aLock held. An implementation that blocks on something other than mCv
        //! **must** release @p aLock for the duration of the block and re-acquire it before
        //! returning -- neither poll() nor MsgWaitForMultipleObjectsEx() can hold a std::mutex, and
        //! holding it would deadlock every thread trying to post. Re-acquiring is what keeps the
        //! state processEvents() reads afterwards guarded.
        //!
        //! Missing a wakeup is not possible even though state can change while unlocked: whatever
        //! changed it also called wakeWaiter(), and the caller re-checks the queue under the lock.
        virtual void waitForEvents
            (
            std::unique_lock<std::mutex>& aLock,
            int aTimeoutMs
            );

        //! Wakes a thread blocked in waitForEvents(). Callable from any thread, so it must not
        //! block. Every mutation of the queue, the timer list or the interrupt flag ends in a call
        //! to this.
        //!
        //! **Called with mMutex released, on every path.** It may run mWakeCallback, which is user
        //! code, and user code that touches the dispatcher it was woken by is the obvious thing to
        //! write -- so calling it under a non-recursive mutex is a deadlock waiting for the first
        //! caller who does the obvious thing. postEvent() always released the lock first; the three
        //! timer paths did not, which made the hazard depend on which operation happened to wake
        //! the loop.
        virtual void wakeWaiter();

        //! Drains and dispatches OS/platform events. Called once per processEvents() pass with
        //! mMutex released, so an implementation may run arbitrary platform code and re-enter this
        //! dispatcher. The default does nothing: the cross-platform dispatcher has no OS source.
        virtual void processPlatformEvents();

        friend class Object;

    protected:
        //! One queued event together with the receiver it targets.
        struct EventPair
        {
            Object* mReceiver;
            Event*  mEvent;
        };

        //! One registered timer's schedule and target.
        struct TimerData
        {
            int mTimerId;
            int mIntervalMs;
            Object*                              mReceiver;
            std::chrono::steady_clock::time_point mNextFire;
        };

        std::deque<EventPair>   mEventQueue;  //!< Events waiting to be dispatched.
        std::vector<TimerData>  mTimers;      //!< Every timer currently registered.
        std::mutex mMutex;                    //!< Guards every other member of this class.
        std::condition_variable mCv;          //!< Wakes processEvents() out of its wait.
        std::atomic<bool>       mInterrupt { false };  //!< Set by interrupt() to stop the loop.

        //! False once close() has run, after which postEvent() refuses every event.
        //!
        //! Atomic rather than guarded by mMutex so postEvent() can reject without taking the lock,
        //! and so close() cannot be ordered after a post that already passed the check -- the
        //! rejection and the queue push both happen under mMutex below, which is what makes the
        //! pairing atomic with respect to a concurrent close().
        bool mAcceptingEvents { true };
        // Set (under mMutex) whenever a timer is registered or unregistered, so a processEvents()
        // call currently sleeping in wait_for() re-evaluates its wait deadline instead of sleeping
        // for the stale duration computed before the change.
        bool mTimersChanged { false };
        // Set (under mMutex) by wakeUp() and consumed by processEvents() once it returns from
        // waiting. Needed because the wait is predicate-based: without a state change to observe, a
        // bare notify_all() from wakeUp() cannot end the wait.
        bool mWakeUpRequested { false };

        //! The batches one dispatch pass is working through, published so that a cancellation
        //! arriving mid-pass can reach them.
        //!
        //! Every pass takes its work out of the shared containers first and then dispatches it with
        //! mMutex released, because a handler is arbitrary user code that will re-enter this
        //! dispatcher. That hand-off is what makes the pass lock-free, and it is also what puts the
        //! work out of reach of unregisterTimer() and removeEventsForReceiver(), which can only see
        //! mEventQueue and mTimers. An entry for a timer killed -- or an object destroyed -- by an
        //! earlier handler in the same pass would then still be delivered, and in the destroyed case
        //! that is a call through a freed pointer.
        //!
        //! A pass fills in only the batches it has: processEvents() publishes mEvents and mTimers,
        //! processDeferredDeletes() publishes mDeletes.
        struct DispatchFrame
        {
            std::deque<EventPair>*  mEvents { nullptr };   //!< Queued events taken from mEventQueue.
            std::vector<EventPair>* mTimers { nullptr };   //!< Timers that expired in this pass.
            std::vector<EventPair>* mDeletes { nullptr };  //!< Deferred deletes taken from mEventQueue.
            DispatchFrame*          mOuter { nullptr };    //!< The pass this one is nested inside.
        };

        //! Every dispatch pass currently running on this dispatcher, innermost first.
        //!
        //! A chain rather than one frame because passes nest: a handler may run a nested
        //! processEvents(), which is an ordinary thing for user code to do, and a cancellation
        //! raised inside the nested pass must still reach the outer pass's batches -- the outer pass
        //! will go on dispatching them after the inner one returns. Publishing a single frame would
        //! make the inner pass hide the outer one and then un-publish it on the way out, which is
        //! worse than not publishing at all, because it looks safe.
        //!
        //! Guarded by mMutex, as is every access to any entry of any frame, so ownership of each
        //! event passes to exactly one party: whoever clears the slot first has it, and the other
        //! sees nullptr and skips.
        DispatchFrame* mDispatchFrames { nullptr };

        //! Cancels every entry in every published batch that targets @p aReceiver, freeing its event
        //! and clearing the slot so the dispatch loop skips it. Callers must hold mMutex.
        void cancelPublishedEntriesFor
            (
            Object* aReceiver
            );

        //! Cancels every published TimerEvent carrying @p aTimerId, for the same reason and with the
        //! same ownership rule. Callers must hold mMutex.
        void cancelPublishedTimerEvents
            (
            int aTimerId
            );

        //! Removes @p aFrame from mDispatchFrames. Callers must hold mMutex.
        void unlinkDispatchFrame
            (
            DispatchFrame* aFrame
            );

        //! Removes a timer and every pending event for it. Callers must hold mMutex.
        //!
        //! Split out of unregisterTimer() so that the wake, which runs user code, happens after the
        //! lock is released.
        bool takeTimerLocked
            (
            int aTimerId
            );

        //! Nudges an adopted thread's own native loop when work is posted; empty if unused.
        std::function<void()> mWakeCallback;

        //! True while mWakeCallback holds a callback, so wakeWaiter() can skip reading it.
        //!
        //! wakeWaiter() runs on every postEvent(), and reading the callback costs a mutex
        //! acquire/release plus a std::function copy-construct and destroy -- on a path where the
        //! callback is almost always absent, because it exists only for a thread whose own native
        //! loop drains our queue (Thread::setWakeCallback(), used by adopted threads). Testing one
        //! atomic first makes the common case free and leaves the callback path exactly as it was.
        //!
        //! Racing a concurrent setWakeCallback() can miss the wake for the post in flight, but the
        //! mutex version could too: it could take the lock a moment before the setter did. A
        //! callback installed concurrently with a post has never been guaranteed to see that post.
        std::atomic<bool> mHasWakeCallback { false };

        //! Guards mWakeCallback -- deliberately NOT mMutex.
        //!
        //! wakeWaiter() runs with mMutex released so that the callback it may invoke is not user
        //! code under our lock, which means it cannot use mMutex to guard the callback either. A
        //! separate lock keeps the read safe without re-entering the one the callers just dropped.
        mutable std::mutex mWakeCallbackMutex;
    };
}

#endif // QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_HPP
