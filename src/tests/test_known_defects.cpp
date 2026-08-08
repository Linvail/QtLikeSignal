//! @file
//!
//! **These tests are EXPECTED TO FAIL.** They exist to prove that the open risks recorded in
//! history/OPEN-RISKS-20260808.md are real defects rather than theory, and they stay red until each
//! one is fixed. Do not "fix" a test here by weakening it -- fix the code, then the test turns green
//! and becomes an ordinary regression test.
//!
//! For a green baseline while these are outstanding:
//! @code
//!   QtLikeSignal-Tests --gtest_filter=-KnownDefect.*
//! @endcode
//!
//! Why these were not caught by a sanitizer: the build uses -fsanitize=thread, and address/thread
//! are mutually exclusive in tools/toolchain-linux.py, so LeakSanitizer never runs. More
//! fundamentally, R23's growth is not a leak -- the accumulated events stay *reachable* from a live
//! object, and LSan only reports unreachable blocks at exit. A process could balloon to gigabytes
//! and still get a clean LSan report.
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

#if defined( __linux__ )
    #include <fstream>
    #include <string>
#endif

using namespace QtLikeSignal;

namespace
{
    //! Resident set size in kB, or -1 where it is not implemented.
    //!
    //! Only Linux is implemented; the tests that need it skip elsewhere rather than pretending.
    long residentSetKb()
    {
        #if defined( __linux__ )
            std::ifstream status( "/proc/self/status" );
            std::string key;
            long value = 0;
            while( status >> key )
            {
                if( key == "VmRSS:" )
                {
                    status >> value;
                    return value;
                }
            }
            return -1;
        #else
            return -1;
        #endif
    }

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

    //! Object that records its own destruction, so deferred deletion can be observed.
    class DestructionRecorder : public Object
    {
    public:
        explicit DestructionRecorder
            (
            std::shared_ptr<std::atomic<bool> > aFlag
            )
            : mFlag( std::move( aFlag ) )
        {
        }

        virtual ~DestructionRecorder() override
        {
            mFlag->store( true );
        }

    private:
        std::shared_ptr<std::atomic<bool> > mFlag;
    };

    //! Creates an Object on a short-lived native thread and returns it after that thread has exited.
    //!
    //! The returned object is "orphaned": auto-adoption gave its creating thread a Thread, and that
    //! Thread was destroyed when the native thread exited, so the object's affinity now reports
    //! thread() == nullptr while still holding the dead thread's ThreadData -- and, crucially, the
    //! live dispatcher that ThreadData still owns.
    template <typename Factory>
    auto makeOrphanedObject
        (
        Factory aFactory
        ) -> decltype( aFactory() )
    {
        decltype( aFactory() ) created = nullptr;
        std::thread creator( [&created, &aFactory]()
            {
                created = aFactory();
            } );
        creator.join();
        return created;
    }
}

// ---------------------------------------------------------------------------------------------
// R23 -- an object whose thread has been destroyed still has a live dispatcher.
//
// Thread::threadBody() clears the dispatcher when a worker finishes, but ~Thread() does not, so an
// adopted thread's ThreadData keeps a live EventDispatcherDefault after the thread is gone. The
// object is then in a state the design does not anticipate: thread() == nullptr, yet work posted to
// it is accepted by a dispatcher that nothing will ever drain.
//
// QtMimic forecloses this. ~Thread() closes the mailbox *before* nulling the back-pointer, stating
// the invariant outright -- "Done BEFORE clearing the back-pointer, so the invariant 'thread() ==
// nullptr implies not accepting' holds" -- and connectImpl() additionally drops when
// ctxData->thread() == nullptr. Measured side by side over five rounds of 200k emits, QtMimic grows
// 276 kB then flattens; QtLikeSignal grows ~30 MB every round without bound.
// ---------------------------------------------------------------------------------------------

//! R23a: deleteLater() on an orphaned object queues a deletion that can never run, leaking it.
//!
//! Deterministic -- no timing, no sampling. QtMimic handles this by checking post()'s return value
//! and falling back to a synchronous delete ("Doing nothing here would leak self forever, which is
//! strictly worse than the thread-affinity violation of deleting it synchronously"). QtLikeSignal
//! finds a live dispatcher, queues a DeferredDeleteEvent into it, reports success, and the object is
//! never destroyed.
TEST( KnownDefect, R23a_DeleteLaterOnAnOrphanedObjectNeverRuns )
{
    auto destroyed = std::make_shared<std::atomic<bool> >( false );

    DestructionRecorder* orphan = makeOrphanedObject( [destroyed]()
        {
            return new DestructionRecorder( destroyed );
        } );

    ASSERT_NE( orphan, nullptr );
    ASSERT_EQ( orphan->thread(), nullptr )
        << "the creating thread's Thread should have been destroyed when that thread exited";

    orphan->deleteLater();

    const bool wasDestroyed = destroyed->load();
    EXPECT_TRUE( wasDestroyed )
        << "deleteLater() on an object whose thread is gone queued a DeferredDeleteEvent into a "
        "dispatcher nothing will ever drain, so the object is leaked outright. It should fall back "
        "to deleting synchronously, as QtMimic does when post() refuses the task.";

    if( !wasDestroyed )
    {
        // Reclaim it so this known-failing test does not also leak on every run.
        delete orphan;
    }
}

//! R23b: queued calls to an orphaned object accumulate without bound.
//!
//! Sampled over repeated rounds rather than once, which is what distinguishes a genuine leak from
//! allocator arena high-water. A single measurement cannot tell them apart: QtMimic's first round
//! also grows (276 kB) and then flattens to zero, while this grows by the same amount every round
//! forever.
TEST( KnownDefect, R23b_QueuedCallsToAnOrphanedObjectAccumulateForever )
{
    if( residentSetKb() < 0 )
    {
        GTEST_SKIP() << "resident-set sampling is implemented for Linux only";
    }

    Object* orphan = makeOrphanedObject( []()
        {
            return new Object();
        } );
    ASSERT_NE( orphan, nullptr );
    ASSERT_EQ( orphan->thread(), nullptr );

    Signal<> sig;
    Object::connect( sig, orphan, []()
        {
        }, ConnectionType::QueuedConnection );

    constexpr int kRounds = 4;
    constexpr int kEmitsPerRound = 200000;
    std::vector<long> growthPerRound;

    for( int round = 0; round < kRounds; ++round )
    {
        const long before = residentSetKb();
        for( int i = 0; i < kEmitsPerRound; ++i )
        {
            sig.emit();
        }
        growthPerRound.push_back( residentSetKb() - before );
    }

    // Ignore the first round: that is where the allocator's arena grows, and it does so even when
    // the events are correctly dropped. Steady-state growth is the signal.
    long steadyStateGrowth = 0;
    for( size_t i = 1; i < growthPerRound.size(); ++i )
    {
        steadyStateGrowth += growthPerRound[i];
    }

    delete orphan;

    EXPECT_LT( steadyStateGrowth, 4096 )
        << "after the first round, " << ( kRounds - 1 ) << " further rounds of " << kEmitsPerRound
        << " queued emits to an object whose thread is gone retained " << steadyStateGrowth
        << " kB. They are being queued into a dispatcher nothing will ever drain, instead of being "
        "dropped. QtMimic retains ~0 kB in the same steady state.";
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
