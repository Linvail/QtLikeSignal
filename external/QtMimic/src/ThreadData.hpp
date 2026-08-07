//! @file
//!
//! Per-thread state that outlives its Thread object.
//!
//! ThreadData is the key to safe thread affinity without dangling pointers.
//! Objects hold shared_ptr<ThreadData>, not raw Thread*. When the Thread
//! is destroyed, ~Thread() nulls the back-pointer, so any surviving holder
//! sees thread() == nullptr rather than a use-after-free. This is exactly
//! how Qt's QObject::thread() works internally.
//!
//! ThreadData also OWNS the event mailbox (the pending-task queue and its
//! synchronization), mirroring Qt's QThreadData::postEventList. Posting a task
//! therefore goes through the ThreadData -- which the poster keeps alive with a
//! shared_ptr -- and never dereferences a Thread* that a concurrent ~Thread()
//! could free.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_THREADDATA_HPP
#define QT_MIMIC_THREADDATA_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace QtMimic
{
    class Thread;

    //----------------------------------------------------------------
    //! @class ThreadData
    //!
    //! Per-thread state that OUTLIVES the Thread object describing it, modelled on Qt's
    //! QThreadData. This inversion is the whole point of the class:
    //!
    //!   - An Object holds a shared_ptr to the ThreadData of the thread it lives in -- never a raw
    //!     Thread*. So does anything else that needs to know "which thread does this belong to",
    //!     notably a queued connection's captured context.
    //!   - ThreadData holds a plain back-pointer to its Thread, cleared by ~Thread().
    //!
    //! The result is that destroying a Thread can never leave a dangling pointer behind: every
    //! holder still has a live ThreadData, and thread() simply starts returning nullptr. Qt relies
    //! on exactly this -- QObject::thread() reads
    //! `d_func()->threadData.loadRelaxed()->thread.loadAcquire()`, where the QThreadData is
    //! refcounted and ~QThread() does `d->data->thread.storeRelease(nullptr)`.
    //!
    //! Before this existed, QtMimic stored a raw Thread* in Object and captured it into connection
    //! closures, then dereferenced it (isCurrent()/post()) on every emit. Both an auto-adopted dummy
    //! Thread (destroyed at native thread exit) and an explicit user-owned Thread (destroyed
    //! whenever the user likes) produced a real, AddressSanitizer-confirmed heap-use-after-free in
    //! Thread::isCurrent().
    //!
    //! Lifetime is managed with shared_ptr rather than Qt's intrusive refcount. Note this class
    //! deliberately does NOT own its Thread: Qt's QThreadData does own the adopted QThread and
    //! deletes it, which forces Qt into a documented refcount hack ("the refcount will become
    //! negative, but that's acceptable") to break the resulting cycle. Nulling the back-pointer
    //! achieves the same safety with no cycle to break.
    //----------------------------------------------------------------
    class ThreadData
    {
    public:
        ThreadData() = default;

        ThreadData
            (
            const ThreadData&
            ) = delete;

        ThreadData& operator=
            (
            const ThreadData&
            ) = delete;

        Thread* thread() const;

        bool post
            (
            std::function<void()> aTask
            );

    private:
        void setThread
            (
            Thread* aThread
            );

        void requestStop();

        void prepareForRun();

        void stopAccepting();

        void setWakeCallback
            (
            std::function<void()> aWake
            );

        // Private data:
        std::deque<std::function<void()> > takeAll();

        std::atomic<Thread*> mThread { nullptr };

        //! Event mailbox (mirrors Qt's QThreadData::postEventList). Owned here, not in Thread, so
        //! that posting is decoupled from the Thread's lifetime. The Thread's event loop (a friend)
        //! drains it under mMutex; post()/requestStop() feed it and wake the loop.
        std::mutex mMutex;                          //!< Protects the mailbox state below
        std::condition_variable mWake;              //!< Wakes the loop on new work / stop
        std::deque<std::function<void()> > mTasks;  //!< Pending tasks (FIFO)
        bool mRunning { true };                      //!< Loop should keep iterating
        bool mAccepting { true };                    //!< post() accepts tasks (false once finished)
        std::function<void()> mWakeCb;              //!< Wakes an external loop on post (optional)

        friend class Thread;
    };

    //----------------------------------------------------------------
    //! @class Affinity
    //!
    //! One Object's thread affinity, held in a separately-allocated box so that it can be read from
    //! any thread without touching the Object itself.
    //!
    //! Why this exists, in one sentence: a queued connection has to resolve the receiver's affinity
    //! at EMIT time (that is what makes moveToThread() work as Qt documents it), but the receiver may
    //! be destroyed concurrently, and boost::signals2 does not make disconnect() wait for an
    //! in-flight emit -- so resolving through the receiver is a use-after-free.
    //!
    //! That is not a theoretical claim. boost keeps the *slot* alive during invocation via
    //! connection_body_base's m_slot_refcount, but signal_impl::operator() copies the connection list
    //! and RELEASES the signal mutex before invoking anything, and slot_call_iterator scopes its own
    //! lock to the "is it still connected" check rather than holding it across the call. So
    //! ~Object() -> disconnect() returns immediately and the Object is freed while another thread is
    //! still inside the slot. A standalone probe (256 connections, 4 emitting threads, receiver
    //! created and deleted on its own Thread) reproduced a heap-use-after-free under
    //! AddressSanitizer on the very first round, reading the shared_ptr inside
    //! Object::threadDataRef() from boost's slot_call_iterator::dereference().
    //!
    //! The box breaks that dependency: the connection's closure holds a shared_ptr to the Affinity,
    //! not to (or through) the Object, so the thing it dereferences on every emit is guaranteed alive
    //! no matter when the receiver dies. The mutex guarding the mutable field lives in the box too,
    //! which is the whole point -- a mutex inside the Object would itself be the freed memory.
    //!
    //! Qt solves the same problem differently, because it can. QObjectPrivate::Connection carries its
    //! own `QAtomicPointer<QThreadData> receiverThreadData` (qobject_p_p.h), doActivate() reads THAT
    //! rather than receiver->d_func()->threadData (qobject.cpp), and moveToThread() pushes the new
    //! value into every incoming connection by walking cd->senders in
    //! QObjectPrivate::setThreadData_helper() (qobject.cpp). That push model is not available here:
    //! QtMimic's connections are opaque boost handles with no per-connection storage to update and no
    //! way to reach into a live slot's captures. Pulling through a shared box gives the same
    //! guarantee -- the affinity a connection reads is always current, and always valid memory --
    //! with one level of indirection instead.
    //----------------------------------------------------------------
    class Affinity
    {
    public:
        explicit Affinity
            (
            std::shared_ptr<ThreadData> aData
            )
            : mData( std::move( aData ) )
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

        //! @return a strong reference to the current affinity. Safe from any thread, at any time,
        //! including after the Object this describes has been destroyed.
        std::shared_ptr<ThreadData> data() const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            return mData;
        }

        //! Re-point at another thread's data. Called only by Object::moveToThread().
        void setData
            (
            std::shared_ptr<ThreadData> aData
            )
        {
            std::lock_guard<std::mutex> locker( mMutex );
            mData = std::move( aData );
        }

    private:
        //! Guards mData: moveToThread() writes from the object's own thread while emits read from any
        //! other. The same reason QObject keeps its threadData in a QAtomicPointer.
        mutable std::mutex mMutex;
        std::shared_ptr<ThreadData> mData;
    };

} // namespace QtMimic

#endif // QT_MIMIC_THREADDATA_HPP
