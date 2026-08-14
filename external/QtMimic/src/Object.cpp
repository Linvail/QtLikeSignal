//! @file
//!
//! Implementation of QtMimic::Object (thread affinity + connection lifetime
//! management).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Object.hpp"

#include "AbstractEventDispatcher.hpp"
#include "Event.hpp"
#include "Thread.hpp"

#include <climits>
#include <unordered_map>
#include <cstdio>
#include <deque>

namespace QtMimic
{

    //! One pending deferred call: the invoker to run, guarded by its own mutex.
    struct CallLaterNode
    {
        std::mutex mMutex;
        std::function<void()> mInvoker;
    };

    //! Process-wide registry of callLater invocations still waiting to run.
    //!
    //! Exists as a friend of Object purely so it can name Object's private CallLaterKey /
    //! CallLaterKeyHash types; a plain file-scope map could not. Not declared in any header.
    struct CallLaterRegistry
    {
        static std::mutex sMutex;
        static std::unordered_map<Object::CallLaterKey,
            std::shared_ptr<CallLaterNode>,
            Object::CallLaterKeyHash>
        sPending;
    };

    std::mutex CallLaterRegistry::sMutex;
    std::unordered_map<Object::CallLaterKey,
        std::shared_ptr<CallLaterNode>,
        Object::CallLaterKeyHash>
    CallLaterRegistry::sPending;

    namespace
    {
        //! Process-wide pool of timer ids, handing out reusable ids rather than an ever-rising
        //! count. Qt does the same with a lock-free QFreeList capped at 2^24 simultaneous timers; a
        //! mutex and a deque is the proportionate equivalent here, since an id is taken once per
        //! startTimer() rather than on any hot path.
        //!
        //! Reuse is FIFO, deliberately. A freed id going straight back out (LIFO) would make the
        //! narrowest recycling hazard trivially reachable: a handler that kills one timer and
        //! starts another would get the same id back immediately, and any delivery for the old
        //! timer still in flight would then match the new one. Taking the oldest free id instead
        //! means an id is only reused after every other freed id has been.
        struct TimerIdPool
        {
            //! Takes an id, reusing the oldest freed one if there is any.
            //! @return the id, or -1 if the space of ids is exhausted.
            static int allocate()
            {
                std::lock_guard<std::mutex> locker( sMutex );
                if( !sFree.empty() )
                {
                    const int id = sFree.front();
                    sFree.pop_front();
                    return id;
                }
                // Never hand out 0 or a negative value: -1 is startTimer()'s failure sentinel and
                // the value Timer::stop() tests against, so an id colliding with it would be
                // indistinguishable from "no timer".
                if( sNextFresh <= 0 )
                {
                    return -1;
                }
                const int id = sNextFresh;
                sNextFresh = ( sNextFresh == INT_MAX ) ? -1 : sNextFresh + 1;
                return id;
            }

            //! Returns an id to the pool.
            static void release
                (
                int aTimerId
                )
            {
                if( aTimerId <= 0 )
                {
                    return;
                }
                std::lock_guard<std::mutex> locker( sMutex );
                sFree.push_back( aTimerId );
            }

            static std::mutex sMutex;      //!< Guards both members below.
            static std::deque<int> sFree;  //!< Freed ids, oldest first.
            static int sNextFresh;         //!< Next never-yet-issued id.
        };

        std::mutex TimerIdPool::sMutex;
        std::deque<int> TimerIdPool::sFree;
        int TimerIdPool::sNextFresh = 1;
    }  // namespace

    //================================================================
    // Object
    //================================================================

    //! @brief Constructor - creates an Object bound to a thread with thread affinity.
    //! @param aThread The Thread this object lives in. If null (the default), the object
    //!        lives in the current thread (Thread::currentThread()). If no thread is current,
    //!        connections to this object behave as direct connections.
    Object::Object
        (
        Thread* aThread
        )
    // Store the thread's ThreadData, not the Thread itself: it outlives the Thread, so this
    // object's affinity can never become a dangling pointer. currentData() auto-adopts the
    // calling thread exactly as current() does when no thread is given. Held in an Affinity
    // box read at emit time, so a later moveToThread() redirects existing connections too.
        : Object( aThread ? aThread->threadData()
            : ( Thread::currentThread() ? Thread::currentThread()->threadData()
            : std::shared_ptr<ThreadData>() ) )
    {
    }

    //! @brief Constructor for internal helpers that already hold stable ThreadData.
    Object::Object
        (
        std::shared_ptr<ThreadData> aThreadData
        )
        : mLife( std::make_shared<int>( 0 ) )
        , mAffinity( std::make_shared<Affinity>( std::move( aThreadData ) ) )
    {
    }

    //! @brief The thread this object currently lives in, or nullptr if it has none -- or if the
    //! Thread it lived in has since been destroyed. This never returns a dangling pointer: the
    //! affinity is stored as a ThreadData (which outlives its Thread), exactly as Qt stores a
    //! refcounted QThreadData rather than a QThread*. Re-read it rather than caching it.
    Thread* Object::thread() const
    {
        const std::shared_ptr<ThreadData> data = threadData();
        return data ? data->thread() : nullptr;
    }

    //! @brief Re-assign this object's thread affinity, following Qt6's QObject::moveToThread rules.
    //! The move is "push-only": the caller must be on the object's current affinity thread, so an
    //! object can be pushed to another thread but never pulled from an arbitrary one. The single
    //! exception, exactly as in Qt, is that an object with no affinity may be pulled to the calling
    //! thread. Affinity is resolved at emit time, so events posted after a successful move --
    //! including through connections made BEFORE it -- are delivered to @p aThread. Passing nullptr
    //! dissociates the object, after which thread() returns nullptr.
    //! @return true if the object now lives in @p aThread (including when it already did, which is a
    //!         successful no-op); false if the move was refused because the caller is not on the
    //!         object's affinity thread. On a false return the affinity is left unchanged.
    bool Object::moveToThread
        (
        Thread* aThread
        )
    {
        Thread* const currentAffinity = thread();
        if( currentAffinity == aThread )
        {
            return true; // already there -- a successful no-op, exactly as Qt returns true
        }

        // Push-only, with Qt's one exception: a thread-less object may be pulled to the caller.
        Thread* const callingThread = Thread::currentThread();
        const bool pullNoAffinityToCaller = ( currentAffinity == nullptr )
            && ( aThread == callingThread );
        if( !pullNoAffinityToCaller && currentAffinity != callingThread )
        {
            return false;
        }

        // Take any running timers off the outgoing thread before the affinity changes. Qt documents
        // this behaviour ("all active timers for the object will be reset ... stopped in the current
        // thread and restarted, with the same interval, in the targetThread"); without it the timers
        // would keep firing on the thread the object just left, delivering timerEvent() somewhere it
        // must not run. The caller is on that outgoing thread (push-only, checked above), so this
        // cannot race its loop's own delivery pass.
        std::vector<AbstractEventDispatcher::TimerRegistration> timersToMove;
        if( const std::shared_ptr<ThreadData> oldData = threadData() )
        {
            if( auto oldDispatcher = oldData->dispatcher() )
            {
                timersToMove = oldDispatcher->takeTimersForReceiver( this );
            }
        }

        const std::shared_ptr<ThreadData> oldAffinity = mAffinity->data();
        const std::shared_ptr<ThreadData> newAffinity
            = aThread ? aThread->threadData() : std::shared_ptr<ThreadData>();
        mAffinity->setData( newAffinity );

        migratePostedEvents( oldAffinity, newAffinity );

        if( !timersToMove.empty() )
        {
            const std::shared_ptr<ThreadData> newData = threadData();
            if( newData )
            {
                // Re-register on the destination thread rather than from here: registerTimer() must
                // run where the timer will be serviced. Qt solves it the same way, queueing the
                // re-registration with invokeMethod(..., Qt::QueuedConnection) so it lands on the
                // new thread. If this object is destroyed before the queued call runs, ~Object()
                // strips its pending events from the dispatcher, so the call is dropped rather than
                // dangling.
                dispatchMetaCall(
                    this,
                    [this, timersToMove]()
                    {
                        if( auto tData = threadData() )
                        {
                            if( auto disp = tData->dispatcher() )
                            {
                                for( const auto& timer : timersToMove )
                                {
                                    disp->registerTimer( timer.mTimerId, timer.mIntervalMs, this );
                                }
                            }
                        }
                    },
                    ConnectionType::Queued );
            }
            else
            {
                // moveToThread(nullptr) leaves nothing to service the timers, so they are gone
                // rather than merely paused, and their ids go back to the pool. The object's own
                // record has to be cleared too, or a later killTimer() would double-release.
                for( const auto& timer : timersToMove )
                {
                    forgetTimerId( timer.mTimerId );
                    TimerIdPool::release( timer.mTimerId );
                }
            }
        }

        return true;
    }

    //! @brief Called when one of this object's timers comes due. Does nothing by default.
    void Object::timerEvent
        (
        TimerEvent* aEvent
        )
    {
        ( void )aEvent;
    }

    //! @brief Start a repeating timer delivering timerEvent() to this object every @p aIntervalMs.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** The timer is owned by
    //! that thread's mailbox and only that thread's event loop can deliver it, so registering from
    //! elsewhere would install a timer whose events the caller is not positioned to receive.
    //! Rejected with a warning on stderr instead, matching Qt, whose QObject::startTimer() likewise
    //! refuses ("Timers cannot be started from another thread"). To start a timer for an object
    //! living in another thread, get onto that thread first.
    //!
    //! An interval of 0 means "fire on every pass of the event loop", as in Qt.
    //! @return the new timer's id, or -1 if the timer could not be started.
    int Object::startTimer
        (
        int aIntervalMs  //!< Interval in milliseconds; 0 fires on every loop pass.
        )
    {
        if( aIntervalMs < 0 )
        {
            std::fprintf( stderr, "Object::startTimer: interval cannot be negative\n" );
            return -1;
        }

        if( thread() != Thread::currentThread() )
        {
            std::fprintf( stderr,
                "Object::startTimer: timers cannot be started from another thread\n" );
            return -1;
        }

        const std::shared_ptr<ThreadData> data = threadData();
        if( !data )
        {
            // Detached by moveToThread(nullptr): there is no mailbox to schedule against, and no
            // loop that would ever drain one.
            std::fprintf( stderr,
                "Object::startTimer: object has no thread, so the timer cannot be started\n" );
            return -1;
        }

        // Only consume an id once the timer is actually going to be registered.
        const int timerId = TimerIdPool::allocate();
        if( timerId < 0 )
        {
            std::fprintf( stderr, "Object::startTimer: no timer ids left\n" );
            return -1;
        }

        auto dispatcher = data->dispatcher();
        if( !dispatcher )
        {
            std::fprintf( stderr,
                "Object::startTimer: this thread has no event dispatcher, so the timer cannot "
                "be started\n" );
            TimerIdPool::release( timerId );
            return -1;
        }

        dispatcher->registerTimer( timerId, aIntervalMs, this );
        {
            // Recorded so ~Object() can hand the id back even if the timer is never killed.
            std::lock_guard<std::mutex> locker( mRunningTimerIdsMutex );
            mRunningTimerIds.push_back( timerId );
        }
        return timerId;
    }

    //! @brief Stop the timer with id @p aTimerId.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, for the same reason as
    //! startTimer(). Calls from another thread are rejected with a warning and do nothing. An id
    //! this object does not own is ignored.
    void Object::killTimer
        (
        int aTimerId  //!< Id returned by startTimer().
        )
    {
        if( thread() != Thread::currentThread() )
        {
            std::fprintf( stderr,
                "Object::killTimer: timers cannot be stopped from another thread\n" );
            return;
        }

        const bool wasOurs = forgetTimerId( aTimerId );

        if( const std::shared_ptr<ThreadData> data = threadData() )
        {
            if( auto dispatcher = data->dispatcher() )
            {
                dispatcher->unregisterTimer( aTimerId );
            }
        }

        // Released only after unregisterTimer() has both dropped the timer and cancelled anything
        // it had already collected for delivery, so the id cannot be reissued while something still
        // naming it is in flight. Only ids this object actually owns go back, so a bogus or
        // repeated killTimer() cannot inject a duplicate into the pool.
        if( wasOurs )
        {
            TimerIdPool::release( aTimerId );
        }
    }

    //! @brief Drop @p aTimerId from this object's running-timer record.
    //! @return true if the id was there, i.e. this object owns it and the caller may release it.
    bool Object::forgetTimerId
        (
        int aTimerId
        )
    {
        std::lock_guard<std::mutex> locker( mRunningTimerIdsMutex );
        auto it = std::find( mRunningTimerIds.begin(), mRunningTimerIds.end(), aTimerId );
        if( it == mRunningTimerIds.end() )
        {
            return false;
        }
        mRunningTimerIds.erase( it );
        return true;
    }

    //! @brief This object's descriptive name, empty unless one was set.
    //!
    //! **Not thread-safe**, exactly as QObject::objectName() is not; see mObjectName.
    std::string Object::objectName() const
    {
        return mObjectName;
    }

    //! @brief Give this object a descriptive name, for logs and diagnostics. Nothing keys off it.
    //!
    //! **Not thread-safe**; see objectName().
    void Object::setObjectName
        (
        const std::string& aName  //!< The new object name.
        )
    {
        mObjectName = aName;
    }


    //! @brief Queue the object's destruction to its affinity thread.
    //! Qt-like QObject::deleteLater(). If the object has no thread, or its thread has stopped/gone
    //! (post() refuses), deletion happens immediately (a thread-affinity violation is preferable to
    //! a deferred delete that would never run and leak the object).
    void Object::deleteLater()
    {
        // Qt-like behavior: multiple deleteLater() calls coalesce into one.
        if( mDeleteLaterPosted.exchange( true ) )
        {
            return;
        }

        auto* event = new DeferredDeleteEvent();
        if( const std::shared_ptr<ThreadData> tData = threadData() )
        {
            // Queue only if there is a live thread to dispatch it. A destroyed Thread leaves its
            // ThreadData -- and that ThreadData's still-working dispatcher -- behind, so without
            // this check the event is accepted by a queue nothing will ever drain and the object is
            // leaked outright rather than deleted. Falling through to the synchronous delete below
            // is the lesser evil: leaking self forever is strictly worse than the thread-affinity
            // violation of deleting it here, on whichever thread called deleteLater(). Not the
            // normal path -- it only triggers for a thread that has already finished or gone away.
            if( tData->thread() != nullptr )
            {
                if( auto disp = tData->dispatcher() )
                {
                    // A refusal means the dispatcher is closing, so nothing would ever drain this
                    // event; postEvent() has already freed it. Fall through to the synchronous
                    // delete rather than leaking the object.
                    mMayHaveQueuedWork.store( true, std::memory_order_release );
                    if( disp->postEvent( this, static_cast<Event*>( event ) ) )
                    {
                        return;
                    }
                    delete this;
                    return;
                }
            }
        }
        delete event;
        delete this;
    }

    //! @brief Route an event to its handler.
    //!
    //! Deliberately private and non-virtual: this is not an extension point. The event queue is the
    //! sole caller (see the friend declaration in the header), and the set of event types is closed.
    //! Override timerEvent() instead to react to timers.
    //! @return true if the event was recognized and handled.
    bool Object::event
        (
        Event* aEvent  //!< The event to handle.
        )
    {
        if( !aEvent )
        {
            return false;
        }

        switch( aEvent->type() )
        {
        case Event::Timer:
            timerEvent( static_cast<TimerEvent*>( aEvent ) );
            return true;

        case Event::DeferredDelete:
            delete this;
            return true;

        case Event::MetaCall:
            static_cast<MetaCallEvent*>( aEvent )->placeMetaCall();
            return true;

        default:
            // The only events that reach this queue are the three above, all posted by Object's own
            // internals. Nothing can inject an arbitrary event for an arbitrary receiver, so any
            // other type is unreachable rather than something to hand to a user hook.
            return false;
        }
    }

    //! @brief Destructor - disconnects all incoming connections and invalidates the life token.
    //! This prevents any queued (not-yet-run) slot invocations from running after the
    //! object is destroyed.
    Object::~Object()
    {
        // Invalidate first so queued slots that check the token will skip. This
        // also makes each connection's Cleanup destructor a no-op (it sees the life
        // token expired), so disconnect() below won't re-enter mIncomingMutex.
        mLife.reset();

        // Strip this object's timers before it goes away. Unlike a queued slot, a timer carries no
        // life token to check -- the mailbox holds a raw Object* and the loop calls timerEvent() on
        // it directly -- so this is the only thing standing between a still-running timer and a call
        // into freed memory. It cancels anything already collected for delivery, too.
        std::vector<int> outstandingTimerIds;
        {
            std::lock_guard<std::mutex> locker( mRunningTimerIdsMutex );
            outstandingTimerIds.swap( mRunningTimerIds );
        }
        // Strips this object's queued events AND its timers in one step: a timer carries no life
        // token to check -- the dispatcher holds a raw Object* and calls timerEvent() on it
        // directly -- so this is the only thing standing between a still-running timer and a call
        // into freed memory. It cancels anything already collected for delivery, too.
        //
        // Skipped entirely for an object that never received queued work and owns no timer: the
        // strip is O(queue + timers) under the dispatcher's lock, and most objects have nothing
        // there. Qt guards the same call the same way, with `if (d->postedEvents)` in ~QObject().
        if( mMayHaveQueuedWork.load( std::memory_order_acquire ) || !outstandingTimerIds.empty() )
        {
            if( const std::shared_ptr<ThreadData> data = threadData() )
            {
                if( auto dispatcher = data->dispatcher() )
                {
                    dispatcher->removeEventsForReceiver( this );
                }

                // Events moved here before this thread had a dispatcher are in no queue yet, so the
                // strip above cannot see them. See ThreadData::mParkedEvents.
                data->removeParkedEventsFor( this );
            }
        }
        for( const int timerId : outstandingTimerIds )
        {
            TimerIdPool::release( timerId );
        }

        // Drop any callLater() invocations still pending for this object. Their closures check the
        // life token before running, so they were already inert; erasing the entries is what stops
        // the registry growing a dead key for every object that ever scheduled one.
        // Only objects that have actually used callLater() can have entries to drop. The scan is
        // O(every pending callLater in the process) and takes a lock shared by every thread, so
        // running it for the majority that never touched the feature was pure cost.
        if( mUsedCallLater.load( std::memory_order_acquire ) )
        {
            std::lock_guard<std::mutex> locker( CallLaterRegistry::sMutex );
            auto& pending = CallLaterRegistry::sPending;
            for( auto it = pending.begin(); it != pending.end();)
            {
                if( it->first.mContext == this )
                {
                    it = pending.erase( it );
                }
                else
                {
                    ++it;
                }
            }
        }

        // Disconnect every connection where this object is the receiver so the signal no longer
        // references this (soon to be destroyed) object.
        //
        // Swap the handles out and disconnect them with mIncomingMutex released. Holding it across
        // disconnect() would nest our mutex inside the Signal's, the reverse of the order
        // ~Cleanup takes them in (a slot is destroyed with the Signal's mutex held), and there is no
        // reason to invite that inversion when a swap avoids it entirely.
        std::vector<Connection> incoming;
        {
            std::lock_guard<std::mutex> locker( mIncomingMutex );
            incoming.swap( mIncoming );
        }
        for( auto& handle : incoming )
        {
            handle.disconnect();
        }
    }

    //! @return true if @p aData is the calling thread's own data.
    //!
    //! Exists because Object.hpp cannot include Thread.hpp -- Thread derives from Object -- yet the
    //! inline connect machinery there has to make exactly this test at emit time.
    bool Object::isCurrentThread
        (
        const std::shared_ptr<ThreadData>& aData
        )
    {
        return aData == Thread::currentThread()->threadData();
    }

    //! @return this object's thread data, or nullptr if it has no affinity. Thread-safe.
    //!
    //! Private: this is internal plumbing with no QObject equivalent -- Qt's
    //! QObjectPrivate::threadData is likewise not public API. It is the handle through which the
    //! dispatcher is reached, so exposing it hands out the machinery every other access-control
    //! decision in this class exists to protect.
    std::shared_ptr<ThreadData> Object::threadData() const
    {
        return mAffinity->data();
    }

    //! @brief Dispatch a metacall to the target object's event loop, honouring @p aType.
    //! Thread-safe.
    //! @return true if the slot ran (direct) or was queued successfully; false if it could not be
    //!         delivered at all, which happens when the target has no thread affinity or its thread
    //!         has no event dispatcher yet. Callers that track pending state must undo it on false.
    bool Object::dispatchMetaCall
        (
        Object* aTarget,              //!< Target Object.
        std::function<void()> aSlot,  //!< Callback function.
        ConnectionType aType          //!< Connection type.
        )
    {
        if( !aTarget )
        {
            return false;
        }

        ConnectionType activeType = aType;
        if( activeType == ConnectionType::Auto )
        {
            activeType = ( Thread::currentThread() == aTarget->thread() )
                         ? ConnectionType::Direct
                         : ConnectionType::Queued;
        }

        if( activeType == ConnectionType::Queued )
        {
            return dispatchMetaCallTo( aTarget->threadData(), aTarget, std::move( aSlot ) );
        }

        aSlot();
        return true;
    }

    //! @brief Carry this object's already-posted events to the thread it has just moved to.
    //!
    //! Called by moveToThread() **after** the affinity has been swapped, which is what makes it safe
    //! without holding both dispatcher mutexes at once: each queue is only ever touched alone, so
    //! two moves in opposite directions cannot deadlock. Qt needs QOrderedMutexLocker precisely
    //! because it moves the events and the affinity together.
    //!
    //! Safe for a reason specific to this function: moveToThread() runs on the object's own thread,
    //! so the old thread is inside this call and cannot be dispatching the events being taken.
    //!
    //! Without this, a queued call posted just before the move runs on the thread the object has
    //! left, silently -- nothing re-checks affinity once an event is queued. Qt migrates them in
    //! QObjectPrivate::setThreadData_helper().
    void Object::migratePostedEvents
        (
        const std::shared_ptr<ThreadData>& aOldData,  //!< Thread being left; may be null.
        const std::shared_ptr<ThreadData>& aNewData   //!< Thread now lived on; may be null.
        )
    {
        if( aOldData == aNewData )
        {
            return;
        }

        const std::shared_ptr<AbstractEventDispatcher> oldDispatcher
            = aOldData ? aOldData->dispatcher() : nullptr;
        if( !oldDispatcher )
        {
            return;
        }

        // Swept more than once: a thread that resolved this object's affinity before the swap can
        // still be inside postEvent() on the old dispatcher. Every such poster is already in flight,
        // so the set drains; the cap bounds a caller that never stops posting to a moving object.
        constexpr int kMaxSweeps = 8;
        for( int sweep = 0; sweep < kMaxSweeps; ++sweep )
        {
            std::vector<Event*> taken = oldDispatcher->takeEventsForReceiver( this );
            if( taken.empty() )
            {
                break;
            }

            for( Event* event : taken )
            {
                if( !aNewData )
                {
                    // moveToThread(nullptr) means this object stops processing events, so there is
                    // no later at which these could run.
                    delete event;
                    continue;
                }

                // Asked in one step, so the destination cannot gain or lose its dispatcher between
                // the question and the answer. A null return means the event is parked and now
                // belongs to the destination's ThreadData.
                const std::shared_ptr<AbstractEventDispatcher> newDispatcher
                    = aNewData->dispatcherOrPark( this, event );
                if( newDispatcher )
                {
                    // postEvent() deletes the event itself when it refuses.
                    newDispatcher->postEvent( this, event );
                }
            }
        }
    }

    //! @brief Dispatch a metacall to an explicitly named thread, ignoring the receiver's affinity.
    //!
    //! The entry point for a caller that knows which thread it means rather than inferring it from
    //! an Object. Thread::post() needs exactly that: it targets the thread's *own* queue, which is
    //! not the same as the queue the Thread object happens to live in -- a Thread is constructed on
    //! one thread and then runs on another, so routing post() through its Object affinity would
    //! deliver to whoever created it until its loop started and re-pointed the affinity at itself.
    //!
    //! Thread-safe. @p aReceiver is not dereferenced *by this function*: it is handed to postEvent()
    //! purely as the key that removeEventsForReceiver() later matches on. It is dereferenced
    //! afterwards, when the dispatcher drains the queue and calls aReceiver->event(), so the
    //! receiver has to still be alive at that point. What guarantees that is ~Object(), which calls
    //! removeEventsForReceiver() and deletes every event still queued for the object before it goes
    //! away.
    //! @return true if the call was queued; false if @p aData is null or its thread has no
    //!         dispatcher, in which case the call is dropped.
    bool Object::dispatchMetaCallTo
        (
        const std::shared_ptr<ThreadData>& aData,  //!< Thread to deliver on; null means nowhere.
        Object* aReceiver,                          //!< Receiver; the queue key here.
        std::function<void()> aSlot                 //!< Callback function.
        )
    {
        if( auto disp = aData ? aData->dispatcher() : nullptr )
        {
            // Moved, not copied: aSlot is a by-value parameter and is dead after this line, and
            // MetaCallEvent's constructor also takes by value and moves, so copying here would buy a
            // second heap allocation on every queued emit for nothing.
            auto* event = new MetaCallEvent( std::move( aSlot ) );
            if( aReceiver )
            {
                aReceiver->mMayHaveQueuedWork.store( true, std::memory_order_release );
            }
            return disp->postEvent( aReceiver, static_cast<Event*>( event ) );
        }
        return false;
    }

    //! @brief Schedule or update a callLater deferred invocation.
    void Object::scheduleCallLater
        (
        Object* aContext,               //!< Target context object.
        const CallLaterKey& aKey,       //!< Key identifying the deferred call.
        std::function<void()> aInvoker  //!< Callback executing the call.
        )
    {
        if( !aContext )
        {
            return;
        }

        // Marked before the entry exists, so ~Object() can never see the entry without the flag.
        aContext->mUsedCallLater.store( true, std::memory_order_release );

        std::shared_ptr<CallLaterNode> node;
        bool isNew = false;

        {
            std::lock_guard<std::mutex> locker( CallLaterRegistry::sMutex );
            auto& pending = CallLaterRegistry::sPending;
            auto it = pending.find( aKey );
            if( it != pending.end() )
            {
                node = it->second;
            }
            else
            {
                node = std::make_shared<CallLaterNode>();
                pending[aKey] = node;
                isNew = true;
            }
        }

        {
            std::lock_guard<std::mutex> nodeLock( node->mMutex );
            node->mInvoker = std::move( aInvoker );
        }

        if( isNew )
        {
            std::weak_ptr<int> weakLife = aContext->objectLife();
            auto metaCall = [aKey, node, weakLife]()
                {
                    std::function<void()> fnToRun;
                    {
                        std::lock_guard<std::mutex> locker( CallLaterRegistry::sMutex );
                        CallLaterRegistry::sPending.erase( aKey );
                    }
                    {
                        std::lock_guard<std::mutex> nodeLock( node->mMutex );
                        fnToRun = std::move( node->mInvoker );
                    }
                    if( fnToRun )
                    {
                        // expired(), not lock(): see objectLife(). Equally safe, and a plain load
                        // rather than an atomic read-modify-write.
                        if( !weakLife.expired() )
                        {
                            fnToRun();
                        }
                    }
                };

            if( !dispatchMetaCall( aContext, metaCall, ConnectionType::Queued ) )
            {
                // The target has no dispatcher yet, so this call can never run. Drop the registry
                // entry we just created: leaving it behind would make the failure permanent, since
                // every later callLater() for the same target would find it, take the "already
                // scheduled" branch above, and never dispatch again -- silently disabling that
                // (context, slot) pair for the rest of the object's life, even once a dispatcher
                // existed. Erasing lets the next call re-arm. This call is still lost; only a retry
                // queue could save it, which would need its own ownership rules.
                std::lock_guard<std::mutex> locker( CallLaterRegistry::sMutex );
                CallLaterRegistry::sPending.erase( aKey );
            }
        }
    }

    //================================================================
    // Object::Cleanup
    //================================================================


    //! Destructor - prune this connection from the receiver's mIncoming.
    Object::Cleanup::~Cleanup()
    {
        if( mLife.expired() )
        {
            return; // receiver gone (or being destroyed) - it clears mIncoming.
        }
        std::lock_guard<std::mutex> locker( mOwner->mIncomingMutex );
        auto& v = mOwner->mIncoming;
        v.erase( std::remove( v.begin(), v.end(), mHandle ), v.end() );
    }

} // namespace QtMimic
