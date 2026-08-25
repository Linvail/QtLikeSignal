// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Per-thread state that outlives its Thread object.
//!
//! ThreadData is the key to safe thread affinity without dangling pointers. Objects hold
//! shared_ptr<ThreadData>, not raw Thread*. When the Thread is destroyed, ~Thread() nulls the
//! back-pointer, so any surviving holder sees thread() == nullptr rather than a use-after-free.
//! This is exactly how Qt's QThreadData/QObject::thread() works internally.
//!
//! ThreadData also OWNS the thread's event dispatcher, which in turn owns the event queue and the
//! timer list. Posting therefore goes through the ThreadData and never dereferences a Thread* that a
//! concurrent ~Thread() could free.
//!
//! **A ThreadData is never destroyed before the process is.** ThreadData::create() files a strong
//! reference in a registry that is deliberately never emptied, so a raw ThreadData* is valid from
//! any thread at any time, forever. That is what lets Affinity hold one in a plain atomic pointer
//! instead of a shared_ptr behind a mutex -- 48 bytes off every Object, and an atomic load rather
//! than a lock on the path every cross-thread emit takes (P2 in history/PERFORMANCE-20260813.md).
//!
//! What that costs is bounded and small: sizeof(ThreadData) plus a control block, once per Thread
//! ever *created*, held for the life of the process. Nothing else leaks with it -- the dispatcher
//! is a separate shared_ptr and is still released the moment the thread's loop ends, which is where
//! the memory that matters actually lives. The registry is never recycled, deliberately: handing a
//! retired ThreadData to a new thread would silently re-home every Object still pointing at it,
//! which is a correctness bug where the alternative is only a few dozen bytes.

#ifndef QT_LIKE_SIGNAL_THREADDATA_HPP
#define QT_LIKE_SIGNAL_THREADDATA_HPP

#include "QtLikeSignal/AbstractEventDispatcher.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace QtLikeSignal
{
    class Event;
    class Object;
    class Thread;

    //----------------------------------------------------------------
    //! Per-thread state owning that thread's event dispatcher.
    //!
    //! Handed out by Object::threadData()/Thread::threadData() as an opaque handle. The dispatcher
    //! is held by shared_ptr and only ever reachable through dispatcher(), which hands back a
    //! *strong* reference. That is what makes cross-thread use safe: a thread finishing can drop its
    //! dispatcher at any moment, and an atomic raw pointer would only have made the pointer load
    //! safe, not the object's lifetime -- the owning thread could free it between another thread's
    //! load and its call. Holding a strong reference for the duration of the call keeps it alive
    //! until that caller is done.
    //!
    //! Also outlives its Thread: mThread is set once by Thread's constructor and nulled by
    //! ~Thread(), so anything holding only this data sees thread() == nullptr rather than a dangling
    //! Thread*. Capturing a raw Thread* instead is not safe: an adopted Thread is destroyed at
    //! native thread exit, and a user-owned one whenever its owner likes.
    //!
    //! Lifetime is managed with shared_ptr rather than Qt's intrusive refcount. Note this class
    //! deliberately does NOT own its Thread: Qt's QThreadData does own the adopted QThread and
    //! deletes it, which forces Qt into a documented refcount hack ("the refcount will become
    //! negative, but that's acceptable") to break the resulting cycle. Nulling the back-pointer
    //! achieves the same safety with no cycle to break.
    //!
    //! Access is private on purpose: a writable dispatcher handle would let outside code redirect a
    //! running loop or drop a dispatcher still in use. Only the classes that legitimately manage a
    //! thread's lifecycle are granted access.
    //----------------------------------------------------------------
    struct ThreadData
    {
    public:
        ThreadData() = default;

        //! Creates a ThreadData and files it in the process-wide registry.
        //!
        //! The only way one should be made. Filing a strong reference is what makes every raw
        //! ThreadData* in this library valid forever, which is what lets Affinity hold one in a
        //! plain atomic pointer. See this file's header for the reasoning and the cost.
        static std::shared_ptr<ThreadData> create();

        //! Frees any events parked for a dispatcher that never arrived. See mParkedEvents.
        ~ThreadData();

        ThreadData
            (
            const ThreadData&
            ) = delete;

        ThreadData& operator=
            (
            const ThreadData&
            ) = delete;

    private:
        Thread* thread() const;

        void setThread
            (
            Thread* aThread
            );

        bool isThreadRunning() const;

        void setThreadRunning
            (
            bool aRunning
            );

        std::shared_ptr<AbstractEventDispatcher> dispatcher() const;

        void setDispatcher
            (
            std::shared_ptr<AbstractEventDispatcher> aDispatcher
            );

        //! Hands back the dispatcher to post @p aEvent to, or parks the event and returns nullptr.
        //!
        //! One call rather than "ask, then decide", so the answer cannot change in between.
        //! Ownership of @p aEvent passes to this ThreadData when it is parked.
        std::shared_ptr<AbstractEventDispatcher> dispatcherOrPark
            (
            Object* aReceiver,
            Event* aEvent
            );

        //! Drops any parked events for @p aReceiver, called by ~Object() before it dies.
        void removeParkedEventsFor
            (
            Object* aReceiver
            );

        std::atomic<Thread*> mThread { nullptr };  //!< Owning thread; nulled by ~Thread().

        //! True while the owning thread's body is executing.
        //!
        //! Lives here rather than in Thread so that "is this object's thread still running?" can be
        //! answered from any thread without dereferencing a Thread* that a concurrent ~Thread()
        //! could free -- the whole point of ThreadData outliving its Thread. Thread::isRunning()
        //! reads through to this, so there is one source of truth rather than a mirror that could
        //! drift.
        std::atomic<bool> mThreadRunning { false };

        mutable std::mutex mDispatcherMutex;                    //!< Guards mDispatcher.
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;  //!< This thread's dispatcher, if any.

        //! One event waiting for this thread to have a dispatcher at all.
        struct ParkedEvent
        {
            Object* mReceiver;
            Event*  mEvent;
        };

        //! Events moved here by Object::moveToThread() before this thread had a dispatcher.
        //!
        //! The canonical idiom builds a Thread, moves objects onto it, and only then calls start()
        //! -- and a Thread has no dispatcher until its run body creates one. Dropping the events
        //! loses work silently; leaving them behind runs them on the thread the object just left.
        //! The dispatcher drains this the moment it is installed.
        std::vector<ParkedEvent> mParkedEvents;

        friend class Object;
        friend class Thread;
        friend class CoreApplication;

        //! Timer::singleShot() validates a context's thread before arming against it.
        friend class Timer;

        //! namesOtherRunningThread() answers ~Object()'s diagnostic without copying a shared_ptr
        //! out, which means asking these two accessors from inside the Affinity's own mutex.
        friend class Affinity;
    };

    //----------------------------------------------------------------
    //! One Object's thread affinity, held in a separately-allocated box so it can be read from any
    //! thread at any time -- including after the Object it describes has been destroyed.
    //!
    //! Why this exists, in one sentence: a queued connection has to resolve the receiver's affinity
    //! at EMIT time (that is what makes moveToThread() affect connections made before it), but the
    //! receiver may be destroyed concurrently, and disconnect() does not wait
    //! for an in-flight emit -- so reading thread()/threadData() straight off the receiver Object is
    //! a use-after-free. The connect() wrapper's life-token check narrows that window but does not
    //! close it: seeing the object alive only proves ~Object() had not yet reached
    //! markObjectDead() at the moment of the check, not that it cannot start immediately afterward,
    //! concurrently with this thread going on to dereference the receiver's own members.
    //!
    //! The box breaks that dependency: connect() captures a shared_ptr<Affinity> at CONNECT time,
    //! and the wrapper resolves affinity through the box at EMIT time, never through the receiver.
    //! Because the box is independently heap-allocated and kept alive by the closure's own
    //! shared_ptr, it is safe to read no matter what has happened to the Object by then.
    //----------------------------------------------------------------
    class Affinity
    {
    public:
        explicit Affinity
            (
            ThreadData* aData   //!< Thread this object lives in; null for none. Never freed.
            )
            : mData( aData )
        {
        }

        Affinity
            (
            const Affinity&
            ) = delete;

        Affinity& operator=
            (
            const Affinity&
            ) = delete;

        //! @return the current affinity, or null. Safe from any thread, at any time, including
        //! after the Object this describes has been destroyed.
        //!
        //! A raw pointer, and it needs no lifetime story of its own: a ThreadData is never
        //! destroyed before the process is. See this file's header for what that buys and costs.
        //! This used to copy a shared_ptr under a mutex, which is what P2 measured at 5-6 ns per
        //! call and 32x degradation at eight threads.
        ThreadData* data() const
        {
            return mData.load( std::memory_order_acquire );
        }

        //! @return false once ~Object() has begun on the object this box describes.
        //!
        //! The life token, folded into the box that already had to exist. It used to be a separate
        //! `std::shared_ptr<int>` on the Object, read through a `weak_ptr` -- a second heap block
        //! per Object, a second free per destruction, and a second capture in the closure of every
        //! connection, all to carry one bit. This box was already allocated, already captured by
        //! those same closures, and already outlives the Object by design.
        //!
        //! A plain atomic load, and deliberately nothing that hands back a strong reference. What
        //! it answers is "had destruction begun at the instant of the check", which is exactly what
        //! `weak_ptr::expired()` answered before it -- and, as before, neither closes the
        //! check-then-use race that follows. What actually stops a destroyed receiver being called
        //! is ~Object() disconnecting its incoming connections and stripping its queued events.
        bool isObjectAlive() const
        {
            return mObjectAlive.load( std::memory_order_acquire );
        }

        //! Records that the object this box describes is being destroyed.
        //!
        //! Called once, at the top of ~Object(), before anything that can run user code. Release
        //! ordering so that a reader seeing `false` also sees everything the destructor did before
        //! saying so.
        void markObjectDead()
        {
            mObjectAlive.store( false, std::memory_order_release );
        }

        //! @return true when this affinity names a *running* thread that is not @p aCaller.
        //!
        //! The question ~Object()'s safety diagnostic asks, answered without handing a strong
        //! reference back out. data() copies a shared_ptr, so asking it cost two atomic
        //! read-modify-writes on top of the mutex, on a path every destruction takes -- and the
        //! copy existed only to keep alive, for three instructions, something this mutex already
        //! keeps alive.
        //!
        //! The Thread* is compared, never dereferenced, which is the same rule data()'s callers
        //! follow and the reason this cannot resurrect the dangling-pointer hazard the Affinity
        //! indirection exists to remove.
        bool namesOtherRunningThread
            (
            const Thread* aCaller   //!< The thread asking; compared, never dereferenced.
            ) const
        {
            const ThreadData* data = mData.load( std::memory_order_acquire );
            return data && data->isThreadRunning() && data->thread() != aCaller;
        }

        //! Re-point at another thread's data. Called by Object::moveToThread() and by
        //! Thread::bindAffinityToSelf(), which is why this is a store and not merely a write: the
        //! second of those runs on the *new* thread, so the write genuinely races the reads.
        void setData
            (
            ThreadData* aData   //!< Thread to move to; null for none. Never freed.
            )
        {
            mData.store( aData, std::memory_order_release );
        }

    private:
        //! The thread this object lives in, or null.
        //!
        //! An atomic pointer rather than a shared_ptr behind a mutex, which is exactly what
        //! QObjectPrivate does with its QAtomicPointer<QThreadData>. Safe only because a ThreadData
        //! outlives the process's use of it by construction -- see this file's header. Release on
        //! the store and acquire on the load, so a reader that sees the new thread also sees
        //! everything moveToThread() did to the dispatchers before publishing it.
        std::atomic<ThreadData*> mData;

        //! False once ~Object() has begun on the object this box describes. See isObjectAlive().
        //!
        //! Outside mMutex on purpose: it is written once and read constantly, by closures that have
        //! no other reason to touch the mutex. An atomic keeps that read a load rather than a lock.
        std::atomic<bool> mObjectAlive { true };
    };

} // namespace QtLikeSignal

#endif // QT_LIKE_SIGNAL_THREADDATA_HPP
