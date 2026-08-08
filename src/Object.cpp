#include "Object.h"

#include "AbstractEventDispatcher.h"
#include "Event.h"
#include "Thread.h"

#include <algorithm>
#include <cstdio>
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

    std::atomic<int> Object::sNextTimerId { 1 };

    //! Constructs an object.
    Object::Object()
        : mLife( std::make_shared<int>( 0 ) )
        // Store the thread's ThreadData, not the Thread itself: it outlives the Thread, so this
        // object's affinity can never become a dangling pointer. Held in an Affinity box read at
        // emit time, so a later moveToThread() redirects existing connections too.
        , mAffinity( std::make_shared<Affinity>(
              Thread::currentThread() ? Thread::currentThread()->threadData() : nullptr ) )
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
        const std::shared_ptr<ThreadData> ownerData = mAffinity->data();
        if( ownerData && ownerData->isThreadRunning()
            && ownerData->thread() != Thread::currentThread() )
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
        std::vector<ConnectionHandle> incoming;
        {
            std::lock_guard<std::mutex> lock( mIncomingMutex );
            incoming.swap( mIncoming );
        }
        for( auto& handle : incoming )
        {
            handle.disconnect();
        }

        // Move the callbacks out from under mCleanupMutex before invoking any of them. Running them
        // while still holding the lock deadlocks on the non-recursive mutex if a callback calls
        // addCleanupCallback() on this same object. A callback registered during the loop below is
        // intentionally dropped -- this object is already being destroyed, so there is no later point
        // at which it could meaningfully run.
        std::vector<std::function<void()> > callbacksToRun;
        {
            std::lock_guard<std::mutex> lock( mCleanupMutex );
            callbacksToRun.swap( mCleanupCallbacks );
        }
        for( auto& cb : callbacksToRun )
        {
            cb();
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
    }

    //! Records a freshly-made connection on its receiver, so ~Object() can disconnect it.
    //!
    //! The receiver is recovered from the Cleanup rather than passed separately, which keeps the
    //! call identical in all ten connect() overloads. Returns the handle so call sites can simply
    //! wrap their existing return expression. Thread-safe.
    ConnectionHandle Object::trackIncoming
        (
        const std::shared_ptr<Cleanup>& aCleanup,  //!< Cleanup token created for this connection.
        ConnectionHandle aHandle                    //!< The connection just established.
        )
    {
        if( !aCleanup || !aCleanup->mOwner )
        {
            return aHandle;
        }

        // Publish the handle into the Cleanup before registering it, so that if the connection is
        // torn down concurrently the destructor below has something to match on rather than the
        // default-constructed handle.
        aCleanup->mHandle = aHandle;

        Object* receiver = aCleanup->mOwner;
        std::lock_guard<std::mutex> lock( receiver->mIncomingMutex );
        receiver->mIncoming.push_back( aHandle );
        return aHandle;
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
                        if( auto life = weakLife.lock() )
                        {
                            fnToRun();
                        }
                    }
                };

            if( !dispatchMetaCall( aContext, metaCall, ConnectionType::QueuedConnection ) )
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
                ConnectionType::QueuedConnection );
        }

        return true;
    }

    //! Gets the object's descriptive name. Thread-safe.
    std::string Object::objectName() const
    {
        std::lock_guard<std::mutex> lock( mNameMutex );
        return mObjectName;
    }

    //! Sets the object's descriptive name. Thread-safe.
    void Object::setObjectName
        (
        const std::string& aName  //!< The new object name.
        )
    {
        std::lock_guard<std::mutex> lock( mNameMutex );
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
            if( auto disp = tData->dispatcher() )
            {
                disp->postEvent( this, static_cast<Event*>( event ) );
                return;
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
                const int timerId = sNextTimerId.fetch_add( 1 );
                disp->registerTimer( timerId, aInterval, this );
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

        if( auto tData = threadData() )
        {
            if( auto disp = tData->dispatcher() )
            {
                disp->unregisterTimer( aId );
            }
        }
    }

    //! Registers a callback to be executed when this object is destroyed. Thread-safe.
    void Object::addCleanupCallback
        (
        std::function<void()> aCallback  //!< The function to execute upon destruction.
        )
    {
        std::lock_guard<std::mutex> lock( mCleanupMutex );
        mCleanupCallbacks.push_back( std::move( aCallback ) );
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
        if( activeType == ConnectionType::AutoConnection )
        {
            Thread* currentThread = Thread::currentThread();
            if( currentThread == targetThread )
            {
                activeType = ConnectionType::DirectConnection;
            }
            else
            {
                activeType = ConnectionType::QueuedConnection;
            }
        }

        if( activeType == ConnectionType::QueuedConnection )
        {
            auto* event = new MetaCallEvent( aSlot );
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

    //! Dispatches a metacall callback based on connection type, resolving affinity from a
    //! previously-captured Affinity box rather than dereferencing the receiver Object. Thread-safe.
    //!
    //! Used exclusively by the connect() wrapper closures, which run at emit time -- possibly much
    //! later, and on a different thread, than connect() itself. boost::signals2's disconnect()
    //! (invoked from ~Object()) does not block for an in-flight emit, so by the time this call
    //! happens the receiver may already be under (or past) destruction; reading its own
    //! thread()/threadData() directly, as the plain Object* overload above does, would then be a
    //! use-after-free. aAffinity was captured by the wrapper when connect() was called, while the
    //! receiver was still definitely alive, and is independently heap-allocated and ref-counted, so
    //! it remains safe to read regardless. @p aReceiver is passed through purely as the dispatcher's
    //! queue key (postEvent()/removeEventsForReceiver() bookkeeping) and is never dereferenced here.
    //! Returns true if the slot ran (direct) or was queued successfully; false if it could not be
    //! delivered, which happens when the receiver has no thread affinity or its thread has no event
    //! dispatcher yet.
    bool Object::dispatchMetaCall
        (
        const std::shared_ptr<Affinity>& aAffinity,  //!< Affinity box captured at connect() time.
        Object* aReceiver,                             //!< Receiver; used only as the dispatcher's queue key.
        std::function<void()> aSlot,                   //!< Callback function.
        ConnectionType aType                         //!< Connection type.
        )
    {
        const std::shared_ptr<ThreadData> targetData = aAffinity ? aAffinity->data() : nullptr;
        Thread* const targetThread = targetData ? targetData->thread() : nullptr;

        ConnectionType activeType = aType;
        if( activeType == ConnectionType::AutoConnection )
        {
            Thread* currentThread = Thread::currentThread();
            activeType = ( currentThread == targetThread )
                ? ConnectionType::DirectConnection
                : ConnectionType::QueuedConnection;
        }

        if( activeType == ConnectionType::QueuedConnection )
        {
            auto* event = new MetaCallEvent( aSlot );
            if( auto disp = targetData ? targetData->dispatcher() : nullptr )
            {
                disp->postEvent( aReceiver, static_cast<Event*>( event ) );
                return true;
            }
            delete event;
            return false;
        }

        aSlot();
        return true;
    }
}
