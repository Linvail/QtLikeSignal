// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Dispatch-overhead benchmarks for QtLikeSignal, measured -- where they are installed -- against Qt 6
//! and boost::signals2.
//!
//! Covers the whole path a signal travels -- connect, emit, receive -- rather than any one function,
//! so the numbers say what a user actually pays. Every library runs the same scenarios with the same
//! slot bodies in the same process, which is the only way to make the comparison mean anything: same
//! machine, same build flags, same cache state, interleaved in time.
//!
//!
//! **Build this in release with no sanitizer before believing any number.** A `-O0` build, or one
//! with a sanitizer enabled, inflates everything here by roughly an order of magnitude and does not
//! inflate the libraries equally. `--mode` belongs on the build command, not on configure:
//!
//! @code
//!   ./waf configure                       # no --enable-*-sanitizer-on-Linux
//!   ./waf install --project=Tests --mode=release
//! @endcode
//!
//! The Qt 6 rows come from test_Qt6_Performance.cpp and the boost ones from
//! test_Boost_Performance.cpp, each compiled into this same binary when its library is found, so
//! every column is measured in one process on one machine. Both are Linux-only; a Windows build
//! reports the two columns it can.
//!
//! boost fills fewer rows than the others, because signals2 has no thread affinity and no event
//! loop. See test_Boost_Performance.cpp for why those rows are left blank rather than redefined.
//!
//! These are microbenchmarks with no work between iterations, which is the condition most flattering
//! to fixed per-emit overhead. Treat the ratios as meaningful and the absolute nanoseconds as
//! indicative.

#include "PerfHarness.hpp"

#include <gtest/gtest.h>

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using PerfHarness::keep;
using PerfHarness::kConnectOps;
using PerfHarness::kDirectOps;
using PerfHarness::kQueuedOps;
using PerfHarness::kQueuedRoundTripOps;
using PerfHarness::kBlockingOps;
using PerfHarness::kDisconnectOps;
using PerfHarness::record;
using PerfHarness::timeLoop;

// =================================================================================================
// QtLikeSignal
// =================================================================================================

//! Measures establishing a connection.
TEST( Performance, QtLikeSignal_Connect )
{
    QtLikeSignal::Object receiver;
    QtLikeSignal::Signal<int> sig;
    const double ns = timeLoop( kConnectOps, [&]( int )
        {
            QtLikeSignal::Object::connect( sig, &receiver, []( int )
            {
            }, QtLikeSignal::ConnectionType::Direct );
        } );
    record( "connect()", "QtLikeSignal", ns );
}

//! Measures emit -> receive on one thread with an explicit direct connection.
TEST( Performance, QtLikeSignal_DirectEmit )
{
    QtLikeSignal::Object receiver;
    QtLikeSignal::Signal<int> sig;
    long long received = 0;
    QtLikeSignal::Object::connect( sig, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, QtLikeSignal::ConnectionType::Direct );

    sig.emit( 1 );
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig.emit( 1 );
            keep( received );
        } );
    record( "emit->receive, direct", "QtLikeSignal", ns );
    EXPECT_GT( received, 0 );
}

//! Measures emit -> receive on one thread through Auto.
TEST( Performance, QtLikeSignal_AutoEmitSameThread )
{
    QtLikeSignal::Object receiver;
    QtLikeSignal::Signal<int> sig;
    long long received = 0;
    QtLikeSignal::Object::connect( sig, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, QtLikeSignal::ConnectionType::Auto );

    sig.emit( 1 );
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig.emit( 1 );
            keep( received );
        } );
    record( "emit->receive, auto same-thread", "QtLikeSignal", ns );
    EXPECT_GT( received, 0 );
}

//! Measures end-to-end cross-thread throughput: emit on this thread, receive on a worker's loop.
TEST( Performance, QtLikeSignal_QueuedEmitCrossThread )
{
    QtLikeSignal::Thread worker( "perf-worker" );
    worker.start();

    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    // Posted once, not spun on. This was an unbounded retry loop -- no deadline at all -- built
    // around a post() that could be refused while the thread was still starting. That refusal was
    // a defect, not a state to wait out; such a post is parked now, so a refusal here means
    // something is genuinely wrong and stops the benchmark instead of hanging it forever.
    ASSERT_TRUE( worker.post( [&ready]()
        {
            ready.set_value();
        } ) );
    ASSERT_EQ( readyFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    // Built here and pushed onto the worker. Object took a Thread* until 2026-08-21 and bound
    // affinity at construction; it takes a parent now, so this is the move it used to avoid.
    QtLikeSignal::Object receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    std::atomic<int> received { 0 };
    QtLikeSignal::Signal<int> sig;
    QtLikeSignal::Object::connect( sig, &receiver, [&received]( int )
        {
            received.fetch_add( 1, std::memory_order_relaxed );
        }, QtLikeSignal::ConnectionType::Queued );

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < kQueuedOps; ++i )
    {
        sig.emit( 1 );
    }
    while( received.load( std::memory_order_relaxed ) < kQueuedOps )
    {
        std::this_thread::yield();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "emit->receive, queued x-thread", "QtLikeSignal",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kQueuedOps );

    EXPECT_EQ( received.load(), kQueuedOps );
    worker.quit();
    worker.wait();
}

//! Measures one queued cross-thread emit at a time, with the queue never allowed to grow.
//!
//! The companion to QueuedEmitCrossThread, and a control on it. That scenario lets the emitter run
//! every one of its emits ahead of the receiver, so what it reports is throughput under saturation
//! rather than the cost of an emit -- and saturation is not a neutral condition. With the queue
//! almost never empty, the wake a dispatcher would otherwise issue is often already pending and
//! skipped, which raised a fair objection: a library whose receiver *keeps up* would pay more wakes
//! than one whose receiver falls behind, so the ratio might be rewarding falling behind rather than
//! measuring dispatch.
//!
//! The objection had real evidence behind it. Measured on Windows on 2026-08-26, Qt 6 delivered
//! 99.8% of its events before its emit loop finished while QtLikeSignal delivered 77%, and Qt's entire
//! cost sat in its emit loop -- total and emit-side agreed to within 0.4 ns/op.
//!
//! **This scenario was written to test that objection, and refutes it.** The emitter here waits for
//! each delivery before making the next, so the queue holds at most one event, every emit pays a
//! full wake and a context switch, and no amount of running ahead can amortise anything. The
//! advantage survives: 0.37x on Windows and 0.87x on Linux, against 0.47x and 0.85x for the
//! saturated row measured beside it. Same ordering, same rough magnitude, on both platforms.
//!
//! So the row stays, as the control that keeps that answer honest rather than as a correction to
//! the other one. The number is a round-trip latency and is two orders of magnitude larger than the
//! saturated row; the two are not comparable to each other and are not meant to be. It is also much
//! noisier, being dominated by scheduler wake-up -- expect a wide spread and quote a range.
//! Measures one blocking cross-thread emit at a time, with the library doing the waiting.
//!
//! **Read this against `queued round-trip`, not against `queued x-thread`.** The two are the same
//! shape -- one wake out, one wake back, nothing allowed to accumulate -- and are run for the same
//! number of operations for exactly that reason. Round-trip is a caller waiting by hand, spinning
//! on a counter until its own emit comes back. This row is the same wait expressed as a connection
//! type. The difference between them is what the library's version costs over the hand-rolled one,
//! and it is the only comparison here that isolates the blocking machinery rather than the
//! scheduler underneath it.
//!
//! Against the saturated row it says nothing at all, for the same reason round-trip says nothing
//! against it: one measures throughput with the emitter free to run ahead, these two measure
//! latency with it forbidden to.
//!
//! Expect it to be noisy, dominated by two context switches per operation, and quote a range.
TEST( Performance, QtLikeSignal_BlockingCrossThread )
{
    QtLikeSignal::Thread worker( "perf-blocking" );
    worker.start();

    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    ASSERT_TRUE( worker.post( [&ready]()
        {
            ready.set_value();
        } ) );
    ASSERT_EQ( readyFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    QtLikeSignal::Object receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    std::atomic<int> received { 0 };
    QtLikeSignal::Signal<int> sig;
    QtLikeSignal::Object::connect( sig, &receiver, [&received]( int )
        {
            received.fetch_add( 1, std::memory_order_release );
        }, QtLikeSignal::ConnectionType::BlockingQueued );

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < kBlockingOps; ++i )
    {
        // No spin of our own: emit() does not return until the slot has run, which is the whole
        // difference between this row and the round-trip one.
        sig.emit( 1 );
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "emit->receive, blocking x-thread", "QtLikeSignal",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kBlockingOps );

    // Exact, not a lower bound. A blocking emit that returned before its slot ran would show up
    // here as a shortfall rather than as a merely surprising number.
    EXPECT_EQ( received.load(), kBlockingOps );

    ASSERT_TRUE( worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } ) );
    worker.quit();
    worker.wait();
}

TEST( Performance, QtLikeSignal_QueuedRoundTrip )
{
    QtLikeSignal::Thread worker( "perf-roundtrip" );
    worker.start();

    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    ASSERT_TRUE( worker.post( [&ready]()
        {
            ready.set_value();
        } ) );
    ASSERT_EQ( readyFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    QtLikeSignal::Object receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    std::atomic<int> received { 0 };
    QtLikeSignal::Signal<int> sig;
    QtLikeSignal::Object::connect( sig, &receiver, [&received]( int )
        {
            received.fetch_add( 1, std::memory_order_release );
        }, QtLikeSignal::ConnectionType::Queued );

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < kQueuedRoundTripOps; ++i )
    {
        sig.emit( 1 );
        // Waits for this emit's own delivery, not for a count reached later. yield() rather than a
        // condition variable because a wait of our own would add its own wake to what is being
        // measured; both libraries are spun on identically, so the overhead cancels in the ratio.
        while( received.load( std::memory_order_acquire ) <= i )
        {
            std::this_thread::yield();
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "emit->receive, queued round-trip", "QtLikeSignal",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kQueuedRoundTripOps );

    EXPECT_EQ( received.load(), kQueuedRoundTripOps );
    worker.quit();
    worker.wait();
}

//! Measures ending a connection through its handle.
TEST( Performance, QtLikeSignal_Disconnect )
{
    QtLikeSignal::Signal<int> sig;
    long long received = 0;

    std::vector<QtLikeSignal::Connection> handles;
    handles.reserve( kDisconnectOps );
    for( int i = 0; i < kDisconnectOps; ++i )
    {
        handles.push_back( sig.connect( [&received]( int aValue )
            {
                received += aValue;
            } ) );
    }

    const auto start = std::chrono::steady_clock::now();
    for( const auto& handle : handles )
    {
        handle.disconnect();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "disconnect()", "QtLikeSignal",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kDisconnectOps );

    sig.emit( 1 );
    EXPECT_EQ( received, 0 );
}

// =================================================================================================

//! Prints the comparison table once every scenario has run.
//!
//! A gtest environment rather than a test, so it runs after all of them regardless of ordering or
//! filtering, and regardless of which libraries were compiled in.
class SummaryPrinter : public ::testing::Environment
{
public:
    virtual void TearDown() override
    {
        PerfHarness::printSummary();
    }

};

//! Entry point. This binary is separate from the correctness suite so its cost is opt-in.
int main
    (
    int aArgc,     //!< Command line argument count.
    char** aArgv   //!< Command line argument vector.
    )
{
    ::testing::InitGoogleTest( &aArgc, aArgv );

    // Before anything is timed, not between tests: the state these settle is process-wide and
    // one-way, so it has to be established while every library is still unmeasured. Without them
    // the table charges whichever library ran first for putting the process into the state every
    // later measurement enjoys.
    PerfHarness::settleAllocatorState();
    PerfHarness::settleHeap();

    ::testing::AddGlobalTestEnvironment( new SummaryPrinter() );
    return RUN_ALL_TESTS();
}
