//! @file
//!
//! **These tests are EXPECTED TO FAIL.** They exist to prove that the open risks recorded in
//! history/OPEN-RISKS-20260808.md are real defects rather than theory, and they stay red until each
//! one is fixed. Do not "fix" a test here by weakening it -- fix the code, then the test turns green
//! and *moves out of this file* into test_defect_regressions.cpp. That matters: the
//! `-KnownDefect.*` baseline below is only trustworthy while everything here is genuinely broken.
//!
//! R23 and R26 were fixed on 2026-08-08 and their tests now live in test_defect_regressions.cpp.
//! R26's original test here asserted the wrong thing and was replaced rather than moved -- see the
//! note in the risk register.
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
