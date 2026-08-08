//! @file
//!
//! **These tests are EXPECTED TO FAIL.** They exist to prove that the open risks recorded in
//! history/OPEN-RISKS-20260808.md are real defects rather than theory, and they stay red until each
//! one is fixed. Do not "fix" a test here by weakening it -- fix the code, then the test turns green
//! and *moves out of this file* into test_defect_regressions.cpp. That matters: the
//! `-KnownDefect.*` baseline below is only trustworthy while everything here is genuinely broken.
//!
//! R23 was fixed on 2026-08-08 and its two tests now live in test_defect_regressions.cpp.
//!
//! For a green baseline while these are outstanding:
//! @code
//!   QtLikeSignal-Tests --gtest_filter=-KnownDefect.*
//! @endcode
//!
//! Why R23 was not caught by a sanitizer, which is worth remembering for the rest: the build uses
//! -fsanitize=thread, and address/thread are mutually exclusive in tools/toolchain-linux.py, so
//! LeakSanitizer never ran. More fundamentally its growth was not a leak -- the accumulated events
//! stayed *reachable* from a live object, and LSan only reports unreachable blocks at exit. Verified:
//! under -fsanitize=address with detect_leaks=1 it retained 124 MB and reported nothing.
//!
//! Two of the recorded risks are deliberately **not** represented here, because a runtime test
//! would be dishonest rather than merely difficult:
//!
//!   * **R25** (`Object::thread()` costs a mutex per call) is a documented trade-off, not a defect.
//!     No requirement states how fast it must be, so a failing test would be inventing one, and any
//!     wall-clock threshold would be machine-dependent. If it ever matters, benchmark it and decide
//!     against a real budget.
//!   * **R27** (`Thread::create()` returns an owning raw pointer with no ownership documentation) is
//!     an API-shape and documentation issue. Nothing observable at runtime distinguishes it from a
//!     correct program: the caller either deletes the pointer or leaks it, and asserting the return
//!     type were `unique_ptr` would fail to *compile* rather than fail as a test, which would break
//!     the build for everyone instead of reporting one defect.

#include <gtest/gtest.h>
#include "CoreApplication.h"
#include "Object.h"
#include "Signal.h"
#include "Thread.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>


using namespace QtLikeSignal;

namespace
{
    //! How long R26's blocking task stalls the event loop, in milliseconds.
    //!
    //! 3.4 periods of R26's 50 ms timer, so it overshoots a deadline by 20 ms -- a phase error far
    //! larger than scheduling jitter, and deliberately not a whole multiple of the interval.
    constexpr int kR26BlockMs = 170;

    //! Blocks the calling event loop, to make it miss a timer deadline on purpose.
    void stallTheLoop()
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( kR26BlockMs ) );
    }

}

// ---------------------------------------------------------------------------------------------
// R24 -- timer ids are consumed monotonically and never returned to a pool.
//
// Object::sNextTimerId only ever increments. Qt explicitly releases ids
// (QAbstractEventDispatcherPrivate::releaseTimerId) so a program that starts and stops timers
// forever reuses a small set. Here the counter climbs until it wraps, at which point it eventually
// hands out -1 -- the value startTimer() returns to mean failure and Timer::stop() tests against.
// ---------------------------------------------------------------------------------------------

//! R24: starting and stopping one timer repeatedly consumes a fresh id every time.
//!
//! Tests the underlying defect -- non-recycling -- rather than the 2^31 wrap it eventually causes,
//! which is not reachable in a test.
TEST( KnownDefect, R24_TimerIdsAreNeverRecycled )
{
    Object owner;
    ASSERT_EQ( owner.thread(), Thread::currentThread() )
        << "startTimer() is thread-confined; this object must live here";

    constexpr int kCycles = 200;
    std::vector<int> ids;
    ids.reserve( kCycles );

    for( int i = 0; i < kCycles; ++i )
    {
        const int id = owner.startTimer( 1000 );
        ASSERT_GT( id, 0 ) << "the adopted thread should have a dispatcher to register with";
        owner.killTimer( id );
        ids.push_back( id );
    }

    const int idSpan = ids.back() - ids.front();
    EXPECT_LE( idSpan, 10 )
        << kCycles << " start/kill cycles on a single timer consumed " << ( idSpan + 1 )
        << " distinct ids (from " << ids.front() << " to " << ids.back()
        << "). Ids are never returned to a pool, so the counter climbs until it wraps and "
        "eventually collides with the -1 failure sentinel.";
}

// ---------------------------------------------------------------------------------------------
// R26 -- a repeating timer's schedule is permanently displaced by one late wakeup.
//
// EventDispatcherDefault re-arms with `t.mNextFire = now + interval`, where `now` is when the
// dispatcher got around to noticing the timer -- not the deadline that just passed. Any lateness is
// therefore folded into the schedule for good, rather than being caught up or skipped past. Qt
// computes the next deadline from the previous one, so a single late pass shifts one fire, not
// every fire thereafter.
// ---------------------------------------------------------------------------------------------

//! R26: after one deliberately-missed deadline, every later fire stays displaced by the overshoot.
//!
//! **Timing-sensitive by nature** -- it has to make the loop late on purpose. The blocking task
//! overshoots a deadline by a large, deliberately non-multiple amount (70 ms on a 50 ms period) so
//! the resulting phase error is far outside scheduling jitter.
TEST( KnownDefect, R26_TimerScheduleStaysDisplacedAfterALateWakeup )
{
    CoreApplication app;

    constexpr int kIntervalMs = 50;
    constexpr size_t kFiresWanted = 5;

    Object context;
    std::vector<double> fireOffsetsMs;

    Timer timer;
    const auto started = std::chrono::steady_clock::now();
    Object::connect( timer.timeout, &timer, [&]()
        {
            fireOffsetsMs.push_back(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started ).count() );
            if( fireOffsetsMs.size() >= kFiresWanted )
            {
                CoreApplication::quit();
            }
        }, ConnectionType::DirectConnection );

    // Stall the loop straight through the first deadline. callLater() rejects lambdas (it cannot
    // hash them for deduplication), so this goes through a plain function.
    Object::callLater( &context, &stallTheLoop );

    timer.start( kIntervalMs );
    ASSERT_EQ( app.exec(), 0 );
    timer.stop();

    ASSERT_GE( fireOffsetsMs.size(), kFiresWanted );

    // Look only at fires well after the stall, by which point the schedule should have recovered.
    double worstPhaseErrorMs = 0.0;
    for( size_t i = 2; i < fireOffsetsMs.size(); ++i )
    {
        const double periods = fireOffsetsMs[i] / kIntervalMs;
        const double phaseErrorMs
            = std::abs( ( periods - std::round( periods ) ) * kIntervalMs );
        worstPhaseErrorMs = std::max( worstPhaseErrorMs, phaseErrorMs );
    }

    EXPECT_LT( worstPhaseErrorMs, 12.0 )
        << "one late wakeup displaced the timer's schedule by " << worstPhaseErrorMs
        << " ms and it never recovered: fires stay off the original cadence because the next "
        "deadline is computed from when the dispatcher woke rather than from the deadline that "
        "elapsed.";
}
