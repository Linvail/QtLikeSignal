// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::Object (thread affinity + connection lifetime
//! management).

#include "QtLikeSignal/Object.hpp"

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <algorithm>
#include <climits>
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
        //! Process-wide pool of timer ids, handing out reusable ids rather than an ever-rising
        //! count.
        //!
        //! Qt does the same with a lock-free QFreeList capped at 2^24 simultaneous timers; a mutex
        //! and a deque is the proportionate equivalent here, since an id is taken once per
        //! startTimer() rather than on any hot path.
        //!
        //! Reuse is **FIFO, deliberately**. A freed id going straight back out (LIFO) would make
        //! the narrowest recycling hazard trivially reachable: a handler that kills one timer and
        //! starts another would get the same id back immediately, and any TimerEvent for the old
        //! timer still in flight would then match the new one. Taking the oldest free id instead
        //! means an id is only reused after every other freed id has been.
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

    //! Constructs an object in the calling thread, optionally attaching it to a parent.
    //!
    //! The affinity is resolved first and the attach happens second, which is the order that makes
    //! setParent()'s cross-thread check meaningful here: this object belongs to the calling thread
    //! at that point, so a parent living elsewhere is refused rather than silently pulling this
    //! object into another thread. A constructor cannot report that, so setParent() warns and this
    //! object is simply left parentless -- which is exactly what Qt's check_parent_thread() does.
    Object::Object
        (
        Object* aParent  //!< Parent that will own this object; null for none.
        )
        : Object( Thread::currentThread() ? Thread::currentThread()->threadData()
            : nullptr )
    {
        if( aParent != nullptr )
        {
            // Discarded deliberately: a refusal has already been reported on stderr, and there is
            // no way to return it from here. The object is usable either way, just parentless.
            ( void )setParent( aParent );
        }
    }

    //! Constructs an object directly on the given thread data. See the declaration.
    Object::Object
        (
        ThreadData* aThreadData  //!< Affinity to start with; may be null.
        )
        : mAffinity( std::make_shared<Affinity>( std::move( aThreadData ) ) )
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
        // Everything here is read through the ThreadData, which the Affinity's own mutex keeps
        // alive for the duration of the question, and the Thread* is only ever *compared*, never
        // dereferenced. Asking the Thread itself (owner->isRunning()) would have reintroduced
        // exactly the dangling-pointer hazard the Affinity indirection exists to remove: thread()
        // can hand back a pointer that a concurrent ~Thread() frees before the call lands.
        //
        // Asked as one question rather than by copying the ThreadData out and reading it here.
        // The copy was two atomic read-modify-writes to keep alive, for the length of two atomic
        // loads, something the mutex already held. See Affinity::namesOtherRunningThread().
        //
        // Asked with currentThreadOrNull(), never currentThread(): the latter adopts the calling
        // thread when it has no Thread yet, and this runs during thread_local teardown -- an
        // adopted Thread being destroyed as its native thread exits -- where adopting re-enters
        // the unique_ptr already being destroyed. A diagnostic must not allocate. A null answer
        // means the caller is not registered, in which case there is nothing to compare and
        // nothing to warn about.
        Thread* const callerThread = Thread::currentThreadOrNull();
        if( callerThread && mAffinity->namesOtherRunningThread( callerThread ) )
        {
            qCWarning( gLogObject )
                << "Object::~Object: object destroyed from a thread other than the one it lives"
                << "in while that thread's event loop is still running; this is not safe. Use"
                << "deleteLater() to destroy an object from another thread.";
        }

        // Invalidate the life flag first. connect()/callLater() wrappers running on other threads
        // check it before posting a call to this object; clearing it up front shrinks the window in
        // which such a wrapper can still observe this object as "alive" to the check-then-post race
        // itself, instead of the whole destructor body (which below runs arbitrary user
        // cleanup-callback code).
        //
        // Doing it before the disconnect loop below is also what keeps that loop re-entrancy-free:
        // disconnect() reaches pruneReceiver(), which checks this very flag and bails out rather
        // than asking for mIncomingMutex while we are already tearing the list down.
        //
        // The flag lives in the affinity box rather than in a shared_ptr<int> of its own; see
        // Affinity::isObjectAlive(). The box outlives us, which is exactly what a life token has to
        // do, so it was already the right place for it.
        mAffinity->markObjectDead();

        // Unlink from our own parent, before anything below can run user code.
        //
        // Qt does this last instead (qobject.cpp:1185), and the reason is specific: ~QObject emits
        // destroyed(this) near the top, and a handler may still ask the dying object for its
        // parent, so the link has to outlive that emission. We have no destroyed() signal -- see
        // the decision not to add one was taken deliberately, together with this dependency on
        // it. With no such consumer, unlinking first is
        // strictly better: it shrinks the window in which a findChild() from some handler can hand
        // back an object that is already half destroyed, which is a weakness Qt documents and
        // lives with rather than one we have to inherit.
        //
        // Safe to touch the parent's list from here because the tree is thread-confined and this
        // object lives in the same thread as its parent, which is the thread the diagnostic above
        // has just checked we are on.
        detachFromParent();

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
            std::lock_guard<Lock> lock( mIncomingMutex );
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

        // Destroy this object's children. A parent owns them: this is the statement that makes
        // `delete window` free a whole subtree, which is the entire point of the tree.
        //
        // The position is chosen twice over. After the disconnect above, so no child destructor can
        // deliver a signal into a parent that is already half torn down. Before the three strips
        // below, so that anything such a destructor posts back at the parent -- a callLater, a
        // queued call, a deleteLater -- is caught by them rather than left in a queue naming an
        // object about to be freed. That second reason is the whole of why this does not simply go
        // last, which is where Qt puts it.
        deleteChildren();

        // Only objects that have actually used callLater() can have entries to drop.
        //
        // The scan below is O(every pending callLater in the process) and takes a lock shared by
        // every thread, so running it unconditionally made destroying an unrelated Object cost
        // 24 us against a backlog of 4000 -- 324x the 75 ns it costs otherwise, and worse as
        // unrelated work queues up elsewhere. Most objects never call callLater() at all, and one
        // flag takes all of them out of that path.
        //
        // The flag is only ever set, never cleared: an object that used the feature once keeps
        // paying the scan, which is the honest trade. Making it exact would mean counting entries
        // per object, which is the deeper fix P1 describes and is not worth it for a bool.
        if( hasFlag( kUsedCallLater ) )
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
        //
        // Guarded by the same kind of set-once flag as the two scans above, and for the same
        // reason: an object that never started a timer has nothing to swap, and taking the mutex to
        // discover that cost every destruction in the program an uncontended lock and unlock. See
        // kUsedTimers.
        std::vector<int> outstandingTimerIds;
        if( hasFlag( kUsedTimers ) )
        {
            // The flag is only ever set after the box exists, so this cannot be null here; read
            // through the pointer rather than asserting, because a diagnostic is not worth a branch
            // that can never be taken.
            if( Extras* extras = mExtras.load( std::memory_order_acquire ) )
            {
                if( extras->mRunningTimerIds )
                {
                    outstandingTimerIds.swap( *extras->mRunningTimerIds );
                }
            }
        }

        // The other O(backlog) scan the destructor used to run for every object, whether or not it
        // could possibly have anything queued: removeEventsForReceiver() walks the whole event
        // queue and the whole timer list under the dispatcher's lock. An object that never received
        // a queued call and never started a timer -- which is most of them -- has nothing there.
        // Qt guards the same call the same way, with `if (d->postedEvents)` in ~QObject().
        if( hasFlag( kMayHaveQueuedWork ) || !outstandingTimerIds.empty() )
        {
            ThreadData* const threadDataCopy = mAffinity->data();
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

        // Last, because everything above may still read through it. Exchanged rather than loaded
        // and deleted so the pointer is null before the box is freed, which keeps a stray read
        // during teardown a null check rather than a use-after-free.
        delete mExtras.exchange( nullptr, std::memory_order_acq_rel );
    }

    //! Gets the thread this object currently lives in, or nullptr if it has none -- or if the
    //! Thread it lived in has since been destroyed. Thread-safe.
    //!
    //! Never returns a dangling pointer: the affinity is stored as a ThreadData (which outlives its
    //! Thread), exactly as Qt stores a refcounted QThreadData rather than a QThread*. Re-read it
    //! rather than caching it.
    Thread* Object::thread() const
    {
        ThreadData* const data = mAffinity->data();
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

        // Transcribed from Qt's QObject::moveToThread(). The general rule is that only the thread
        // that owns an object may re-home it, with one exception: an object that has no affinity
        // yet may be adopted by the calling thread. That exception is what makes the two normal
        // idioms work -- moving a freshly constructed object onto a worker, and Thread adopting
        // itself once its run loop starts -- while still rejecting one thread yanking another
        // thread's live object away.
        Thread* const callerThread = Thread::currentThread();
        const bool adoptingUnownedObject = ( currentAffinity == nullptr )
            && ( aThread == callerThread );
        if( !adoptingUnownedObject && currentAffinity != callerThread )
        {
            qCWarning( gLogObject )
                << "Object::moveToThread: current thread is not the object's thread; cannot"
                << "move it to the target thread";
            return false;
        }

        // A child cannot be moved on its own. Its affinity is its parent's, and the tree is
        // thread-confined on exactly that invariant -- moving one node out would leave a parent and
        // a child in different threads, with the child's links then reachable from two of them.
        // Move the parent and the subtree follows; or detach first and move the object afterwards.
        // Qt refuses the same call for the same reason (qobject.cpp:1715).
        if( parent() != nullptr )
        {
            qCWarning( gLogObject )
                << "Object::moveToThread: object has a parent; move the parent instead, or"
                << "setParent(nullptr) first";
            return false;
        }

        // Every refusal is behind us, so nothing below can fail.
        moveSubtreeToThread( aThread );
        return true;
    }

    //! Applies a move to this object and everything under it. See moveToThread().
    //!
    //! Recursive, like Qt's moveToThread_helper(), but **children first and this object last**, and
    //! that order is load-bearing rather than stylistic.
    //!
    //! Re-homing an object arms the destination thread against it: moveSelfToThread() ends by
    //! migrating the object's posted events, and the moment they land in the destination queue that
    //! thread may run them. If one of them is a DeferredDeleteEvent -- an ordinary deleteLater()
    //! issued before the move -- the object is destroyed there and then, and destroying it destroys
    //! its children too. So after this object has been moved, neither it nor anything under it may
    //! be touched again.
    //!
    //! Moving the children first means nothing needs to be. The first version of this did the
    //! reverse, moved self and then read firstChild(), and ThreadSanitizer caught it against
    //! ObjectDefectTest.MoveToThreadCarriesAPendingDeleteLaterToTheNewThread: a use-after-free with
    //! every test still passing, because the read usually won the race.
    //!
    //! This is also the answer to the question the plan left open -- collect the subtree first, or
    //! migrate node by node -- and the reason is not the one it guessed. It is not about being
    //! atomic against a failure part-way through; every refusal happens in moveToThread() before
    //! any of this runs. It is about not touching an object one has just handed to another thread.
    //!
    //! `next` is read before the child is moved, for the same reason: that child may be gone by the
    //! time the loop wants its sibling. What is *not* covered, here or in Qt, is a destructor that
    //! deletes its own siblings while this walk is in progress; a queued delete arriving mid-walk
    //! is ordinary, that is not.
    void Object::moveSubtreeToThread
        (
        Thread* aThread  //!< The thread the whole subtree moves to; nullptr clears affinity.
        )
    {
        for( Object* child = firstChild(); child != nullptr; )
        {
            Object* const next = child->nextSibling();
            child->moveSubtreeToThread( aThread );
            child = next;
        }

        // Last, and nothing below this line may touch this object.
        moveSelfToThread( aThread );
    }

    //! Re-homes this one object, without touching its children. See moveToThread().
    //!
    //! Split out of moveToThread() when the move learned to carry a subtree: the checks belong to
    //! the call, and this is the work, which every node in the subtree needs done to it.
    void Object::moveSelfToThread
        (
        Thread* aThread  //!< The thread this object moves to; nullptr clears affinity.
        )
    {
        if( thread() == aThread )
        {
            // Already there. Reached for a child whose affinity somehow already matches; skipping
            // it avoids taking its timers off a dispatcher only to hand them straight back.
            return;
        }

        // Take any active timers off the outgoing dispatcher before the affinity changes. Qt
        // documents this behaviour ("all active timers for the object will be reset ... stopped in
        // the current thread and restarted, with the same interval, in the targetThread"); without
        // it the timers would keep firing on the thread the object just left, delivering
        // timerEvent() somewhere it no longer lives. The caller is on that outgoing thread
        // (push-only, checked above), so this cannot race its loop's own delivery pass.
        std::vector<AbstractEventDispatcher::TimerRegistration> timersToMove;
        {
            ThreadData* const oldData = mAffinity->data();
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
        ThreadData* const oldData = mAffinity->data();
        ThreadData* const newData = aThread ? aThread->threadData() : nullptr;
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
                return;
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
    }

    //! Returns the extras box, creating it the first time anything needs it.
    //!
    //! See Object::Extras for why the members live behind a pointer at all. Both callers --
    //! setObjectName() and startTimer() -- are thread-confined to this object's own thread, so two
    //! threads should never arrive here at once.
    //!
    //! The compare-exchange is there anyway, and costs nothing on the path that matters: an object
    //! that already has a box returns on the first load. It only runs on the one allocation per
    //! object, and it turns a contract violation from "two live boxes, one of them silently
    //! orphaned with the name in it" into "one box, no leak". A cheap way to make misuse boring.
    Object::Extras& Object::ensureExtras()
    {
        if( Extras* existing = mExtras.load( std::memory_order_acquire ) )
        {
            return *existing;
        }

        std::unique_ptr<Extras> fresh( new Extras() );
        Extras* expected = nullptr;
        if( mExtras.compare_exchange_strong( expected, fresh.get(),
            std::memory_order_release, std::memory_order_acquire ) )
        {
            return *fresh.release();
        }
        return *expected;   // somebody else won; ours is freed on the way out
    }

    //! Gets the object's descriptive name, empty unless one was set.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** The name is a plain
    //! std::string with no lock, so a concurrent setObjectName() is a data race -- exactly as
    //! QObject::objectName() has no locking either. See mObjectName for why it is unguarded.
    std::string Object::objectName() const
    {
        const Extras* extras = mExtras.load( std::memory_order_acquire );
        if( extras == nullptr || !extras->mObjectName )
        {
            return std::string();
        }
        return *extras->mObjectName;
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
        std::unique_ptr<std::string>& name = ensureExtras().mObjectName;
        if( name )
        {
            *name = aName;
        }
        else
        {
            name.reset( new std::string( aName ) );
        }
    }

    //! Gets this object's parent, or nullptr. See the declaration.
    Object* Object::parent() const
    {
        const Extras* extras = extrasOrNull();
        return extras ? extras->mParent : nullptr;
    }

    //! Gets the first of this object's children, or nullptr. See the declaration.
    Object* Object::firstChild() const
    {
        const Extras* extras = extrasOrNull();
        return extras ? extras->mFirstChild : nullptr;
    }

    //! Gets the next child of this object's parent, or nullptr. See the declaration.
    Object* Object::nextSibling() const
    {
        const Extras* extras = extrasOrNull();
        return extras ? extras->mNextSibling : nullptr;
    }

    //! Counts this object's direct children. See the declaration.
    std::size_t Object::childCount() const
    {
        std::size_t count = 0;
        for( const Object* child = firstChild(); child != nullptr; child = child->nextSibling() )
        {
            ++count;
        }
        return count;
    }

    //! Re-parents this object, refusing rather than silently orphaning. See the declaration.
    bool Object::setParent
        (
        Object* aParent  //!< The new parent, or nullptr to detach.
        )
    {
        if( aParent == parent() )
        {
            // Already there -- including the nullptr-to-nullptr case. Nothing to do and nothing to
            // refuse, matching moveToThread()'s treatment of a move to the thread it already lives
            // in.
            return true;
        }

        if( aParent != nullptr )
        {
            // A parent part-way through its own destructor frees its extras box on the last line of
            // it, so linking into that box now would leave this object pointing at storage about to
            // be released -- and its child list is being emptied at the same moment. Refusing here
            // is also what stops ~Object()'s child loop from running forever if a child's
            // destructor tries to attach something new to the parent that is destroying it: the
            // life flag is cleared before that loop runs, so this returns false instead.
            if( !aParent->mAffinity->isObjectAlive() )
            {
                qCWarning( gLogObject )
                    << "Object::setParent: the requested parent is being destroyed; the parent"
                    << "is unchanged";
                return false;
            }

            // A child lives in its parent's thread. That invariant is what lets the whole tree go
            // unguarded (see Extras::mParent), so a link that would break it is refused rather
            // than accepted and worked around. Qt refuses it too (qobject.cpp:2341) -- but leaves
            // the object with *no* parent at all, which neither honours the request nor keeps the
            // previous state, and silently leaks anything that was relying on the old parent to
            // free it. Keeping the old parent is the whole reason this function returns a bool.
            if( aParent->thread() != thread() )
            {
                qCWarning( gLogObject )
                    << "Object::setParent: the requested parent lives in a different thread; the"
                    << "parent is unchanged";
                return false;
            }

            // Walking up from the proposed parent reaches this object exactly when the new link
            // would close a cycle, which covers setParent(this) on the first step. O(depth), on a
            // path taken once per re-parent. Qt looks for the same thing by depth limit
            // (CheckForParentChildLoopsWarnDepth, 4096) in debug builds only, and warns rather
            // than refusing; a cycle makes the destructor recurse forever, so it is worth catching
            // in release too.
            for( Object* ancestor = aParent; ancestor != nullptr; ancestor = ancestor->parent() )
            {
                if( ancestor == this )
                {
                    qCWarning( gLogObject )
                        << "Object::setParent: the requested parent is this object or one of its"
                        << "descendants; the parent is unchanged";
                    return false;
                }
            }
        }

        // Both refusals are behind us, so the old link can go before the new one is made. Detaching
        // first keeps the object in exactly one child list at every point in between.
        detachFromParent();
        if( aParent != nullptr )
        {
            attachToParent( aParent );
        }
        return true;
    }

    //! Links this object into aParent's child list. See the declaration.
    //!
    //! At the head, because the order of the list is not part of the contract -- the same reason
    //! ConnectionNode::registerWithReceiver() does it, and what keeps this O(1).
    void Object::attachToParent
        (
        Object* aParent  //!< The new parent; never null, and never this object's current one.
        )
    {
        Extras& parentExtras = aParent->ensureExtras();
        Extras& selfExtras   = ensureExtras();

        selfExtras.mParent      = aParent;
        selfExtras.mPrevSibling = nullptr;
        selfExtras.mNextSibling = parentExtras.mFirstChild;
        if( parentExtras.mFirstChild != nullptr )
        {
            // The existing head is a child, so it has a box already; this is a read, not a create.
            parentExtras.mFirstChild->extrasOrNull()->mPrevSibling = this;
        }
        parentExtras.mFirstChild = this;
    }

    //! Unlinks this object from its parent's child list. See the declaration.
    void Object::detachFromParent()
    {
        Extras* selfExtras = extrasOrNull();
        if( selfExtras == nullptr || selfExtras->mParent == nullptr )
        {
            return;
        }

        // Everything reachable from here has a box: this object has one (it has a parent), the
        // parent has one (it has a child), and so does every sibling.
        Extras* parentExtras = selfExtras->mParent->extrasOrNull();
        if( selfExtras->mPrevSibling != nullptr )
        {
            selfExtras->mPrevSibling->extrasOrNull()->mNextSibling = selfExtras->mNextSibling;
        }
        else
        {
            parentExtras->mFirstChild = selfExtras->mNextSibling;
        }
        if( selfExtras->mNextSibling != nullptr )
        {
            selfExtras->mNextSibling->extrasOrNull()->mPrevSibling = selfExtras->mPrevSibling;
        }

        selfExtras->mPrevSibling = nullptr;
        selfExtras->mNextSibling = nullptr;
        selfExtras->mParent      = nullptr;
    }

    namespace
    {
        //! Writes one node and everything under it, indented by @p aDepth.
        void dumpSubtree
            (
            const Object* aNode,  //!< Node to write; never null.
            int aDepth            //!< How far down the tree it sits, for the indent.
            )
        {
            // One string rather than a value per column, because a record puts a space
            // between values and the indent has to be exact for a tree to read as one. The
            // leading space is what makes every level four wider than the last: the pointer
            // that follows is a second value and brings its own separator, so a line that
            // started at column zero would sit one short of the rest.
            const std::string name = aNode->objectName();
            std::string line( 1 + static_cast<std::size_t>( aDepth ) * 4, ' ' );
            if( !name.empty() )
            {
                line += '\"';
                line += name;
                line += "\" ";
            }

            // Info and not Debug, unlike Qt's QObject::dumpObjectTree(), which goes to qDebug().
            // Nobody calls this by accident: it produces output because it was asked to, and a
            // dump that is silent unless a filter rule was set first is a dump that wasted the
            // caller's time.
            qCInfo( gLogObject ) << line << static_cast<const void*>( aNode );

            for( const Object* child = aNode->firstChild(); child != nullptr;
                child = child->nextSibling() )
            {
                dumpSubtree( child, aDepth + 1 );
            }
        }
    }

    //! Writes this object's subtree to stderr. See the declaration.
    void Object::dumpObjectTree() const
    {
        dumpSubtree( this, 0 );
    }

    //! Destroys every child of this object. See the declaration.
    //!
    //! **Unlink first, then delete**, and re-read the head rather than holding a `next` pointer.
    //! Those two choices are what make this safe against a child destructor that touches its
    //! siblings, and they are why this needs no equivalent of Qt's currentChildBeingDeleted
    //! (qobject_p.h:219-222).
    //!
    //! Qt has that member because deleteChildren() walks a QList by index: it must mark the slot it
    //! is currently inside so that a sibling deleted from within that destructor finds an entry
    //! already cleared rather than corrupting the walk. An intrusive list has no index to
    //! invalidate. Detaching the child before deleting it leaves the list wholly consistent at the
    //! moment the destructor runs, so a sibling deleted from there unlinks itself normally, and
    //! re-reading mFirstChild afterwards picks up whatever is left.
    //!
    //! The loop terminates even against a destructor that tries to attach something new to the
    //! parent being destroyed: ~Object() clears the life flag before it gets here, and setParent()
    //! refuses a dead parent. Qt tolerates that append instead, by re-reading children.size().
    //!
    //! The child's own ~Object() calls detachFromParent() too; it is a no-op by then, because this
    //! already unlinked it and cleared its parent pointer.
    //!
    //! **A child must be heap-allocated.** This calls `delete` on it. Standard C++ cannot ask
    //! whether a pointer names automatic or dynamic storage, so nothing here can check it -- Qt
    //! documents the same hazard and warns rather than preventing it ("If any of these objects are
    //! on the stack or global, sooner or later your program will crash", qobject.cpp:1030).
    void Object::deleteChildren()
    {
        Extras* selfExtras = extrasOrNull();
        if( selfExtras == nullptr )
        {
            return;
        }

        while( Object* child = selfExtras->mFirstChild )
        {
            child->detachFromParent();
            delete child;
        }
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
        // fetch_or rather than the load-then-set setFlag() uses: this is the one flag whose
        // caller needs to know whether it was the one that set it, so the read-modify-write is the
        // operation, not an optimisation to skip.
        if( ( mFlags.fetch_or( kDeleteLaterPosted, std::memory_order_acq_rel )
            & kDeleteLaterPosted ) != 0 )
        {
            return;
        }

        auto* event = new DeferredDeleteEvent();
        if( ThreadData* const tData = threadData() )
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
                    setFlag( kMayHaveQueuedWork );
                    if( disp->postEvent( this, static_cast<Event*>( event ),
                        OverflowPolicy::DropNewest, EventPriority::kNormal ) )
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
            qCWarning( gLogTimer )
                << "Object::startTimer: interval cannot be negative";
            return -1;
        }

        // Thread-confined, as in Qt. The timer lives in the dispatcher belonging to this object's
        // thread, and only that thread's event loop can ever deliver the resulting timerEvent().
        // Registering from elsewhere would either race that dispatcher's lifetime or quietly
        // install a timer whose events the caller is not positioned to receive, so refuse it
        // outright rather than doing something surprising.
        if( thread() != Thread::currentThread() )
        {
            qCWarning( gLogTimer )
                << "Object::startTimer: timers cannot be started from another thread";
            return -1;
        }

        ThreadData* const data = threadData();
        if( !data )
        {
            qCWarning( gLogTimer )
                << "Object::startTimer: object has no thread, so the timer cannot be started";
            return -1;
        }

        auto dispatcher = data->dispatcher();
        if( !dispatcher )
        {
            qCWarning( gLogTimer )
                << "Object::startTimer: this thread has no event dispatcher, so the timer cannot"
                << "be started";
            return -1;
        }

        // Consumed only once the timer is certain to be registered, so no failure path above has
        // an id to give back.
        const int timerId = TimerIdPool::allocate();
        if( timerId < 0 )
        {
            qCWarning( gLogTimer ) << "Object::startTimer: no timer ids left";
            return -1;
        }

        dispatcher->registerTimer( timerId, aIntervalMs, this );
        {
            // The box first, then the flag: ~Object() tests the flag and then reads the box, so
            // publishing them in this order is what makes "flag set implies box exists" true.
            Extras& extras = ensureExtras();

            // Set before the push, so a destructor that reads it as false cannot be racing a push
            // it will then miss. See kUsedTimers.
            setFlag( kUsedTimers );

            // Recorded so ~Object() can hand the id back even if the timer is never killed.
            if( !extras.mRunningTimerIds )
            {
                extras.mRunningTimerIds.reset( new std::vector<int>() );
            }
            extras.mRunningTimerIds->push_back( timerId );
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
            qCWarning( gLogTimer )
                << "Object::killTimer: timers cannot be stopped from another thread";
            return;
        }

        const bool wasOurs = forgetTimerId( aTimerId );

        if( ThreadData* const data = threadData() )
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

    //! Gets how long until one of this object's timers next fires, in milliseconds. See the
    //! declaration.
    int Object::remainingTime
        (
        int aTimerId  //!< A timer id from startTimer().
        ) const
    {
        ThreadData* const data = threadData();
        if( !data )
        {
            return -1;
        }

        auto dispatcher = data->dispatcher();
        if( !dispatcher )
        {
            return -1;
        }

        return dispatcher->timerRemainingTimeMs( aTimerId );
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
        // No box, or no list in it, means no timer was ever started on this object, so there is
        // nothing to forget.
        Extras* extras = mExtras.load( std::memory_order_acquire );
        if( extras == nullptr || !extras->mRunningTimerIds )
        {
            return false;
        }

        std::vector<int>& ids = *extras->mRunningTimerIds;
        auto it = std::find( ids.begin(), ids.end(), aTimerId );
        if( it == ids.end() )
        {
            return false;
        }
        ids.erase( it );
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
        aContext->setFlag( kUsedCallLater );

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
            std::shared_ptr<Affinity> ctxAffinity = aContext->mAffinity;
            auto metaCall = [aKey, node, ctxAffinity]()
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
                        // A plain atomic load; see Affinity::isObjectAlive().
                        if( ctxAffinity->isObjectAlive() )
                        {
                            fnToRun();
                        }
                    }
                };

            if( !dispatchMetaCall( aContext, metaCall, ConnectionType::Queued ) )
            {
                // The target has no dispatcher yet, so this call can never run. Drop the registry
                // entry we just created: leaving it behind is what made this failure permanent,
                // since every later callLater() for the same target would find it, take the
                // "already scheduled" branch above, and never dispatch again -- silently disabling
                // that (context, slot) pair for the rest of the object's life, even once a
                // dispatcher existed. Erasing lets the next call re-arm. This call is still lost;
                // only a retry queue could save it, which would need its own ownership rules.
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
        ThreadData* aData
        )
    {
        // Bare pointers on both sides, and no ownership anywhere near this. It used to take a
        // shared_ptr by reference and call .get(), which was already avoiding an atomic increment
        // and decrement per Auto emit; now Affinity hands out a raw pointer and there is nothing
        // left to avoid.
        return aData == Thread::currentThread()->threadData();
    }

    //! Gets the thread data container holding this object's event dispatcher.
    //!
    //! Private: this is internal plumbing with no QObject equivalent -- Qt's
    //! QObjectPrivate::threadData is likewise not public API. It is the handle through which the
    //! dispatcher is reached, so exposing it hands out the machinery every other access-control
    //! decision in this class exists to protect. Returns nullptr if this object has no affinity.
    //! Thread-safe.
    ThreadData* Object::threadData() const
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
        ThreadData* aOldData,  //!< Thread the object is leaving; may be null.
        ThreadData* aNewData   //!< Thread it now lives on; may be null.
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
        // because a caller that never stops posting to a moving object is misusing it, and a
        // bounded loop is better than one that can be kept spinning.
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
                    = aNewData->dispatcherOrPark( this, event, EventPriority::kNormal );
                if( newDispatcher )
                {
                    // The priority the event was posted at is not carried across: the queue
                    // it is leaving holds that, and takeEventsForReceiver() hands back bare Event
                    // pointers. A move is rare and a demotion to kNormal is the conservative
                    // answer -- an event does not silently gain rank by changing threads. Carrying
                    // it would mean widening that hand-off, which is worth doing when something
                    // needs it.
                    //
                    // Unconditionally, because this is a move rather than a post: every one
                    // of these events was admitted once already, on the dispatcher it is leaving.
                    // Putting them through admission again would let moveToThread() silently
                    // destroy accepted work whenever the destination happened to sit near its
                    // ceiling, which is a surprising place to lose an event. The method deletes
                    // the event itself if it refuses, which it only does for a closed dispatcher.
                    // At kNormal: takeEventsForReceiver() hands back bare Event pointers, so
                    // the rank these were posted at is not recoverable here. A parked event does
                    // keep its rank, because ThreadData records it -- see ParkedEvent.
                    newDispatcher->postEventUnconditionally( this, event,
                        EventPriority::kNormal );
                }
            }
        }
    }

    //! Routes an event to its handler. Returns true if the event was recognised and handled.
    //!
    //! Deliberately private and non-virtual: this is not an extension point. The event queue is the
    //! sole caller (see the friend declaration in the header), and the set of event types is
    //! closed. Override timerEvent() instead to react to timers.
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

        // Event::MetaCall is deliberately absent, and dispatchEvent() is where it goes instead.
        // Routing one back through here would mean dereferencing the receiver to make this virtual
        // call, which is exactly what a queued emit must never do -- see dispatchEvent().

        default:
            // An application's own type, which this class knows nothing about. False means
            // unhandled, and an override that recognises the type is what turns it into true --
            // the same answer QObject::event() gives for a type no Qt class claims.
            //
            // Reached only through postEvent(), which admits nothing below Event::User. So the
            // three casts above can never be handed an event that is not of the class they name:
            // an application cannot construct one of those subclasses, and a plain Event carrying
            // an internal type is refused at the queue's edge rather than arriving here.
            return false;
        }
    }

    //! Delivers one dispatched event, choosing a route that does not touch a receiver that need not
    //! still be there. See the declaration.
    bool Object::dispatchEvent
        (
        Object* aReceiver,  //!< Receiver the event was queued against; never null, checked by the caller.
        Event* aEvent       //!< The event to deliver; never null, checked by the caller.
        )
    {
        if( aEvent->type() == Event::MetaCall )
        {
            // The receiver is not dereferenced here, and that is the whole reason this function
            // exists rather than the dispatcher calling event() straight.
            //
            // A queued emit names its receiver only as the key removeEventsForReceiver() matches
            // on. Everything the call actually needs -- the slot, the arguments, and the life token
            // that decides whether to run at all -- travels inside the event, so the pointer is
            // never followed. That is what makes the documented pattern safe: destroy a receiver on
            // one thread while its worker is still draining thousands of metacalls queued for it,
            // and each one finds the token dead and does nothing.
            //
            // Reading the receiver's vtable to make a virtual call would end that. event() is
            // virtual, so `aReceiver->event( aEvent )` loads the vptr -- from an object whose
            // destructor may be running concurrently on another thread. The load is a genuine race
            // rather than a pedantic one, and ThreadSanitizer reports it as such.
            static_cast<MetaCallEvent*>( aEvent )->placeMetaCall();
            return true;
        }

        // The remaining types dereference the receiver whatever route they take -- a timer reaches
        // the virtual timerEvent(), and a deferred delete runs the destructor -- so going through
        // the virtual hook costs them nothing they were not already paying. Both are posted by
        // Object's own internals against a receiver that has to be alive to be delivered to, which
        // is the contract ~Object() maintains by stripping the queue before it returns.
        return aReceiver->event( aEvent );
    }

    //! Posts an application's event to the receiver's thread. See the declaration.
    bool Object::postEvent
        (
        Object* aReceiver,       //!< Object to deliver to; null refuses the post.
        Event* aEvent,           //!< The event; owned from here on, null refuses the post.
        OverflowPolicy aPolicy,  //!< What to do if the receiving queue is full.
        int aPriority            //!< Higher runs first.
        )
    {
        if( !aEvent )
        {
            return false;
        }

        if( !aReceiver )
        {
            delete aEvent;
            return false;
        }

        // The check that keeps event()'s three casts sound. An application can reach Event's
        // protected constructor from its own subclass and pass MetaCall to it, and the result would
        // be dispatched by casting to MetaCallEvent -- a cast to a class the object is not.
        // Refusing here is what makes that unreachable rather than merely discouraged.
        if( !Event::isUserType( aEvent->type() ) )
        {
            qCWarning( gLogObject )
                << "postEvent refused a reserved event type "
                << static_cast<int>( aEvent->type() )
                << "; an application's own types start at Event::User ("
                << static_cast<int>( Event::User ) << ").";
            delete aEvent;
            return false;
        }

        // Read here, once, and handed on as a plain pointer: a ThreadData outlives the process's
        // use of it, so the answer cannot go stale underneath the post the way a Thread* could.
        // This is the one place the receiver is dereferenced, which is why it must be alive for the
        // duration of this call but not until delivery.
        ThreadData* const data = aReceiver->threadData();
        if( !data )
        {
            // No affinity, so no loop that would ever run this. Dropped rather than run inline on
            // the calling thread: delivering to an object that lives nowhere, on whichever thread
            // happened to post, is precisely the surprise moveToThread( nullptr ) exists to avoid.
            delete aEvent;
            return false;
        }

        return postEventTo( data, aReceiver, aEvent, aPolicy, aPriority );
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
            // MetaCallEvent::create() moves it into the event's own block, so copying here would
            // buy a second heap allocation on every queued emit for nothing.
            return dispatchMetaCallTo( aTarget->threadData(), aTarget, std::move( aSlot ) );
        }

        aSlot();
        return true;
    }

    //! Queues, parks or discards an event on an explicitly named thread.
    //!
    //! Everything dispatchMetaCallTo() does that does not depend on the callable's type, split out
    //! so that only the allocation is a template, and the same body the public postEvent() reaches
    //! once it has resolved the receiver's affinity. Both callers have already rejected a null
    //! @p aData, so reaching here means there is a thread to aim at.
    //!
    //! Takes ownership of @p aEvent on every path: it is handed to the dispatcher, parked, or
    //! deleted here.
    //!
    //! Thread-safe. @p aReceiver is not dereferenced *by this function*: it is handed to the
    //! dispatcher's postEvent() purely as the key that removeEventsForReceiver() later matches on.
    //! It is dereferenced afterwards, when the dispatcher drains the queue and calls
    //! aReceiver->event(), so the receiver has to still be alive at that point. What guarantees
    //! that is ~Object(), which calls removeEventsForReceiver() and deletes every event still
    //! queued for the object before it goes away.
    //!
    //! Returns true if the call was queued; false if the thread has no dispatcher, in which case
    //! the call is dropped.
    bool Object::postEventTo
        (
        ThreadData* aData,       //!< Thread to deliver on; never null, checked by the caller.
        Object* aReceiver,       //!< Receiver; the queue key here.
        Event* aEvent,           //!< The built event; owned from here on.
        OverflowPolicy aPolicy,  //!< What to do if the receiving queue is full.
        int aPriority            //!< Higher runs first.
        )
    {
        // A thread that is running but has not installed a dispatcher yet is in the window between
        // start() -- which publishes isThreadRunning() before the OS thread exists -- and run()
        // creating the dispatcher on that new thread. A call posted into that window used to be
        // dropped on the floor: this function asked for dispatcher(), got null, and returned false.
        // Thread::post() passes that false straight back to a caller that has no way to retry,
        // every queued emit and every callLater() goes through here, and none of them expects a
        // call to simply vanish because the receiving thread was still starting up. Park instead,
        // and setDispatcher() hands the queue over the moment the dispatcher appears.
        //
        // Gated on isThreadRunning() rather than parking whenever there is no dispatcher, because
        // the *other* way to have no dispatcher is to have finished: run() clears the running flag
        // (Thread.cpp:193) before it releases the dispatcher (Thread.cpp:222), so a stopped thread
        // still reports false here and keeps the old refuse-and-report behaviour. Parking there
        // would accept work into a queue nothing will ever drain, which is precisely the leak
        // deleteLater() takes its own separate path to avoid.
        const bool threadIsComingUp = aData->isThreadRunning();

        // Set before the event is handed anywhere, parked or queued, because it is what tells
        // ~Object() that this receiver may have work to strip -- and the strip covers the parked
        // list as well as the dispatcher's queue.
        if( aReceiver )
        {
            aReceiver->setFlag( kMayHaveQueuedWork );
        }

        if( threadIsComingUp )
        {
            // Takes ownership of the event only when it parks; a dispatcher that appeared while we
            // were getting here is handed back instead, and then the post below is the normal path.
            if( auto disp = aData->dispatcherOrPark( aReceiver, aEvent, aPriority ) )
            {
                return disp->postEvent( aReceiver, aEvent, aPolicy, aPriority );
            }
            return true;
        }

        if( auto disp = aData->dispatcher() )
        {
            return disp->postEvent( aReceiver, aEvent, aPolicy, aPriority );
        }

        delete aEvent;
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
        if( mOwner == nullptr || !mOwnerLife || !mOwnerLife->isObjectAlive() )
        {
            return;
        }

        std::lock_guard<Object::Lock> lock( mOwner->mIncomingMutex );
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
        // disconnect loop non-re-entrant: it clears the life flag before disconnecting, so this
        // returns before it can ask for a mutex ~Object() is holding.
        if( mOwner == nullptr || !mOwnerLife || !mOwnerLife->isObjectAlive() )
        {
            return;
        }

        std::lock_guard<Object::Lock> lock( mOwner->mIncomingMutex );
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
