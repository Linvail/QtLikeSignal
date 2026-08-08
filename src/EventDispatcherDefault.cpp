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

        std::vector<EventPair>    eventsToProcess;
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
                    t.mNextFire = now + std::chrono::milliseconds( t.mIntervalMs );
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

                auto wakeCondition = [this]
                    {
                        return !mEventQueue.empty() || mInterrupt || mTimersChanged
                               || mWakeUpRequested;
                    };

                if( mTimers.empty() )
                {
                    // Nothing is scheduled, so there is no deadline to poll for: block until
                    // something actually happens rather than waking ten times a second forever.
                    // Every state this predicate tests is changed under mMutex by a caller that
                    // then notifies (postEvent, registerTimer, unregisterTimer, interrupt, wakeUp),
                    // so there is no wakeup to miss.
                    mCv.wait( lock, wakeCondition );
                }
                else
                {
                    mCv.wait_for( lock, maxWait, wakeCondition );
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
                // above; they were never handed off to the dispatch loop below, so free them here to
                // avoid leaking them.
                for( auto& ep : timerEventsToProcess )
                {
                    delete ep.mEvent;
                }
                return false;
            }

            // Drain current queued events
            while( !mEventQueue.empty() )
            {
                eventsToProcess.push_back( mEventQueue.front() );
                mEventQueue.pop_front();
            }
        }

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
        for( const auto& ep : timerEventsToProcess )
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

            ep.mReceiver->event( ep.mEvent );
            delete ep.mEvent;
            processedAny = true;
        }

        return processedAny;
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
                mCv.notify_all();
                return;
            }
        }
        mTimers.push_back( td );
        mTimersChanged = true;
        mCv.notify_all();
    }

    //! Unregisters a timer by ID. Returns true if timer was found and removed, false otherwise.
    //! Thread-safe.
    bool EventDispatcherDefault::unregisterTimer
        (
        int aTimerId  //!< Unique timer identifier.
        )
    {
        std::lock_guard<std::mutex> lock( mMutex );
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
            mCv.notify_all();
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
        mCv.notify_all();
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
            mCv.notify_all();
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
        mCv.notify_all();
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
        mCv.notify_all();
    }
}
