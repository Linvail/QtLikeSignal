#include "EventDispatcherDefault.h"
#include "Event.h"
#include "Object.h"
#include <algorithm>
#include <unordered_set>

namespace QtLikeSignal
{
    //! Constructs a new default event dispatcher.
    EventDispatcherDefault::EventDispatcherDefault() = default;

    //! Destroys the default event dispatcher and frees pending events.
    EventDispatcherDefault::~EventDispatcherDefault()
    {
        std::lock_guard<std::mutex> lock( mMutex );
        while( !mEventQueue.empty() )
        {
            delete mEventQueue.front().mEvent;
            mEventQueue.pop_front();
        }
    }

    //! Processes pending events and expired timers once without an internal infinite loop. Returns
    //! true if any events or timers were processed, false otherwise.
    //!
    //! Thread-safe. Called by thread event loop.
    bool EventDispatcherDefault::processEvents()
    {
        // Consume the interrupt rather than merely testing it. interrupt() means "return from the
        // pass that is running now", not "refuse to work ever again" -- but the flag used to latch
        // true forever, since nothing anywhere cleared it. Every later call then returned instantly,
        // so a loop driving this dispatcher (CoreApplication::exec() after a quit(), which reuses the
        // same dispatcher instead of building a fresh one the way a restarted Thread does) spun at
        // 100% CPU until something else stopped it. Qt consumes it in exactly the same place and for
        // the same reason: `const bool wasInterrupted = d->interrupt.fetchAndStoreRelaxed(false);`
        // at the top of QEventDispatcherWin32::processEvents().
        if( mInterrupt.exchange( false ) )
        {
            return false;
        }

        // A deque, matching mEventQueue, so the whole batch can be taken with a swap below rather
        // than copied out entry by entry.
        std::deque<EventPair>     eventsToProcess;
        std::vector<EventPair>    timerEventsToProcess;
        std::chrono::milliseconds maxWait { 100 };

        {
            std::unique_lock<std::mutex> lock( mMutex );

            auto now = std::chrono::steady_clock::now();

            // Collect expired timers
            for( auto& t : mTimers )
            {
                if( now >= t.mNextFire )
                {
                    timerEventsToProcess.push_back( { t.mReceiver, new TimerEvent( t.mTimerId ) } )
                    ;

                    // Advance from the deadline that just elapsed, not from the moment we noticed
                    // it. `now` is whenever this pass got around to looking, so re-arming from it
                    // folds that lateness into the cadence permanently -- one pass 20 ms late and
                    // every fire thereafter is 20 ms off, with the error compounding under load.
                    t.mNextFire += std::chrono::milliseconds( t.mIntervalMs );

                    if( t.mNextFire < now )
                    {
                        // The loop was blocked for longer than a whole interval, so keeping the
                        // original cadence would mean firing repeatedly to work off a backlog
                        // nobody asked for. Give up on it and resynchronise. Qt makes the same two
                        // choices in the same order (calculateNextTimeout(), qtimerinfo_unix.cpp).
                        t.mNextFire = now + std::chrono::milliseconds( t.mIntervalMs );
                    }
                }
            }

            // Determine wait time for next timer if no events present
            if( mEventQueue.empty() && timerEventsToProcess.empty() )
            {
                if( !mTimers.empty() )
                {
                    auto minFire = mTimers.front().mNextFire;
                    for( const auto& t : mTimers )
                    {
                        if( t.mNextFire < minFire )
                        {
                            minFire = t.mNextFire;
                        }
                    }
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
                const int timeoutMs = mTimers.empty()
                                      ? -1
                                      : static_cast<int>( maxWait.count() );
                waitForEvents( lock, timeoutMs );

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
                // above; they were never handed off to the dispatch loop below, so free them here to
                // avoid leaking them.
                for( auto& ep : timerEventsToProcess )
                {
                    delete ep.mEvent;
                }
                return false;
            }

            // Take the whole queue in one move rather than copying it out entry by entry. The old
            // loop cost a copy per event plus the growth reallocations of the destination, all of
            // it under mMutex and therefore in the way of every thread trying to post. Qt walks its
            // postEventList in place and QtMimic swaps its deque; this is the same idea. mEventQueue
            // is left empty, which is exactly what the drain loop left behind too.
            eventsToProcess.swap( mEventQueue );

            // Publish the batch so unregisterTimer() can cancel entries in it while the handlers
            // below run. Set before *any* dispatching, since a queued metacall can kill a timer just
            // as a timer handler can.
            mDispatchingTimerBatch = &timerEventsToProcess;
        }

        // Retracts the published batch however this function leaves, so a pointer to a dead local
        // can never outlive the pass.
        struct BatchRetractor
        {
            ~BatchRetractor()
            {
                std::lock_guard<std::mutex> lock( mOwner->mMutex );
                mOwner->mDispatchingTimerBatch = nullptr;
            }

            EventDispatcherDefault* mOwner;
        } batchRetractor { this };

        // Drain the OS's own event source, with mMutex released so platform code may re-enter this
        // dispatcher (a native handler is free to post an event or start a timer). Done before our
        // own dispatch below so an OS message that arrived during the wait is not held back a full
        // pass behind the queued work it may itself have produced.
        processPlatformEvents();

        bool processedAny = false;

        // Tracks receivers that were deleted via a DeferredDeleteEvent processed earlier in this
        // same batch. Both eventsToProcess and timerEventsToProcess are snapshots drained/collected
        // before dispatch begins, so removeEventsForReceiver() (called from ~Object()) cannot strip
        // a receiver's remaining entries out of these local vectors -- without this guard, a later
        // entry for the same (now-deleted) receiver would be a use-after-free.
        std::unordered_set<Object*> deletedReceivers;

        // Dispatch queued events
        for( const auto& ep : eventsToProcess )
        {
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
            ep.mReceiver->event( ep.mEvent );
            if( isDeferredDelete )
            {
                deletedReceivers.insert( ep.mReceiver );
            }
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
            EventPair ep { nullptr, nullptr };
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

            ep.mReceiver->event( ep.mEvent );
            delete ep.mEvent;
            processedAny = true;
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

        // Copy under its own lock, then invoke released: this is reached both with and without
        // mMutex held, and the callback is user code that must not be run while holding a lock we
        // might not even own.
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock( mWakeCallbackMutex );
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
        std::lock_guard<std::mutex> lock( mWakeCallbackMutex );
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

        std::lock_guard<std::mutex> lock( mMutex );
        auto now = std::chrono::steady_clock::now();
        TimerData td;
        td.mTimerId    = aTimerId;
        td.mIntervalMs = aInterval;
        td.mReceiver   = aObject;
        td.mNextFire   = now + std::chrono::milliseconds( aInterval );

        for( auto& t : mTimers )
        {
            if( t.mTimerId == aTimerId )
            {
                t             = td;
                mTimersChanged = true;
                wakeWaiter();
                return;
            }
        }
        mTimers.push_back( td );
        mTimersChanged = true;
        wakeWaiter();
    }

    //! Unregisters a timer by ID. Returns true if timer was found and removed, false otherwise.
    //! Thread-safe.
    bool EventDispatcherDefault::unregisterTimer
        (
        int aTimerId  //!< Unique timer identifier.
        )
    {
        std::lock_guard<std::mutex> lock( mMutex );

        // Drop any TimerEvent for this timer that has already been queued but not yet delivered.
        //
        // This became necessary when timer ids started being recycled. Previously a stale event was
        // harmless: ids only ever climbed, so its id could never match a live timer again and
        // Timer::timerEvent()'s id check discarded it. Now that killTimer() returns the id to a pool,
        // a later startTimer() can be handed the same one -- and the stale event would then match the
        // *new* timer and fire it spuriously. Purging here is what keeps recycling from trading an
        // unreachable counter overflow for a reachable wrong-behaviour bug.
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

        if( mDispatchingTimerBatch )
        {
            for( auto& ep : *mDispatchingTimerBatch )
            {
                if( ep.mEvent && ep.mEvent->type() == Event::Timer
                    && static_cast<TimerEvent*>( ep.mEvent )->timerId() == aTimerId )
                {
                    delete ep.mEvent;
                    ep.mEvent = nullptr;
                }
            }
        }

        auto it = std::remove_if( mTimers.begin(),
            mTimers.end(),
            [aTimerId]( const TimerData& aTd )
            {
                return aTd.mTimerId == aTimerId;
            } );
        if( it != mTimers.end() )
        {
            mTimers.erase( it, mTimers.end() );
            mTimersChanged = true;
            wakeWaiter();
            return true;
        }
        return false;
    }

    //! Thread-safely posts an event to the dispatcher's queue.
    void EventDispatcherDefault::postEvent
        (
        Object* aReceiver,  //!< The target object receiving the event.
        Event* aEvent       //!< The event to be dispatched.
        )
    {
        if( !aReceiver || !aEvent )
        {
            delete aEvent;
            return;
        }

        {
            std::lock_guard<std::mutex> lock( mMutex );
            mEventQueue.push_back( { aReceiver, aEvent } );
        }
        wakeWaiter();
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

        auto itTimer
            = std::remove_if( mTimers.begin(),
            mTimers.end(),
            [aReceiver]( const TimerData& aTd )
            {
                return aTd.mReceiver == aReceiver;
            } );
        mTimers.erase( itTimer, mTimers.end() );
    }

    //! Unregisters the receiver's timers and returns them for re-registration elsewhere. Returns
    //! the removed registrations, empty if the receiver had none. Thread-safe.
    std::vector<AbstractEventDispatcher::TimerRegistration>EventDispatcherDefault::
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
            // The wait deadline was computed from a timer list that no longer holds these entries.
            mTimersChanged = true;
            wakeWaiter();
        }

        return taken;
    }

    //! Dispatches any pending deferred-delete events, destroying their receivers. Thread-safe.
    //! Intended to run on the dispatcher's own thread.
    void EventDispatcherDefault::processDeferredDeletes()
    {
        // Destroying an object can queue further deferred deletes -- a cleanup callback may
        // deleteLater() something else -- so keep draining until none remain, as Qt does rather than
        // capping the number of passes.
        //
        // Receivers already destroyed in an earlier pass are tracked for the same reason
        // processEvents() tracks them: two deleteLater() calls on one object queue two events, and
        // dispatching the second after the first has destroyed it would be a use-after-free.
        std::unordered_set<Object*> deletedReceivers;

        for(;;)
        {
            std::vector<EventPair> deferredDeletes;
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
            }

            if( deferredDeletes.empty() )
            {
                break;
            }

            // Dispatch with mMutex released: ~Object() calls removeEventsForReceiver(), which takes
            // the same non-recursive mutex and would otherwise deadlock.
            for( const auto& ep : deferredDeletes )
            {
                if( ep.mReceiver && deletedReceivers.insert( ep.mReceiver ).second )
                {
                    ep.mReceiver->event( ep.mEvent );
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
        // changed -- previously wakeUp() only ever "worked" because the wait was capped at 100ms and
        // would have returned on its own anyway.
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
