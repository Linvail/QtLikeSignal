// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for ConnectionType::BlockingQueued and Thread::invokeAndWait().
//!
//! The interesting cases are not the ones where the call succeeds. A blocking call has three
//! endings -- it runs, its receiver dies before the loop reaches it, or the loop is torn down with
//! the event still queued -- and only the first wakes the waiter by running anything. The other two
//! are what the latch exists for, and a defect in either shows up as a test that never returns
//! rather than one that fails, so each is driven from a helper thread with a deadline.

#include "gtest/gtest.h"

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! How long a blocking call is given before the test calls it hung.
    //!
    //! Generous on purpose. Every use is a deadline on an operation that should take microseconds,
    //! so the only way to reach it is a defect, and a wide margin is what stops a loaded build
    //! machine reporting one that is not there.
    constexpr std::chrono::seconds kDeadline { 5 };

    //! Waits for @p aThread to have a dispatcher, so a post cannot land in the parked window.
    bool waitForLoop
        (
        Thread& aThread   //!< Thread to wait on. Must have been started.
        )
    {
        const auto limit = std::chrono::steady_clock::now() + kDeadline;
        while( aThread.eventDispatcher() == nullptr )
        {
            if( std::chrono::steady_clock::now() >= limit )
            {
                return false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return true;
    }

    //! Runs @p aWork on a helper thread and fails the test rather than hanging if it overruns.
    //!
    //! A blocking call that never returns would otherwise stop the whole suite with no output, and
    //! a hung run says much less than a failed one.
    template <typename Work>
    ::testing::AssertionResult completesWithin
        (
        Work aWork   //!< The blocking operation under test.
        )
    {
        std::future<void> done = std::async( std::launch::async, std::move( aWork ) );
        if( done.wait_for( kDeadline ) != std::future_status::ready )
        {
            // Deliberately leaked: the future's destructor would block on the very thing that has
            // already been shown not to finish, turning a reported failure into a hung process.
            new std::future<void>( std::move( done ) );
            return ::testing::AssertionFailure() << "blocked past the deadline";
        }
        done.get();
        return ::testing::AssertionSuccess();
    }

    //! Records which thread its slot ran on, and how long the caller was made to wait.
    class BlockingReceiver : public Object
    {
    public:
        //! Sleeps briefly, then records the calling thread. The sleep is what makes "the emitter
        //! waited" observable: without it the slot could finish before the emitter even blocked.
        void onValue
            (
            int aValue   //!< Value the signal carried.
            )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            mValue.store( aValue );
            mRanOn.store( Thread::currentThread() );
            mCalls.fetch_add( 1 );
        }

        std::atomic<int> mValue { 0 };            //!< Last value delivered.
        std::atomic<Thread*> mRanOn { nullptr };  //!< Thread the slot ran on.
        std::atomic<int> mCalls { 0 };            //!< How many times the slot ran.
    };

    //! Counts its own copies, so a test can assert an argument was never copied.
    struct CopyCounted
    {
        CopyCounted() = default;

        CopyCounted
            (
            const CopyCounted& aOther
            )
            : mCopies( aOther.mCopies + 1 )
        {
        }

        CopyCounted& operator=
            (
            const CopyCounted&
            ) = delete;

        int mCopies { 0 };   //!< How many copies stand between this and the original.
    };
}

//! Fails if emit() returns before a BlockingQueued slot has finished.
//!
//! The slot sleeps, so an emit that did not wait would be observable as a call count still at zero
//! on the line after it.
TEST( BlockingConnectionTest, EmitDoesNotReturnUntilTheSlotHasFinished )
{
    Thread worker( "blocking-finish" );
    worker.start();
    ASSERT_TRUE( waitForLoop( worker ) );

    BlockingReceiver receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    Signal<int> signal;
    Object::connect( signal, &receiver, &BlockingReceiver::onValue,
        ConnectionType::BlockingQueued );

    ASSERT_TRUE( completesWithin( [&signal]()
        {
            signal.emit( 7 );
        } ) );

    EXPECT_EQ( receiver.mCalls.load(), 1 );
    EXPECT_EQ( receiver.mValue.load(), 7 );
    EXPECT_EQ( receiver.mRanOn.load(), &worker ) << "the slot ran on the wrong thread";

    ASSERT_TRUE( worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } ) );
    worker.quit();
    worker.wait();
}

//! Fails if a BlockingQueued emit copies its arguments.
//!
//! Queued has to copy -- it returns long before the slot runs -- but BlockingQueued holds the
//! caller's frame alive for the whole call, so the slot can be handed the original. This is the
//! one place where the blocking connection is cheaper than the non-blocking one, and it is worth a
//! guard because the obvious implementation of BlockingQueued is "Queued, then wait", which would
//! quietly reintroduce the copy.
TEST( BlockingConnectionTest, ArgumentsAreNotCopied )
{
    Thread worker( "blocking-nocopy" );
    worker.start();
    ASSERT_TRUE( waitForLoop( worker ) );

    Object receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    std::atomic<int> seenCopies { -1 };
    Signal<const CopyCounted&> signal;
    Object::connect( signal, &receiver,
        [&seenCopies]( const CopyCounted& aArgument )
        {
            seenCopies.store( aArgument.mCopies );
        },
        ConnectionType::BlockingQueued );

    const CopyCounted original;
    ASSERT_TRUE( completesWithin( [&signal, &original]()
        {
            signal.emit( original );
        } ) );

    EXPECT_EQ( seenCopies.load(), 0 )
        << "the argument was copied " << seenCopies.load()
        << " time(s); a blocking call holds the caller's frame and should pass the original";

    ASSERT_TRUE( worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } ) );
    worker.quit();
    worker.wait();
}

//! Fails if a BlockingQueued emit to a receiver on the emitting thread deadlocks.
//!
//! Qt deadlocks here and asserts to say so. This runs the slot inline instead, which satisfies the
//! only thing the caller was promised. A regression would hang rather than fail, hence the deadline.
TEST( BlockingConnectionTest, SameThreadRunsInlineRatherThanDeadlocking )
{
    std::atomic<int> calls { 0 };
    std::atomic<int> value { 0 };
    std::atomic<Thread*> ranOn { nullptr };
    std::atomic<Thread*> emittedFrom { nullptr };

    // Receiver, connection and emit all live on the helper thread, because that is what makes the
    // receiver's affinity the emitting thread. Building the receiver out here instead would put it
    // on the test's own thread and quietly measure the cross-thread path, which is the mistake this
    // comment exists to stop the next reader repeating.
    ASSERT_TRUE( completesWithin( [&calls, &value, &ranOn, &emittedFrom]()
        {
            BlockingReceiver receiver;
            Signal<int> signal;
            Object::connect( signal, &receiver, &BlockingReceiver::onValue,
            ConnectionType::BlockingQueued );

            emittedFrom.store( Thread::currentThread() );
            signal.emit( 3 );

            calls.store( receiver.mCalls.load() );
            value.store( receiver.mValue.load() );
            ranOn.store( receiver.mRanOn.load() );
        } ) );

    EXPECT_EQ( calls.load(), 1 );
    EXPECT_EQ( value.load(), 3 );
    EXPECT_EQ( ranOn.load(), emittedFrom.load() )
        << "an inline call must run on the emitting thread";
}

//! Fails if a receiver whose thread has already finished leaves the emitter blocked.
//!
//! The connection is made while the worker is alive and the emit happens after it has gone, so the
//! call is refused rather than queued. Nothing on the other side will ever settle the latch, so an
//! implementation that waited unconditionally would hang here.
TEST( BlockingConnectionTest, EmitToAFinishedThreadReturnsInsteadOfHanging )
{
    BlockingReceiver receiver;

    {
        Thread worker( "blocking-gone" );
        worker.start();
        ASSERT_TRUE( waitForLoop( worker ) );
        ASSERT_TRUE( receiver.moveToThread( &worker ) );
        ASSERT_TRUE( worker.post( [&receiver]()
            {
                receiver.moveToThread( nullptr );
            } ) );
        worker.quit();
        worker.wait();
    }

    Signal<int> signal;
    Object::connect( signal, &receiver, &BlockingReceiver::onValue,
        ConnectionType::BlockingQueued );

    ASSERT_TRUE( completesWithin( [&signal]()
        {
            signal.emit( 11 );
        } ) );

    EXPECT_EQ( receiver.mCalls.load(), 0 ) << "the slot must not have run";
}

//! Fails if tearing the loop down around a queued blocking call leaves the emitter blocked.
//!
//! The third ending, and the one no amount of care at the call site can prevent: the call is
//! accepted, sits in the queue, and the thread ends before the loop reaches it. Whether the slot
//! ends up running is a race this test deliberately does not pin down -- either the loop drains the
//! event or the teardown discards it, and both are correct. What must hold in both is that the
//! emitter is released, which is exactly what settling from the guard's destructor buys.
TEST( BlockingConnectionTest, LoopTornDownAroundTheCallReleasesTheEmitter )
{
    Thread worker( "blocking-teardown" );
    worker.start();
    ASSERT_TRUE( waitForLoop( worker ) );

    BlockingReceiver receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    Signal<int> signal;
    Object::connect( signal, &receiver, &BlockingReceiver::onValue,
        ConnectionType::BlockingQueued );

    // Occupy the loop so the emit below is still sitting in the queue when the thread is asked to
    // stop. Without this the call would be delivered immediately and the test would prove nothing.
    std::atomic<bool> holdLoop { true };
    ASSERT_TRUE( worker.post( [&holdLoop]()
        {
            while( holdLoop.load() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        } ) );

    std::future<void> emitter = std::async( std::launch::async, [&signal]()
        {
            signal.emit( 5 );
        } );

    // Long enough for the emit to have reached the queue and blocked behind the occupying task.
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    worker.quit();
    holdLoop.store( false );
    worker.wait();

    ASSERT_EQ( emitter.wait_for( kDeadline ), std::future_status::ready )
        << "the emitter was still blocked after its receiver's thread had ended";
    emitter.get();

    // Both outcomes are legal here; only being released is required. Recorded rather than asserted
    // so a change in which way the race falls does not read as a regression.
    RecordProperty( "slotRan", receiver.mCalls.load() );
}

//! Fails if invokeAndWait() returns before the task has finished, or loses what it wrote.
//!
//! The documented way to get a result out is a local captured by reference, which is only sound
//! because the caller is blocked for the whole call. This is that pattern, asserted.
TEST( InvokeAndWaitTest, ReturnsAfterTheTaskHasWrittenItsResult )
{
    Thread worker( "invoke-result" );
    worker.start();
    ASSERT_TRUE( waitForLoop( worker ) );

    int answer = 0;
    Thread* ranOn = nullptr;
    bool ran = false;

    ASSERT_TRUE( completesWithin( [&worker, &answer, &ranOn, &ran]()
        {
            ran = worker.invokeAndWait( [&answer, &ranOn]()
                {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
                    answer = 42;
                    ranOn  = Thread::currentThread();
                } );
        } ) );

    EXPECT_TRUE( ran );
    EXPECT_EQ( answer, 42 );
    EXPECT_EQ( ranOn, &worker ) << "the task ran on the wrong thread";

    worker.quit();
    worker.wait();
}

//! Fails if invokeAndWait() from the thread itself deadlocks instead of running inline.
//!
//! The loop that would run the task is the one that would be blocked waiting for it, so queueing
//! here is a deadlock against oneself. Note this diverges from post(), which defers always.
TEST( InvokeAndWaitTest, FromTheThreadItselfRunsInline )
{
    Thread worker( "invoke-inline" );
    worker.start();
    ASSERT_TRUE( waitForLoop( worker ) );

    std::atomic<bool> ran { false };
    std::atomic<bool> nested { false };

    ASSERT_TRUE( completesWithin( [&worker, &ran, &nested]()
        {
            const bool posted = worker.invokeAndWait( [&worker, &nested]()
                {
                    // Already on the worker: this inner call must not queue behind itself.
                    nested = worker.invokeAndWait( []()
                        {
                        } );
                } );
            ran = posted;
        } ) );

    EXPECT_TRUE( ran.load() );
    EXPECT_TRUE( nested.load() ) << "a nested invokeAndWait() on the same thread must run inline";

    worker.quit();
    worker.wait();
}

//! Fails if invokeAndWait() to a thread with no loop reports success or blocks.
TEST( InvokeAndWaitTest, RefusesWhenTheThreadIsNotRunning )
{
    Thread worker( "invoke-never-started" );

    std::atomic<bool> taskRan { false };
    bool result = true;

    ASSERT_TRUE( completesWithin( [&worker, &taskRan, &result]()
        {
            result = worker.invokeAndWait( [&taskRan]()
                {
                    taskRan = true;
                } );
        } ) );

    EXPECT_FALSE( result ) << "a thread that was never started cannot run anything";
    EXPECT_FALSE( taskRan.load() );
}

//! Fails if invokeAndWait() jumps the queue.
//!
//! It promises completion, not priority: whatever was posted first still runs first, because it
//! goes through the one event queue rather than a second channel of its own.
TEST( InvokeAndWaitTest, RunsBehindWhateverWasAlreadyQueued )
{
    Thread worker( "invoke-order" );
    worker.start();
    ASSERT_TRUE( waitForLoop( worker ) );

    std::vector<int> order;
    ASSERT_TRUE( worker.post( [&order]()
        {
            order.push_back( 1 );
        } ) );
    ASSERT_TRUE( worker.post( [&order]()
        {
            order.push_back( 2 );
        } ) );

    ASSERT_TRUE( completesWithin( [&worker, &order]()
        {
            static_cast<void>( worker.invokeAndWait( [&order]()
                {
                    order.push_back( 3 );
                } ) );
        } ) );

    ASSERT_EQ( order.size(), 3u );
    EXPECT_EQ( order[0], 1 );
    EXPECT_EQ( order[1], 2 );
    EXPECT_EQ( order[2], 3 );

    worker.quit();
    worker.wait();
}
