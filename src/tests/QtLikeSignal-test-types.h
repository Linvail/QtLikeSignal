
#ifndef QTLIKESIGNAL_TEST_TYPES
#define QTLIKESIGNAL_TEST_TYPES

//! @file
//!
//! Shared test fixtures for the QtLikeSignal suite.
//!
//! Deliberately parallel to QtMimic's QtMimic-test-types.hpp: same classes, same members, same
//! order, so a test written against one compiles against the other. See
//! history/TEST-UNIFICATION-PLAN-20260810.md.

#include "Object.h"
#include "Signal.h"
#include "Thread.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    class Producer : public QtLikeSignal::Object
    {
    public:
        explicit Producer
            (
            QtLikeSignal::Thread* aThread = nullptr
            )
            : QtLikeSignal::Object( aThread )
        {
        }

        QtLikeSignal::Signal<int> produced;
        QtLikeSignal::Signal<int, int> produced2Args;
    };

    class Consumer : public QtLikeSignal::Object
    {
    public:
        explicit Consumer
            (
            QtLikeSignal::Thread* aThread = nullptr
            )
            : QtLikeSignal::Object( aThread )
        {
        }

        void onProduced
            (
            int aValue
            )
        {
            mLast = aValue;
            mSlotThread = std::this_thread::get_id();
            ++mCount;
        }

        //! Const-qualified slot to verify connect() can resolve it.
        void onProducedConst
            (
            int aValue
            ) const
        {
            mLastConst = aValue;
        }

        //! Overloaded slot to verify connect() can resolve the correct one.
        void onProduced
            (
            int unused1,
            int unused2
            )
        {
            mLast = unused1 + unused2;
            mSlotThread = std::this_thread::get_id();
            ++mCount;
        }

        //! Non-void return type to verify connect() can resolve it.
        int onProducedReturnInt
            (
            int aValue
            )
        {
            mLast = aValue;
            mSlotThread = std::this_thread::get_id();
            ++mCount;
            return mLast;
        }

        std::atomic<int> mLast { 0 };
        std::atomic<int> mCount { 0 };
        std::thread::id mSlotThread;

        mutable std::atomic<int> mLastConst { 0 };
    };

    class ConsumerDerived : public Consumer
    {
    public:
        explicit ConsumerDerived
            (
            QtLikeSignal::Thread* aThread = nullptr
            )
            : Consumer( aThread )
        {
        }

        QtLikeSignal::SignalView<int>& getSignalView() const
        {
            return mPrivateSignal.view();
        }

        void emitPrivateSignal
            (
            int aValue
            )
        {
            mPrivateSignal.emit( aValue );
        }

    private:
        QtLikeSignal::Signal<int> mPrivateSignal;
    };

    // Receiver whose slot bumps an atomic counter that is owned by the caller and
    // therefore outlives the receiver. This lets a test observe (after the receiver
    // is destroyed) whether the slot ever ran.
    class ExternalCounter : public QtLikeSignal::Object
    {
    public:
        ExternalCounter
            (
            QtLikeSignal::Thread* aThread,
            std::atomic<int>& aCounter
            )
            : QtLikeSignal::Object( aThread )
            , mCounter( aCounter )
        {
        }

        void onProduced
            (
            int
            )
        {
            ++mCounter;
        }

        //! Same bookkeeping, in the no-argument shape a timer needs: Timer::singleShot()'s member
        //! overload and Timer::timeout both target a slot taking nothing.
        void onTimeout()
        {
            ++mCounter;
        }

    private:
        std::atomic<int>& mCounter;
    };

    class DeleteProbe : public QtLikeSignal::Object
    {
    public:
        DeleteProbe
            (
            QtLikeSignal::Thread* aThread,
            std::mutex& aMutex,
            std::condition_variable& aCv,
            bool& aDone,
            std::thread::id& aDtorThread,
            std::atomic<int>& aDtorCount
            )
            : QtLikeSignal::Object( aThread )
            , mMutex( aMutex )
            , mCv( aCv )
            , mDone( aDone )
            , mDtorThread( aDtorThread )
            , mDtorCount( aDtorCount )
        {
        }

        ~DeleteProbe() override
        {
            mDtorCount.fetch_add( 1 );
            {
                std::lock_guard<std::mutex> lock( mMutex );
                mDtorThread = std::this_thread::get_id();
                mDone = true;
            }
            mCv.notify_one();
        }

    private:
        std::mutex& mMutex;
        std::condition_variable& mCv;
        bool& mDone;
        std::thread::id& mDtorThread;
        std::atomic<int>& mDtorCount;
    };

}

#endif  // QTLIKESIGNAL_TEST_TYPES