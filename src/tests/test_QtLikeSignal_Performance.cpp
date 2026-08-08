//! @file
//!
//! Dispatch-overhead benchmarks for QtLikeSignal, measured against QtMimic as a reference.
//!
//! Covers the whole path a signal travels -- connect, emit, receive -- rather than any one function,
//! so the numbers say what a user actually pays. Both libraries run the same scenarios with the same
//! slot bodies in the same process, which is the only way to make the comparison mean anything: same
//! machine, same build flags, same cache state, interleaved in time.
//!
//! **Build this in release with no sanitizer before believing any number.** The default test
//! configuration is `-O0` plus ThreadSanitizer, which inflates everything here by roughly an order of
//! magnitude and does not inflate the two libraries equally:
//!
//! @code
//!   ./waf configure --mode=release
//!   ./waf install --project=Tests --mode=release
//! @endcode
//!
//! These are microbenchmarks with no work between iterations, which is the condition most flattering
//! to fixed per-emit overhead. Treat the ratios as meaningful and the absolute nanoseconds as
//! indicative. See history/PERFORMANCE-20260808.md for where the overhead goes.

#include <gtest/gtest.h>

#include "Object.h"
#include "Signal.h"
#include "Thread.h"

// QtMimic, reached through its exported include directory. Its headers use .hpp so they do not
// collide with ours. Both libraries do have a ThreadData.hpp, but each is pulled in by a quoted
// include from its own directory, and a quoted include searches the including file's directory
// first -- so neither library can accidentally pick up the other's.
#include "Object.hpp"
#include "Signal.hpp"
#include "Thread.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace
{
    //! One measurement: what was measured, for which library, and what it cost.
    struct Result
    {
        std::string mScenario;   //!< Scenario name, identical across libraries so rows pair up.
        std::string mLibrary;    //!< "QtLikeSignal" or "QtMimic".
        double mNsPerOp;         //!< Nanoseconds per operation.
    };

    std::vector<Result> gResults;

    //! Records a measurement and echoes it, so a truncated run still shows its progress.
    void record
        (
        const std::string& aScenario,
        const std::string& aLibrary,
        double aNsPerOp
        )
    {
        gResults.push_back( { aScenario, aLibrary, aNsPerOp } );
        std::printf( "  %-34s %-13s %10.1f ns/op\n", aScenario.c_str(), aLibrary.c_str(), aNsPerOp );
        std::fflush( stdout );
    }

    //! Times @p aBody run @p aCount times and returns nanoseconds per iteration.
    template <typename Body>
    double timeLoop
        (
        int aCount,
        Body aBody
        )
    {
        const auto start = std::chrono::steady_clock::now();
        for( int i = 0; i < aCount; ++i )
        {
            aBody( i );
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::nano>( elapsed ).count() / aCount;
    }

    //! Stops the optimiser deleting a loop whose result is never read.
    //!
    //! Without this the same-thread emit loops can be removed wholesale, which shows up as a
    //! suspiciously round 0.0 ns/op rather than as an error.
    template <typename T> inline void keep( T&& aValue )
    {
        #if defined( _MSC_VER )
            volatile auto sink = aValue;
            ( void )sink;
        #else
            asm volatile ( "" : : "r,m"( aValue ) : "memory" );
        #endif
    }

    // Iteration counts. The queued scenarios are smaller because each in-flight emit holds a heap
    // allocation until the receiving loop drains it, so a large count measures allocator behaviour
    // as much as dispatch.
    constexpr int kConnectOps = 20000;
    constexpr int kDirectOps  = 1000000;
    constexpr int kQueuedOps  = 200000;
}

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
                }, ConnectionType::DirectConnection );
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
        }, ConnectionType::DirectConnection );

    sig.emit( 1 );   // warm up
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig.emit( 1 );
            keep( received );
        } );
    record( "emit->receive, direct", "QtLikeSignal", ns );
    EXPECT_GT( received, 0 );
}

//! Measures emit -> receive on one thread through AutoConnection.
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
        }, ConnectionType::AutoConnection );

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
        }, ConnectionType::QueuedConnection );

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
    worker.join();
}

// =================================================================================================

//! Prints the paired comparison once every scenario has run.
//!
//! Registered as a gtest environment rather than a test so it runs after all of them regardless of
//! ordering or filtering.
class SummaryPrinter : public ::testing::Environment
{
public:
    virtual void TearDown() override
    {
        if( gResults.empty() )
        {
            return;
        }

        std::printf( "\n%-34s %14s %14s %10s\n", "scenario", "QtLikeSignal", "QtMimic", "ratio" );
        std::printf( "%s\n", std::string( 76, '-' ).c_str() );

        for( const auto& lhs : gResults )
        {
            if( lhs.mLibrary != "QtLikeSignal" )
            {
                continue;
            }
            for( const auto& rhs : gResults )
            {
                if( rhs.mLibrary == "QtMimic" && rhs.mScenario == lhs.mScenario )
                {
                    std::printf( "%-34s %11.1f ns %11.1f ns %9.2fx\n", lhs.mScenario.c_str(),
                        lhs.mNsPerOp, rhs.mNsPerOp, lhs.mNsPerOp / rhs.mNsPerOp );
                    break;
                }
            }
        }
        std::printf( "\nratio > 1 means QtLikeSignal is slower.\n" );
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
    ::testing::AddGlobalTestEnvironment( new SummaryPrinter() );
    return RUN_ALL_TESTS();
}
