// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Shared timing harness for the dispatch benchmarks, so every library is measured by identical
//! code and the results land in one table.
//!
//! Each library gets its own translation unit, which is a requirement rather than tidiness: Qt
//! defines `emit` as an empty macro, so a file that includes both Qt headers and QtLikeSignal would
//! turn every `sig.emit( 1 )` into a syntax error. Nothing in this header may use `emit`.

#ifndef QT_LIKE_SIGNAL_PERF_HARNESS_HPP
#define QT_LIKE_SIGNAL_PERF_HARNESS_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace PerfHarness
{
    //! One measurement: what was measured, for which library, and what it cost.
    struct Result
    {
        std::string mScenario;   //!< Scenario name; identical across libraries so rows line up.
        std::string mLibrary;    //!< Library the measurement belongs to.
        double mNsPerOp;         //!< Nanoseconds per operation.
    };

    //! The collected results.
    //!
    //! A function-local static rather than a namespace-scope object so the benchmark translation
    //! units share one instance without a definition in some arbitrary .cpp, and without depending
    //! on static initialisation order between them.
    inline std::vector<Result>& results()
    {
        static std::vector<Result> sResults;
        return sResults;
    }

    //! Records a measurement and echoes it, so a truncated run still shows its progress.
    inline void record
        (
        const std::string& aScenario,   //!< Scenario name.
        const std::string& aLibrary,    //!< Library measured.
        double aNsPerOp                 //!< Nanoseconds per operation.
        )
    {
        results().push_back( { aScenario, aLibrary, aNsPerOp } );
        std::printf( "  %-34s %-13s %10.1f ns/op\n", aScenario.c_str(), aLibrary.c_str(), aNsPerOp )
        ;
        std::fflush( stdout );
    }

    //! Times @p aBody run @p aCount times and returns nanoseconds per iteration.
    template <typename Body>
    double timeLoop
        (
        int aCount,   //!< Iterations to run.
        Body aBody    //!< Callable invoked with the iteration index.
        );

    //! Stops the optimiser deleting a loop whose result is never read.
    //!
    //! Without this the same-thread emit loops can be removed wholesale, which shows up as a
    //! suspiciously round 0.0 ns/op rather than as an error.
    template <typename T> inline void keep
        (
        T&& aValue   //!< Value to pretend to consume.
        )
    {
        #if defined( _MSC_VER )
            volatile auto sink = aValue;
            ( void )sink;
        #else
            asm volatile ( "" : : "r,m" ( aValue ) : "memory" );
        #endif
    }

    //! Puts the process into its multi-threaded allocator state before anything is measured.
    //!
    //! glibc keeps a fast, lock-free path for malloc while a process is still single-threaded, and
    //! drops it permanently the moment a second thread has existed -- `__libc_single_threaded` is
    //! never set back. Whichever benchmark first starts a worker thread therefore makes every
    //! allocation after it more expensive, for every library, for the rest of the run.
    //!
    //! Spawning and joining one thread up front settles every library into the same state, which is
    //! also the state any threaded application is in.
    inline void settleAllocatorState()
    {
        std::thread( []()
            {
            } ).join();
    }

    //! The library the ratio columns are expressed against.
    //!
    //! Named, rather than taken from whichever library happened to record first. That used to be
    //! how the baseline was chosen, and it was quietly fragile: registration order across
    //! translation units is link order, so adding the boost benchmark as a fourth file moved boost
    //! to the front and inverted every ratio in the table -- the same numbers, now meaning the
    //! opposite thing, with only the footer to say so. A table that can silently change direction
    //! is worse than no table.
    //!
    //! QtLikeSignal is the baseline because it is the library these benchmarks now belong to; the
    //! useful question is always what it costs against the others.
    inline const char* baselineLibrary()
    {
        return "QtLikeSignal";
    }

    //! Prints one row per scenario with a column per library, plus ratios against the baseline.
    //!
    //! The baseline column comes first and the rest follow first appearance. A library that skipped
    //! a scenario leaves a gap rather than a misleading zero.
    inline void printSummary()
    {
        auto& all = results();
        if( all.empty() )
        {
            return;
        }

        // Distinct scenarios and libraries, both in first-appearance order.
        std::vector<std::string> scenarios;
        std::vector<std::string> libraries;
        for( const auto& r : all )
        {
            if( std::find( scenarios.begin(), scenarios.end(), r.mScenario ) == scenarios.end() )
            {
                scenarios.push_back( r.mScenario );
            }
            if( std::find( libraries.begin(), libraries.end(), r.mLibrary ) == libraries.end() )
            {
                libraries.push_back( r.mLibrary );
            }
        }

        // Move the baseline to the front, so the ratio columns read the same way no matter what
        // order the translation units registered in. Absent -- a filtered run measuring only one
        // other library -- the first column stands in, and the footer still names whatever it is.
        const auto baseline = std::find( libraries.begin(), libraries.end(), baselineLibrary() );
        if( baseline != libraries.end() )
        {
            std::rotate( libraries.begin(), baseline, baseline + 1 );
        }

        //! Looks a measurement up, or returns -1 if that library skipped that scenario.
        auto lookup = [&all]( const std::string& aScenario, const std::string& aLibrary )
            {
                for( const auto& r : all )
                {
                    if( r.mScenario == aScenario && r.mLibrary == aLibrary )
                    {
                        return r.mNsPerOp;
                    }
                }
                return -1.0;
            };

        std::printf( "\n%-34s", "scenario" );
        for( const auto& lib : libraries )
        {
            std::printf( " %14s", lib.c_str() );
        }
        for( size_t i = 1; i < libraries.size(); ++i )
        {
            std::printf( " %12s", ( "vs " + libraries[i] ).c_str() );
        }
        std::printf( "\n%s\n", std::string( 34 + 15 * libraries.size()
            + 13 * ( libraries.size() - 1 ), '-' ).c_str() );

        for( const auto& scenario : scenarios )
        {
            std::printf( "%-34s", scenario.c_str() );
            for( const auto& lib : libraries )
            {
                const double ns = lookup( scenario, lib );
                if( ns < 0.0 )
                {
                    std::printf( " %14s", "-" );
                }
                else
                {
                    std::printf( " %11.1f ns", ns );
                }
            }
            const double base = lookup( scenario, libraries[0] );
            for( size_t i = 1; i < libraries.size(); ++i )
            {
                const double other = lookup( scenario, libraries[i] );
                if( base > 0.0 && other > 0.0 )
                {
                    std::printf( " %11.2fx", base / other );
                }
                else
                {
                    std::printf( " %12s", "-" );
                }
            }
            std::printf( "\n" );
        }

        std::printf( "\nRatios are %s against each other library; > 1 means %s is slower.\n",
            libraries[0].c_str(), libraries[0].c_str() );
    }

    // Iteration counts, shared so every library does the same amount of work. The queued scenario
    // is smaller because each in-flight emit holds a heap allocation until the receiving loop
    // drains it, so a large count measures allocator behaviour as much as dispatch.
    constexpr int kConnectOps = 20000;
    constexpr int kDirectOps  = 1000000;
    constexpr int kQueuedOps  = 200000;

    // The round-trip scenario waits for each delivery before making the next emit, so one operation
    // costs a wake and a context switch rather than a queue insertion. Two orders of magnitude
    // smaller than kQueuedOps because one operation is that much more expensive; the two rows are
    // not comparable to each other and are not meant to be.
    constexpr int kQueuedRoundTripOps = 20000;

    // The blocking scenario is the same shape as the round-trip one -- one wake out, one wake back,
    // per operation -- and is deliberately given the *same* count so the two rows can be read
    // against each other directly. That comparison is the point of the row: round-trip is a caller
    // doing the waiting by hand, blocking is the library doing it, and the question is what the
    // library's version costs over the hand-rolled one.
    constexpr int kBlockingOps = kQueuedRoundTripOps;

    // The deferred-call scenarios post without waiting, and are given kQueuedOps for the same
    // reason that row is: what they compare is the cost of *making* a deferred call -- the event,
    // the queue insertion, the dispatch -- across payloads that differ. Pacing them round-trip
    // instead would put a thread wake in front of every operation, and a wake is two orders of
    // magnitude dearer than the difference being looked for, so every row would report the wake.
    constexpr int kDeferredCallOps = kQueuedOps;

    //! Iterations for the thread pool scenarios.
    //!
    //! Fewer than the signal counts, because every one of these hands work to another thread and
    //! waits for it to arrive: the loop is bounded by two thread wake-ups rather than by a call.
    constexpr int kPoolOps = 20000;

    // Deferred calls made while counting allocations. Two orders of magnitude smaller than
    // kDeferredCallOps because nothing there is timed -- this count only has to be large enough
    // that the one-off allocations behind the first call round away.
    constexpr int kDeferredCallAllocOps = 200;

    // How many timers the idle-pass scenario keeps registered, and none of them due. Two orders of
    // magnitude between the ends, so a per-pass cost that grows with the number of registered
    // timers shows up as growth in the table rather than as a number with nothing to compare
    // against.
    constexpr int kIdlePassTimerCounts[] = { 1, 16, 256 };

    // Passes of the loop timed per measurement. Each is a lock, a comparison against the next
    // deadline, and the bookkeeping a pass does whether or not it dispatches anything, so this is
    // large enough to swamp the clock and small enough to keep the whole scenario under a second
    // per library.
    constexpr int kIdlePassOps = 100000;

    // The interval every timer in that scenario is given: an hour, so none of them can come due
    // during a run however slow the machine. The scenario is the loop finding it has nothing to do.
    constexpr int kIdlePassIntervalMs = 3600000;

    //! Names the idle-pass row for @p aTimers timers, so every library labels it identically.
    //!
    //! A function rather than three literals per library, because the rows only line up in the
    //! summary table if the strings match exactly, and three copies of a string in two files is
    //! how they stop matching.
    inline std::string idlePassScenario
        (
        int aTimers   //!< Timers registered for that row.
        )
    {
        return "idle pass, " + std::to_string( aTimers )
               + ( aTimers == 1 ? " timer" : " timers" );
    }

    // Connections resident on one signal before the disconnect scenario ends them one by one.
    // Deliberately the same as kConnectOps, so the connect() and disconnect() rows describe the two
    // halves of the same connection's life and can be read against each other directly.
    constexpr int kDisconnectOps = kConnectOps;

    //! Grows and faults in the heap the benchmarks will need, before anything is measured.
    //!
    //! Running each test in its own process is not the fix. That makes every library cold rather
    //! than one of them, and the cold penalty is not uniform: ours is about 1.7x, Qt 6's about
    //! 1.1x, so the ratios move anyway. The state to settle into is warm, because it is the state a
    //! process is in by the time any of this code runs in earnest.
    //!
    //! The blocks are written to, not merely allocated. Reserving address space is cheap; it is the
    //! page fault on first touch that costs, and an untouched allocation does not pay it here.
    inline void settleHeap()
    {
        constexpr std::size_t kBlocks = 2 * static_cast<std::size_t>( kConnectOps );
        constexpr std::size_t kBlockBytes = 64;

        std::vector<void*> blocks;
        blocks.reserve( kBlocks );
        for( std::size_t i = 0; i < kBlocks; ++i )
        {
            void* block = ::operator new( kBlockBytes );
            std::memset( block, 0, kBlockBytes );
            blocks.push_back( block );
        }
        for( void* block : blocks )
        {
            ::operator delete( block );
        }
    }

    //! Runs @p aBody @p aRepeats times and keeps the fastest result.
    //!
    //! The minimum, not the mean or the median. A constant-factor regression of a few nanoseconds
    //! sits inside the run-to-run spread of a loaded machine, and averaging buries it -- comparing
    //! medians over four runs once reported two builds as identical when one was 29% slower. The
    //! minimum estimates how fast the code can go; everything above it is the scheduler.
    template <typename Body>
    double bestOf
        (
        int aRepeats,   //!< How many times to measure.
        Body aBody      //!< Returns one measurement.
        )
    {
        double best = aBody();
        for( int i = 1; i < aRepeats; ++i )
        {
            best = std::min( best, aBody() );
        }
        return best;
    }

    //! Counts heap allocations between start() and stop(), process-wide.
    //!
    //! Exact, and therefore the most durable check in this file: an allocation count does not vary
    //! with machine speed, load, or compiler version, so a threshold on it never flakes and never
    //! needs recalibrating. Two of the regressions this suite exists to catch -- a per-emit
    //! allocation, and connect() spreading a connection over more blocks -- are countable rather
    //! than timeable.
    //!
    //! Requires the operator new/delete replacements in PerfAllocationCounter.cpp; without them the
    //! counter simply stays at zero, so the tests that use it are skipped rather than wrong.
    //! QtLikeSignal measurements, callable from the Qt translation unit.
    //!
    //! Declared here and defined in test_QtLikeSignal_Regression.cpp because the timing guards
    //! compare libraries and have to live somewhere that can measure both -- and no single file
    //! can: Qt defines `emit` as an empty macro, so a translation unit that includes Qt headers
    //! cannot also call `signal.emit( 1 )`. This header is the seam, and contains no `emit` for the
    //! same reason.
    namespace Measure
    {
        //! Nanoseconds per emit through an explicit direct connection.
        double qtLikeSignalDirectEmitNs();

        //! Nanoseconds per emit through an auto connection whose receiver shares this thread.
        double qtLikeSignalAutoEmitNs();

        //! Nanoseconds per connect().
        double qtLikeSignalConnectNs();

    }

    namespace Allocations
    {
        //! True when the counting operator new is linked in. See PerfAllocationCounter.cpp.
        bool available();

        //! Starts counting on the calling thread. Nested calls are not supported.
        void start();

        //! Stops counting and returns how many allocations happened since start().
        long stop();

    }
}

#include <chrono>

namespace PerfHarness
{
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
}

#endif // QT_LIKE_SIGNAL_PERF_HARNESS_HPP
