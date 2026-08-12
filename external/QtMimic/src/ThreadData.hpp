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
//! timer list. Posting therefore goes through the ThreadData -- which the poster keeps alive with a
//! shared_ptr -- and never dereferences a Thread* that a concurrent ~Thread() could free.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_THREADDATA_HPP
#define QT_MIMIC_THREADDATA_HPP

#include "AbstractEventDispatcher.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace QtMimic
{
    class Thread;

    //----------------------------------------------------------------
    //! @class ThreadData
    //!
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
    //! ~Thread(). Capturing a raw Thread* instead -- for an auto-adopted Thread destroyed at native
    //! thread exit, or an explicit user-owned Thread destroyed whenever the user likes -- produced a
    //! real, AddressSanitizer-confirmed heap-use-after-free.
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

    private:
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

        friend class Object;
        friend class Thread;
        friend class CoreApplication;
    };

    //----------------------------------------------------------------
    //! @class Affinity
    //!
    //! One Object's thread affinity, held in a separately-allocated box so it can be read from any
    //! thread at any time -- including after the Object it describes has been destroyed.
    //!
    //! Why this exists, in one sentence: a queued connection has to resolve the receiver's affinity
    //! at EMIT time (that is what makes moveToThread() affect connections made before it), but the
    //! receiver may be destroyed concurrently, and boost::signals2 does not make disconnect() wait
    //! for an in-flight emit -- so reading thread()/threadData() straight off the receiver Object is
    //! a use-after-free. The connect() wrapper's weak_ptr<int> life-token check narrows that window
    //! but does not close it: a successful lock() only proves ~Object() had not yet reached
    //! mLife.reset() at the moment of the check, not that it cannot start immediately afterward,
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
        //! Guards mData: moveToThread() writes from the object's own thread while emits read from
        //! any other. The same reason QObject keeps its threadData in a QAtomicPointer.
        mutable std::mutex mMutex;
        std::shared_ptr<ThreadData> mData;
    };

} // namespace QtMimic

#endif // QT_MIMIC_THREADDATA_HPP
