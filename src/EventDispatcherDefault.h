#ifndef QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_H
#define QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_H

#include "AbstractEventDispatcher.h"
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

        virtual void postEvent
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

        //! Wakes a thread blocked in waitForEvents(). Callable from any thread, with or without
        //! mMutex held, so it must not block. Every mutation of the queue, the timer list or the
        //! interrupt flag ends in a call to this.
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
        // Set (under mMutex) whenever a timer is registered or unregistered, so a processEvents()
        // call currently sleeping in wait_for() re-evaluates its wait deadline instead of sleeping
        // for the stale duration computed before the change.
        bool mTimersChanged { false };
        // Set (under mMutex) by wakeUp() and consumed by processEvents() once it returns from
        // waiting. Needed because the wait is predicate-based: without a state change to observe, a
        // bare notify_all() from wakeUp() cannot end the wait.
        bool mWakeUpRequested { false };

        //! The timer batch currently being dispatched, or nullptr outside a dispatch pass.
        //!
        //! Timers that expire together are collected into one batch and then dispatched with mMutex
        //! released, so a killTimer() from inside one handler lands after the others' events already
        //! exist. This lets unregisterTimer() reach into that batch and cancel them. Guarded by
        //! mMutex, and every read of an entry takes mMutex too, so cancelling races nothing.
        std::vector<EventPair>* mDispatchingTimerBatch { nullptr };

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
        //! wakeWaiter() is reached both with mMutex held (registerTimer, unregisterTimer) and
        //! without it (postEvent, wakeUp, interrupt). Reading the callback under mMutex would
        //! therefore relock a non-recursive mutex this thread already owns on half the paths. A
        //! separate lock makes the read safe from either.
        mutable std::mutex mWakeCallbackMutex;
    };
}

#endif // QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_H
