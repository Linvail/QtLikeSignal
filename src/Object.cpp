#include "Object.h"

#include "AbstractEventDispatcher.h"
#include "Event.h"
#include "Thread.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <unordered_map>

namespace QtLikeSignal
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

    //! Process-wide pool of timer ids, handing out reusable ids rather than an ever-rising count.
    //!
    //! Qt does the same with a lock-free QFreeList capped at 2^24 simultaneous timers; a mutex and a
    //! deque is the proportionate equivalent here, since allocation happens once per startTimer()
    //! rather than on any hot path.
    //!
    //! Reuse is **FIFO, deliberately**. A freed id going straight back out (LIFO) would make the
    //! narrowest recycling hazard trivially reachable: a handler that kills one timer and starts
    //! another would get the same id back immediately, and any TimerEvent for the old timer still
    //! in flight would then match the new one. Taking the oldest free id instead means an id is only
    //! reused after every other freed id has been, which is what makes that window practically
    //! unreachable rather than merely unlikely.
    struct TimerIdPool
    {
        //! Takes an id, reusing the oldest freed one if there is any.
        static int allocate()
        {
            std::lock_guard<std::mutex> lock( sMutex );
            if( !sFree.empty() )
            {
                const int id = sFree.front();
                sFree.pop_front();
                return id;
            }
            // Never hand out 0 or a negative value: -1 is startTimer()'s failure sentinel and the
            // value Timer::stop() tests against, so an id colliding with it would be indisguishable
            // from "no timer". The old monotonic counter would eventually have wrapped onto exactly
            // that.
            if( sNextFresh <= 0 )
            {
                return -1;
            }
            return sNextFresh++;
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
            std::lock_guard<std::mutex> lock( sMutex );
            sFree.push_back( aTimerId );
        }

        static std::mutex sMutex;        //!< Guards both members below.
        static std::deque<int> sFree;    //!< Freed ids, oldest first.
        static int sNextFresh;           //!< Next never-yet-issued id.
    };

    std::mutex TimerIdPool::sMutex;
    std::deque<int> TimerIdPool::sFree;
    int TimerIdPool::sNextFresh = 1;

    //! Constructs an object living in the given thread, or in the calling thread if none is given.
    Object::Object
        (
        Thread* aThread  //!< Thread this object lives in; null means the calling thread.
        )
        : mLife( std::make_shared<int>( 0 ) )
        // Store the thread's ThreadData, not the Thread itself: it outlives the Thread, so this
        // object's affinity can never become a dangling pointer. Held in an Affinity box read at
        // emit time, so a later moveToThread() redirects existing connections too.
        , mAffinity( std::make_shared<Affinity>(
            aThread ? aThread->threadData()
            : ( Thread::currentThread() ? Thread::currentThread()->threadData() : nullptr ) ) )
    {
    }

    //! Destroys the object and triggers all registered cleanup callbacks.
    Object::~Object()
    {
        // Qt does not guarantee this is safe either: deleting a QObject directly from a thread
        // other than the one it lives in, while that thread's own event loop may still be
        // dispatching to it, is documented in Qt's own source as a malformed program ("QObject:
        // shared QObject was deleted directly. The program is malformed and may crash." --
        // qobject.cpp). We make the same contract explicit instead of trying to engineer around
        // it: destruction is only safe from this object's own thread, or via deleteLater() (which
        // defers the actual delete onto that thread, where it is safe by construction). This is a
        // diagnostic only; it changes no behavior below.
        //
        // Gated on the owning thread still running, not just "a different thread": destroying an
        // object from another thread AFTER its affinity thread's loop has already stopped (the
        // ordinary "workerThread.quit(); workerThread.wait();" teardown idiom used all over the
        // test suite, where a moved-to object is then destroyed by the test's own thread) is
        // completely safe -- there is no loop left to race. That also naturally covers a Thread
        // destroying itself (it self-adopts via moveToThread(this)): ~Thread() calls quit()+wait()
        // before this base destructor runs, so the flag is already clear by the time we get here
        // regardless of which thread ends up calling delete on it.
        //
        // Everything here is read through the ThreadData, which this scope keeps alive, and the
        // Thread* is only ever *compared*, never dereferenced. Asking the Thread itself
        // (owner->isRunning()) would have reintroduced exactly the dangling-pointer hazard the
        // Affinity indirection exists to remove: thread() can hand back a pointer that a
        // concurrent ~Thread() frees before the call lands.
        //
        // Asked with currentThreadOrNull(), never currentThread(): the latter adopts the calling
        // thread when it has no Thread yet, and this runs during thread_local teardown -- an
        // adopted Thread being destroyed as its native thread exits -- where adopting re-enters
        // the unique_ptr already being destroyed. A diagnostic must not allocate. A null answer
        // means the caller is not registered, in which case there is nothing to compare and
        // nothing to warn about.
        const std::shared_ptr<ThreadData> ownerData = mAffinity->data();
        Thread* const callerThread = Thread::currentThreadOrNull();
        if( ownerData && callerThread && ownerData->isThreadRunning()
            && ownerData->thread() != callerThread )
        {
            std::fprintf( stderr,
                "Object::~Object: object destroyed from a thread other than the one it lives "
                "in while that thread's event loop is still running; this is not safe. Use "
                "deleteLater() to destroy an object from another thread.\n" );
        }

        // Invalidate the life token first. connect()/callLater() wrappers running on other threads
        // check objectLife().lock() before posting a call to this object; resetting mLife up front
        // shrinks the window in which such a wrapper can still observe this object as "alive" to
        // the check-then-post race itself, instead of the whole destructor body (which below runs
        // arbitrary user cleanup-callback code).
        //
        // Doing it before the disconnect loop below is also what keeps that loop re-entrancy-free:
        // disconnect() destroys the slot, which destroys its captured Cleanup, whose destructor
        // checks this very token and bails out rather than trying to prune mIncoming while we are
        // already tearing it down.
        mLife.reset();

        // Disconnect every connection where this object is the receiver, so the sender stops
        // holding a slot that can never do anything again. The life token above already makes such
        // a slot inert, but inert is not gone: it keeps its captured state and is still walked on
        // every emit, so a long-lived signal accumulates dead slots without bound. Qt does the
        // same thing by walking cd->senders in ~QObject().
        //
        // Swap the handles out and disconnect them with mIncomingMutex released. Holding it across
        // disconnect() would nest our mutex inside boost's signal mutex, the reverse of the order
        // ~Cleanup takes them in (boost destroys slots with its signal mutex held), and there is no
        // reason to invite that inversion when a swap avoids it entirely.
        std::vector<Connection> incoming;
        {
            std::lock_guard<std::mutex> lock( mIncomingMutex );
            incoming.swap( mIncoming );
        }
        for( auto& handle : incoming )
        {
            handle.disconnect();
        }

        {
            std::lock_guard<std::mutex> lock( CallLaterRegistry::sMutex );
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

        std::shared_ptr<ThreadData> threadDataCopy = mAffinity->data();
        if( threadDataCopy )
        {
            if( auto dispatcher = threadDataCopy->dispatcher() )
            {
                dispatcher->removeEventsForReceiver( this );
            }
        }

        // Hand back any ids whose timers were still running. removeEventsForReceiver() above has
        // already dropped this object's timers and its queued events, so nothing can still be
        // referring to them by the time they are reissued.
        std::vector<int> outstandingTimerIds;
        {
            std::lock_guard<std::mutex> lock( mRunningTimerIdsMutex );
            outstandingTimerIds.swap( mRunningTimerIds );
        }
        for( const int timerId : outstandingTimerIds )
        {
            TimerIdPool::release( timerId );
        }
    }

    //! Removes this connection from its receiver's mIncoming as the connection is destroyed.
    //!
    //! Runs when boost destroys the slot holding this token -- a manual disconnect(), the sender
    //! Signal being destroyed, or ~Object()'s own disconnect loop. Without it mIncoming would only
    //! ever grow for an object that outlives connections made to it, and would eventually hold
    //! stale handles.
    Object::Cleanup::~Cleanup()
    {
        // The receiver is gone or already being destroyed; ~Object() has taken mIncoming over and
        // touching mOwner here would be a use-after-free. This is also what makes ~Object()'s own
        // disconnect loop non-re-entrant, since it resets the token before disconnecting.
        if( mLife.expired() )
        {
            return;
        }

        std::lock_guard<std::mutex> lock( mOwner->mIncomingMutex );
        auto& handles = mOwner->mIncoming;
        handles.erase( std::remove( handles.begin(), handles.end(), mHandle ), handles.end() );
    }

    //! Internal helper to schedule or update a callLater deferred invocation.
    void Object::scheduleCallLater
        (
        Object* aContext,               //!< Target context object.
        const CallLaterKey& aKey,       //!< Key identifying the deferred call.
        std::function<void()> aInvoker   //!< Callback executing the call.
        )
    {
        if( !aContext )
        {
            return;
        }

        std::shared_ptr<CallLaterNode> node;
        bool isNew = false;

        {
            std::lock_guard<std::mutex> lock( CallLaterRegistry::sMutex );
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
                        std::lock_guard<std::mutex> lock( CallLaterRegistry::sMutex );
                        CallLaterRegistry::sPending.erase( aKey );
                    }
                    {
                        std::lock_guard<std::mutex> nodeLock( node->mMutex );
                        fnToRun = std::move( node->mInvoker );
                    }
                    if( fnToRun )
                    {
                        // expired(), not lock(): see objectLife(). Equally safe, and a plain
                        // load rather than an atomic read-modify-write.
                        if( !weakLife.expired() )
                        {
                            fnToRun();
                        }
                    }
                };

            if( !dispatchMetaCall( aContext, metaCall, ConnectionType::Queued ) )
            {
                // The target has no dispatcher yet, so this call can never run. Drop the registry
                // entry we just created: leaving it behind is what made this failure permanent, since
                // every later callLater() for the same target would find it, take the "already
                // scheduled" branch above, and never dispatch again -- silently disabling that
                // (context, slot) pair for the rest of the object's life, even once a dispatcher
                // existed. Erasing lets the next call re-arm. This call is still lost; only a
                // retry queue could save it, which would need its own ownership rules.
                std::lock_guard<std::mutex> lock( CallLaterRegistry::sMutex );
                CallLaterRegistry::sPending.erase( aKey );
            }
        }
    }


    //! Return true if the specified aData belongs to the calling thread.
    //! We need this helper function because we cannot include Thread.h in Object.h.
    bool Object::isCurrentThread
        (
        const std::shared_ptr<ThreadData>& aData
        )
    {
        return aData == Thread::currentThread()->threadData();
    }

    //! Gets the thread affinity of this object. Thread-safe. Never returns a dangling pointer: the
    //! affinity is stored as a ThreadData (which outlives its Thread), so this reports nullptr once
    //! the Thread it lived in has been destroyed rather than a stale Thread*.
    Thread* Object::thread() const
    {
        const std::shared_ptr<ThreadData> data = mAffinity->data();
        return data ? data->thread() : nullptr;
    }

    //! Gets the thread data container holding this object's event dispatcher.
    //!
    //! Private: this is internal plumbing with no QObject equivalent -- Qt's
    //! QObjectPrivate::threadData is likewise not public API. It is the handle through which the
    //! dispatcher is reached, so exposing it hands out the machinery every other access-control
    //! decision in this class exists to protect. Returns nullptr if this object has no affinity.
    //! Thread-safe.
    std::shared_ptr<ThreadData> Object::threadData() const
    {
        return mAffinity->data();
    }

    //! Changes the thread affinity of this object.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, matching Qt's
    //! QObject::moveToThread() ("Current thread is not the object's thread. Cannot move to target
    //! thread"). Only the thread that currently owns an object may hand it to another; letting any
    //! thread re-home an object at will would race the owner's own use of it.
    //!
    //! Qt's one exception is reproduced: an object with *no* thread affinity yet may be adopted by
    //! the calling thread. That is what lets a freshly constructed object be moved onto a worker,
    //! and what lets Thread adopt itself when its run loop starts. Returns true if the object now
    //! lives in the requested thread (including when it already did); false if the move was
    //! refused, in which case the affinity is unchanged.
    bool Object::moveToThread
        (
        Thread* aThread  //!< The new thread this object will live in; nullptr clears the affinity.
        )
    {
        Thread* const currentAffinity = thread();
        Thread* const callerThread = Thread::currentThread();

        if( currentAffinity == aThread )
        {
            // Already there; nothing to do and nothing to refuse.
            return true;
        }

        // Transcribed from Qt's QObject::moveToThread(). The general rule is that only the thread that
        // owns an object may re-home it, with one exception: an object that has no affinity yet may be
        // adopted by the calling thread. That exception is what makes the two normal idioms work --
        // moving a freshly constructed object onto a worker, and Thread adopting itself once its run
        // loop starts -- while still rejecting one thread yanking another thread's live object away.
        const bool adoptingUnownedObject = ( currentAffinity == nullptr && aThread == callerThread )
        ;
        if( !adoptingUnownedObject && currentAffinity != callerThread )
        {
            std::fprintf( stderr,
                "Object::moveToThread: current thread is not the object's thread; cannot "
                "move it to the target thread\n" );
            return false;
        }

        // Take any active timers off the outgoing dispatcher before the affinity changes. Qt documents
        // this behaviour ("all active timers for the object will be reset ... stopped in the current
        // thread and restarted, with the same interval, in the targetThread"); without it the timers
        // would keep firing on the thread the object just left, delivering timerEvent() somewhere it
        // no longer lives.
        std::vector<AbstractEventDispatcher::TimerRegistration> timersToMove;
        {
            std::shared_ptr<ThreadData> oldData = mAffinity->data();
            if( oldData )
            {
                if( auto oldDispatcher = oldData->dispatcher() )
                {
                    timersToMove = oldDispatcher->takeTimersForReceiver( this );
                }
            }
        }

        // Resolve the new thread's data and store it in the Affinity box in one step, so concurrent
        // readers of thread()/threadData() (notably a connect() wrapper resolving affinity at emit
        // time) never see a half-updated pairing of thread and dispatcher.
        std::shared_ptr<ThreadData> newData = aThread ? aThread->threadData() : nullptr;
        mAffinity->setData( std::move( newData ) );

        if( !timersToMove.empty() )
        {
            // Re-register on the destination thread rather than from here: registerTimer() must run
            // where the timer will be serviced. Qt solves it the same way, queueing the
            // re-registration with invokeMethod(..., Qt::QueuedConnection) so it lands on the new
            // thread. If this object is destroyed before the queued call runs, ~Object() strips its
            // pending events from the dispatcher, so the call is dropped rather than dangling.
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

        return true;
    }

    //! Gets the object's descriptive name. Thread-safe.
    std::string Object::objectName() const
    {
        return mObjectName;
    }

    //! Sets the object's descriptive name. Thread-safe.
    void Object::setObjectName
        (
        const std::string& aName  //!< The new object name.
        )
    {
        mObjectName = aName;
    }

    //! Schedules this object for deletion in the event loop. Thread-safe.
    void Object::deleteLater()
    {
        // De-bounce repeated calls: only the first ever posts a DeferredDeleteEvent, matching Qt's
        // own guard ("De-bounce QDeferredDeleteEvents" over QObjectPrivate::deleteLaterCalled,
        // qobject.cpp).
        //
        // This is Qt parity and defense-in-depth, not a fix for a reachable bug: a duplicate event
        // is already harmless, since the deletedReceivers set in processEvents() covers the case
        // where both land in one batch, and ~Object()'s removeEventsForReceiver() strips any that
        // are still queued. What the guard adds is that the invariant "at most one deferred delete
        // exists per object" holds at the source, rather than depending on two separate downstream
        // mechanisms to keep covering every interleaving -- plus it skips a redundant allocation.
        if( mDeleteLaterPosted.exchange( true ) )
        {
            return;
        }

        auto* event = new DeferredDeleteEvent();
        if( auto tData = threadData() )
        {
            // Queue only if there is a live thread to dispatch it. A destroyed Thread leaves its
            // ThreadData -- and that ThreadData's still-working dispatcher -- behind, so without
            // this check the event is accepted by a queue nothing will ever drain and the object is
            // leaked outright rather than deleted. Falling through to the synchronous delete below
            // is the lesser evil, and the same trade QtMimic makes when its post() refuses the task:
            // "Doing nothing here would leak self forever, which is strictly worse than the
            // thread-affinity violation of deleting it synchronously."
            if( tData->thread() != nullptr )
            {
                if( auto disp = tData->dispatcher() )
                {
                    // A refusal means the dispatcher is closing, so nothing would ever drain this
                    // event; postEvent() has already freed it. Fall through to the synchronous
                    // delete rather than leaking the object.
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

    //! Internal event dispatch plumbing; routes an event to its handler.
    //!
    //! Deliberately private and non-virtual: this is not an extension point. The event queue is
    //! the sole caller (see the friend declaration in the header), and the set of event types is
    //! closed. Override timerEvent() instead to react to timers. Returns true if the event was
    //! recognized and handled.
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
            // The only events that reach this queue are the three above, all posted by Object's
            // own internals. Nothing can inject an arbitrary event for an arbitrary receiver, so
            // any other type is unreachable rather than something to hand to a user hook.
            return false;
        }
    }

    //! Handles timer events sent to this object.
    void Object::timerEvent
        (
        TimerEvent* aEvent  //!< The timer event containing the timer ID.
        )
    {
        ( void )aEvent;
    }

    //! Starts a timer for this object with the specified interval.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** Timers are owned by the
    //! dispatcher of the thread the object lives in, and only that thread's event loop can deliver
    //! the resulting timerEvent(). Calling from any other thread is rejected with a warning on
    //! stderr and returns -1, matching Qt, whose QObject::startTimer() likewise refuses
    //! ("Timers cannot be started from another thread"). To start a timer for an object living in
    //! another thread, get onto that thread first -- for example with callLater(). Returns the
    //! unique timer ID, or -1 if the timer could not be started.
    int Object::startTimer
        (
        int aInterval  //!< Interval in milliseconds.
        )
    {
        // Thread-confined, as in Qt. The timer lives in the dispatcher belonging to this object's
        // thread, and only that thread's event loop can ever deliver the resulting timerEvent().
        // Registering from elsewhere would either race that dispatcher's lifetime or quietly install
        // a timer whose events the caller is not positioned to receive, so refuse it outright rather
        // than doing something surprising.
        if( thread() != Thread::currentThread() )
        {
            std::fprintf( stderr,
                "Object::startTimer: timers cannot be started from another thread\n" );
            return -1;
        }

        if( auto tData = threadData() )
        {
            if( auto disp = tData->dispatcher() )
            {
                // Only consume an id once the timer is actually going to be registered.
                const int timerId = TimerIdPool::allocate();
                if( timerId < 0 )
                {
                    std::fprintf( stderr,
                        "Object::startTimer: no timer ids left\n" );
                    return -1;
                }
                disp->registerTimer( timerId, aInterval, this );
                {
                    // Recorded so ~Object() can hand the id back even if the timer is never killed.
                    std::lock_guard<std::mutex> lock( mRunningTimerIdsMutex );
                    mRunningTimerIds.push_back( timerId );
                }
                return timerId;
            }
        }

        std::fprintf( stderr,
            "Object::startTimer: this thread has no event dispatcher, so the timer cannot "
            "be started\n" );
        return -1;
    }

    //! Kills the timer with the specified ID.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, for the same reason as
    //! startTimer(). Calls from another thread are rejected with a warning and do nothing.
    void Object::killTimer
        (
        int aId  //!< The timer ID to stop.
        )
    {
        // Thread-confined for the same reason as startTimer().
        if( thread() != Thread::currentThread() )
        {
            std::fprintf( stderr,
                "Object::killTimer: timers cannot be stopped from another thread\n" )
            ;
            return;
        }

        bool wasOurs = false;
        {
            std::lock_guard<std::mutex> lock( mRunningTimerIdsMutex );
            auto it = std::find( mRunningTimerIds.begin(), mRunningTimerIds.end(), aId );
            if( it != mRunningTimerIds.end() )
            {
                mRunningTimerIds.erase( it );
                wasOurs = true;
            }
        }

        if( auto tData = threadData() )
        {
            if( auto disp = tData->dispatcher() )
            {
                disp->unregisterTimer( aId );
            }
        }

        // Released only after unregisterTimer() has both dropped the timer and purged any event it
        // had already queued, so the id cannot be reissued while something still referring to it is
        // in the queue. Only ids this object actually owns are returned, so a bogus or double
        // killTimer() cannot inject a duplicate into the pool.
        if( wasOurs )
        {
            TimerIdPool::release( aId );
        }
    }

    //! Dispatches a metacall callback to the target object's event loop based on connection type.
    //! Thread-safe. Returns true if the slot ran (direct) or was queued successfully; false if it
    //! could not be delivered at all, which happens when the target has no thread affinity or its
    //! thread has no event dispatcher yet. Callers that track pending state must undo it when this
    //! returns false.
    bool Object::dispatchMetaCall
        (
        Object* aTarget,               //!< Target Object.
        std::function<void()> aSlot,    //!< Callback function.
        ConnectionType aType         //!< Connection type.
        )
    {
        if( !aTarget )
        {
            return false;
        }

        Thread* targetThread = aTarget->thread();
        ConnectionType activeType = aType;
        if( activeType == ConnectionType::Auto )
        {
            Thread* currentThread = Thread::currentThread();
            if( currentThread == targetThread )
            {
                activeType = ConnectionType::Direct;
            }
            else
            {
                activeType = ConnectionType::Queued;
            }
        }

        if( activeType == ConnectionType::Queued )
        {
            // Moved, not copied: aSlot is a by-value parameter and is dead after this line, and
            // MetaCallEvent's constructor also takes by value and moves, so copying here bought a
            // second heap allocation on every queued emit for nothing.
            auto* event = new MetaCallEvent( std::move( aSlot ) );
            if( auto tData = aTarget->threadData() )
            {
                if( auto disp = tData->dispatcher() )
                {
                    disp->postEvent( aTarget, static_cast<Event*>( event ) );
                    return true;
                }
            }
            delete event;
            return false;
        }

        aSlot();
        return true;
    }

    //! Dispatches a metacall to an explicitly named thread, ignoring the receiver's affinity.
    //!
    //! Thread-safe. @p aReceiver is not dereferenced *by this function*: it is handed to
    //! postEvent() purely as the key that removeEventsForReceiver() later matches on. It is
    //! dereferenced afterwards, though -- when the dispatcher drains the queue it calls
    //! aReceiver->event() (EventDispatcherDefault::processEvents()), so the receiver has to still
    //! be alive at that point. Nothing here establishes that; what does is ~Object(), which calls
    //! removeEventsForReceiver() and deletes every event still queued for the object before it
    //! goes away.
    //!
    //! Returns true if the call was queued; false if @p aData is null or its thread has no
    //! dispatcher, in which case the call is dropped.
    //!
    //! @TODO Consideing moving this function to ThreadData.
    bool Object::dispatchMetaCallTo
        (
        const std::shared_ptr<ThreadData>& aData,  //!< Thread to deliver on; null means nowhere.
        Object* aReceiver,                           //!< Receiver; the queue key here, dereferenced later by the dispatcher.
        std::function<void()> aSlot                 //!< Callback function.
        )
    {
        // Moved, not copied: aSlot is a by-value parameter and is dead after this line, and
        // MetaCallEvent's constructor also takes by value and moves, so copying here bought a
        // second heap allocation on every queued emit for nothing.
        if( auto disp = aData ? aData->dispatcher() : nullptr )
        {
            auto* event = new MetaCallEvent( std::move( aSlot ) );
            return disp->postEvent( aReceiver, static_cast<Event*>( event ) );
        }
        return false;
    }
}
