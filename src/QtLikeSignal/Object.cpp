// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::Object (thread affinity + connection lifetime
//! management).

#include "QtLikeSignal/Object.hpp"

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <algorithm>
#include <climits>
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

    namespace
    {
        //! Process-wide pool of timer ids, handing out reusable ids rather than an ever-rising count.
        //!
        //! Qt does the same with a lock-free QFreeList capped at 2^24 simultaneous timers; a mutex and a
        //! deque is the proportionate equivalent here, since an id is taken once per startTimer() rather
        //! than on any hot path.
        //!
        //! Reuse is **FIFO, deliberately**. A freed id going straight back out (LIFO) would make the
        //! narrowest recycling hazard trivially reachable: a handler that kills one timer and starts
        //! another would get the same id back immediately, and any TimerEvent for the old timer still
        //! in flight would then match the new one. Taking the oldest free id instead means an id is only
        //! reused after every other freed id has been.
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
    }

    //! Constructs an object living in the given thread, or in the calling thread if none is given.
    Object::Object
        (
        Thread* aThread  //!< Thread this object lives in; null means the calling thread.
        )
    // Store the thread's ThreadData, not the Thread itself: it outlives the Thread, so this
    // object's affinity can never become a dangling pointer. Held in an Affinity box read at emit
    // time, so a later moveToThread() redirects existing connections too.
        : Object( aThread ? aThread->threadData()
            : ( Thread::currentThread() ? Thread::currentThread()->threadData()
            : std::shared_ptr<ThreadData>() ) )
    {
    }

    //! Constructs an object directly on the given thread data. See the declaration.
    Object::Object
        (
        std::shared_ptr<ThreadData> aThreadData  //!< Affinity to start with; may be null.
        )
        : mLife( std::make_shared<int>( 0 ) )
        , mAffinity( std::make_shared<Affinity>( std::move( aThreadData ) ) )
    {
    }

    //! Destroys the object, disconnecting its incoming connections and invalidating its life
    //! token, so a queued slot invocation that has not yet run does not run afterwards.
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
        // disconnect() reaches pruneReceiver(), which checks this very token and bails out rather
        // than asking for mIncomingMutex while we are already tearing the list down.
        mLife.reset();

        // Disconnect every connection where this object is the receiver, so the sender stops
        // holding a slot that can never do anything again. The life token above already makes such
        // a slot inert, but inert is not gone: it keeps its captured state and is still walked on
        // every emit, so a long-lived signal accumulates dead slots without bound. Qt does the
        // same thing by walking cd->senders in ~QObject().
        //
        // Take the list out and end the connections with mIncomingMutex released. Holding it across
        // removeConnection() would nest our mutex inside the Signal's, the reverse of the order
        // pruneReceiver() takes them in, and there is no reason to invite that inversion.
        //
        // The nodes are upgraded to shared_ptr *while the mutex is held*, and that is the whole of
        // why this is safe. A node in the list is one whose prune has not run, so its Signal still
        // holds it -- but the instant we unlink it here, a Signal disconnecting on another thread
        // may drop the last reference. Holding one ourselves closes that window. The upgrade cannot
        // fail: every node is created by make_shared.
        std::vector<std::shared_ptr<Private::ConnectionNode> > incoming;
        {
            std::lock_guard<std::mutex> lock( mIncomingMutex );
            incoming.reserve( mIncomingCount );
            for( Private::ConnectionNode* node = mIncomingHead; node != nullptr; )
            {
                Private::ConnectionNode* next = node->mNextIncoming;

                // Unlinked here rather than left to pruneReceiver(), which will not run: it sees
                // the expired life token above and returns before it reaches the list. A node that
                // outlives us must not keep a pointer to one that does not.
                node->mPrevIncoming = nullptr;
                node->mNextIncoming = nullptr;
                node->mInIncoming   = false;
                node->mIncomingDone = true;
                incoming.push_back( node->shared_from_this() );
                node = next;
            }
            mIncomingHead  = nullptr;
            mIncomingCount = 0;
        }
        for( const auto& node : incoming )
        {
            node->mConnected.store( false, std::memory_order_release );
            if( auto impl = node->mImpl.lock() )
            {
                impl->removeConnection( node.get() );
            }
        }

        // Only objects that have actually used callLater() can have entries to drop.
        //
        // The scan below is O(every pending callLater in the process) and takes a lock shared by
        // every thread, so running it unconditionally made destroying an unrelated Object cost
        // 24 us against a backlog of 4000 -- 324x the 75 ns it costs otherwise, and worse as
        // unrelated work queues up elsewhere. Most objects never call callLater() at all, and one
        // flag takes all of them out of that path. See PERFORMANCE-20260813.md (P1).
        //
        // The flag is only ever set, never cleared: an object that used the feature once keeps
        // paying the scan, which is the honest trade. Making it exact would mean counting entries
        // per object, which is the deeper fix P1 describes and is not worth it for a bool.
        if( mUsedCallLater.load( std::memory_order_acquire ) )
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

        // Taken before the strip below, because whether this object owns any timer is half of what
        // decides if that strip has anything to do.
        std::vector<int> outstandingTimerIds;
        {
            std::lock_guard<std::mutex> lock( mRunningTimerIdsMutex );
            outstandingTimerIds.swap( mRunningTimerIds );
        }

        // The other O(backlog) scan the destructor used to run for every object, whether or not it
        // could possibly have anything queued: removeEventsForReceiver() walks the whole event
        // queue and the whole timer list under the dispatcher's lock. An object that never received
        // a queued call and never started a timer -- which is most of them -- has nothing there.
        // Qt guards the same call the same way, with `if (d->postedEvents)` in ~QObject().
        if( mMayHaveQueuedWork.load( std::memory_order_acquire ) || !outstandingTimerIds.empty() )
        {
            std::shared_ptr<ThreadData> threadDataCopy = mAffinity->data();
            if( threadDataCopy )
            {
                if( auto dispatcher = threadDataCopy->dispatcher() )
                {
                    dispatcher->removeEventsForReceiver( this );
                }

                // Events moved here before this thread had a dispatcher are not in any queue yet,
                // so the strip above cannot see them. See ThreadData::mParkedEvents.
                threadDataCopy->removeParkedEventsFor( this );
            }
        }

        // Hand back any ids whose timers were still running. The strip above has already dropped
        // this object's timers and its queued events, so nothing can still be referring to them by
        // the time they are reissued.
        for( const int timerId : outstandingTimerIds )
        {
            TimerIdPool::release( timerId );
        }
    }

    //! Gets the thread this object currently lives in, or nullptr if it has none -- or if the
    //! Thread it lived in has since been destroyed. Thread-safe.
    //!
    //! Never returns a dangling pointer: the affinity is stored as a ThreadData (which outlives its
    //! Thread), exactly as Qt stores a refcounted QThreadData rather than a QThread*. Re-read it
    //! rather than caching it.
    Thread* Object::thread() const
    {
        const std::shared_ptr<ThreadData> data = mAffinity->data();
        return data ? data->thread() : nullptr;
    }

    //! Changes the thread affinity of this object, following Qt's QObject::moveToThread rules.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** The move is push-only:
    //! only the thread that currently owns an object may hand it to another, so an object can be
    //! pushed to another thread but never pulled from an arbitrary one. Qt refuses the same call
    //! the same way ("Current thread is not the object's thread. Cannot move to target thread").
    //!
    //! Qt's one exception is reproduced: an object with *no* affinity yet may be pulled to the
    //! calling thread. That is what lets a freshly constructed object be moved onto a worker, and
    //! what lets Thread adopt itself when its run loop starts.
    //!
    //! Affinity is resolved at emit time, so events posted after a successful move -- including
    //! through connections made BEFORE it -- are delivered to @p aThread. Passing nullptr
    //! dissociates the object, after which thread() returns nullptr.
    //!
    //! Returns true if the object now lives in the requested thread (including when it already
    //! did); false if the move was refused, in which case the affinity is unchanged.
    bool Object::moveToThread
        (
        Thread* aThread  //!< The new thread this object will live in; nullptr clears the affinity.
        )
    {
        Thread* const currentAffinity = thread();
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
        Thread* const callerThread = Thread::currentThread();
        const bool adoptingUnownedObject = ( currentAffinity == nullptr )
            && ( aThread == callerThread );
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
        // no longer lives. The caller is on that outgoing thread (push-only, checked above), so
        // this cannot race its loop's own delivery pass.
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
        const std::shared_ptr<ThreadData> oldData = mAffinity->data();
        std::shared_ptr<ThreadData> newData = aThread ? aThread->threadData() : nullptr;
        mAffinity->setData( newData );

        migratePostedEvents( oldData, newData );

        if( !timersToMove.empty() )
        {
            if( newData == nullptr )
            {
                // moveToThread(nullptr) leaves nothing to service the timers, so they are gone
                // rather than merely paused, and their ids go back to the pool at once instead of
                // waiting for ~Object(). The object's own record has to be cleared too, or a later
                // killTimer() would release the same id a second time. Queueing the
                // re-registration below would achieve nothing: there is no dispatcher to run it.
                for( const auto& timer : timersToMove )
                {
                    forgetTimerId( timer.mTimerId );
                    TimerIdPool::release( timer.mTimerId );
                }
                return true;
            }

            // Re-register on the destination thread rather than from here: registerTimer() must run
            // where the timer will be serviced. Qt solves it the same way, queueing the
            // re-registration with invokeMethod(..., Qt::QueuedConnection) so it lands on the new
            // thread. If this object is destroyed before the queued call runs, ~Object() strips its
            // pending events from the dispatcher, so the call is dropped rather than dangling.
            dispatchMetaCall(
                this,
                [this, timersToMove]()
                {
                    if( auto data = threadData() )
                    {
                        if( auto dispatcher = data->dispatcher() )
                        {
                            for( const auto& timer : timersToMove )
                            {
                                dispatcher->registerTimer( timer.mTimerId, timer.mIntervalMs,
                                this );
                            }
                        }
                    }
                },
                ConnectionType::Queued );
        }

        return true;
    }

    //! Gets the object's descriptive name, empty unless one was set.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** The name is a plain
    //! std::string with no lock, so a concurrent setObjectName() is a data race -- exactly as
    //! QObject::objectName() has no locking either. See mObjectName for why it is unguarded.
    // This said "Thread-safe" until 2026-08-13 and was never true; see R15.
    std::string Object::objectName() const
    {
        return mObjectName;
    }

    //! Gives this object a descriptive name, for logs and diagnostics. Nothing keys off it.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, for the same reason as
    //! objectName() above.
    void Object::setObjectName
        (
        const std::string& aName  //!< The new object name.
        )
    {
        mObjectName = aName;
    }

    //! Schedules this object for deletion in the event loop. Thread-safe.
    //!
    //! Qt-like QObject::deleteLater(). If the object has no thread, or its thread has stopped or
    //! gone (post() refuses), deletion happens immediately: a thread-affinity violation is
    //! preferable to a deferred delete that would never run and would leak the object.
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

    //! Called when one of this object's timers comes due. Does nothing by default.
    void Object::timerEvent
        (
        TimerEvent* aEvent  //!< The timer event containing the timer ID.
        )
    {
        ( void )aEvent;
    }

    //! Starts a repeating timer delivering timerEvent() to this object every @p aIntervalMs.
    //! Returns the new timer's id, or -1 if it could not be started.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** Timers are owned by the
    //! dispatcher of the thread the object lives in, and only that thread's event loop can deliver
    //! the resulting timerEvent(), so registering from elsewhere would install a timer whose events
    //! the caller is not positioned to receive. Rejected with a warning on stderr instead, matching
    //! Qt, whose QObject::startTimer() likewise refuses ("Timers cannot be started from another
    //! thread"). To start a timer for an object living in another thread, get onto that thread
    //! first -- for example with callLater().
    //!
    //! An interval of 0 means "fire on every pass of the event loop", as in Qt.
    int Object::startTimer
        (
        int aIntervalMs  //!< Interval in milliseconds.
        )
    {
        if( aIntervalMs < 0 )
        {
            std::fprintf( stderr, "Object::startTimer: interval cannot be negative\n" );
            return -1;
        }

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

        const std::shared_ptr<ThreadData> data = threadData();
        if( !data )
        {
            std::fprintf( stderr,
                "Object::startTimer: object has no thread, so the timer cannot be started\n" );
            return -1;
        }

        auto dispatcher = data->dispatcher();
        if( !dispatcher )
        {
            std::fprintf( stderr,
                "Object::startTimer: this thread has no event dispatcher, so the timer cannot "
                "be started\n" );
            return -1;
        }

        // Consumed only once the timer is certain to be registered, so no failure path above has
        // an id to give back.
        const int timerId = TimerIdPool::allocate();
        if( timerId < 0 )
        {
            std::fprintf( stderr, "Object::startTimer: no timer ids left\n" );
            return -1;
        }

        dispatcher->registerTimer( timerId, aIntervalMs, this );
        {
            // Recorded so ~Object() can hand the id back even if the timer is never killed.
            std::lock_guard<std::mutex> lock( mRunningTimerIdsMutex );
            mRunningTimerIds.push_back( timerId );
        }
        return timerId;
    }

    void Object::killTimer
        (
        int aTimerId  //!< The timer ID to stop.
        )
    {
        // Thread-confined for the same reason as startTimer().
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

        // Released only after unregisterTimer() has both dropped the timer and purged any event it
        // had already queued, so the id cannot be reissued while something still referring to it is
        // in the queue. Only ids this object actually owns are returned, so a bogus or double
        // killTimer() cannot inject a duplicate into the pool.
        if( wasOurs )
        {
            TimerIdPool::release( aTimerId );
        }
    }

    //! Stops the timer with id @p aTimerId.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, for the same reason as
    //! startTimer(). Calls from another thread are rejected with a warning and do nothing. An id
    //! this object does not own is ignored.
    //! Drops @p aTimerId from this object's record of running timers. Returns whether it was there.
    //!
    //! The id is *not* returned to the pool here. Both callers have to unregister the timer with
    //! the dispatcher first, and releasing before that could reissue an id an already-queued
    //! TimerEvent still names.
    bool Object::forgetTimerId
        (
        int aTimerId  //!< The timer ID to forget.
        )
    {
        std::lock_guard<std::mutex> lock( mRunningTimerIdsMutex );
        auto it = std::find( mRunningTimerIds.begin(), mRunningTimerIds.end(), aTimerId );
        if( it == mRunningTimerIds.end() )
        {
            return false;
        }
        mRunningTimerIds.erase( it );
        return true;
    }

    //! Schedules or updates a callLater deferred invocation.
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

        // Marked before the entry exists, so ~Object() can never see the entry without the flag.
        // The reverse -- flag set, entry already gone -- costs one wasted scan and nothing else.
        aContext->mUsedCallLater.store( true, std::memory_order_release );

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

    //! Returns true if @p aData belongs to the calling thread.
    //!
    //! Exists because Object.hpp cannot include Thread.hpp -- Thread derives from Object -- yet the
    //! inline connect machinery there has to make exactly this test at emit time.
    bool Object::isCurrentThread
        (
        const std::shared_ptr<ThreadData>& aData
        )
    {
        // Compared as bare pointers. Asking for the shared_ptr instead cost an atomic increment and
        // decrement on every Auto emit, to answer a question that never needed ownership: the
        // caller already holds aData alive, and the other side is this very thread's own data.
        return aData.get() == Thread::currentThread()->threadDataPtr();
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

    //! Carries this object's already-posted events from @p aOldData's dispatcher to @p aNewData's.
    //!
    //! Called by moveToThread() **after** the affinity has been swapped, which is what makes it
    //! safe without holding both dispatcher mutexes at once: each queue is only ever touched alone,
    //! so two moves in opposite directions cannot deadlock. Qt needs `QOrderedMutexLocker` over the
    //! two post-event lists precisely because it moves the events and the affinity together.
    //!
    //! Safe for a reason specific to this function: **moveToThread() runs on the object's own
    //! thread**, so the old thread is inside this call and cannot be dispatching the events being
    //! taken. The only other writer is a foreign thread that read the affinity before the swap and
    //! is still on its way into the old queue, which is what the re-sweep below is for.
    //!
    //! Without this, a queued call posted just before the move runs on the thread the object has
    //! left, silently -- nothing re-checks affinity once an event is queued. That is the one
    //! guarantee a queued connection exists to provide. Qt migrates them in
    //! QObjectPrivate::setThreadData_helper().
    void Object::migratePostedEvents
        (
        const std::shared_ptr<ThreadData>& aOldData,  //!< Thread the object is leaving; may be null.
        const std::shared_ptr<ThreadData>& aNewData   //!< Thread it now lives on; may be null.
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

        // Swept more than once. A thread that resolved this object's affinity before the swap can
        // still be inside postEvent() on the old dispatcher, and its event would be stranded by a
        // single pass. Every such poster is already in flight, so the set drains; the cap is there
        // because a caller that never stops posting to a moving object is misusing it, and a bounded
        // loop is better than one that can be kept spinning.
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
                    // moveToThread(nullptr) means "this object stops processing events", so there
                    // is no later at which these could run. Qt parks them on an orphan QThreadData
                    // whose loop never runs, which comes to the same thing without the bookkeeping.
                    delete event;
                    continue;
                }

                // Asked in one step, so the destination cannot gain or lose its dispatcher between
                // the question and the answer. A null return means the event is parked and now
                // belongs to the destination's ThreadData -- see ThreadData::mParkedEvents, which
                // is what makes "moveToThread() before start()" work.
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

    //! Routes an event to its handler. Returns true if the event was recognised and handled.
    //!
    //! Deliberately private and non-virtual: this is not an extension point. The event queue is the
    //! sole caller (see the friend declaration in the header), and the set of event types is closed.
    //! Override timerEvent() instead to react to timers.
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

    //! Dispatches a metacall to the target object's event loop, honouring @p aType. Thread-safe.
    //!
    //! Returns true if the slot ran (direct) or was queued successfully; false if it could not be
    //! delivered at all, which happens when the target has no thread affinity or its thread has no
    //! event dispatcher yet. Callers that track pending state must undo it when this returns false.
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

        ConnectionType activeType = aType;
        if( activeType == ConnectionType::Auto )
        {
            activeType = ( Thread::currentThread() == aTarget->thread() )
                         ? ConnectionType::Direct
                         : ConnectionType::Queued;
        }

        if( activeType == ConnectionType::Queued )
        {
            // Moved, not copied: aSlot is a by-value parameter and is dead after this line, and
            // MetaCallEvent's constructor also takes by value and moves, so copying here would buy a
            // second heap allocation on every queued emit for nothing.
            return dispatchMetaCallTo( aTarget->threadData(), aTarget, std::move( aSlot ) );
        }

        aSlot();
        return true;
    }

    //! Dispatches a metacall to an explicitly named thread, ignoring the receiver's affinity.
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
    //!
    //! Returns true if the call was queued; false if @p aData is null or its thread has no
    //! dispatcher, in which case the call is dropped.
    // TODO: consider moving this function to ThreadData.
    bool Object::dispatchMetaCallTo
        (
        const std::shared_ptr<ThreadData>& aData,  //!< Thread to deliver on; null means nowhere.
        Object* aReceiver,                          //!< Receiver; the queue key here.
        std::function<void()> aSlot                 //!< Callback function.
        )
    {
        // Moved, not copied: aSlot is a by-value parameter and is dead after this line, and
        // MetaCallEvent's constructor also takes by value and moves, so copying here bought a
        // second heap allocation on every queued emit for nothing.
        if( auto disp = aData ? aData->dispatcher() : nullptr )
        {
            auto* event = new MetaCallEvent( std::move( aSlot ) );
            if( aReceiver )
            {
                aReceiver->mMayHaveQueuedWork.store( true, std::memory_order_release );
            }
            return disp->postEvent( aReceiver, static_cast<Event*>( event ) );
        }
        return false;
    }

    //! Links this node into the receiver's incoming list. See ConnectionNode.
    //!
    //! Defined here rather than in Connection.hpp because it is the receiver's list it touches, and
    //! that needs Object to be complete.
    void Private::ConnectionNode::registerWithReceiver()
    {
        // No receiver, or one already being destroyed. Signal::connect() takes the first branch:
        // a slot subscribed without an Object has no incoming list to appear in.
        if( mOwner == nullptr || mLife.expired() )
        {
            return;
        }

        std::lock_guard<std::mutex> lock( mOwner->mIncomingMutex );
        if( mIncomingDone )
        {
            return;
        }

        // At the head, because the order of the list does not matter: it is a set of connections to
        // end, and ~Object() ends all of them.
        mPrevIncoming = nullptr;
        mNextIncoming = mOwner->mIncomingHead;
        if( mNextIncoming != nullptr )
        {
            mNextIncoming->mPrevIncoming = this;
        }
        mOwner->mIncomingHead = this;
        mInIncoming = true;
        ++mOwner->mIncomingCount;
    }

    //! Unlinks this node from the receiver's incoming list. See ConnectionNode.
    void Private::ConnectionNode::pruneReceiver()
    {
        // The receiver is gone or already being destroyed; ~Object() has taken the list over and
        // touching mOwner here would be a use-after-free. This is also what makes ~Object()'s own
        // disconnect loop non-re-entrant: it resets the life token before disconnecting, so this
        // returns before it can ask for a mutex ~Object() is holding.
        if( mOwner == nullptr || mLife.expired() )
        {
            return;
        }

        std::lock_guard<std::mutex> lock( mOwner->mIncomingMutex );
        mIncomingDone = true;
        if( !mInIncoming )
        {
            return;
        }

        if( mPrevIncoming != nullptr )
        {
            mPrevIncoming->mNextIncoming = mNextIncoming;
        }
        else
        {
            mOwner->mIncomingHead = mNextIncoming;
        }
        if( mNextIncoming != nullptr )
        {
            mNextIncoming->mPrevIncoming = mPrevIncoming;
        }

        mPrevIncoming = nullptr;
        mNextIncoming = nullptr;
        mInIncoming   = false;
        --mOwner->mIncomingCount;
    }
}
