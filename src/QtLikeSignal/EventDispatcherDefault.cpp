// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Cross-platform event dispatcher implementation.

#include "QtLikeSignal/EventDispatcherDefault.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Object.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_set>

namespace QtLikeSignal
{
    namespace
    {
        //! Most queue entries whose storage a pass will hand back for the next one to reuse.
        //!
        //! 1024 entries is 16 kB, and a queue that ever grew past it was a burst rather than a
        //! steady state. Capping the hand-back is what keeps one such burst from pinning that
        //! memory for the life of the thread: a vector never shrinks on its own, where the deque
        //! this queue used to be freed its blocks as it drained.
        constexpr std::size_t kMaxRetainedQueueEntries = 1024;

        //! Milliseconds from @p aNow to @p aDeadline, floored at 0 and capped at INT_MAX.
        //!
        //! Shared by the two remaining-time queries so they cannot answer the same question
        //! differently. Truncated rather than rounded up, so a host told "3 ms" wakes at or a
        //! fraction before the deadline rather than a fraction after: an early wake costs a pass
        //! that delivers nothing, where a late one is a late timer.
        //!
        //! The cap cannot fire today -- an interval is an int of milliseconds, so a remaining time
        //! cannot exceed one by construction. It is here because that rests on every caller of
        //! registerTimer() continuing to pass an int, and a silent wrap to a negative number would
        //! read as "nothing is scheduled": a timer that stops firing rather than a number that
        //! looks wrong.
        int millisecondsUntil
            (
            std::chrono::steady_clock::time_point aDeadline,  //!< When the timer is next due.
            std::chrono::steady_clock::time_point aNow        //!< The instant to measure from.
            )
        {
            if( aDeadline <= aNow )
            {
                return 0;
            }

            const long long ms
                = std::chrono::duration_cast<std::chrono::milliseconds>( aDeadline - aNow ).count();

            if( ms > static_cast<long long>( std::numeric_limits<int>::max() ) )
            {
                return std::numeric_limits<int>::max();
            }
            return static_cast<int>( ms );
        }
    }

    //! Constructs a new default event dispatcher.
    EventDispatcherDefault::EventDispatcherDefault() = default;

    //! Destroys the default event dispatcher and frees pending events.
    EventDispatcherDefault::~EventDispatcherDefault()
    {
        std::lock_guard<std::mutex> lock( mMutex );
        for( const EventPair& ep : mEventQueue )
        {
            delete ep.mEvent;
        }
        mEventQueue.clear();
    }

    //! Processes pending events and expired timers once without an internal infinite loop. Returns
    //! true if any events or timers were processed, false otherwise.
    //!
    //! Thread-safe. Called by thread event loop.
    bool EventDispatcherDefault::processEvents
        (
        ProcessEventsFlag aFlag   //!< Whether an idle pass may block. See the enum.
        )
    {
        // Consume the interrupt rather than merely testing it. interrupt() means "return from the
        // pass that is running now", not "refuse to work ever again". Leaving it set would make
        // every later pass return instantly, so a loop reusing this dispatcher -- exec() after a
        // quit() -- would spin at 100% CPU. Qt consumes it in the same place:
        // `d->interrupt.fetchAndStoreRelaxed(false)` at the top of processEvents().
        if( mInterrupt.exchange( false ) )
        {
            return false;
        }

        // The same type as mEventQueue, so the whole batch can be taken with a swap below rather
        // than copied out entry by entry.
        std::vector<EventPair>    eventsToProcess;
        std::vector<EventPair>    timerEventsToProcess;

        //! Entries the dispatch loop may still take out of the live queue ahead of its own batch.
        //!
        //! Set to the queue's depth at the moment this pass takes its batch, and spent one per
        //! preemption. It is what bounds the pass: without it a producer posting high-priority
        //! events faster than they dispatch would keep one processEvents() call running forever.
        std::size_t stealBudget = 0;
        std::chrono::milliseconds maxWait { 100 };

        // Declared out here so it outlives both batches it points at.
        DispatchFrame frame;

        {
            std::unique_lock<std::mutex> lock( mMutex );

            auto now = std::chrono::steady_clock::now();

            // Last pass's timer-batch storage, handed over before anything is collected into it, so
            // a steady state of the same timers firing every tick regrows nothing. See
            // mSpareTimerBatch.
            timerEventsToProcess.swap( mSpareTimerBatch );

            // Collect expired timers.
            //
            // **The nothing-due case is this one comparison.** It used to be a walk of every
            // registered timer, and then a second walk to find the minimum deadline, both under
            // this lock and both on every pass -- while the pass count is set by traffic rather
            // than by timers, since a thread taking cross-thread posts runs a pass per wake. See
            // mTimers.
            while( !mTimers.empty() && now >= mTimers.front().mNextFire )
            {
                // Out of the heap and into the scratch list, not straight into the batch. A
                // zero-interval timer re-arms to a deadline that is still due, so putting it back
                // before this loop has finished reading the front would make the loop never end.
                // See mExpiredTimers.
                std::pop_heap( mTimers.begin(), mTimers.end(), FiresLater() );
                mExpiredTimers.push_back( mTimers.back() );
                mTimers.pop_back();
            }

            // Back into the order they were registered in, which is the order the walk this
            // replaced delivered them in. The heap hands them back by deadline, and for two timers
            // due in the same pass that is a different order -- an observable change in which
            // handler runs first, from a change that was meant to be about finding the *next*
            // deadline faster. Restoring it here is what keeps the reordering invisible.
            //
            // Skipped for the overwhelmingly common batch of one, and O(E log E) on a batch of E
            // that a pass is about to dispatch anyway.
            if( mExpiredTimers.size() > 1 )
            {
                std::sort( mExpiredTimers.begin(),
                    mExpiredTimers.end(),
                    []( const TimerData& aLhs, const TimerData& aRhs )
                    {
                        return aLhs.mSequence < aRhs.mSequence;
                    } );
            }

            for( TimerData& t : mExpiredTimers )
            {
                // Advance from the deadline that just elapsed, not from the moment we noticed it.
                // `now` is whenever this pass got around to looking, so re-arming from it folds
                // that lateness into the cadence permanently -- one pass 20 ms late and every fire
                // thereafter is 20 ms off, with the error compounding under load.
                t.mNextFire += std::chrono::milliseconds( t.mIntervalMs );

                if( t.mNextFire < now )
                {
                    // The loop was blocked for longer than a whole interval, so keeping the
                    // original cadence would mean firing repeatedly to work off a backlog nobody
                    // asked for. Give up on it and resynchronise. Qt makes the same two choices in
                    // the same order (calculateNextTimeout(), qtimerinfo_unix.cpp).
                    t.mNextFire = now + std::chrono::milliseconds( t.mIntervalMs );
                }

                // Re-armed and back in the heap *before* the event is built, so that an allocation
                // failure cannot leave the timer dropped as well as the expiry lost.
                mTimers.push_back( t );
                std::push_heap( mTimers.begin(), mTimers.end(), FiresLater() );

                timerEventsToProcess.push_back(
                    { t.mReceiver, new TimerEvent( t.mTimerId ), EventPriority::kNormal } )
                ;
            }

            // Emptied, not freed: the capacity is what makes the next tick's collection cost
            // nothing.
            mExpiredTimers.clear();

            // Determine wait time for next timer if no events present
            if( mEventQueue.empty() && timerEventsToProcess.empty() )
            {
                if( !mTimers.empty() )
                {
                    // The front of the heap, not a scan. This is what the ordering buys.
                    const auto minFire = mTimers.front().mNextFire;
                    if( minFire > now )
                    {
                        maxWait = std::chrono::duration_cast<std::chrono::milliseconds>( minFire -
                            now )
                        ;
                    }
                    else
                    {
                        maxWait = std::chrono::milliseconds( 0 );
                    }
                }

                // Clear the change flag right before waiting, still holding mMutex, so any
                // registerTimer()/unregisterTimer() call that runs concurrently is guaranteed to
                // either land before this point (already reflected in maxWait above) or after
                // (blocked on mMutex until we release it inside wait_for, then setting the flag and
                // notifying) -- there is no window where a change can be lost.
                mTimersChanged = false;

                // Hand the blocking to the platform. -1 means "no deadline": nothing is scheduled,
                // so block until something actually happens rather than waking ten times a second
                // forever. Every state the wake condition tests is changed under mMutex by a caller
                // that then calls wakeWaiter(), so there is no wakeup to miss.
                //
                // waitForEvents() is given the lock and is required to release it while it is
                // actually blocked and to re-acquire it before returning, so everything read below
                // is still guarded. That contract is what lets a platform subclass block in poll()
                // or MsgWaitForMultipleObjectsEx(), neither of which can hold a std::mutex.
                // AllEvents skips the wait entirely rather than waiting with a zero timeout,
                // because the two are not the same thing on every backend: a poll() of 0 ms is
                // cheap, but MsgWaitForMultipleObjectsEx still enters an alertable wait and can
                // dispatch an APC. Not calling it at all is the only version that reliably gives
                // the caller its thread straight back.
                //
                // Nothing is lost by skipping it. Everything the wait would have noticed --
                // queued events, expired timers -- was already collected above under this same
                // lock, and anything arriving after this point is for the next pass either way.
                if( aFlag == ProcessEventsFlag::WaitForMoreEvents )
                {
                    const int timeoutMs = mTimers.empty()
                                          ? -1
                                          : static_cast<int>( maxWait.count() );
                    waitForEvents( lock, timeoutMs );
                }

                // wakeUp() is a one-shot "return from the wait now" request; consume it so a later
                // processEvents() call does not treat it as still pending.
                mWakeUpRequested = false;
            }

            // Consumed, not just tested -- same reason as at the top of this function: an interrupt
            // that arrived while we were waiting has now been acted on, and leaving it set would
            // make every subsequent pass return instantly.
            if( mInterrupt.exchange( false ) )
            {
                // timerEventsToProcess may already hold heap-allocated TimerEvent objects collected
                // above; they were never handed off to the dispatch loop below, so free them here
                // to avoid leaking them.
                //
                // **As the code stands this batch is always empty, and that is worth knowing before
                // anybody deletes these two lines as dead.** Collection and this check are one hold
                // of mMutex -- the wait between them is skipped whenever anything was collected,
                // because there is already work to do -- and interrupt() takes mMutex too. So no
                // other thread can set the flag inside the span; it blocks until the pass releases
                // the lock, which is after this. The only path that arrives here with the flag set
                // from elsewhere goes through waitForEvents(), and that runs only when the batch is
                // empty. Measured 2026-09-08: zero non-empty arrivals across the whole test suite.
                //
                // Kept anyway. It is two lines, it costs nothing on a batch of zero, and it stops
                // being dead the moment the wait condition above changes -- which is exactly the
                // kind of change that would reintroduce the leak it was written for.
                for( auto& ep : timerEventsToProcess )
                {
                    delete ep.mEvent;
                }

                // Hand the storage back on the way out too, or an interrupted pass would leave the
                // next one to regrow it. Still under mMutex, and every event in it has just been
                // freed.
                timerEventsToProcess.clear();
                mSpareTimerBatch.swap( timerEventsToProcess );
                return false;
            }

            // Take the whole queue in one move rather than copying it out entry by entry. The old
            // loop cost a copy per event plus the growth reallocations of the destination, all of
            // it under mMutex and therefore in the way of every thread trying to post. Qt walks its
            // postEventList in place and QtLikeSignal swaps its queue; this is the same idea.
            // mEventQueue is left empty, which is exactly what the drain loop left behind too.
            //
            // Two swaps, not one, and the first is what keeps this container cheaper than the
            // deque it replaced rather than dearer. Taking the queue with a bare swap would leave
            // mEventQueue holding the fresh local's buffer -- no capacity at all -- so every post
            // arriving during this pass would regrow it from nothing, one allocation per doubling.
            // Handing it last pass's buffer first means the queue starts this pass with storage it
            // has already paid for, and in a steady state the two buffers simply circulate.
            eventsToProcess.swap( mSpareBatch );
            eventsToProcess.swap( mEventQueue );

            // Publish both batches so unregisterTimer() and removeEventsForReceiver() can cancel
            // entries in them while the handlers below run. Linked in before *any* dispatching,
            // since a queued metacall can kill a timer or destroy an object just as a timer handler
            // can, and while still holding the lock, so there is no window in which a pass owns
            // work that no canceller can see. How many entries the preemption below may take out of
            // the live queue.
            //
            // The batch's own size, so a pass dispatches at most twice what it took. That is what
            // bounds it: a producer posting high-priority events faster than they dispatch would
            // otherwise keep one pass running forever and processEvents() would never return to
            // its caller.
            //
            // Deliberately not the queue's depth, which is the obvious thing to reach for and is
            // always zero here -- the swap two lines up is what emptied it.
            stealBudget = eventsToProcess.size();

            frame.mEvents   = &eventsToProcess;
            frame.mTimers   = &timerEventsToProcess;
            frame.mOuter    = mDispatchFrames;
            mDispatchFrames = &frame;
        }

        // Unlinks the frame however this function leaves, so a pointer to a dead local can never
        // outlive the pass.
        struct FrameRetractor
        {
            ~FrameRetractor()
            {
                std::lock_guard<std::mutex> lock( mOwner->mMutex );
                mOwner->unlinkDispatchFrame( mFrame );
            }

            EventDispatcherDefault* mOwner;
            DispatchFrame*          mFrame;
        } frameRetractor { this, &frame };

        // Tell a native loop when the next deadline is, now that this pass has re-armed whatever
        // fired. Here rather than at the end of the function, so a host learns the next deadline
        // while this pass is still dispatching rather than a whole batch of handlers later.
        //
        // With mMutex released, because it runs user code. Costs one atomic load on a dispatcher
        // that has no deadline callback, which is every dispatcher belonging to a thread running
        // exec(). Handlers that start or stop a timer report their own changes from
        // registerTimer()/unregisterTimer(), so nothing needs saying again on the way out.
        notifyDeadlineChanged();

        // Drain the OS's own event source, with mMutex released so platform code may re-enter this
        // dispatcher (a native handler is free to post an event or start a timer). Done before our
        // own dispatch below so an OS message that arrived during the wait is not held back a full
        // pass behind the queued work it may itself have produced.
        processPlatformEvents();

        //! Records a dispatch for as long as it runs, and puts back what was in flight before.
        //!
        //! A guard rather than a pair of calls because a handler is arbitrary application code that
        //! may throw, and a record left saying "still dispatching" after the stack unwound would
        //! read as a permanent stall -- a watchdog reporting a fault that has already ended.
        //!
        //! Local to this function on purpose. A class declared here has the access this member
        //! function has, so it can call endDispatch() without that having to become public; a free
        //! class in an anonymous namespace could not.
        //!
        //! Costs nothing when tracking is off. The destructor tests the flag it was handed rather
        //! than re-reading the atomic, so switching tracking off in the middle of a dispatch cannot
        //! leave mDispatching stuck true.
        class DispatchRecord
        {
        public:
            DispatchRecord
                (
                EventDispatcherDefault* aOwner,  //!< The loop dispatching; never null.
                bool aEnabled,                   //!< Whether tracking was on when this began.
                const LoopHealth& aPrevious      //!< What was in flight before.
                )
                : mOwner( aOwner )
                , mEnabled( aEnabled )
                , mPrevious( aPrevious )
            {
            }

            ~DispatchRecord()
            {
                if( mEnabled )
                {
                    mOwner->endDispatch( mPrevious );
                }
            }

            DispatchRecord
                (
                const DispatchRecord&
                ) = delete;

            DispatchRecord& operator=
                (
                const DispatchRecord&
                ) = delete;

        private:
            EventDispatcherDefault* mOwner;
            bool mEnabled;
            LoopHealth mPrevious;
        };

        bool processedAny = false;

        // Tracks receivers that were deleted via a DeferredDeleteEvent processed earlier in this
        // same batch.
        //
        // Both batches are published above, so ~Object() -> removeEventsForReceiver() normally
        // cancels a destroyed receiver's remaining entries and this set has nothing to add. It
        // stays because that path is affinity-dependent: ~Object() cancels through the dispatcher
        // its *current* affinity names, so an object whose affinity changed after these events were
        // posted cancels somewhere else and leaves ours behind. This set covers the deferred-delete
        // case of that regardless of where the object thinks it lives, and it is one hash lookup.
        std::unordered_set<Object*> deletedReceivers;

        // Dispatch queued events
        for( size_t i = 0; i < eventsToProcess.size(); )
        {
            // Taken under the lock, clearing our slot as we go, exactly as the timer loop below
            // does: whoever clears an entry owns its event, and the other side sees nullptr and
            // skips. That is what lets removeEventsForReceiver() cancel an entry belonging to an
            // object destroyed by an earlier handler in this same batch.
            //
            // The extra lock per event is deliberate and cheap -- an uncontended acquire against
            // the several hundred nanoseconds a queued metacall already costs. Reading an entry
            // unguarded would race the very cancellation the publication exists to allow.
            EventPair ep { nullptr, nullptr, EventPriority::kNormal };
            {
                std::lock_guard<std::mutex> lock( mMutex );

                // **Preemption.** A pass takes the whole queue in one swap and dispatches it with
                // the mutex released, which is what makes the pass lock-free and lets the two
                // buffers circulate without allocating. The cost of that is that an event posted
                // *during* a pass is invisible to it -- so a telltale posted while a thousand-event
                // repaint batch is in flight would wait out the whole batch however high its
                // priority, and the feature would look implemented without doing its job.
                //
                // Qt has no such problem because it walks its postEventList in place behind an
                // insertionOffset. Adopting that here would undo the batch swap, which the
                // allocation guards depend on. So the pass peeks at the live queue instead: if its
                // front outranks the batch entry we were about to run, that goes first.
                //
                // Costs nothing. The loop already holds this lock to claim its entry, and this is a
                // comparison and a branch inside it.
                //
                // Strictly greater, so an equal priority never preempts -- that would let a
                // late-posted event overtake one already waiting at the same rank and break
                // same-priority FIFO, which is the one ordering guarantee everything else relies
                // on.
                const bool preempt = stealBudget > 0
                    && !mEventQueue.empty()
                    && eventsToProcess[i].mEvent != nullptr
                    && mEventQueue.front().mPriority > eventsToProcess[i].mPriority;

                if( preempt )
                {
                    ep = mEventQueue.front();
                    mEventQueue.erase( mEventQueue.begin() );
                    --stealBudget;

                    // i is deliberately not advanced: the batch entry we stepped over is still
                    // ours to run, on the next turn of this loop.
                }
                else
                {
                    ep                        = eventsToProcess[i];
                    eventsToProcess[i].mEvent = nullptr;
                    ++i;
                }
            }

            if( !ep.mReceiver || !ep.mEvent )
            {
                delete ep.mEvent;
                continue;
            }
            if( deletedReceivers.count( ep.mReceiver ) )
            {
                delete ep.mEvent;
                continue;
            }

            const bool isDeferredDelete = ( ep.mEvent->type() == Event::DeferredDelete );

            {
                const bool tracking = mHealthTracking.load( std::memory_order_relaxed );
                LoopHealth previous;
                if( tracking )
                {
                    previous = health();
                    beginDispatch( ep.mReceiver, static_cast<int>( ep.mEvent->type() ), 0 );
                }
                const DispatchRecord record( this, tracking, previous );

                Object::dispatchEvent( ep.mReceiver, ep.mEvent );
            }

            if( isDeferredDelete )
            {
                deletedReceivers.insert( ep.mReceiver );
            }

            // **This delete is deliberately not exception-safe, and that is the design.**
            //
            // Left here rather than behind a scope guard: if dispatchEvent() threw, this line would
            // not run and neither would the deletes for the entries still waiting in
            // eventsToProcess, so the whole remaining batch would leak. That is a real consequence
            // and it is accepted, because a slot in this library cannot throw:
            //
            // - **An application is expected to build without exceptions** -- -fno-exceptions on
            //   gcc and clang, and on MSVC an application that simply never throws. That is the
            //   contract AbstractEventDispatcher::processEvents() states, and a slot that cannot
            //   throw cannot reach this.
            // - **On a worker thread it would not leak anyway.** An exception escaping here unwinds
            //   through exec(), run() and threadBody() into threadEntry(), which is a
            //   pthread_create/_beginthreadex callback; escaping that calls std::terminate. The
            //   process dies rather than leaking, on every thread but the one running the main
            //   loop.
            //
            // Making it safe means owning both batch vectors -- an owning entry type or a RAII
            // drain -- on the hottest path in the library, to protect a case the build flags
            // already exclude. Measured before rejecting: an EventPair is copied per dispatch and
            // the queued-emit path is guarded at 0.00 allocations, so a unique_ptr here is not
            // free.
            //
            // Reported by review on 2026-09-05, confirmed by experiment, and closed as By Design.
            // A reviewer finding it again has found the thing this comment describes.
            delete ep.mEvent;
            processedAny = true;
        }

        // Dispatch timer events
        for( size_t i = 0; i < timerEventsToProcess.size(); ++i )
        {
            // Take the entry out of the batch under the lock, clearing our slot as we go. That
            // hands ownership over in one atomic step: either unregisterTimer() got here first and
            // we see nullptr, or we did and it sees nullptr. Neither can free the event twice, and
            // a timer killed by an earlier handler in this same batch is simply skipped.
            EventPair ep { nullptr, nullptr, EventPriority::kNormal };
            {
                std::lock_guard<std::mutex> lock( mMutex );
                ep = timerEventsToProcess[i];
                timerEventsToProcess[i].mEvent = nullptr;
            }

            if( !ep.mReceiver || !ep.mEvent )
            {
                delete ep.mEvent;
                continue;
            }
            if( deletedReceivers.count( ep.mReceiver ) )
            {
                delete ep.mEvent;
                continue;
            }

            {
                const bool tracking = mHealthTracking.load( std::memory_order_relaxed );
                LoopHealth previous;
                if( tracking )
                {
                    previous = health();

                    // The timer id goes in as well as the type: "stalled in timer 7" is a report
                    // someone can act on, where "stalled in a timer" usually is not.
                    beginDispatch( ep.mReceiver, static_cast<int>( ep.mEvent->type() ),
                        static_cast<TimerEvent*>( ep.mEvent )->timerId() );
                }
                const DispatchRecord record( this, tracking, previous );

                Object::dispatchEvent( ep.mReceiver, ep.mEvent );
            }

            delete ep.mEvent;
            processedAny = true;
        }

        // Keep this batch's storage for the next pass to hand to the queue. Every entry above was
        // consumed, which is why clearing it is not a leak.
        //
        // Not a refinement: without the circulation this container is *worse* than the deque it
        // replaced on libstdc++, which allocates one block per 32 entries rather than one per
        // entry. Measured at 1.26 blocks per queued emit against the deque's 1.00 -- fixing Windows
        // by breaking Linux -- and at 1.00 with it.
        {
            std::lock_guard<std::mutex> lock( mMutex );
            if( eventsToProcess.capacity() > mSpareBatch.capacity()
                && eventsToProcess.capacity() <= kMaxRetainedQueueEntries )
            {
                eventsToProcess.clear();
                mSpareBatch.swap( eventsToProcess );
            }

            // The timer batch circulates on the same terms and for the same reason. Both are still
            // reachable through this pass's published frame until the retractor below unlinks it,
            // which is why this is under mMutex: a canceller walking the frame sees an emptied
            // vector rather than entries being swapped out from under it.
            if( timerEventsToProcess.capacity() > mSpareTimerBatch.capacity()
                && timerEventsToProcess.capacity() <= kMaxRetainedQueueEntries )
            {
                timerEventsToProcess.clear();
                mSpareTimerBatch.swap( timerEventsToProcess );
            }
        }

        return processedAny;
    }

    //! Blocks on the condition variable until there is work or @p aTimeoutMs elapses.
    //!
    //! The cross-platform implementation of the wait hook. Unlike the platform subclasses this one
    //! keeps @p aLock: std::condition_variable releases and re-acquires it internally, which is the
    //! same contract from the caller's point of view.
    void EventDispatcherDefault::waitForEvents
        (
        std::unique_lock<std::mutex>& aLock,  //!< Lock on mMutex, held on entry and on return.
        int aTimeoutMs                          //!< Milliseconds to wait, or -1 to wait indefinitely.
        )
    {
        auto wakeCondition = [this]
            {
                return !mEventQueue.empty() || mInterrupt.load() || mTimersChanged
                       || mWakeUpRequested;
            };

        if( aTimeoutMs < 0 )
        {
            mCv.wait( aLock, wakeCondition );
        }
        else
        {
            mCv.wait_for( aLock, std::chrono::milliseconds( aTimeoutMs ), wakeCondition );
        }
    }

    //! Wakes a thread blocked in waitForEvents(). Thread-safe and non-blocking.
    void EventDispatcherDefault::wakeWaiter()
    {
        mCv.notify_all();

        // Almost always false, and this runs on every postEvent(), so the whole read below -- a
        // mutex acquire/release and a std::function copy-construct and destroy -- is skipped for
        // any thread that is not draining our queue from its own native loop. See
        // mHasWakeCallback.
        if( !mHasWakeCallback.load( std::memory_order_acquire ) )
        {
            return;
        }

        // Copy under its own lock, then invoke released. Every caller has already dropped mMutex,
        // so the callback is free to post, start a timer, or otherwise call straight back in.
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock( mCallbackMutex );
            callback = mWakeCallback;
        }
        if( callback )
        {
            callback();
        }
    }

    //! Installs the callback invoked whenever work is queued for this thread. Thread-safe.
    void EventDispatcherDefault::setWakeCallback
        (
        std::function<void()> aCallback  //!< Invoked on post; nullptr clears.
        )
    {
        std::lock_guard<std::mutex> lock( mCallbackMutex );
        mWakeCallback = std::move( aCallback );

        // Published after the callback itself, and read with acquire in wakeWaiter(), so a waker
        // that sees the flag is guaranteed to see the callback behind it. Still inside the lock so
        // two concurrent setters cannot leave the flag disagreeing with the callback.
        mHasWakeCallback.store( static_cast<bool>( mWakeCallback ), std::memory_order_release );
    }

    //! Drains OS/platform events. No-op here: the cross-platform dispatcher has no OS event source.
    void EventDispatcherDefault::processPlatformEvents()
    {
    }

    //! Registers a timer for a target object. Thread-safe.
    void EventDispatcherDefault::registerTimer
        (
        int aTimerId,     //!< Unique timer identifier.
        int aInterval,    //!< Interval in milliseconds.
        Object* aObject  //!< Target object to receive TimerEvent.
        )
    {
        if( !aObject || aInterval < 0 )
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock( mMutex );
            auto now = std::chrono::steady_clock::now();
            TimerData td;
            td.mTimerId    = aTimerId;
            td.mIntervalMs = aInterval;
            td.mReceiver   = aObject;
            td.mNextFire   = now + std::chrono::milliseconds( aInterval );
            td.mSequence   = 0;

            bool replaced = false;
            for( auto& t : mTimers )
            {
                if( t.mTimerId == aTimerId )
                {
                    // Keeps the sequence this id was first registered with, so re-registering an
                    // id -- which is what moveToThread() does to carry a running timer across --
                    // does not move it in the firing order among same-pass expiries. The list this
                    // replaced kept the entry where it was for the same reason.
                    td.mSequence = t.mSequence;
                    t            = td;
                    replaced     = true;
                    break;
                }
            }
            if( !replaced )
            {
                td.mSequence = mNextTimerSequence++;
                mTimers.push_back( td );
            }

            // The whole heap, not a sift from the new entry: on the replace path it is an entry
            // already in the middle whose deadline changed, and no heap operation reaches that.
            // O(T), on a path that has just walked the list anyway.
            rebuildTimerHeapLocked();

            mTimersChanged = true;
        }

        // Woken with mMutex released, matching postEvent(): wakeWaiter() may run the thread's wake
        // callback, which is user code and must be free to call back in.
        wakeWaiter();

        // A new timer may well be the earliest one, and a thread with its own native loop has no
        // other way to find that out. Reports nothing when it is not.
        notifyDeadlineChanged();
    }

    //! Unregisters a timer by ID. Returns true if timer was found and removed, false otherwise.
    //! Thread-safe.
    bool EventDispatcherDefault::unregisterTimer
        (
        int aTimerId  //!< Unique timer identifier.
        )
    {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock( mMutex );
            removed = takeTimerLocked( aTimerId );
        }

        if( removed )
        {
            // Woken with mMutex released, matching postEvent(). See wakeWaiter().
            wakeWaiter();

            // Killing the earliest timer pushes the deadline out, and a native loop that is not
            // told simply wakes once for a deadline that is no longer there. Harmless, but the
            // contract is that a move is reported, so report it.
            notifyDeadlineChanged();
        }
        return removed;
    }

    //! Removes timer @p aTimerId and every pending event for it. Returns true if it was registered.
    //! Callers must hold mMutex.
    bool EventDispatcherDefault::takeTimerLocked
        (
        int aTimerId  //!< Unique timer identifier.
        )
    {
        // Drop any TimerEvent for this timer that has already been queued but not yet delivered.
        //
        // This became necessary when timer ids started being recycled. Previously a stale event was
        // harmless: ids only ever climbed, so its id could never match a live timer again and
        // Timer::timerEvent()'s id check discarded it. Now that killTimer() returns the id to a
        // pool, a later startTimer() can be handed the same one -- and the stale event would then
        // match the *new* timer and fire it spuriously. Purging here is what keeps recycling from
        // trading an unreachable counter overflow for a reachable wrong-behaviour bug.
        //
        // Both places a TimerEvent can be waiting have to be covered, and the second one is the one
        // that matters: TimerEvents are only ever created inside the collection loop, straight into
        // the dispatch batch, so mEventQueue holds them only if something posts one directly. The
        // batch is where a killTimer() from inside a sibling timer's handler actually finds them.
        auto itQueue = std::remove_if( mEventQueue.begin(),
            mEventQueue.end(),
            [aTimerId]( const EventPair& aEp )
            {
                if( aEp.mEvent && aEp.mEvent->type() == Event::Timer
                && static_cast<TimerEvent*>( aEp.mEvent )->timerId() == aTimerId )
                {
                    delete aEp.mEvent;
                    return true;
                }
                return false;
            } );
        mEventQueue.erase( itQueue, mEventQueue.end() );

        cancelPublishedTimerEvents( aTimerId );

        auto it = std::remove_if( mTimers.begin(),
            mTimers.end(),
            [aTimerId]( const TimerData& aTd )
            {
                return aTd.mTimerId == aTimerId;
            } );
        if( it != mTimers.end() )
        {
            mTimers.erase( it, mTimers.end() );

            // remove_if shuffles survivors forward, which is exactly the operation a heap cannot
            // survive. Rebuilding is O(T) on a timer being killed, which is a cold path.
            rebuildTimerHeapLocked();

            mTimersChanged = true;
            return true;
        }
        return false;
    }

    //! Restores the heap invariant over mTimers. See the declaration.
    void EventDispatcherDefault::rebuildTimerHeapLocked()
    {
        std::make_heap( mTimers.begin(), mTimers.end(), FiresLater() );
    }

    //! Milliseconds from @p aNow to the earliest deadline. See the declaration.
    int EventDispatcherDefault::remainingTimeLocked
        (
        std::chrono::steady_clock::time_point aNow  //!< The instant to measure from.
        ) const
    {
        if( mTimers.empty() )
        {
            return -1;
        }

        return millisecondsUntil( mTimers.front().mNextFire, aNow );
    }

    //! Milliseconds until the earliest timer deadline. See the declaration.
    int EventDispatcherDefault::remainingTimeMs() const
    {
        // Sampled before the lock, so a wait on a contended mutex is charged to the caller rather
        // than shortening the answer it is given.
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock( mMutex );
        return remainingTimeLocked( now );
    }

    //! Milliseconds until one timer next fires. See the declaration.
    int EventDispatcherDefault::timerRemainingTimeMs
        (
        int aTimerId  //!< Unique timer identifier.
        ) const
    {
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock( mMutex );

        // Linear, and deliberately so: this answers a question an application asks about one timer
        // it owns, not something the dispatch loop runs. See mTimers on why there is no index.
        //
        // mExpiredTimers does not have to be searched as well. It holds entries only inside the
        // collection loop, which never releases mMutex while it does, so no holder of this lock can
        // ever see a registered timer missing from mTimers.
        for( const TimerData& t : mTimers )
        {
            if( t.mTimerId == aTimerId )
            {
                return millisecondsUntil( t.mNextFire, now );
            }
        }
        return -1;
    }

    //! Installs the callback invoked when the earliest deadline moves. See the declaration.
    void EventDispatcherDefault::setDeadlineCallback
        (
        std::function<void( int aMsFromNow )> aCallback  //!< Invoked on a move; nullptr clears.
        )
    {
        {
            std::lock_guard<std::mutex> lock( mCallbackMutex );
            mDeadlineCallback = std::move( aCallback );

            // Published after the callback itself and read with acquire, so a reporter that sees
            // the flag is guaranteed to see the callback behind it. Same pairing as
            // setWakeCallback().
            mHasDeadlineCallback.store( static_cast<bool>( mDeadlineCallback ),
                std::memory_order_release );
        }

        {
            std::lock_guard<std::mutex> lock( mMutex );

            // Forces the report below through the "has it moved?" filter, so a host is told the
            // current deadline on installation instead of waiting for the next change -- which on
            // a thread whose timers are all already running would be a wait for nothing.
            //
            // A value no real deadline can equal: every mNextFire is steady_clock::now() plus an
            // interval. Saying "reported, and it was the beginning of time" therefore differs from
            // every scheduled state and from the not-scheduled state alike.
            mHasReportedDeadline = true;
            mReportedDeadline    = std::chrono::steady_clock::time_point::min();
        }

        notifyDeadlineChanged();
    }

    //! Reports the earliest deadline if it has moved. See the declaration.
    void EventDispatcherDefault::notifyDeadlineChanged()
    {
        // The whole cost of this feature for a dispatcher that does not use it, which is every one
        // belonging to a thread that runs exec().
        //
        // Relaxed rather than acquire, unlike the same test in wakeWaiter(). Nothing is read behind
        // this load: the callback itself is copied under mCallbackMutex below, and that mutex is
        // what synchronises with the setter. This load only decides whether taking it is worth the
        // trouble. Losing the race means missing one report for a callback installed at that
        // instant -- and setDeadlineCallback() reports the current deadline to its own caller
        // anyway, so there is nothing left to miss.
        if( !mHasDeadlineCallback.load( std::memory_order_relaxed ) )
        {
            return;
        }

        int msFromNow = -1;
        {
            const auto now = std::chrono::steady_clock::now();

            std::lock_guard<std::mutex> lock( mMutex );

            const bool scheduled = !mTimers.empty();
            const auto deadline  = scheduled
                                   ? mTimers.front().mNextFire
                                   : std::chrono::steady_clock::time_point {};

            if( scheduled == mHasReportedDeadline && deadline == mReportedDeadline )
            {
                // Something changed, but not the thing this reports. A timer started behind the
                // earliest one is the ordinary case.
                return;
            }

            mHasReportedDeadline = scheduled;
            mReportedDeadline    = deadline;
            msFromNow            = remainingTimeLocked( now );
        }

        // Copied under its own lock and called with nothing held, matching wakeWaiter(): the
        // callback belongs to a native loop and is free to call straight back in.
        std::function<void( int )> callback;
        {
            std::lock_guard<std::mutex> lock( mCallbackMutex );
            callback = mDeadlineCallback;
        }
        if( callback )
        {
            callback( msFromNow );
        }
    }

    //! Puts an entry in descending priority order. See the declaration.
    void EventDispatcherDefault::insertByPriorityLocked
        (
        Object* aReceiver,  //!< The receiver; the queue key.
        Event* aEvent,      //!< The event; owned by the queue from here on.
        int aPriority       //!< Higher runs first.
        )
    {
        // Appending is the whole of the common case: almost everything is posted at kNormal, so the
        // tail is at least as high as the newcomer and the search never runs. Qt takes the same
        // shortcut in QPostEventList::addEvent().
        if( mEventQueue.empty() || mEventQueue.back().mPriority >= aPriority )
        {
            mEventQueue.push_back( { aReceiver, aEvent, aPriority } );
            return;
        }

        // upper_bound, not lower_bound, and the difference is same-priority FIFO: lower would place
        // the newcomer *before* the entries it ties with, quietly reversing the posting order that
        // every queued signal depends on. Qt's comment says the same thing about its own insert.
        const auto at = std::upper_bound( mEventQueue.begin(), mEventQueue.end(), aPriority,
            []( int aWanted, const EventPair& aEntry )
            {
                return aWanted > aEntry.mPriority;
            } );

        mEventQueue.insert( at, { aReceiver, aEvent, aPriority } );
    }

    //! Decides whether @p aEvent joins the queue, and puts it there if so. See the declaration.
    bool EventDispatcherDefault::admitLocked
        (
        Object* aReceiver,       //!< The receiver; the queue key.
        Event* aEvent,           //!< The event; owned from here on.
        OverflowPolicy aPolicy,  //!< What to do if the queue is full.
        int aPriority            //!< Higher runs first.
        )
    {
        // Acts at any depth, unlike the other two, because that is what the policy is for: an
        // update-request already waiting does not need a second one whether the queue holds three
        // events or a thousand. A version that waited for the ceiling would let a hundred identical
        // repaints accumulate and call it healthy.
        //
        // Equivalence is receiver plus type, which is why a metacall cannot use this: every queued
        // signal carries Event::MetaCall, so this scan would collapse unrelated slots into one
        // call. Object::postEvent() refuses that combination before it gets here, and nothing
        // inside this library asks for it.
        if( aPolicy == OverflowPolicy::Coalesce )
        {
            for( const EventPair& ep : mEventQueue )
            {
                if( ep.mReceiver == aReceiver && ep.mEvent
                    && ep.mEvent->type() == aEvent->type() )
                {
                    // Coalesced, not failed: the caller's intent -- "make sure this is pending" --
                    // is satisfied, so this reports success and is not counted as a drop.
                    delete aEvent;
                    return true;
                }
            }
        }

        // Unbounded is the default and has to stay free: one compare against zero, before anything
        // that could look at the policy or walk the queue.
        const bool full = ( mCapacity != 0 ) && ( mEventQueue.size() >= mCapacity );

        // Admitted whatever the depth. Dropping one leaks the object it names, outright and
        // forever, and under a flood that would leak exactly when memory is scarce -- which
        // inverts the point of having a ceiling. Timer events need no such exemption: they are
        // built inside processEvents() into their own batch and never come through here.
        const bool exempt = ( aEvent->type() == Event::DeferredDelete );

        if( full && !exempt )
        {
            if( aPolicy == OverflowPolicy::DropOldest )
            {
                // Sheds from the *lowest priority present*, not from the front of the queue. The
                // queue is sorted descending, so its front is the most important thing in it, and
                // evicting there would let a bound undo a priority -- the two features have to
                // compose or neither is worth having. Within the lowest band the oldest goes, which
                // is what this policy is named for.
                //
                // Two passes rather than one clever one. Finding the band and then finding its
                // oldest member are different questions, and a single reverse walk that answers
                // both is the kind of code that is right once and wrong after the next edit. Both
                // passes are over a queue the application gave a ceiling to.
                bool haveBand = false;
                int lowestBand = 0;
                for( const EventPair& ep : mEventQueue )
                {
                    // A deferred delete is never the victim, at any priority: dropping one leaks
                    // the object it names. Skipped rather than allowed to define the band, so a
                    // pending delete cannot shield the ordinary events sharing its priority.
                    if( !ep.mEvent || ep.mEvent->type() == Event::DeferredDelete )
                    {
                        continue;
                    }

                    if( !haveBand || ep.mPriority < lowestBand )
                    {
                        lowestBand = ep.mPriority;
                        haveBand = true;
                    }
                }

                // Only when the newcomer is at least as important as the band it would displace.
                //
                // Without this test the policy inverts the very ordering it is meant to protect: a
                // queue holding nothing but high-priority events would accept a low-priority
                // newcomer by evicting one of them, so a flood of unimportant work could shed the
                // important work it arrived behind. Shedding load has to drop the *least* important
                // thing, and when the newcomer is that thing, the newcomer is what goes.
                //
                // Equal is allowed, and that is DropOldest doing its job: within one band the
                // freshest value is the one worth having.
                if( haveBand && aPriority >= lowestBand )
                {
                    for( auto it = mEventQueue.begin(); it != mEventQueue.end(); ++it )
                    {
                        if( !it->mEvent || it->mEvent->type() == Event::DeferredDelete
                            || it->mPriority != lowestBand )
                        {
                            continue;
                        }

                        // The first entry of that band, which is its oldest: equal priorities are
                        // kept in posting order by the insert above.
                        delete it->mEvent;
                        mEventQueue.erase( it );
                        ++mDroppedEvents;
                        insertByPriorityLocked( aReceiver, aEvent, aPriority );
                        return true;
                    }
                }

                // Either every entry is a deferred delete, or the newcomer outranks nothing in
                // the queue. Falls through to a refusal rather than making an exception: the
                // alternative is growing past a ceiling the application asked for, or inverting the
                // priority the queue was ordered by.
            }

            ++mDroppedEvents;
            delete aEvent;
            return false;
        }

        insertByPriorityLocked( aReceiver, aEvent, aPriority );
        return true;
    }

    //! Thread-safely posts an event to the dispatcher's queue.
    bool EventDispatcherDefault::postEvent
        (
        Object* aReceiver,       //!< The target object receiving the event.
        Event* aEvent,           //!< The event to be dispatched.
        OverflowPolicy aPolicy,  //!< What to do if the queue is full.
        int aPriority            //!< Higher runs first.
        )
    {
        if( !aReceiver || !aEvent )
        {
            delete aEvent;
            return false;
        }

        bool admitted = false;
        bool report = false;

        {
            std::lock_guard<std::mutex> lock( mMutex );

            // Tested under the same lock the push uses, so close() cannot slip between the two: a
            // post either lands entirely before the close or is refused entirely. Refusing here is
            // what lets deleteLater() fall back to a synchronous delete instead of stranding the
            // object in a queue nothing will drain.
            if( !mAcceptingEvents )
            {
                delete aEvent;
                return false;
            }

            admitted = admitLocked( aReceiver, aEvent, aPolicy, aPriority );

            // Latched so the warning fires on the first drop of a burst rather than on every one:
            // a queue shedding a thousand events a second would otherwise make the log the second
            // thing overwhelming the system. A success clears it, so a later burst is reported
            // afresh.
            if( admitted )
            {
                mOverflowReported = false;
            }
            else if( !mOverflowReported )
            {
                mOverflowReported = true;
                report = true;
            }
        }

        // Logged with mMutex released, for the same reason wakeWaiter() is called there: a sink is
        // application code, and running it under this lock would deadlock the first application
        // whose sink posts.
        if( report )
        {
            qCWarning( gLogDispatcher )
                << "event queue is full and is now dropping events. Capacity:"
                << static_cast<unsigned long long>( eventQueueCapacity() )
                << ", dropped so far:" << droppedEventCount();
        }

        if( !admitted )
        {
            return false;
        }

        wakeWaiter();
        return true;
    }

    //! Appends an event without consulting the capacity. See the declaration.
    bool EventDispatcherDefault::postEventUnconditionally
        (
        Object* aReceiver,  //!< The target object receiving the event.
        Event* aEvent,      //!< The event to be dispatched.
        int aPriority       //!< Rank to keep it at; higher runs first.
        )
    {
        if( !aReceiver || !aEvent )
        {
            delete aEvent;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock( mMutex );
            if( !mAcceptingEvents )
            {
                delete aEvent;
                return false;
            }
            // Through the sorted insert rather than a bare append, so a carried-over event cannot
            // land behind something lower-priority that was queued after it.
            //
            // A parked event keeps the rank it was posted at: ThreadData::ParkedEvent records it,
            // because parking is a delay and not a demotion. A moved one arrives at kNormal,
            // because takeEventsForReceiver() hands back bare Event pointers and the rank is not
            // recoverable there -- see migratePostedEvents(), where the caller passes it.
            insertByPriorityLocked( aReceiver, aEvent, aPriority );
        }
        wakeWaiter();
        return true;
    }

    //! Sets how many events this queue will hold. Thread-safe.
    void EventDispatcherDefault::setEventQueueCapacity
        (
        std::size_t aCapacity  //!< Maximum queued events; 0 for unbounded.
        )
    {
        std::lock_guard<std::mutex> lock( mMutex );
        mCapacity = aCapacity;

        // Deliberately does not trim a queue that is already deeper than the new ceiling. Throwing
        // away events that were already accepted is a worse surprise than refusing new ones, so the
        // queue simply drains back under the limit.
    }

    //! @return the capacity, or 0 if unbounded. Thread-safe.
    std::size_t EventDispatcherDefault::eventQueueCapacity() const
    {
        std::lock_guard<std::mutex> lock( mMutex );
        return mCapacity;
    }

    //! @return how many events are queued right now. Thread-safe.
    std::size_t EventDispatcherDefault::eventQueueDepth() const
    {
        std::lock_guard<std::mutex> lock( mMutex );
        return mEventQueue.size();
    }

    //! @return events refused or evicted since this dispatcher was created. Thread-safe.
    unsigned long long EventDispatcherDefault::droppedEventCount() const
    {
        std::lock_guard<std::mutex> lock( mMutex );
        return mDroppedEvents;
    }

    //! Turns liveness tracking on or off. Thread-safe. See the declaration.
    void EventDispatcherDefault::setHealthTrackingEnabled
        (
        bool aEnabled  //!< True to record what this loop is dispatching.
        )
    {
        mHealthTracking.store( aEnabled, std::memory_order_relaxed );
    }

    //! @return whether liveness tracking is on. Thread-safe.
    bool EventDispatcherDefault::isHealthTrackingEnabled() const
    {
        return mHealthTracking.load( std::memory_order_relaxed );
    }

    //! Records that a dispatch is starting. See the declaration.
    //!
    //! The write half of the seqlock: raise the counter to odd, write, raise it to even. A reader
    //! that catches it odd, or that sees it move across its read, tries again.
    void EventDispatcherDefault::beginDispatch
        (
        const Object* aReceiver,  //!< Receiver; recorded, never dereferenced.
        int aEventType,           //!< Event::Type of what is being dispatched.
        int aTimerId              //!< Timer id, or 0.
        )
    {
        const unsigned seq = mHealthSeq.load( std::memory_order_relaxed );
        mHealthSeq.store( seq + 1, std::memory_order_relaxed );

        // Release here and acquire in the reader, so the record's fields cannot be seen before the
        // counter that says they are being written. Without it a compiler or a processor is free to
        // publish the fields first, and a reader would see a consistent counter over a half-written
        // record -- the exact failure the seqlock exists to prevent.
        std::atomic_thread_fence( std::memory_order_release );

        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();

        mHealth.mDispatching.store( true, std::memory_order_relaxed );
        mHealth.mDispatchStartTicks.store( now, std::memory_order_relaxed );
        mHealth.mReceiver.store( aReceiver, std::memory_order_relaxed );
        mHealth.mEventType.store( aEventType, std::memory_order_relaxed );
        mHealth.mTimerId.store( aTimerId, std::memory_order_relaxed );

        std::atomic_thread_fence( std::memory_order_release );
        mHealthSeq.store( seq + 2, std::memory_order_relaxed );
    }

    //! Records that a dispatch has finished. See the declaration.
    void EventDispatcherDefault::endDispatch
        (
        const LoopHealth& aPrevious  //!< The record as it was before beginDispatch().
        )
    {
        const unsigned seq = mHealthSeq.load( std::memory_order_relaxed );
        mHealthSeq.store( seq + 1, std::memory_order_relaxed );
        std::atomic_thread_fence( std::memory_order_release );

        // The start of the dispatch that just ended, not the time now: recording the end would cost
        // a second clock read per event and nothing needs the difference. What a watchdog tests is
        // mDispatching against mDispatchStart, and this field only answers "when did this loop last
        // do anything".
        mHealth.mLastProgressTicks.store(
            mHealth.mDispatchStartTicks.load( std::memory_order_relaxed ),
            std::memory_order_relaxed );
        mHealth.mDispatchCount.fetch_add( 1, std::memory_order_relaxed );

        // Restored rather than cleared, so a nested processEvents() leaves the dispatch it
        // interrupted reported again on the way out. The innermost is what is running; the outer
        // one not making progress is what says it is stuck.
        mHealth.mDispatching.store( aPrevious.mDispatching, std::memory_order_relaxed );
        mHealth.mDispatchStartTicks.store(
            aPrevious.mDispatching ? aPrevious.mDispatchStart.time_since_epoch().count() : 0,
            std::memory_order_relaxed );
        mHealth.mReceiver.store( aPrevious.mReceiver, std::memory_order_relaxed );
        mHealth.mEventType.store( aPrevious.mEventType, std::memory_order_relaxed );
        mHealth.mTimerId.store( aPrevious.mTimerId, std::memory_order_relaxed );

        std::atomic_thread_fence( std::memory_order_release );
        mHealthSeq.store( seq + 2, std::memory_order_relaxed );
    }

    //! @return what this loop is doing, from one consistent moment. Thread-safe.
    LoopHealth EventDispatcherDefault::health() const
    {
        if( !mHealthTracking.load( std::memory_order_relaxed ) )
        {
            // Nothing has been recorded, so hand back a reading that says so rather than one that
            // looks plausible and is arbitrarily stale.
            return LoopHealth {};
        }

        // The read half of the seqlock. Retry while the counter is odd -- a write is in progress --
        // or while it moved across the copy, which means one landed during it.
        //
        // Unbounded in principle and bounded in practice: the writer holds it odd for the length of
        // five stores, and a reader that loses twice has been extraordinarily unlucky rather than
        // starved. There is no lock to wait on, so a writer is never delayed by this.
        for(;;)
        {
            const unsigned before = mHealthSeq.load( std::memory_order_relaxed );
            if( before & 1u )
            {
                continue;
            }

            std::atomic_thread_fence( std::memory_order_acquire );

            using Clock = std::chrono::steady_clock;

            LoopHealth copy;
            copy.mDispatching = mHealth.mDispatching.load( std::memory_order_relaxed );
            copy.mDispatchStart = Clock::time_point( Clock::duration(
                mHealth.mDispatchStartTicks.load( std::memory_order_relaxed ) ) );
            copy.mLastProgress = Clock::time_point( Clock::duration(
                mHealth.mLastProgressTicks.load( std::memory_order_relaxed ) ) );
            copy.mDispatchCount = mHealth.mDispatchCount.load( std::memory_order_relaxed );
            copy.mReceiver = mHealth.mReceiver.load( std::memory_order_relaxed );
            copy.mEventType = mHealth.mEventType.load( std::memory_order_relaxed );
            copy.mTimerId = mHealth.mTimerId.load( std::memory_order_relaxed );

            std::atomic_thread_fence( std::memory_order_acquire );

            if( mHealthSeq.load( std::memory_order_relaxed ) == before )
            {
                return copy;
            }
        }
    }

    //! Stops this dispatcher accepting further events. One-way; there is no reopen. Thread-safe.
    void EventDispatcherDefault::close()
    {
        std::lock_guard<std::mutex> lock( mMutex );
        mAcceptingEvents = false;
    }

    namespace
    {
        //! Cancels the entries of one published batch that @p aMatches selects.
        template <typename Batch, typename Predicate>
        void cancelBatchEntries
            (
            Batch* aBatch,             //!< The published batch, or nullptr.
            const Predicate& aMatches  //!< True for an entry that should not be dispatched.
            )
        {
            if( !aBatch )
            {
                return;
            }

            for( auto& ep : *aBatch )
            {
                if( ep.mEvent && aMatches( ep ) )
                {
                    // Freed here rather than left for the dispatch loop: clearing the slot is what
                    // tells that loop to skip the entry, so nobody will look at the event again.
                    delete ep.mEvent;
                    ep.mEvent = nullptr;
                }
            }
        }

        //! Moves the entries of one published batch that target @p aReceiver into @p aTaken.
        //!
        //! Hands the event over rather than deleting it, and clears the slot so the dispatch loop
        //! skips it.
        template <typename Batch>
        void takeBatchEntries
            (
            Batch* aBatch,                 //!< The published batch, or nullptr.
            Object* aReceiver,             //!< The receiver whose entries should be taken.
            std::vector<Event*>& aTaken    //!< Collects the events taken.
            )
        {
            if( !aBatch )
            {
                return;
            }

            for( auto& ep : *aBatch )
            {
                if( ep.mEvent && ep.mReceiver == aReceiver )
                {
                    aTaken.push_back( ep.mEvent );
                    ep.mEvent = nullptr;
                }
            }
        }

        //! Applies @p aMatches to every batch of every running pass.
        template <typename Frame, typename Predicate>
        void cancelInEveryFrame
            (
            Frame* aFrames,            //!< Innermost running pass, or nullptr.
            const Predicate& aMatches  //!< True for an entry that should not be dispatched.
            )
        {
            for( Frame* frame = aFrames; frame; frame = frame->mOuter )
            {
                cancelBatchEntries( frame->mEvents, aMatches );
                cancelBatchEntries( frame->mTimers, aMatches );
                cancelBatchEntries( frame->mDeletes, aMatches );
            }
        }
    }

    //! Cancels every published entry targeting @p aReceiver. Callers must hold mMutex.
    void EventDispatcherDefault::cancelPublishedEntriesFor
        (
        Object* aReceiver  //!< The receiver whose entries should be cancelled.
        )
    {
        cancelInEveryFrame( mDispatchFrames,
            [aReceiver]( const EventPair& aEp )
            {
                return aEp.mReceiver == aReceiver;
            } );
    }

    //! Cancels every published TimerEvent carrying @p aTimerId. Callers must hold mMutex.
    void EventDispatcherDefault::cancelPublishedTimerEvents
        (
        int aTimerId  //!< The timer whose pending events should be cancelled.
        )
    {
        cancelInEveryFrame( mDispatchFrames,
            [aTimerId]( const EventPair& aEp )
            {
                return aEp.mEvent->type() == Event::Timer
                       && static_cast<TimerEvent*>( aEp.mEvent )->timerId() == aTimerId;
            } );
    }

    //! Removes @p aFrame from the chain of running passes. Callers must hold mMutex.
    //!
    //! Unlinks that specific frame rather than popping the head, so two threads driving the same
    //! dispatcher cannot corrupt the chain.
    void EventDispatcherDefault::unlinkDispatchFrame
        (
        DispatchFrame* aFrame  //!< The frame to remove.
        )
    {
        for( DispatchFrame** link = &mDispatchFrames; *link; link = &( *link )->mOuter )
        {
            if( *link == aFrame )
            {
                *link = aFrame->mOuter;
                return;
            }
        }
    }

    //! Removes and deletes all pending events for the specified receiver. Thread-safe.
    void EventDispatcherDefault::removeEventsForReceiver
        (
        Object* aReceiver  //!< The target receiver object.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        bool removedTimers = false;
        {
            std::lock_guard<std::mutex> lock( mMutex );

            auto itQueue = std::remove_if( mEventQueue.begin(),
                mEventQueue.end(),
                [aReceiver]( const EventPair& aEp )
                {
                    if( aEp.mReceiver == aReceiver )
                    {
                        delete aEp.mEvent;
                        return true;
                    }
                    return false;
                } );
            mEventQueue.erase( itQueue, mEventQueue.end() );

            // A pass in progress has already taken its work out of the containers above, so an
            // object destroyed from inside a handler would leave its remaining entries in that
            // pass.
            cancelPublishedEntriesFor( aReceiver );

            auto itTimer
                = std::remove_if( mTimers.begin(),
                mTimers.end(),
                [aReceiver]( const TimerData& aTd )
                {
                    return aTd.mReceiver == aReceiver;
                } );
            if( itTimer != mTimers.end() )
            {
                mTimers.erase( itTimer, mTimers.end() );
                rebuildTimerHeapLocked();
                removedTimers = true;
            }
        }

        // With mMutex released, and only when this actually took a timer away. This runs from
        // ~Object(), so it can put a native loop's re-arm inside a destructor -- which is safe,
        // since the lock is gone and the callback's whole job is to re-arm a wait, but it is worth
        // knowing before adding anything heavier to that callback.
        if( removedTimers )
        {
            notifyDeadlineChanged();
        }
    }

    //! Removes the receiver's pending events and hands them over, still alive. Thread-safe.
    //!
    //! Reaches the running passes as well as the queue, so it also works when moveToThread() is
    //! called from inside a handler.
    std::vector<Event*> EventDispatcherDefault::takeEventsForReceiver
        (
        Object* aReceiver  //!< The receiver whose events should be taken.
        )
    {
        std::vector<Event*> taken;
        if( !aReceiver )
        {
            return taken;
        }

        std::lock_guard<std::mutex> lock( mMutex );

        auto itQueue = std::remove_if( mEventQueue.begin(),
            mEventQueue.end(),
            [aReceiver, &taken]( const EventPair& aEp )
            {
                if( aEp.mReceiver == aReceiver && aEp.mEvent )
                {
                    taken.push_back( aEp.mEvent );
                    return true;
                }
                return false;
            } );
        mEventQueue.erase( itQueue, mEventQueue.end() );

        for( DispatchFrame* frame = mDispatchFrames; frame; frame = frame->mOuter )
        {
            takeBatchEntries( frame->mEvents, aReceiver, taken );
            takeBatchEntries( frame->mTimers, aReceiver, taken );
            takeBatchEntries( frame->mDeletes, aReceiver, taken );
        }

        return taken;
    }

    //! Unregisters the receiver's timers and returns them for re-registration elsewhere. Returns
    //! the removed registrations, empty if the receiver had none. Thread-safe.
    std::vector<AbstractEventDispatcher::TimerRegistration> EventDispatcherDefault::
    takeTimersForReceiver
        (
        Object* aReceiver  //!< The receiver whose timers should be taken.
        )
    {
        std::vector<TimerRegistration> taken;
        if( !aReceiver )
        {
            return taken;
        }

        bool removedAny = false;
        {
            std::lock_guard<std::mutex> lock( mMutex );

            auto it = std::remove_if( mTimers.begin(),
                mTimers.end(),
                [aReceiver, &taken]( const TimerData& aTd )
                {
                    if( aTd.mReceiver != aReceiver )
                    {
                        return false;
                    }
                    taken.push_back( { aTd.mTimerId, aTd.mIntervalMs } );
                    return true;
                } );
            if( it != mTimers.end() )
            {
                mTimers.erase( it, mTimers.end() );
                rebuildTimerHeapLocked();

                // The wait deadline was computed from a list that no longer holds these entries.
                mTimersChanged = true;
                removedAny     = true;
            }
        }

        if( removedAny )
        {
            // Woken with mMutex released, matching postEvent(). See wakeWaiter().
            wakeWaiter();
            notifyDeadlineChanged();
        }

        return taken;
    }

    //! Dispatches any pending deferred-delete events, destroying their receivers. Thread-safe.
    //! Intended to run on the dispatcher's own thread.
    void EventDispatcherDefault::processDeferredDeletes()
    {
        // Destroying an object can queue further deferred deletes -- a cleanup callback may
        // deleteLater() something else -- so keep draining until none remain, as Qt does rather
        // than capping the number of passes.
        //
        // Receivers already destroyed in an earlier pass are tracked for the same reason
        // processEvents() tracks them: two deleteLater() calls on one object queue two events, and
        // dispatching the second after the first has destroyed it would be a use-after-free.
        std::unordered_set<Object*> deletedReceivers;

        for(;;)
        {
            std::vector<EventPair> deferredDeletes;
            DispatchFrame frame;
            {
                std::lock_guard<std::mutex> lock( mMutex );
                for( auto it = mEventQueue.begin(); it != mEventQueue.end();)
                {
                    if( it->mEvent && it->mEvent->type() == Event::DeferredDelete )
                    {
                        deferredDeletes.push_back(*it );
                        it = mEventQueue.erase( it );
                    }
                    else
                    {
                        ++it;
                    }
                }

                // Published for the same reason processEvents() publishes its batches: this is
                // work that has left mEventQueue, and destroying one of these objects can destroy
                // another that is also in this batch.
                frame.mDeletes  = &deferredDeletes;
                frame.mOuter    = mDispatchFrames;
                mDispatchFrames = &frame;
            }

            // Unlinks the frame however this iteration ends, including the break below.
            struct FrameRetractor
            {
                ~FrameRetractor()
                {
                    std::lock_guard<std::mutex> lock( mOwner->mMutex );
                    mOwner->unlinkDispatchFrame( mFrame );
                }

                EventDispatcherDefault* mOwner;
                DispatchFrame*          mFrame;
            } frameRetractor { this, &frame };

            if( deferredDeletes.empty() )
            {
                break;
            }

            // Dispatch with mMutex released: ~Object() calls removeEventsForReceiver(), which takes
            // the same non-recursive mutex and would otherwise deadlock.
            for( size_t i = 0; i < deferredDeletes.size(); ++i )
            {
                // Taken under the lock, as in processEvents(): an object destroyed earlier in this
                // batch cancels its own remaining entries, and whoever clears the slot owns it.
                EventPair ep { nullptr, nullptr, EventPriority::kNormal };
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    ep                        = deferredDeletes[i];
                    deferredDeletes[i].mEvent = nullptr;
                }

                if( ep.mReceiver && ep.mEvent && deletedReceivers.insert( ep.mReceiver ).second )
                {
                    Object::dispatchEvent( ep.mReceiver, ep.mEvent );
                }
                delete ep.mEvent;
            }
        }
    }

    //! Wakes up the event loop if waiting. Thread-safe.
    void EventDispatcherDefault::wakeUp()
    {
        // The flag must be set under mMutex, not just notified. processEvents() waits on a
        // predicate, so a bare notify_all() is a no-op unless some state the predicate tests has
        // changed, so a bare notify_all() would be a no-op against a predicate-based wait.
        {
            std::lock_guard<std::mutex> lock( mMutex );
            mWakeUpRequested = true;
        }
        wakeWaiter();
    }

    //! Interrupts processEvents execution. Thread-safe.
    void EventDispatcherDefault::interrupt()
    {
        // Taking mMutex here is what makes the unbounded wait in processEvents() safe. Setting the
        // atomic without the lock leaves a lost-wakeup window: a waiter that has already evaluated
        // its predicate as false, but has not yet atomically released the lock and blocked, would
        // miss both the flag and the notification. That was survivable while the wait was capped at
        // 100ms; with no cap it would hang forever. Blocking on mMutex here means this can only
        // land either fully before the predicate check or after the waiter is genuinely blocked.
        {
            std::lock_guard<std::mutex> lock( mMutex );
            mInterrupt = true;
        }
        wakeWaiter();
    }
}
