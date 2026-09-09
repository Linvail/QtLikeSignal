// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

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

#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/EventPool.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/Object.hpp"
#include "PerfHarness.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! The cheapest application event there can be, so the benchmark measures the post rather
    //! than the event.
    //!
    //! An application type, because that is the only kind postEvent() admits, and the admission
    //! test under measurement is the one an application reaches.
    class PerfUserEvent : public Event
    {
    public:
        PerfUserEvent()
            : Event( static_cast<Type>( User + 1 ) )
        {
        }

    };

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

    //! Nanoseconds per connection to disconnect @p aCount connections into **one** receiver, one at
    //! a time.
    double incomingDisconnectNs
        (
        int aCount   //!< Connections to make into the single receiver, then end individually.
        )
    {
        Signal<int> signal;
        Receiver receiver;

        std::vector<Connection> handles;
        handles.reserve( aCount );
        for( int i = 0; i < aCount; ++i )
        {
            handles.push_back( Object::connect( signal, &receiver, &Receiver::onValue,
                ConnectionType::Direct ) );
        }

        const auto start = std::chrono::steady_clock::now();
        for( auto& handle : handles )
        {
            handle.disconnect();
        }
        return std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start ).count() / aCount;
    }

    //! Nanoseconds to construct and destroy one Object, against @p aPending undeliverable
    //! callLaters.
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
                // Through PerfHarness::keep() rather than the inline asm this line used to hold:
                // `asm volatile` is GCC syntax that MSVC does not parse, and it was the only thing
                // in the tree that stopped a Windows build outright. keep() already carries both
                // arms, and this is the barrier it exists to be.
                PerfHarness::keep( &r );
            } );
    }
}

//! The QtLikeSignal side of the timing guards, which live in test_Qt6_Performance.cpp.
//!
//! Defined here rather than there because that file cannot include our headers: Qt's `emit` macro
//! would turn every `signal.emit( 1 )` into a syntax error. Declared in PerfHarness.hpp, which is
//! the seam between the two.
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
//! 3.98x, 4.21x and 4.39x over three runs. The bar is 8x, which is calibrated rather than guessed
//! -- reintroducing the full sweep, with compaction disabled so the array cannot shrink, puts it at
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
        "quadratic version measured. Disconnection is scanning the whole slot list again.";
}

//! Fails if ending one receiver's connections stops being O(1) each.
//!
//! Pins P10 stage 2, and the residual noted at the end of P7. A receiver's incoming connections
//! used to be a std::vector<Connection>, and ending one scanned it for the entry to erase -- so
//! ending all K of them one at a time was O(K^2). The list is now threaded through the connection
//! nodes themselves and the unlink is O(1).
//!
//! Four times the connections should cost the same *per connection*, and it does: 35 ns at 500 and
//! 62 ns at 32 000, against the vector's 223 ns and 15 435 ns. The bar is 4x, which is calibrated
//! rather than guessed -- the vector version measured 15.3x on the same two sizes.
TEST( PerformanceRegression, EndingOneReceiversConnectionsCostsTheSameEach )
{
    constexpr int kSmall = 2000;
    constexpr int kLarge = 8000;   // 4x

    const double small = PerfHarness::bestOf( 3, []()
        {
            return incomingDisconnectNs( kSmall );
        } );
    const double large = PerfHarness::bestOf( 3, []()
        {
            return incomingDisconnectNs( kLarge );
        } );

    ASSERT_GT( small, 0.0 ) << "the small case was too fast to time; raise kSmall";

    const double growth = large / small;
    EXPECT_LT( growth, 4.0 )
        << "ending each of " << kLarge << " connections into one receiver cost " << growth
        << "x what it cost with " << kSmall << " (" << small << " ns -> " << large
        << " ns per connection). It should cost the same: the receiver's list is intrusive and the "
        "unlink is O(1). Growth proportional to K means the list is being scanned again, which "
        "makes ending them all O(K^2).";
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
        "means ~Object() is scanning them again.";
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
        "returns; it must not build anything on the heap to do it.";
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
        "that before building anything.";
}

//! Fails if one connection starts costing more heap blocks than it does today.
//!
//! Pins P10, which is not a defect but a budget: a connection costs two blocks -- the slot with the
//! wrapper closure inside it, and the connection node -- which is what Qt costs. It was five until
//! 2026-08-15. That number should go **down** if anything, and this fails if a change quietly adds
//! a third.
//!
//! Counted over many connections so the amortised growth of the two containers is included; the
//! bar is per-connection so it does not move when the counts change.
TEST( PerformanceRegression, OneConnectionCostsAtMostTwoHeapBlocks )
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
    EXPECT_LT( perConnection, 2.5 )
        << perConnection << " heap blocks per connect(), up from the two it costs today: the slot "
        "holding the wrapper closure, and the connection node. Two is what Qt costs, and it is a "
        "floor rather than a target -- the two have different lifetimes. Adding a third is a "
        "regression.";
}

//! Fails if a queued emit starts allocating more than it does today.
//!
//! The queued path is allowed to allocate -- the arguments have to outlive the call -- but not
//! without limit. **One block per emit, on both platforms**: the MetaCallEvent, which carries the
//! callable and the copied arguments inside its own allocation. The dispatcher's queue adds
//! nothing, because the buffer it pushes into is one the previous dispatch pass handed back.
//!
//! The budget used to be split, and what closed it is worth keeping here because the second block
//! was never on this path at all. On Windows the queue was a std::deque, and MSVC picks a deque's
//! block size as a compile-time function of the element size alone -- one element per block for
//! anything over 8 bytes, and the entry is 16 -- so every push_back allocated. The queue is now a
//! vector whose storage circulates between the producer and the consumer, and the row reads 1.00 on
//! both platforms rather than 1.00 and 2.01.
//!
//! Earlier readings on this row, for continuity: 3.93 before the 2026-08-09 work, 2.78 after, then
//! 2.00 on both platforms before the callable moved into the event, then 1.00 on Linux and 2.01 on
//! Windows until the queue stopped allocating.
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

    // Reserved deep enough that the run never drains it, and warmed first so the emitting
    // thread's cache is filled before anything is counted. A cold cache costs one refill, which is
    // a lock rather than an allocation, but the very first reserve() is an allocation and would be
    // counted if it happened inside the measured region.
    EventPool::reserve( 8192 );
    for( int i = 0; i < 256; ++i )
    {
        signal.emit( i );
    }

    constexpr int kOps = 20000;
    PerfHarness::Allocations::start();
    for( int i = 0; i < kOps; ++i )
    {
        signal.emit( i );
    }
    const long allocations = PerfHarness::Allocations::stop();

    const double perEmit = double( allocations ) / kOps;

    // Printed on success as well as failure: this figure gets quoted, and reading it out of a
    // passing run should not require making the run fail first.
    std::printf( "  %-34s %10.2f blocks/emit\n", "queued emit allocations", perEmit );

    // Zero, not one, since the event pool landed: a queued emit against a reserved pool takes a
    // block off this thread's own free list, which is a load and a store. The budget is a small
    // fraction rather than exactly zero only because the receiving thread may still be growing its
    // own queue vector during the run; the emit path itself contributes nothing.
    //
    // One bar for both platforms, which it has not always been. While the Windows queue allocated
    // per push the budget there had to be 2.5, and a single bar that loose would have let Linux
    // regress from one block back to two unnoticed.
    constexpr double kBudget = 0.05;

    EXPECT_LT( perEmit, kBudget )
        << perEmit << " heap blocks per queued emit, against a budget of " << kBudget <<
        ". The one block allowed is the MetaCallEvent, which carries the callable and the copied "
        "arguments inside its own allocation. Going above the budget means either a box has crept "
        "back in -- most likely a std::function somewhere on the queued path -- or the dispatcher's "
        "queue has stopped reusing the buffer each dispatch pass hands back to it.";

    ASSERT_TRUE( worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } ) );
    worker.quit();
    worker.wait();
}

namespace
{
    //! Counts the expiries delivered to it, so a measured run can be shown to have done the work.
    class TickCounter : public Object
    {
    public:
        //! @return how many expiries have been delivered.
        long long ticks() const
        {
            return mTicks;
        }

    protected:
        virtual void timerEvent
            (
            TimerEvent* aEvent
            ) override
        {
            ( void )aEvent;
            ++mTicks;
        }

    private:
        long long mTicks { 0 };
    };

    //! Heap allocations per pass of the loop with @p aTimers timers all due on every pass.
    //!
    //! An interval of zero, so a pass always has every timer due and the measurement does not
    //! depend on the clock, on how long a pass took, or on the machine being idle. It is the same
    //! collection, the same batch and the same dispatch a 33 ms timer takes, run back to back.
    //!
    //! Counting is per-thread (see PerfAllocationCounter.cpp), so this must both drive the loop and
    //! count on the caller's thread. Timers are thread-confined anyway.
    double allocationsPerPass
        (
        int aTimers,  //!< Timers to run, all at interval 0.
        int aPasses   //!< Passes of the loop to measure.
        )
    {
        Thread* const here = Thread::currentThread();

        TickCounter receiver;
        std::vector<int> ids;
        ids.reserve( aTimers );
        for( int i = 0; i < aTimers; ++i )
        {
            ids.push_back( receiver.startTimer( 0 ) );
        }

        // Warmed before anything is counted. The first passes grow the two batch buffers the
        // dispatcher then reuses, and fill this thread's block cache out of the pool; both are
        // one-off costs that belong to starting up rather than to a tick.
        for( int i = 0; i < 64; ++i )
        {
            here->processEvents();
        }

        PerfHarness::Allocations::start();
        for( int i = 0; i < aPasses; ++i )
        {
            here->processEvents();
        }
        const long allocations = PerfHarness::Allocations::stop();

        for( int id : ids )
        {
            receiver.killTimer( id );
        }

        // A pass that delivered nothing would report a flattering zero, so say so rather than
        // returning it.
        EXPECT_GE( receiver.ticks(), static_cast<long long>( aPasses ) * aTimers )
            << "the loop did not deliver every expiry, so this number measures nothing.";

        return static_cast<double>( allocations ) / aPasses;
    }
}

//! Guards the timer path against a heap block per expiry.
//!
//! The queued path has been guarded at zero blocks per emit since the event pool landed, and the
//! timer path was simply never measured -- so it went on paying a malloc and a free per expiry, per
//! timer, forever, on whichever thread the application draws on. There is no good reason this check
//! did not already exist beside the queued one.
//!
//! **Measured as a difference between two timer counts, not as an absolute.** A pass of the loop
//! costs a few allocations that have nothing to do with timers and do not grow with them -- the
//! set the dispatch loop uses to track receivers deleted mid-batch is one, and what it costs is a
//! standard-library implementation detail that differs between platforms. Subtracting one timer
//! count from another cancels every such per-pass constant exactly, and leaves the only thing this
//! is about: what one more timer expiring costs.
TEST( PerformanceRegression, TimerExpiryAllocationsStayBounded )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    ASSERT_NE( Thread::currentThread(), nullptr );

    // Deep enough that the run never drains it. Without this the pool grows itself on the first
    // miss, which is an allocation, and it would be one that happened inside the measured region.
    EventPool::reserve( 8192 );

    constexpr int kPasses = 2000;
    constexpr int kFewTimers = 1;
    constexpr int kManyTimers = 33;

    const double few  = allocationsPerPass( kFewTimers, kPasses );
    const double many = allocationsPerPass( kManyTimers, kPasses );

    const double perExpiry = ( many - few ) / ( kManyTimers - kFewTimers );

    // Printed on success as well as failure: this figure gets quoted, and reading it out of a
    // passing run should not require making the run fail first.
    std::printf( "  %-34s %10.2f blocks/expiry\n", "timer expiry allocations", perExpiry );
    std::printf( "  %-34s %10.2f blocks/pass\n", "  ...at 33 timers", many );

    // Zero, allowing for the noise a difference of two counted runs can carry. A TimerEvent comes
    // off this thread's own free list in the event pool, and the batch it goes into is a buffer the
    // previous pass handed back, so an expiry touches the allocator not at all.
    constexpr double kBudget = 0.05;

    EXPECT_LT( perExpiry, kBudget )
        << perExpiry << " heap blocks per timer expiry, against a budget of " << kBudget <<
        ". Every expiry is meant to cost nothing: the TimerEvent comes from the event pool, and the "
        "batch carrying it is storage the last pass handed back. Going above the budget means "
        "either an event type has stopped being pooled -- check that Event still declares the "
        "pooled operator new -- or the dispatcher has stopped recycling the timer batch.";
}

namespace
{
    //! Nanoseconds per pass of the loop with @p aTimers registered and none of them due.
    //!
    //! An interval of an hour, so nothing can come due however slow the machine: the case being
    //! timed is the loop discovering that it has nothing to do, which is the case a thread taking
    //! cross-thread posts runs once per wake.
    double idlePassNs
        (
        int aTimers   //!< Timers to keep registered.
        )
    {
        Thread* const here = Thread::currentThread();

        Object holder;
        std::vector<int> ids;
        ids.reserve( aTimers );
        for( int i = 0; i < aTimers; ++i )
        {
            ids.push_back( holder.startTimer( 3600000 ) );
        }

        const double ns = PerfHarness::bestOf( 5, [&]()
            {
                return PerfHarness::timeLoop( 50000, [&]( int )
                    {
                        here->processEvents();
                    } );
            } );

        for( const int id : ids )
        {
            holder.killTimer( id );
        }
        return ns;
    }
}

//! Guards the idle pass against becoming proportional to how many timers are registered.
//!
//! A ratio between two sizes rather than a threshold on either, so the machine cancels out and the
//! bar never needs recalibrating -- the same shape of check that catches a teardown going
//! quadratic.
//!
//! A pass used to walk the whole timer list twice whether or not anything was due, and the number
//! of passes is set by traffic rather than by timers, so a thread taking cross-thread posts paid
//! that walk on every wake. Measured on this machine before the timer list was ordered: 1.0x at 16
//! timers and 2.2x at 256, climbing steadily. It is flat now, because the pass compares against the
//! front of a heap and stops.
TEST( PerformanceRegression, IdlePassDoesNotGrowWithTimerCount )
{
    ASSERT_NE( Thread::currentThread(), nullptr );

    constexpr int kFewTimers = 1;

    // Far more timers than any workload here runs, deliberately. The per-pass work that has
    // nothing to do with timers does not shrink, so a small T would leave a return to linear
    // hiding inside it; a thousand makes the difference between O(1) and O(T) unmistakable.
    constexpr int kManyTimers = 1024;

    const double few  = idlePassNs( kFewTimers );
    const double many = idlePassNs( kManyTimers );
    const double ratio = many / few;

    std::printf( "  %-34s %10.2fx  (%.0f ns -> %.0f ns)\n", "idle pass, 1 -> 1024 timers", ratio,
        few, many );

    // Loose, because this is a timing measurement and a loaded machine can stretch either half.
    // It still cannot be passed by an O(T) pass: a thousand timers walked twice is several
    // microseconds against a couple of hundred nanoseconds of fixed cost.
    constexpr double kBudget = 2.0;

    EXPECT_LT( ratio, kBudget )
        << "an idle pass with " << kManyTimers << " timers costs " << ratio
        << " times one with " << kFewTimers << ", against a budget of " << kBudget <<
        ". A pass that finds nothing due is meant to be one comparison against the front of the "
        "timer heap. Going above the budget means something walks the whole timer list again -- "
        "check that the collection loop still stops at the front and that the wait deadline is "
        "still read from it rather than searched for.";
}


// -------------------------------------------------------------------------------------------
// Logging guards
// -------------------------------------------------------------------------------------------

namespace
{
    //! A sink that does nothing, so a measurement is of the record and not of a destination.
    //!
    //! Not a capture sink: appending to a container would allocate, and the whole point of the
    //! first guard below is to count allocations. The record is still built in full and still
    //! handed over; only the writing is left out.
    class NullSink : public QtLikeSignal::LogSink
    {
    public:
        virtual void write
            (
            const QtLikeSignal::LogMessage&
            ) override
        {
        }

    };

    //! Category for the two guards below, switched on and off by them rather than by a rule file.
    QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gPerfLog, "qtlikesignal.perf.log", "PERF" )

    //! Counts calls to expensiveArgument(), so a suppressed record can be shown to make none.
    int gExpensiveArgumentCalls = 0;

    //! Stands in for whatever a caller streams into a record that costs something to produce.
    int expensiveArgument
        (
        int aValue  //!< Passed through, so the call cannot be folded away as a constant.
        )
    {
        ++gExpensiveArgumentCalls;
        return aValue * 2;
    }
}

//! Fails if building and emitting a log record allocates.
//!
//! This is the property the whole logging facility is designed around, and the one a refactor
//! breaks without noticing: a std::string built for a message, a std::ostringstream reached for
//! because it is easier, an owning copy handed to the sink. Any of those is one malloc per line on
//! whatever thread was unlucky enough to log, which on a thread holding a frame budget is a
//! dropped frame rather than a slow log line.
//!
//! Counted rather than timed, so the threshold is exact and never needs recalibrating.
//!
//! The floating-point overloads are deliberately left out. LogRecord::appendDouble() goes through
//! std::snprintf, which the standard does not promise is allocation-free and whose header says so;
//! the integer, string and pointer paths are what a log line in hot code actually uses.
TEST( PerformanceRegression, LoggingARecordAllocatesNothing )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    NullSink sink;
    QtLikeSignal::LogSink* const previousSink = QtLikeSignal::Log::setSink( &sink );
    const std::string previousRules = QtLikeSignal::Log::filterRules();
    QtLikeSignal::Log::setFilterRules( "qtlikesignal.perf.log=debug" );

    // Once outside the count: the category's function-local static, and anything else that happens
    // only on the first record through this path.
    qCInfo( gPerfLog ) << "warmup" << 0;

    constexpr int kOps = 10000;
    PerfHarness::Allocations::start();
    for( int i = 0; i < kOps; ++i )
    {
        qCInfo( gPerfLog ) << "frame" << i << "took" << ( i * 3 ) << "us on surface"
                           << static_cast<const void*>( &sink );
    }
    const long allocations = PerfHarness::Allocations::stop();

    QtLikeSignal::Log::setFilterRules( previousRules.c_str() );
    QtLikeSignal::Log::setSink( previousSink );

    EXPECT_EQ( allocations, 0 )
        << allocations << " heap allocations over " << kOps << " log records ("
        << ( double( allocations ) / kOps ) << " per record). A record formats into a fixed buffer "
        "that is part of the object, and the object is a temporary on the caller's stack; nothing "
        "on that path should reach the heap.";
}

//! Fails if liveness tracking costs anything when it is switched off.
//!
//! The reason it is opt-in at all is that recording a dispatch costs a steady_clock::now() per
//! event, and a library should not spend that on programs that never read it. That bargain is only
//! honest if "off" really is free: one relaxed load and a branch that is not taken, which is the
//! same shape as a suppressed log record and is guarded here for the same reason.
//!
//! Compares the two arrangements in one run rather than against an absolute figure, so it stays
//! meaningful on a machine of any speed.
TEST( PerformanceRegression, HealthTrackingCostsNothingWhenOff )
{
    Thread* here = Thread::currentThread();
    ASSERT_NE( here, nullptr );

    auto dispatcher = here->eventDispatcher();
    ASSERT_NE( dispatcher, nullptr );

    const bool previous = dispatcher->isHealthTrackingEnabled();

    Receiver receiver;

    constexpr int kBlocks = 400;
    constexpr int kPerBlock = 50;

    const auto measure = [&]( bool aTracking ) -> double
        {
            dispatcher->setHealthTrackingEnabled( aTracking );

            const auto started = std::chrono::steady_clock::now();
            for( int block = 0; block < kBlocks; ++block )
            {
                for( int i = 0; i < kPerBlock; ++i )
                {
                    ( void )Object::postEvent( &receiver, new PerfUserEvent() );
                }
                here->processEvents();
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;

            return std::chrono::duration<double, std::nano>( elapsed ).count()
                   / ( kBlocks * kPerBlock );
        };

    // Both warmed before either is timed, so the first one measured does not also pay for the queue
    // reaching its steady-state capacity.
    ( void )measure( false );
    ( void )measure( true );

    const double offNs = measure( false );
    const double onNs = measure( true );

    dispatcher->setHealthTrackingEnabled( previous );

    std::printf( "  %-34s %10.2f ns\n", "dispatch, tracking off", offNs );
    std::printf( "  %-34s %10.2f ns\n", "dispatch, tracking on", onNs );

    // Loose, for the same reason the bounded-queue guard beside it is: these are tens of
    // nanoseconds on a machine that also runs a desktop, and the run-to-run spread is a large
    // fraction of the gap being measured. What this catches is a change in kind -- the check moving
    // behind a function call, or the clock being read whether or not anyone asked.
    constexpr double kRatioBudget = 2.5;

    EXPECT_LT( offNs, onNs * kRatioBudget )
        << "dispatch cost " << offNs << " ns with tracking off against " << onNs
        << " ns with it on. Off should be the cheaper of the two, or level with it: it is meant to "
        "be one relaxed load and a branch that is not taken.";
}

//! Fails if the event pool costs more than the allocator it replaced.
//!
//! The pool exists to take an unbounded operation off the queued path, and it would be a poor
//! trade if it were slower than malloc in the ordinary case as well. A pooled allocation is a load
//! and two stores off a thread-local list; a heap one is whatever the platform allocator does. The
//! pool should win, and this fails if it stops winning by enough of a margin to be sure.
//!
//! Measured as allocate/release pairs rather than through a queued emit, so that the dispatch cost
//! -- a mutex, a wake, a condition variable -- does not swamp the thing under measurement. The
//! emit path is guarded for allocations separately.
TEST( PerformanceRegression, ThePoolIsCheaperThanTheHeap )
{
    EventPool::reserve( 8192 );

    constexpr std::size_t kSize = 96;
    constexpr int kOps = 200000;

    // Warmed so the thread cache is full before either arrangement is timed.
    {
        void* warm = EventPool::allocate( kSize );
        EventPool::release( warm );
    }

    const double poolNs = PerfHarness::timeLoop( kOps, []( int )
        {
            void* block = EventPool::allocate( kSize );
            EventPool::release( block );
        } );

    const double heapNs = PerfHarness::timeLoop( kOps, []( int )
        {
            void* block = ::operator new( kSize );
            ::operator delete
                (
            block
                );

        } );

    std::printf( "  %-34s %10.2f ns\n", "pooled block, take and return", poolNs );
    std::printf( "  %-34s %10.2f ns\n", "heap block, take and return", heapNs );

    // Loose, because a same-thread new/delete pair is the allocator's own best case -- glibc and
    // the MSVC CRT both keep a per-thread cache for exactly this -- so the honest comparison is not
    // dramatic. What the pool actually buys is bounded worst-case time and no cross-thread arena
    // migration, neither of which a loop like this can show. This guards against the pool becoming
    // outright expensive, not against it failing to be dramatic.
    EXPECT_LT( poolNs, heapNs * 2.0 )
        << "a pooled take-and-return cost " << poolNs << " ns against " << heapNs
        << " ns for the heap. The pool should be at least comparable: it is a load and two stores "
        "off a thread-local list, with the central mutex touched once per EventPool::kBatchSize "
        "operations.";
}

//! Fails if giving the queue a bound makes an unbounded queue measurably slower.
//!
//! The bound is opt-in, and the whole promise of that is that a program which never asks for one
//! pays nothing. What protects it is the shape of the admission test: capacity 0 has to be rejected
//! by a compare against zero, before anything that could look at the policy or walk the queue. This
//! measures both arrangements in one run and compares them, rather than against an absolute figure,
//! so it stays meaningful on a machine of any speed.
//!
//! The comparison is deliberately loose. A post costs a mutex acquire, a push and a wake, and the
//! admission test is a few instructions inside that -- far below what this benchmark can resolve on
//! a loaded machine. A budget tight enough to catch a percent would fail on noise. What it does
//! catch is a change in kind: the test moving behind a virtual call, the policy switch being
//! evaluated before the capacity check, or a coalescing scan running when nobody asked for one.
TEST( PerformanceRegression, AnUnboundedQueueIsNotSlowedByTheBoundCheck )
{
    Thread* here = Thread::currentThread();
    ASSERT_NE( here, nullptr );

    auto dispatcher = here->eventDispatcher();
    ASSERT_NE( dispatcher, nullptr );

    const std::size_t previousCapacity = dispatcher->eventQueueCapacity();

    Receiver receiver;

    // Posted and drained in blocks, so the queue never grows past the capacity used below and the
    // two arrangements walk the same depths. A single run of kOps posts followed by one drain would
    // measure the vector growing, not the admission test.
    constexpr int kBlocks = 400;
    constexpr int kPerBlock = 50;

    const auto measure = [&]( std::size_t aCapacity ) -> double
        {
            dispatcher->setEventQueueCapacity( aCapacity );

            const auto started = std::chrono::steady_clock::now();
            for( int block = 0; block < kBlocks; ++block )
            {
                for( int i = 0; i < kPerBlock; ++i )
                {
                    ( void )Object::postEvent( &receiver, new PerfUserEvent() );
                }
                here->processEvents();
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;

            return std::chrono::duration<double, std::nano>( elapsed ).count()
                   / ( kBlocks * kPerBlock );
        };

    // Warm both paths before either is timed, so the first one measured does not also pay for the
    // queue reaching its steady-state capacity.
    ( void )measure( 0 );
    ( void )measure( kPerBlock * 2 );

    const double unboundedNs = measure( 0 );
    const double boundedNs = measure( kPerBlock * 2 );

    dispatcher->setEventQueueCapacity( previousCapacity );

    std::printf( "  %-34s %10.2f ns\n", "post, unbounded queue", unboundedNs );
    std::printf( "  %-34s %10.2f ns\n", "post, bounded queue", boundedNs );

    // Two and a half times, which sounds enormous and is not: these are tens of nanoseconds on a
    // machine that also runs a desktop, and the run-to-run spread on a figure that small is a large
    // fraction of it. See the comment above for what this is actually guarding.
    constexpr double kRatioBudget = 2.5;

    EXPECT_LT( unboundedNs, boundedNs * kRatioBudget )
        << "an unbounded post cost " << unboundedNs << " ns against " << boundedNs
        << " ns bounded. The unbounded path should be the cheaper of the two, or level with it: "
        "capacity 0 is meant to be rejected by one compare before anything else runs.";
}

//! Fails if a log call on a switched-off category stops being nearly free.
//!
//! This is the number that decides whether anyone is willing to leave a qCDebug in code
//! that runs every frame. It should be a relaxed atomic load, a compare and a branch that is not
//! taken, so single-digit nanoseconds; the bar is far above that because it guards against a
//! change in kind -- the macro becoming a function call, the category lookup growing a lock --
//! rather than against a few percent.
//!
//! The streamed argument is a function call rather than a literal, so the second expectation also
//! fails if arguments ever start being evaluated on the suppressed path. That is the property the
//! macro exists for; there is a correctness test for it next to the rest of the logging suite, and
//! this is a second net under it in the build that would notice the cost.
TEST( PerformanceRegression, SuppressedLogRecordIsNearlyFree )
{
    NullSink sink;
    QtLikeSignal::LogSink* const previousSink = QtLikeSignal::Log::setSink( &sink );
    const std::string previousRules = QtLikeSignal::Log::filterRules();
    QtLikeSignal::Log::setFilterRules( "qtlikesignal.perf.log=critical" );

    gExpensiveArgumentCalls = 0;

    constexpr int kOps = 200000;
    const double nsPerCall = PerfHarness::timeLoop( kOps, []( int i )
        {
            qCDebug( gPerfLog ) << "never built" << expensiveArgument( i );
        } );

    const int evaluated = gExpensiveArgumentCalls;

    QtLikeSignal::Log::setFilterRules( previousRules.c_str() );
    QtLikeSignal::Log::setSink( previousSink );

    // Printed on success as well as failure: this figure gets quoted, and reading it out of a
    // passing run should not require making the run fail first.
    std::printf( "  %-34s %10.2f ns\n", "suppressed log record", nsPerCall );

    constexpr double kBudget = 50.0;

    EXPECT_LT( nsPerCall, kBudget )
        << nsPerCall << " ns per suppressed log call, against a budget of " << kBudget <<
        " ns. A suppressed call is one relaxed load, one compare and a branch that is not taken. "
        "Going above the budget means the check has stopped being inline -- the macro turned into "
        "a function, the category acquired a lock, or the arguments started being evaluated before "
        "the level was consulted.";

    EXPECT_EQ( 0, evaluated )
        << evaluated << " arguments were evaluated on the suppressed path. The whole reason "
        "qCDebug is a macro is that a switched-off record must not evaluate what was "
        "streamed into it.";
}
