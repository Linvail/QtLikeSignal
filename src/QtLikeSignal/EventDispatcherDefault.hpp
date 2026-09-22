// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Cross-platform event dispatcher: queue, timers, condition-variable wait.

#ifndef QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_HPP
#define QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_HPP

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
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

        ~EventDispatcherDefault() override;

        bool processEvents
            (
            ProcessEventsFlag aFlag
            ) override;

        void wakeUp() override;

        void interrupt() override;

        void close() override;

        void processDeferredDeletes() override;

        void setEventQueueCapacity
            (
            std::size_t aCapacity
            ) override;

        std::size_t eventQueueCapacity() const override;

        std::size_t eventQueueDepth() const override;

        unsigned long long droppedEventCount() const override;

        void setHealthTrackingEnabled
            (
            bool aEnabled
            ) override;

        bool isHealthTrackingEnabled() const override;

        LoopHealth health() const override;

        void setWakeCallback
            (
            std::function<void()> aCallback
            ) override;

        int remainingTimeMs() const override;

        void setDeadlineCallback
            (
            std::function<void( int aMsFromNow )> aCallback
            ) override;

    protected:
        // Mirrors the access level AbstractEventDispatcher gives these. The base class's access
        // already governs every call made through the AbstractEventDispatcher* that
        // Thread::eventDispatcher() hands out, so this is belt-and-suspenders -- it closes the
        // remaining gap for a caller holding a EventDispatcherDefault* directly.
        void registerTimer
            (
            int aTimerId,
            int aInterval,
            Object* aObject
            ) override;

        bool unregisterTimer
            (
            int aTimerId
            ) override;

        int timerRemainingTimeMs
            (
            int aTimerId
            ) const override;

        bool postEvent
            (
            Object* aReceiver,
            Event* aEvent,
            OverflowPolicy aPolicy,
            int aPriority
            ) override;

        bool postEventUnconditionally
            (
            Object* aReceiver,
            Event* aEvent,
            int aPriority
            ) override;

        void removeEventsForReceiver
            (
            Object* aReceiver
            ) override;

        std::vector<TimerRegistration> takeTimersForReceiver
            (
            Object* aReceiver
            ) override;

        std::vector<Event*> takeEventsForReceiver
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
        //! caller who does the obvious thing.
        virtual void wakeWaiter();

        //! Drains and dispatches OS/platform events. Called once per processEvents() pass with
        //! mMutex released, so an implementation may run arbitrary platform code and re-enter this
        //! dispatcher. The default does nothing: the cross-platform dispatcher has no OS source.
        virtual void processPlatformEvents();

        friend class Object;

    protected:
        //! One queued event together with the receiver it targets and the priority it was posted
        //! at.
        //!
        //! The priority lives here rather than on the Event because it is a property of the post,
        //! not of the thing posted: the same event type is urgent from one producer and routine
        //! from another. Qt splits them the same way -- QEvent carries no priority, QPostEvent
        //! does.
        struct EventPair
        {
            Object* mReceiver;
            Event*  mEvent;
            int mPriority;
        };

        //! One registered timer's schedule and target.
        struct TimerData
        {
            int mTimerId;
            int mIntervalMs;
            Object*                              mReceiver;
            std::chrono::steady_clock::time_point mNextFire;

            //! Registration order: a lower value was registered first. Never reused.
            //!
            //! Present only so the firing order survives mTimers becoming a heap. A pass used to
            //! collect expiries by walking the list in registration order; a heap hands them back
            //! in deadline order, which for two timers due in the same pass is a different order.
            //! Carrying the registration index lets the collection loop put the batch back the way
            //! it was, so the reordering is confined to how the next deadline is found and is not
            //! observable to any handler.
            //!
            //! 64 bits, so it cannot wrap. A 32-bit counter would take about 50 days to overflow
            //! at one registration per millisecond, and the ordering it decides would then invert
            //! rather than fail -- the kind of defect that surfaces once, in the field, after the
            //! vehicle has been running long enough for nobody to connect the two.
            unsigned long long mSequence;
        };

        //! Events waiting to be dispatched.
        //!
        //! A vector, not a deque, although this is a queue. Nothing here pops the front: a pass
        //! takes the whole queue with one swap and walks the batch by index, and the only
        //! front-to-back drain is the destructor. So none of the three things a deque is for --
        //! O(1) removal at the front, references that survive a push, growth without moving what is
        //! already there -- is used, and its cost is not free anywhere and is severe on one
        //! implementation: MSVC picks its block size as a compile-time function of the element size
        //! alone, one element per block for anything over 8 bytes, so a 16-byte entry allocated a
        //! block on every push. That was one heap block per queued emit, measured at 1.001 blocks
        //! per push against 0.03 for a 32-entry libstdc++ block, and it was the whole of the
        //! remaining Windows allocation on the queued path.
        std::vector<EventPair>  mEventQueue;

        //! One drained dispatch batch's storage, kept so the next pass can hand it to mEventQueue.
        //!
        //! Always empty; only its capacity is worth anything. A pass swaps it into the queue as it
        //! takes the queue away, and swaps its own drained buffer back in at the end, so two
        //! buffers circulate between the producer and the consumer and a steady state allocates
        //! nothing. Nested passes take it in turn; the innermost finds it empty and allocates,
        //! which is the same cost the pass paid before this existed. Guarded by mMutex.
        std::vector<EventPair>  mSpareBatch;

        //! The same, for the batch a pass collects expired timers into.
        //!
        //! A second buffer rather than a second use of mSpareBatch, because a pass holds both at
        //! once. Without it a pass that fires T timers regrows a vector from nothing every time --
        //! about log2( T ) allocations per tick, on the thread a frame budget belongs to, which
        //! would have made the pooled TimerEvent below it a fraction of the fix rather than the
        //! whole of it.
        //!
        //! Always empty outside a pass, and taken and handed back under mMutex exactly as
        //! mSpareBatch is. A nested pass finds it already taken, allocates its own, and pays what
        //! every pass paid before this existed.
        std::vector<EventPair>  mSpareTimerBatch;

        //! Every timer currently registered, as a min-heap on mNextFire: the front is the next one
        //! due.
        //!
        //! A heap, not a list, because a pass used to walk it twice whether or not anything was
        //! due -- once to collect expiries and once to find the minimum deadline -- and the pass
        //! count is not bounded by timer activity: a thread taking cross-thread posts runs a pass
        //! per wake, so an O(T) scan coupled to traffic that has nothing to do with timers. The
        //! nothing-due case is now one comparison against the front, which is the case that sits on
        //! a frame budget. Qt keeps its Unix timer list sorted by deadline for the same reason.
        //!
        //! A heap rather than a sorted vector because the operation the frame path repeats is
        //! "re-arm the timer that just fired", which is one sift-down here and an O(T) shift in a
        //! sorted vector.
        //!
        //! Deliberately **no id-to-index side map**. registerTimer(), takeTimerLocked() and
        //! removeEventsForReceiver() still search this linearly, and every one of them is a cold
        //! path -- a timer starting, stopping, or an object dying -- against a T that is a couple
        //! of dozen in every workload this library is aimed at. Add one when a measurement asks
        //! for it.
        std::vector<TimerData>  mTimers;

        //! The expiries one collection pass has taken off the heap, kept so its storage is reused.
        //!
        //! Two phases rather than one, and the separation is load-bearing: a timer whose interval
        //! is zero re-arms to a deadline that is still due, so putting it back while the loop is
        //! still reading the front of the heap would make that loop never end. Taking every expiry
        //! out first and putting them all back afterwards is what keeps a zero-interval timer to
        //! one expiry per pass, which is what "fire on every pass of the loop" means and what the
        //! linear scan gave for free.
        //!
        //! Always empty outside the locked region that fills it, and only its capacity is worth
        //! anything: a steady state of the same T timers firing every tick reuses this buffer and
        //! allocates nothing. Guarded by mMutex, and never held across an unlock, so a nested pass
        //! finds it exactly as the outer pass left it.
        std::vector<TimerData>  mExpiredTimers;

        //! Stamped into the next timer registered, and never reused. Guarded by mMutex.
        unsigned long long mNextTimerSequence { 1 };

        //! Guards every other member of this class.
        //!
        //! Mutable because the capacity, depth and drop-count accessors are const and still have to
        //! take it: they read state a concurrent post is writing.
        mutable std::mutex mMutex;
        std::condition_variable mCv;          //!< Wakes processEvents() out of its wait.
        std::atomic<bool>       mInterrupt { false };  //!< Set by interrupt() to stop the loop.

        //! True while this loop records what it is dispatching. See setHealthTrackingEnabled().
        //!
        //! Atomic and outside mMutex: the dispatch loop tests it once per event, on a path that
        //! deliberately does not take the lock for anything it can avoid, and the answer changing
        //! a moment late is harmless.
        std::atomic<bool> mHealthTracking { false };

        //! The seqlock guarding mHealth: even when settled, odd while being written.
        //!
        //! The record is several fields written by this loop's own thread and read by another, and
        //! a torn read would pair the receiver of one dispatch with the start time of another --
        //! a report naming the wrong handler, which is worse than no report.
        //!
        //! A mutex would also work and would put a watchdog's polling in the way of the dispatch
        //! path. This does not: the writer increments, writes, increments again, and never waits;
        //! a reader retries while the counter is odd or has moved. One writer and many readers is
        //! exactly the arrangement a seqlock is for.
        std::atomic<unsigned> mHealthSeq { 0 };

        //! What this loop is dispatching. Written only by the loop's own thread, under
        //! mHealthSeq.
        //!
        //! **Every field is a relaxed atomic, and that is not decoration.** A seqlock written the
        //! textbook way -- plain fields, fences on the counter -- is a data race in the C++ memory
        //! model, whatever it does on any particular processor, and ThreadSanitizer reports it as
        //! one. Relaxed atomics make the accesses defined without emitting anything a plain load or
        //! store would not: the fences on mHealthSeq still do all the ordering.
        //!
        //! The two time points are stored as their tick counts rather than as time_point, so the
        //! atomics are over a 64-bit integer and are lock-free on every target here. health()
        //! rebuilds the time_points.
        struct HealthRecord
        {
            std::atomic<bool> mDispatching { false };
            std::atomic<long long> mDispatchStartTicks { 0 };
            std::atomic<long long> mLastProgressTicks { 0 };
            std::atomic<unsigned long long> mDispatchCount { 0 };
            std::atomic<const Object*> mReceiver { nullptr };
            std::atomic<int> mEventType { 0 };
            std::atomic<int> mTimerId { 0 };
        };

        HealthRecord mHealth;

        //! Records that a dispatch is starting. Called on the loop's own thread only.
        void beginDispatch
            (
            const Object* aReceiver,  //!< Receiver; recorded, never dereferenced.
            int aEventType,           //!< Event::Type of what is being dispatched.
            int aTimerId              //!< Timer id, or 0.
            );

        //! Records that a dispatch has finished, restoring @p aPrevious.
        //!
        //! Takes what was in flight before rather than simply clearing, so a nested processEvents()
        //! leaves the outer dispatch reported again on the way out.
        void endDispatch
            (
            const LoopHealth& aPrevious  //!< The record as it was before beginDispatch().
            );

        //! Most events the queue will hold, or 0 for unbounded. Guarded by mMutex.
        //!
        //! Zero rather than a large default, so the check is `mCapacity && depth >= mCapacity` and
        //! an application that never sets one pays a compare against a register rather than
        //! anything the policy switch can reach.
        std::size_t mCapacity { 0 };

        //! Events refused or evicted over this dispatcher's life. Guarded by mMutex.
        //!
        //! Never reset, so two readings subtract to a rate. Sixty-four bits because a queue being
        //! flooded at kilohertz would wrap a 32-bit counter in a long-running vehicle, and a
        //! wrapped diagnostic counter reads as health.
        unsigned long long mDroppedEvents { 0 };

        //! False while the queue has been at capacity continuously. Guarded by mMutex.
        //!
        //! Gates the warning to the first drop of a burst rather than every drop: a queue shedding
        //! a thousand events a second would otherwise make the log the second thing overwhelming
        //! the system. Cleared again once a post succeeds, so a later burst is reported afresh.
        bool mOverflowReported { false };

        //! Admits @p aEvent under @p aPolicy, or refuses it. Callers must hold mMutex.
        //!
        //! Split out of postEvent() so that the whole admission decision is one function to read
        //! and one function to get right, rather than being spread through the locking.
        //! @return true if the event is now in the queue, false if it was refused and deleted.
        bool admitLocked
            (
            Object* aReceiver,       //!< The receiver; the queue key.
            Event* aEvent,           //!< The event; owned from here on.
            OverflowPolicy aPolicy,  //!< What to do if the queue is full.
            int aPriority            //!< Higher runs first.
            );

        //! Inserts an entry in descending priority order, keeping equal priorities in posting
        //! order. Callers must hold mMutex.
        void insertByPriorityLocked
            (
            Object* aReceiver,  //!< The receiver; the queue key.
            Event* aEvent,      //!< The event; owned by the queue from here on.
            int aPriority       //!< Higher runs first.
            );

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
        //! earlier handler in the same pass would then still be delivered, and in the destroyed
        //! case that is a call through a freed pointer.
        //!
        //! A pass fills in only the batches it has: processEvents() publishes mEvents and mTimers,
        //! processDeferredDeletes() publishes mDeletes.
        struct DispatchFrame
        {
            std::vector<EventPair>* mEvents { nullptr };   //!< Queued events taken from mEventQueue.
            std::vector<EventPair>* mTimers { nullptr };   //!< Timers that expired in this pass.
            std::vector<EventPair>* mDeletes { nullptr };  //!< Deferred deletes taken from mEventQueue.
            DispatchFrame*          mOuter { nullptr };    //!< The pass this one is nested inside.
        };

        //! Every dispatch pass currently running on this dispatcher, innermost first.
        //!
        //! A chain rather than one frame because passes nest: a handler may run a nested
        //! processEvents(), which is an ordinary thing for user code to do, and a cancellation
        //! raised inside the nested pass must still reach the outer pass's batches -- the outer
        //! pass will go on dispatching them after the inner one returns.
        //!
        //! Guarded by mMutex, as is every access to any entry of any frame, so ownership of each
        //! event passes to exactly one party: whoever clears the slot first has it, and the other
        //! sees nullptr and skips.
        DispatchFrame* mDispatchFrames { nullptr };

        //! Cancels every entry in every published batch that targets @p aReceiver, freeing its
        //! event and clearing the slot so the dispatch loop skips it. Callers must hold mMutex.
        void cancelPublishedEntriesFor
            (
            Object* aReceiver
            );

        //! Cancels every published TimerEvent carrying @p aTimerId, for the same reason and with
        //! the same ownership rule. Callers must hold mMutex.
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

        //! Orders the timer heap: true when @p aLhs is due later than @p aRhs.
        //!
        //! Reversed, because std::push_heap and friends build a *max*-heap out of the comparison
        //! they are given, and what this wants at the front is the earliest deadline.
        //!
        //! Compares deadlines and nothing else. It does not need to be a total order: two timers
        //! due at the same instant may come off the heap either way round, and the collection loop
        //! puts the whole batch back into registration order afterwards.
        //!
        //! A stateless functor rather than a free function, because every sift step of every
        //! re-arm calls it: a functor's operator() is inlined into the algorithm, where a function
        //! *pointer* leaves an indirect call per comparison on the path this whole ordering exists
        //! to make cheap.
        struct FiresLater
        {
            //! @return true if @p aLhs is due later than @p aRhs.
            bool operator()
                (
                const TimerData& aLhs,  //!< Left-hand timer.
                const TimerData& aRhs   //!< Right-hand timer.
                ) const
            {
                return aLhs.mNextFire > aRhs.mNextFire;
            }

        };

        //! Restores the heap invariant over the whole of mTimers. Callers must hold mMutex.
        //!
        //! For the paths that change or remove an entry in place -- a re-registration, a kill, an
        //! object dying -- where the element that moved is not the one the heap operations reach.
        //! O(T), and every caller is a cold path; see mTimers.
        void rebuildTimerHeapLocked();

        //! @return milliseconds from @p aNow to the earliest deadline, 0 if one has passed, -1 if
        //! nothing is scheduled. Callers must hold mMutex.
        int remainingTimeLocked
            (
            std::chrono::steady_clock::time_point aNow  //!< The instant to measure from.
            ) const;

        //! Reports the earliest deadline to mDeadlineCallback, if it has moved since the last
        //! report. **Callers must not hold mMutex.**
        //!
        //! Costs one relaxed-ordered atomic load for a dispatcher with no deadline callback, which
        //! is every dispatcher but one belonging to a thread running its own native loop. Only past
        //! that does it take mMutex to read the front of the heap.
        void notifyDeadlineChanged();

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

        //! Tells a native loop how long it may sleep before the next deadline; empty if unused.
        std::function<void( int )> mDeadlineCallback;

        //! True while mDeadlineCallback holds a callback. Same purpose as mHasWakeCallback.
        //!
        //! Read once per dispatch pass and once per timer registration, so the dispatcher belonging
        //! to a thread that runs exec() -- which is every thread but an adopted one -- pays one
        //! atomic load and nothing else for a feature it does not use.
        std::atomic<bool> mHasDeadlineCallback { false };

        //! The deadline last handed to mDeadlineCallback. Guarded by mMutex.
        //!
        //! What makes this a report of a *move* rather than a report of every mutation. Without it
        //! a timer started far behind the earliest one would still wake the host to tell it
        //! nothing had changed.
        std::chrono::steady_clock::time_point mReportedDeadline {};

        //! False while mReportedDeadline means "nothing was scheduled". Guarded by mMutex.
        bool mHasReportedDeadline { false };

        //! Guards mWakeCallback and mDeadlineCallback -- deliberately NOT mMutex.
        //!
        //! wakeWaiter() runs with mMutex released so that the callback it may invoke is not user
        //! code under our lock, which means it cannot use mMutex to guard the callback either. A
        //! separate lock keeps the read safe without re-entering the one the callers just dropped.
        //! notifyDeadlineChanged() is in the same position for the same reason, so the two share
        //! this lock: it is held only to copy a std::function out, never across a call into one.
        mutable std::mutex mCallbackMutex;
    };
}

#endif // QT_LIKE_SIGNAL_EVENTDISPATCHERDEFAULT_HPP
