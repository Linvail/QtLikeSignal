
#ifndef QT_MIMIC_TEST_TYPES
#define QT_MIMIC_TEST_TYPES

//! @file
//!
//! GoogleTest suite for the QtMimic framework.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Object.hpp"
#include "Thread.hpp"

// ---------------------------------------------------------------------------------------------
// Feature macros.
//
// The two suites hold the same tests in the same order (see
// history/TEST-UNIFICATION-PLAN-20260810.md). Where one library has an API the other does not, the
// test is still written in *both* files and guarded by one of these, so the absence is visible on
// the side that lacks it instead of the test simply not existing there.
//
// Add a macro here rather than deleting a test from one file.
// ---------------------------------------------------------------------------------------------
#define LIB_HAS_CALL_LATER                 0  //!< Object::callLater()
#define LIB_HAS_EVENT_DISPATCHER           0  //!< Thread::eventDispatcher(), the Event types
#define LIB_HAS_CLEANUP_CALLBACKS          0  //!< Object::addCleanupCallback()
#define LIB_HAS_VIRTUAL_RUN                1  //!< Thread::run() is virtual and subclassable
#define LIB_HAS_THREAD_CREATE              0  //!< Thread::create()
#define LIB_HAS_THREAD_IS_RUNNING          0  //!< Thread::isRunning()/isFinished()
#define LIB_HAS_WAIT_TIMEOUT               0  //!< Thread::wait( ms ) returning bool
#define LIB_HAS_POST_REJECTED_BEFORE_START 0  //!< post() fails until the dispatcher exists
#define LIB_HAS_SHUTDOWN_DEFERRED_DELETE   0  //!< pending deleteLater() runs at app shutdown
#define LIB_HAS_EXTERNAL_DISPATCHER        1  //!< Thread::setDispatcher()/setWaiter()

#include "gtest/gtest.h"
#include <chrono>
#include <future>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    class Producer : public QtMimic::Object
    {
    public:
        explicit Producer
            (
            QtMimic::Thread* aThread = nullptr
            )
            : QtMimic::Object( aThread )
        {
        }

        QtMimic::Signal<int> produced;
        QtMimic::Signal<int, int> produced2Args;
    };

    class Consumer : public QtMimic::Object
    {
    public:
        explicit Consumer
            (
            QtMimic::Thread* aThread = nullptr
            )
            : QtMimic::Object( aThread )
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
            QtMimic::Thread* aThread = nullptr
            )
            : Consumer( aThread )
        {
        }

        QtMimic::SignalView<int>& getSignalView() const
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
        QtMimic::Signal<int> mPrivateSignal;
    };

    // Receiver whose slot bumps an atomic counter that is owned by the caller and
    // therefore outlives the receiver. This lets a test observe (after the receiver
    // is destroyed) whether the slot ever ran.
    class ExternalCounter : public QtMimic::Object
    {
    public:
        ExternalCounter
            (
            QtMimic::Thread* aThread,
            std::atomic<int>& aCounter
            )
            : QtMimic::Object( aThread )
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

    class DeleteProbe : public QtMimic::Object
    {
    public:
        DeleteProbe
            (
            QtMimic::Thread* aThread,
            std::mutex& aMutex,
            std::condition_variable& aCv,
            bool& aDone,
            std::thread::id& aDtorThread,
            std::atomic<int>& aDtorCount
            )
            : QtMimic::Object( aThread )
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


    // -----------------------------------------------------------------------------------------
    // Shared helper vocabulary.
    //
    // Defined here, once per suite, so every test file on both sides can be written against the
    // same names. Anything that has to differ between the libraries belongs in this header and
    // nowhere else -- that is what lets the .cpp bodies be identical text.
    // -----------------------------------------------------------------------------------------

    //! How long a test waits for something that should happen within a few event-loop turns. Long
    //! enough that a loaded CI box does not fail spuriously, short enough that a genuine hang is
    //! still a test failure rather than a timeout at a higher level.
    constexpr auto kPatience = std::chrono::seconds( 5 );

    //! Posts a task and blocks until it has run, which can only happen once the thread's loop is
    //! actually draining its queue. Stronger than any isRunning()-style flag: it proves the loop
    //! is servicing work, which is what every test here depends on.
    //!
    //! Retries rather than posting once: a thread whose loop has not come up yet may refuse the
    //! task outright, which is a "not ready" answer and not a failure.
    //! @return true if the thread drained the marker task within the timeout.
    inline bool waitUntilRunning
        (
        QtMimic::Thread& aThread,  //!< The thread to wait for.
        int aTimeoutMs = 5000    //!< How long to wait before giving up.
        )
    {
        std::promise<void> ran;
        std::future<void> ranFuture = ran.get_future();
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds( aTimeoutMs );

        while( !aThread.post( [&ran]()
            {
                ran.set_value();
            } ) )
        {
            if( std::chrono::steady_clock::now() > deadline )
            {
                return false;
            }
            std::this_thread::yield();
        }
        return ranFuture.wait_for( std::chrono::milliseconds( aTimeoutMs ) ) ==
               std::future_status::ready;
    }

    //! Runs @p aBody on @p aThread and blocks until it has finished. Timer start()/stop() and
    //! startTimer()/killTimer() are thread-confined, so most tests need to get onto the worker
    //! before touching one.
    template <typename Body> void runOnThread
        (
        QtMimic::Thread& aThread,  //!< The thread to run on.
        Body aBody               //!< The work to run there.
        )
    {
        std::promise<void> done;
        auto doneFuture = done.get_future();
        ASSERT_TRUE( aThread.post( [&aBody, &done]()
            {
                aBody();
                done.set_value();
            } ) ) << "worker refused the task.";
        ASSERT_EQ( doneFuture.wait_for( kPatience ), std::future_status::ready )
            << "worker never ran the task.";
    }

    //! Blocks until everything currently queued on @p aThread has been drained.
    //!
    //! Needed before quitting a worker that may still hold a single-shot helper's own
    //! deleteLater(): quitting while that delete is queued strands the object, which the leak
    //! checker then reports.
    inline void drainQueuedTasks
        (
        QtMimic::Thread& aThread  //!< The thread to drain.
        )
    {
        std::promise<void> drained;
        auto drainedFuture = drained.get_future();
        if( !aThread.post( [&drained]()
            {
                drained.set_value();
            } ) )
        {
            return;  // loop already stopped, so there is nothing left to drain
        }
        EXPECT_EQ( drainedFuture.wait_for( kPatience ), std::future_status::ready )
            << "worker event loop did not drain.";
    }

    //! Spins until @p aPredicate holds or kPatience runs out.
    //! @return the final value of the predicate.
    template <typename Predicate> bool waitFor
        (
        Predicate aPredicate  //!< Condition to wait for.
        )
    {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while( !aPredicate() )
        {
            if( std::chrono::steady_clock::now() > deadline )
            {
                return false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return true;
    }

}

#endif  // QT_MIMIC_TEST_TYPES