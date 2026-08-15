//! @file
//!
//! Guards that **fail** on a significant performance regression, rather than printing a number for
//! somebody to notice.
//!
//! The benchmarks next door measure; these judge. That distinction is the point of the file: a
//! table of nanoseconds catches nothing unless a human reads it, remembers what it said last month,
//! and can tell a real regression from a busy machine. Every check here is written so that it
//! cannot be fooled by any of those three.
//!
//! Three kinds of check, in increasing order of how much they can be trusted.
//!
//! **Shape.** Does the cost per item stay flat as the workload grows? This catches the class of
//! defect that actually hurt this project: P7 turned tearing down N receivers into O(N^2), which
//! took 16 000 of them from 4 ms to 671 ms, and no absolute threshold would have flagged it early
//! because at small N it looked fine. A ratio between two sizes is immune to machine speed.
//!
//! **Count.** How many heap blocks does one operation take? Exact, integral, and identical on every
//! machine, so the threshold never needs recalibrating and the test never flakes. P3 (an allocation
//! on every emit) was exactly this kind of regression.
//!
//! **Time.** Absolute nanoseconds, guarded only against a *large* multiple, and only where nothing
//! cheaper will do. See test_Qt6_Performance.cpp for the timing guards, which are expressed as
//! ratios against Qt 6 measured in the same process -- that calibrates the machine away.
//!
//! Thresholds are deliberately loose: several times the current value, so ordinary variation never
//! fails and a genuine regression cannot pass. A guard that cries wolf gets deleted, and then it
//! guards nothing.

#include <gtest/gtest.h>

#include "QtLikeSignal/Object.hpp"
#include "PerfHarness.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <chrono>
#include <memory>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! Receives the benchmark signal.
    class Receiver : public Object
    {
    public:
        void onValue
            (
            int aValue
            )
        {
            mSum += aValue;
        }

        int mSum { 0 };
    };

    //! Milliseconds to connect @p aCount receivers to one signal and then destroy them all.
    //!
    //! Only the teardown is timed. Connecting is the setup, and timing it too would blur the very
    //! thing this measures.
    double teardownMs
        (
        int aCount   //!< Receivers to create, connect, and destroy.
        )
    {
        Signal<int> signal;
        std::vector<std::unique_ptr<Receiver> > receivers;
        receivers.reserve( aCount );
        for( int i = 0; i < aCount; ++i )
        {
            receivers.push_back( std::unique_ptr<Receiver>( new Receiver() ) );
            Object::connect( signal, receivers.back().get(), &Receiver::onValue,
                ConnectionType::Direct );
        }

        const auto start = std::chrono::steady_clock::now();
        receivers.clear();
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start ).count();
    }

    //! Nanoseconds to construct and destroy one Object, against @p aPending undeliverable callLaters.
    double constructDestroyNs
        (
        int aPending   //!< Size of the backlog to build first.
        )
    {
        std::vector<std::unique_ptr<Receiver> > owners;
        owners.reserve( aPending );
        for( int i = 0; i < aPending; ++i )
        {
            owners.push_back( std::unique_ptr<Receiver>( new Receiver() ) );
            owners.back()->callLater( owners.back().get(), &Receiver::onValue, 1 );
        }

        constexpr int kReps = 20000;
        return PerfHarness::timeLoop( kReps, []( int )
            {
                Receiver r;
                asm volatile ( "" : : "r" ( &r ) : "memory" );
            } );
    }
}

//! The QtLikeSignal side of the timing guards, which live in test_Qt6_Performance.cpp.
//!
//! Defined here rather than there because that file cannot include our headers: Qt's `emit` macro
//! would turn every `signal.emit( 1 )` into a syntax error. Declared in PerfHarness.hpp, which is the
//! seam between the two.
namespace PerfHarness
{
    namespace Measure
    {
        double qtLikeSignalDirectEmitNs()
        {
            Signal<int> signal;
            Receiver receiver;
            Object::connect( signal, &receiver, &Receiver::onValue, ConnectionType::Direct );
            signal.emit( 1 );   // warm up
            return PerfHarness::timeLoop( PerfHarness::kDirectOps, [&]( int )
                {
                    signal.emit( 1 );
                    PerfHarness::keep( receiver.mSum );
                } );
        }

        double qtLikeSignalAutoEmitNs()
        {
            Signal<int> signal;
            Receiver receiver;
            Object::connect( signal, &receiver, &Receiver::onValue, ConnectionType::Auto );
            signal.emit( 1 );
            return PerfHarness::timeLoop( PerfHarness::kDirectOps, [&]( int )
                {
                    signal.emit( 1 );
                    PerfHarness::keep( receiver.mSum );
                } );
        }

        double qtLikeSignalConnectNs()
        {
            Signal<int> signal;
            Receiver receiver;
            return PerfHarness::timeLoop( PerfHarness::kConnectOps, [&]( int )
                {
                    Object::connect( signal, &receiver, &Receiver::onValue,
                    ConnectionType::Direct );
                } );
        }
    }
}

// -------------------------------------------------------------------------------------------
// Shape guards
// -------------------------------------------------------------------------------------------

//! Fails if destroying the receivers of one signal stops being linear in their number.
//!
//! Pins P7. `Connection::disconnect()` used to scan the signal's whole slot list to find the one
//! entry that had just died, which made destroying N receivers O(N^2) with no ceiling: 671 ms for
//! 16 000, against boost::signals2's 2.7 ms. Each slot now carries its own index.
//!
//! Four times the receivers should cost about four times the teardown, and it does: measured at
//! 3.98x, 4.21x and 4.39x over three runs. The bar is 8x, which is calibrated rather than guessed --
//! reintroducing the full sweep, with compaction disabled so the array cannot shrink, puts it at
//! 12.17x. So the bar sits with roughly a factor of two of clearance on each side.
TEST( PerformanceRegression, TeardownStaysLinearInTheNumberOfReceivers )
{
    constexpr int kSmall = 2000;
    constexpr int kLarge = 8000;   // 4x

    const double small = PerfHarness::bestOf( 3, []()
        {
            return teardownMs( kSmall );
        } );
    const double large = PerfHarness::bestOf( 3, []()
        {
            return teardownMs( kLarge );
        } );

    ASSERT_GT( small, 0.0 ) << "the small case was too fast to time; raise kSmall";

    const double growth = large / small;
    EXPECT_LT( growth, 8.0 )
        << "destroying " << kLarge << " receivers cost " << growth << "x destroying " << kSmall
        << " (" << small << " ms -> " << large <<
        " ms). Four times the work should cost about four "
        "times as much, and does: this measured 4.0-4.4x when it was written. Twelve is what the "
        "quadratic version measured. Disconnection is scanning the whole slot list again -- see "
        "PERFORMANCE-20260813.md (P7).";
}

//! Fails if destroying an unrelated Object starts to depend on how much work is queued elsewhere.
//!
//! Pins P1. `~Object()` used to walk the process-wide callLater registry and the dispatcher's whole
//! event queue on every destruction, whether or not the object had ever used either. 4 000 pending
//! entries made destroying an unrelated object 324x more expensive. Two flags now skip both scans
//! for an object that never used the features.
//!
//! This is the one guard that would catch a *reintroduced* global scan, and it is the most valuable
//! shape in the file: unlike P7 it degrades with unrelated activity elsewhere in the process, so it
//! is invisible to any benchmark that measures one thing at a time.
TEST( PerformanceRegression, DestroyingAnObjectIgnoresOtherObjectsBacklogs )
{
    const double empty = PerfHarness::bestOf( 3, []()
        {
            return constructDestroyNs( 0 );
        } );
    const double loaded = PerfHarness::bestOf( 3, []()
        {
            return constructDestroyNs( 4000 );
        } );

    ASSERT_GT( empty, 0.0 );

    const double growth = loaded / empty;
    EXPECT_LT( growth, 3.0 )
        << "constructing and destroying an Object cost " << growth
        << "x more with 4 000 unrelated callLater entries pending (" << empty << " ns -> "
        << loaded << " ns). It should cost the same: the object never called callLater() and never "
        "received a queued call, so neither backlog is any of its business. A ratio in the hundreds "
        "means ~Object() is scanning them again -- see PERFORMANCE-20260813.md (P1).";
}

// -------------------------------------------------------------------------------------------
// Count guards
// -------------------------------------------------------------------------------------------

//! Fails if emitting through a direct connection allocates.
//!
//! Pins P3. Every emit used to build the wrapper's closure on the heap before discovering the
//! connection was direct -- one malloc and one free per emit, on the hottest path in the library.
//! The decision now happens before the closure is built, so a direct emit allocates nothing at all.
//!
//! Exact rather than timed: zero is zero on every machine.
TEST( PerformanceRegression, DirectEmitAllocatesNothing )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    Signal<int> signal;
    Receiver receiver;
    Object::connect( signal, &receiver, &Receiver::onValue, ConnectionType::Direct );

    signal.emit( 1 );   // once outside the count, so any one-off setup is not attributed to it

    constexpr int kOps = 10000;
    PerfHarness::Allocations::start();
    for( int i = 0; i < kOps; ++i )
    {
        signal.emit( i );
    }
    const long allocations = PerfHarness::Allocations::stop();

    EXPECT_EQ( allocations, 0 )
        << allocations << " heap allocations over " << kOps << " direct emits ("
        << ( double( allocations ) / kOps ) << " per emit). A direct connection calls the slot and "
        "returns; it must not build anything on the heap to do it -- see PERFORMANCE-20260813.md "
        "(P3).";
}

//! Fails if emitting through a same-thread auto connection allocates.
//!
//! The other half of P3, and the one more likely to regress: the auto path has to resolve the
//! receiver's affinity before it can conclude the call is inline, so it is one careless refactor
//! away from packaging the arguments first and deciding afterwards.
TEST( PerformanceRegression, SameThreadAutoEmitAllocatesNothing )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    Signal<int> signal;
    Receiver receiver;
    Object::connect( signal, &receiver, &Receiver::onValue, ConnectionType::Auto );

    signal.emit( 1 );

    constexpr int kOps = 10000;
    PerfHarness::Allocations::start();
    for( int i = 0; i < kOps; ++i )
    {
        signal.emit( i );
    }
    const long allocations = PerfHarness::Allocations::stop();

    EXPECT_EQ( allocations, 0 )
        << allocations << " heap allocations over " << kOps << " same-thread auto emits. Auto "
        "resolves to a direct call when the receiver lives on the emitting thread, and must decide "
        "that before building anything -- see PERFORMANCE-20260813.md (P3).";
}

//! Fails if one connection starts costing more heap blocks than it does today.
//!
//! Pins P10, which is not a defect but a budget: a connection currently costs five blocks -- the
//! wrapper closure, the Slot, the Cleanup token, the ConnectionState, and the receiver's mIncoming
//! entry -- against Qt's two. That number should go **down** if anything, and this fails if a change
//! quietly adds a sixth.
//!
//! Counted over many connections so the amortised growth of the two containers is included; the
//! bar is per-connection so it does not move when the counts change.
TEST( PerformanceRegression, OneConnectionCostsAtMostFiveHeapBlocks )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    constexpr int kOps = 5000;

    Signal<int> signal;
    std::vector<std::unique_ptr<Receiver> > receivers;
    receivers.reserve( kOps );
    for( int i = 0; i < kOps; ++i )
    {
        receivers.push_back( std::unique_ptr<Receiver>( new Receiver() ) );
    }

    PerfHarness::Allocations::start();
    for( int i = 0; i < kOps; ++i )
    {
        Object::connect( signal, receivers[i].get(), &Receiver::onValue, ConnectionType::Direct );
    }
    const long allocations = PerfHarness::Allocations::stop();

    const double perConnection = double( allocations ) / kOps;
    EXPECT_LT( perConnection, 5.5 )
        << perConnection << " heap blocks per connect(), up from the five this was written at: the "
        "wrapper closure, the Slot, the Cleanup token, the ConnectionState, and the receiver's "
        "mIncoming entry. Qt manages two. Adding a sixth is a regression -- see "
        "PERFORMANCE-20260813.md (P10).";
}

//! Fails if a queued emit starts allocating more than it does today.
//!
//! The queued path is allowed to allocate -- the arguments have to outlive the call -- but not
//! without limit. Three blocks per emit is the budget: the closure with its argument tuple, the
//! MetaCallEvent, and the queue's own growth. It sat at 3.93 before the 2026-08-09 work and 2.78
//! after, so the bar is set above the worse of the two.
TEST( PerformanceRegression, QueuedEmitAllocationsStayBounded )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    Thread* here = Thread::currentThread();
    ASSERT_NE( here, nullptr );

    Thread worker( "regression-worker" );
    worker.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 3 );
    while( worker.eventDispatcher() == nullptr
        && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    ASSERT_NE( worker.eventDispatcher(), nullptr );

    Signal<int> signal;
    Receiver receiver;
    ASSERT_TRUE( receiver.moveToThread( &worker ) );
    Object::connect( signal, &receiver, &Receiver::onValue, ConnectionType::Queued );

    constexpr int kOps = 20000;
    PerfHarness::Allocations::start();
    for( int i = 0; i < kOps; ++i )
    {
        signal.emit( i );
    }
    const long allocations = PerfHarness::Allocations::stop();

    const double perEmit = double( allocations ) / kOps;
    EXPECT_LT( perEmit, 4.0 )
        << perEmit <<
        " heap blocks per queued emit. The budget is three -- the closure holding the "
        "copied arguments, the MetaCallEvent, and the queue's growth. It measured 3.93 before the "
        "2026-08-09 work and 2.78 after; going back above four means a copy has crept back in.";

    ASSERT_TRUE( worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } ) );
    worker.quit();
    worker.wait();
}
