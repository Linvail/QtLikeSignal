// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Dispatch-overhead benchmarks for QtLikeSignal, measured against QtMimic and -- where they are
//! installed -- against Qt 6 and boost::signals2.
//!
//! Covers the whole path a signal travels -- connect, emit, receive -- rather than any one function,
//! so the numbers say what a user actually pays. Both libraries run the same scenarios with the same
//! slot bodies in the same process, which is the only way to make the comparison mean anything: same
//! machine, same build flags, same cache state, interleaved in time.
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
//! indicative. See history/PERFORMANCE-20260808.md for where the overhead goes.

#include "PerfHarness.hpp"

#include <gtest/gtest.h>

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"

// QtMimic, by path rather than through its exported include directory. Both libraries now use
// .hpp and every header this file wants exists under both names, so a bare "Object.hpp" would
// resolve by include-path order -- which is not something a benchmark comparing the two should
// depend on. The path says which library is meant.
#include "QtMimic/Object.hpp"
#include "QtMimic/Signal.hpp"
#include "QtMimic/Thread.hpp"

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
using PerfHarness::kTeardownResident;
using PerfHarness::record;
using PerfHarness::timeLoop;

// =================================================================================================
// QtLikeSignal
// =================================================================================================

//! Measures establishing a connection.
TEST( Performance, QtLikeSignal_Connect )
{
    using namespace QtLikeSignal;

    Object receiver;
    Signal<int> sig;
    const double ns = timeLoop( kConnectOps, [&]( int )
        {
            Object::connect( sig, &receiver, []( int )
            {
            }, ConnectionType::Direct );
        } );
    record( "connect()", "QtLikeSignal", ns );
}

//! Measures emit -> receive on one thread with an explicit direct connection.
TEST( Performance, QtLikeSignal_DirectEmit )
{
    using namespace QtLikeSignal;

    Object receiver;
    Signal<int> sig;
    long long received = 0;
    Object::connect( sig, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, ConnectionType::Direct );

    sig.emit( 1 );   // warm up
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig.emit( 1 );
            keep( received );
        } );
    record( "emit->receive, direct", "QtLikeSignal", ns );
    EXPECT_GT( received, 0 );
}

//! Measures emit -> receive on one thread through ConnectionType::Auto.
//!
//! Same delivery as the direct case, but Auto has to resolve the receiver's thread affinity on every
//! emit, so the difference against the row above is the cost of that resolution.
TEST( Performance, QtLikeSignal_AutoEmitSameThread )
{
    using namespace QtLikeSignal;

    Object receiver;
    Signal<int> sig;
    long long received = 0;
    Object::connect( sig, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, ConnectionType::Auto );

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
//!
//! Timed until the last message has actually been *received*, not merely posted, so this is the full
//! queue-and-dispatch round trip rather than the cost of enqueueing.
TEST( Performance, QtLikeSignal_QueuedEmitCrossThread )
{
    using namespace QtLikeSignal;

    Thread worker;
    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    // Confirm the loop is actually running before timing anything.
    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    ASSERT_TRUE( worker.post( [&ready]()
        {
            ready.set_value();
        } ) );
    ASSERT_EQ( readyFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    Object receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    std::atomic<int> received { 0 };
    Signal<int> sig;
    Object::connect( sig, &receiver, [&received]( int )
        {
            received.fetch_add( 1, std::memory_order_relaxed );
        }, ConnectionType::Queued );

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

//! Measures destroying N receivers that are all connected to one long-lived signal.
//!
//! The scenario P7 was found in, kept in the comparison table because it is the one operation where
//! the boost we replaced is still ahead of us. Teardown is quadratic if a disconnect has to search
//! the signal's slot list, and quadratic again if it has to search the receiver's own list, so this
//! row is where both of those come back if either regresses.
//!
//! Only the teardown is timed. Connecting is setup, and timing it too would blur what this measures.
TEST( Performance, QtLikeSignal_TeardownAtScale )
{
    using namespace QtLikeSignal;

    Signal<int> sig;
    long long received = 0;

    std::vector<std::unique_ptr<Object> > receivers;
    receivers.reserve( kTeardownResident );
    for( int i = 0; i < kTeardownResident; ++i )
    {
        receivers.push_back( std::unique_ptr<Object>( new Object() ) );
        Object::connect( sig, receivers.back().get(), [&received]( int aValue )
            {
                received += aValue;
            }, ConnectionType::Direct );
    }

    const auto start = std::chrono::steady_clock::now();
    receivers.clear();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "destroy N receivers", "QtLikeSignal",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kTeardownResident );

    // Proves the destructors really did disconnect, so the row above is not the time to destroy
    // N objects that were never attached to anything.
    sig.emit( 1 );
    EXPECT_EQ( received, 0 );
}

// =================================================================================================
// QtMimic
// =================================================================================================

//! Measures establishing a connection.
TEST( Performance, QtMimic_Connect )
{
    QtMimic::Object receiver;
    QtMimic::Signal<int> sig;
    const double ns = timeLoop( kConnectOps, [&]( int )
        {
            QtMimic::Object::connect( sig, &receiver, []( int )
            {
            }, QtMimic::ConnectionType::Direct );
        } );
    record( "connect()", "QtMimic", ns );
}

//! Measures emit -> receive on one thread with an explicit direct connection.
TEST( Performance, QtMimic_DirectEmit )
{
    QtMimic::Object receiver;
    QtMimic::Signal<int> sig;
    long long received = 0;
    QtMimic::Object::connect( sig, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, QtMimic::ConnectionType::Direct );

    sig.emit( 1 );
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig.emit( 1 );
            keep( received );
        } );
    record( "emit->receive, direct", "QtMimic", ns );
    EXPECT_GT( received, 0 );
}

//! Measures emit -> receive on one thread through Auto.
TEST( Performance, QtMimic_AutoEmitSameThread )
{
    QtMimic::Object receiver;
    QtMimic::Signal<int> sig;
    long long received = 0;
    QtMimic::Object::connect( sig, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, QtMimic::ConnectionType::Auto );

    sig.emit( 1 );
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig.emit( 1 );
            keep( received );
        } );
    record( "emit->receive, auto same-thread", "QtMimic", ns );
    EXPECT_GT( received, 0 );
}

//! Measures end-to-end cross-thread throughput: emit on this thread, receive on a worker's loop.
TEST( Performance, QtMimic_QueuedEmitCrossThread )
{
    QtMimic::Thread worker( "perf-worker" );
    worker.start();

    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    while( !worker.post( [&ready]()
        {
            ready.set_value();
        } ) )
    {
        std::this_thread::yield();
    }
    ASSERT_EQ( readyFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    // QtMimic binds affinity at construction rather than through a move.
    QtMimic::Object receiver( &worker );

    std::atomic<int> received { 0 };
    QtMimic::Signal<int> sig;
    QtMimic::Object::connect( sig, &receiver, [&received]( int )
        {
            received.fetch_add( 1, std::memory_order_relaxed );
        }, QtMimic::ConnectionType::Queued );

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

    record( "emit->receive, queued x-thread", "QtMimic",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kQueuedOps );

    EXPECT_EQ( received.load(), kQueuedOps );
    worker.quit();
    worker.wait();
}

//! Measures destroying N receivers that are all connected to one long-lived signal.
TEST( Performance, QtMimic_TeardownAtScale )
{
    QtMimic::Signal<int> sig;
    long long received = 0;

    std::vector<std::unique_ptr<QtMimic::Object> > receivers;
    receivers.reserve( kTeardownResident );
    for( int i = 0; i < kTeardownResident; ++i )
    {
        receivers.push_back( std::unique_ptr<QtMimic::Object>( new QtMimic::Object() ) );
        QtMimic::Object::connect( sig, receivers.back().get(), [&received]( int aValue )
            {
                received += aValue;
            }, QtMimic::ConnectionType::Direct );
    }

    const auto start = std::chrono::steady_clock::now();
    receivers.clear();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "destroy N receivers", "QtMimic",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kTeardownResident );

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
