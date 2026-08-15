#include "QtLikeSignal/EventDispatcherDefault.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Object.hpp"
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

        // Declared out here so it outlives both batches it will point at, and so the retractor
        // below can name it.
        DispatchFrame frame;

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

            // Publish both batches so unregisterTimer() and removeEventsForReceiver() can cancel
            // entries in them while the handlers below run. Linked in before *any* dispatching,
            // since a queued metacall can kill a timer or destroy an object just as a timer handler
            // can, and while still holding the lock, so there is no window in which a pass owns work
            // that no canceller can see.
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

        // Drain the OS's own event source, with mMutex released so platform code may re-enter this
        // dispatcher (a native handler is free to post an event or start a timer). Done before our
        // own dispatch below so an OS message that arrived during the wait is not held back a full
        // pass behind the queued work it may itself have produced.
        processPlatformEvents();

        bool processedAny = false;

        // Tracks receivers that were deleted via a DeferredDeleteEvent processed earlier in this
        // same batch.
        //
        // Both batches are published above, so ~Object() -> removeEventsForReceiver() normally
        // cancels a destroyed receiver's remaining entries and this set has nothing to add. It stays
        // because that path is affinity-dependent: ~Object() cancels through the dispatcher its
        // *current* affinity names, so an object whose affinity changed after these events were
        // posted cancels somewhere else and leaves ours behind. This set covers the deferred-delete
        // case of that regardless of where the object thinks it lives, and it is one hash lookup.
        std::unordered_set<Object*> deletedReceivers;

        // Dispatch queued events
        for( size_t i = 0; i < eventsToProcess.size(); ++i )
        {
            // Take the entry out of the batch under the lock, clearing our slot as we go, exactly as
            // the timer loop below does. That hands ownership over in one atomic step: either
            // removeEventsForReceiver() got here first and we see nullptr, or we did and it sees
            // nullptr. Neither can free the event twice, and an event whose receiver was destroyed
            // by an earlier handler in this same batch is simply skipped.
            //
            // The extra lock per event is deliberate and cheap -- an uncontended acquire against the
            // several hundred nanoseconds a queued metacall already costs. It is what makes the
            // published batch above worth anything: reading an entry unguarded would race the very
            // cancellation the publication exists to allow.
            EventPair ep { nullptr, nullptr };
            {
                std::lock_guard<std::mutex> lock( mMutex );
                ep                        = eventsToProcess[i];
                eventsToProcess[i].mEvent = nullptr;
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

        // Copy under its own lock, then invoke released. Every caller has already dropped mMutex, so
        // the callback is free to post, start a timer, or otherwise call straight back in.
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

        {
            std::lock_guard<std::mutex> lock( mMutex );
            auto now = std::chrono::steady_clock::now();
            TimerData td;
            td.mTimerId    = aTimerId;
            td.mIntervalMs = aInterval;
            td.mReceiver   = aObject;
            td.mNextFire   = now + std::chrono::milliseconds( aInterval );

            bool replaced = false;
            for( auto& t : mTimers )
            {
                if( t.mTimerId == aTimerId )
                {
                    t        = td;
                    replaced = true;
                    break;
                }
            }
            if( !replaced )
            {
                mTimers.push_back( td );
            }
            mTimersChanged = true;
        }

        // Woken with mMutex released, matching postEvent(). See wakeWaiter().
        wakeWaiter();
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
            mTimersChanged = true;
            return true;
        }
        return false;
    }

    //! Thread-safely posts an event to the dispatcher's queue.
    bool EventDispatcherDefault::postEvent
        (
        Object* aReceiver,  //!< The target object receiving the event.
        Event* aEvent       //!< The event to be dispatched.
        )
    {
        if( !aReceiver || !aEvent )
        {
            delete aEvent;
            return false;
        }

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
            mEventQueue.push_back( { aReceiver, aEvent } );
        }
        wakeWaiter();
        return true;
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
        //!
        //! A template only because the queued-event batch is a deque and the other two are vectors;
        //! there is one behaviour here, not three. A null batch means the pass does not have that
        //! kind of work, which is normal.
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
        //! The event is handed over rather than deleted, and the slot is cleared so the dispatch
        //! loop skips it -- the same ownership handover cancelBatchEntries() performs, minus the
        //! delete.
        template <typename Batch>
        void takeBatchEntries
            (
            Batch* aBatch,                   //!< The published batch, or nullptr.
            Object* aReceiver,               //!< The receiver whose entries should be taken.
            std::vector<Event*>& aTaken      //!< Collects the events taken.
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

    //! Cancels every published entry targeting @p aReceiver. Callers must hold mMutex; see
    //! mDispatchFrames in the header for why that is what makes this safe.
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
    //! Unlinks that specific frame rather than popping the head. Passes on one thread nest strictly,
    //! but two threads driving the same dispatcher would not, and searching costs nothing at these
    //! depths.
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

        // The queue is not the only place this receiver's events can be waiting. A dispatch pass in
        // progress on this thread has already taken its work out of the containers above, so an
        // object destroyed from inside a handler -- the common case, since that is where user code
        // runs -- would leave its remaining entries in that pass and be called again after it was
        // freed. Cancel them where they actually are.
        cancelPublishedEntriesFor( aReceiver );

        auto itTimer
            = std::remove_if( mTimers.begin(),
            mTimers.end(),
            [aReceiver]( const TimerData& aTd )
            {
                return aTd.mReceiver == aReceiver;
            } );
        mTimers.erase( itTimer, mTimers.end() );
    }

    //! Removes the receiver's pending events and hands them over, still alive. Thread-safe.
    //!
    //! Reaches the running passes as well as the queue, using the same publication R28 added: an
    //! entry taken out of a batch is cleared rather than deleted, and the dispatch loop skips a
    //! cleared slot. That is what makes this work when moveToThread() is called from inside a
    //! handler, which is where the object's own thread usually is when it moves itself.
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
                // The wait deadline was computed from a timer list that no longer holds these
                // entries.
                mTimersChanged = true;
                removedAny     = true;
            }
        }

        if( removedAny )
        {
            // Woken with mMutex released, matching postEvent(). See wakeWaiter().
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

                // Published for the same reason processEvents() publishes its two batches: this is
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
                // batch cancels its own remaining entries through removeEventsForReceiver(), and
                // whoever clears the slot first owns the event.
                EventPair ep { nullptr, nullptr };
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    ep                        = deferredDeletes[i];
                    deferredDeletes[i].mEvent = nullptr;
                }

                if( ep.mReceiver && ep.mEvent && deletedReceivers.insert( ep.mReceiver ).second )
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
