//! @file
//!
//! How many times a signal's arguments get copied on the way to a slot.
//!
//! This is a behavioural contract, not a micro-optimisation: a signal carrying anything bigger than
//! an int pays these copies on every emit, per receiver. The counts are asserted exactly rather than
//! bounded, so a regression shows up as a number rather than as a vague slowdown nobody notices.
//!
//! Modelled on QtMimic's DeepArgumentCopying tests, which cover the same contract for that library.

#include <gtest/gtest.h>
#include "Object.hpp"
#include "Signal.hpp"
#include "Thread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! Counts every copy made of itself, process-wide.
    //!
    //! Moves are counted separately: moving is cheap and expected, copying is what costs, and
    //! conflating them would hide the thing these tests exist to measure.
    struct CopyCountingPayload
    {
        static std::atomic<int> sCopies;   //!< Copy constructions since the last reset.
        static std::atomic<int> sMoves;    //!< Move constructions since the last reset.

        //! Resets both counters.
        static void reset()
        {
            sCopies.store( 0 );
            sMoves.store( 0 );
        }

        CopyCountingPayload() = default;

        CopyCountingPayload
            (
            const CopyCountingPayload&
            )
        {
            sCopies.fetch_add( 1, std::memory_order_relaxed );
        }

        CopyCountingPayload
            (
            CopyCountingPayload&&
            ) noexcept
        {
            sMoves.fetch_add( 1, std::memory_order_relaxed );
        }

        CopyCountingPayload& operator=
            (
            const CopyCountingPayload&
            ) = delete;

        CopyCountingPayload& operator=
            (
            CopyCountingPayload&&
            ) = delete;

    };

    std::atomic<int> CopyCountingPayload::sCopies { 0 };
    std::atomic<int> CopyCountingPayload::sMoves { 0 };

    //! Receiver that accepts the payload by const reference, so it adds no copies of its own.
    class PayloadReceiver : public Object
    {
    public:
        //! Slot; deliberately does nothing with the payload.
        void onPayload
            (
            const CopyCountingPayload& aPayload
            )
        {
            ( void )aPayload;
            mCalls.fetch_add( 1, std::memory_order_relaxed );
        }

        //! How many times the slot has run.
        int calls() const
        {
            return mCalls.load( std::memory_order_relaxed );
        }

    private:
        std::atomic<int> mCalls { 0 };
    };

    //! Blocks until every task queued on @p aWorker before this call has been drained.
    //!
    //! notify_one() is called **inside** the lock, which matters: notifying after releasing it lets
    //! the waiter observe the predicate, return, and destroy this condition_variable while the
    //! worker is still inside notify_one(). ThreadSanitizer caught exactly that. Holding the lock
    //! across the notify means the waiter cannot reacquire the mutex -- and so cannot leave this
    //! function -- until the notify has finished.
    void drainWorker
        (
        Thread& aWorker
        )
    {
        std::mutex mutex;
        std::condition_variable done;
        bool drained = false;
        ASSERT_TRUE( aWorker.post( [&]()
            {
                std::lock_guard<std::mutex> lock( mutex );
                drained = true;
                done.notify_one();
            } ) );

        std::unique_lock<std::mutex> lock( mutex );
        ASSERT_TRUE( done.wait_for( lock, std::chrono::seconds( 5 ), [&drained]()
            {
                return drained;
            } ) );
    }

    constexpr int kReceivers = 100;
}

//! A direct connection must not copy the arguments at all.
//!
//! The slot is called synchronously with the values the emitter already has, so there is nothing to
//! store and nothing to copy -- Qt does not copy here either, passing a stack array of pointers to
//! the caller's own arguments.
//!
//! This did not hold until 2026-08-09. The connect() wrapper captured the arguments into a closure
//! *before* deciding how to deliver, so every receiver cost a copy even when the call was about to
//! be made inline. Reverting that fix makes this test report 100 copies instead of 0.
TEST( ObjectArgumentCopyingTest, DirectConnectionCopiesNothing )
{
    Signal<const CopyCountingPayload&> sig;

    std::vector<std::unique_ptr<PayloadReceiver> > receivers;
    for( int i = 0; i < kReceivers; ++i )
    {
        receivers.push_back( std::make_unique<PayloadReceiver>() );
        Object::connect( sig, receivers.back().get(), &PayloadReceiver::onPayload,
            ConnectionType::Direct );
    }

    CopyCountingPayload payload;
    CopyCountingPayload::reset();
    sig.emit( payload );

    EXPECT_EQ( CopyCountingPayload::sCopies.load(), 0 )
        << "a direct connection copied the argument; it is delivered synchronously, so there is "
        "nothing to store and nothing to copy.";
    for( const auto& r : receivers )
    {
        EXPECT_EQ( r->calls(), 1 );
    }
}

//! An Auto connection resolving to a same-thread call must not copy either.
//!
//! Same delivery as the direct case once the affinity check says "same thread", so the same
//! contract applies.
TEST( ObjectArgumentCopyingTest, SameThreadAutoConnectionCopiesNothing )
{
    Signal<const CopyCountingPayload&> sig;

    std::vector<std::unique_ptr<PayloadReceiver> > receivers;
    for( int i = 0; i < kReceivers; ++i )
    {
        receivers.push_back( std::make_unique<PayloadReceiver>() );
        Object::connect( sig, receivers.back().get(), &PayloadReceiver::onPayload,
            ConnectionType::Auto );
    }

    CopyCountingPayload payload;
    CopyCountingPayload::reset();
    sig.emit( payload );

    EXPECT_EQ( CopyCountingPayload::sCopies.load(), 0 )
        << "an Auto connection that resolved to a same-thread call copied the argument.";
}

//! A queued connection must copy exactly once per receiver, and no more.
//!
//! One copy is unavoidable: the call outlives the emit, so the argument has to be stored somewhere
//! the receiving thread can read later. More than one per receiver means the stored copy is being
//! copied again on its way into the event.
TEST( ObjectArgumentCopyingTest, QueuedConnectionCopiesOncePerReceiver )
{
    Thread worker;
    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    Signal<const CopyCountingPayload&> sig;

    std::vector<std::unique_ptr<PayloadReceiver> > receivers;
    for( int i = 0; i < kReceivers; ++i )
    {
        receivers.push_back( std::make_unique<PayloadReceiver>() );
        ASSERT_TRUE( receivers.back()->moveToThread( &worker ) );
        Object::connect( sig, receivers.back().get(), &PayloadReceiver::onPayload,
            ConnectionType::Queued );
    }

    CopyCountingPayload payload;
    CopyCountingPayload::reset();
    sig.emit( payload );

    drainWorker( worker );

    EXPECT_EQ( CopyCountingPayload::sCopies.load(), kReceivers )
        << "a queued connection should store the argument once per receiver; anything more is the "
        "stored copy being copied again on its way into the event.";
    for( const auto& r : receivers )
    {
        EXPECT_EQ( r->calls(), 1 );
    }

    worker.quit();
    worker.wait();
}

//! Events still queued when a thread dies must release the arguments they captured.
//!
//! Modelled on QtMimic's BlackHole test. A dispatcher that frees its pending events without
//! destroying their captured state would leak every argument still in flight, which is invisible to
//! LeakSanitizer for as long as the queue itself is reachable.
TEST( ObjectArgumentCopyingTest, UnprocessedEventsReleaseTheirArguments )
{
    CopyCountingPayload::reset();
    std::atomic<int>& copies = CopyCountingPayload::sCopies;

    {
        auto worker = std::make_unique<Thread>();
        worker->start();
        while( !worker->eventDispatcher() )
        {
            std::this_thread::yield();
        }

        auto receiver = std::make_unique<PayloadReceiver>();
        ASSERT_TRUE( receiver->moveToThread( worker.get() ) );

        Signal<const CopyCountingPayload&> sig;
        Object::connect( sig, receiver.get(), &PayloadReceiver::onPayload,
            ConnectionType::Queued );

        // Flood the queue so most of these cannot possibly have been dispatched yet.
        CopyCountingPayload payload;
        for( int i = 0; i < 10000; ++i )
        {
            sig.emit( payload );
        }

        // Tear down without draining. The worker is stopped first so the receiver is then destroyed
        // in contract -- its thread is no longer running -- which keeps this test from tripping
        // ~Object()'s cross-thread destruction warning over something it is not trying to exercise.
        // The queued events are still undelivered either way, which is the point.
        worker->quit();
        worker->wait();
        receiver.reset();
        worker.reset();
    }

    // Nothing asserts a copy count here -- the point is that this completes without leaking or
    // crashing. Under AddressSanitizer a dispatcher that dropped events without destroying them
    // reports the leak directly.
    EXPECT_GT( copies.load(), 0 ) << "the flood should have copied the payload at least once";
    SUCCEED();
}
