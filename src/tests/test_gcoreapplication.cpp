#include <gtest/gtest.h>
#include "CoreApplication.h"
#include "Object.h"
#include "Signal.h"
#include "Thread.h"
#include "Timer.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <thread>

using namespace QtLikeSignal;

//! @file
//!
//! Tests for CoreApplication, which had no coverage at all before this file existed -- which is
//! how a 100% CPU spin in exec() (R18) survived unnoticed.
//!
//! Every test here constructs its own CoreApplication and lets it go out of scope before
//! returning. That matters more than usual: constructing one *adopts the calling thread*, setting
//! the process-wide Thread::sCurrentThread, and gtest runs every test on that same thread. A test
//! that leaked an application would silently give the main thread an affinity for the rest of the
//! binary and change how AutoConnection resolves in every test that follows.

//! Runs the application's event loop and returns once quit() has taken effect.
//!
//! Arms a single-shot Timer on the main thread before entering exec(), so the loop stops itself
//! from the inside. Deterministic, and it exercises the timer path through the main dispatcher
//! rather than relying on a sleeping helper thread.
static int execUntilQuit
    (
    CoreApplication& aApp,   //!< Application whose loop to run.
    int aDelayMs = 10        //!< How long to let the loop run before quitting.
    )
{
    Timer stopper;
    stopper.setSingleShot( true );
    Object::connect( stopper.timeout, &stopper, []()
        {
            CoreApplication::quit();
        }, ConnectionType::DirectConnection );
    stopper.start( aDelayMs );
    return aApp.exec();
}

//! A CoreApplication subclass, which is how the class is meant to be used.
class TestApplication : public CoreApplication
{
public:
    //! Marks the application as initialised.
    void init()
    {
        mInitialised = true;
    }

    //! Marks the application as torn down.
    void deInit()
    {
        mInitialised = false;
    }

    //! True between init() and deInit().
    bool isInitialised() const
    {
        return mInitialised;
    }

private:
    bool mInitialised { false };
};

//! Verifies the documented usage from the mission: derive, construct, init, exec, deInit.
TEST( CoreApplicationTest, DerivedApplicationRunsAndReturnsExitCode )
{
    TestApplication app;
    app.init();
    EXPECT_TRUE( app.isInitialised() );

    Timer stopper;
    stopper.setSingleShot( true );
    Object::connect( stopper.timeout, &stopper, []()
        {
            CoreApplication::exit( 42 );
        }, ConnectionType::DirectConnection );
    stopper.start( 10 );

    EXPECT_EQ( app.exec(), 42 );

    app.deInit();
    EXPECT_FALSE( app.isInitialised() );
}

//! Verifies instance() reports the application while it exists and nothing once it is gone.
TEST( CoreApplicationTest, InstanceTracksApplicationLifetime )
{
    EXPECT_EQ( CoreApplication::instance(), nullptr );
    {
        CoreApplication app;
        EXPECT_EQ( CoreApplication::instance(), &app );
    }
    EXPECT_EQ( CoreApplication::instance(), nullptr );
}

//! Verifies constructing the application adopts the calling thread, and destroying it releases it.
//!
//! Adoption is what gives main-thread Objects an affinity, and therefore what lets them receive
//! timers, posted events and deleteLater(). The release half matters just as much here: without
//! it every later test in this binary would inherit the affinity.
TEST( CoreApplicationTest, ConstructionAdoptsTheCallingThreadAndDestructionReleasesIt )
{
    EXPECT_EQ( Thread::currentThread(), nullptr );
    {
        CoreApplication app;
        ASSERT_NE( Thread::currentThread(), nullptr );

        // An object created now lives in the adopted main thread.
        Object owned;
        EXPECT_EQ( owned.thread(), Thread::currentThread() );
        EXPECT_EQ( app.thread(), Thread::currentThread() );
    }
    EXPECT_EQ( Thread::currentThread(), nullptr );
}

//! Verifies the command-line overload captures arguments and the default constructor reports none.
TEST( CoreApplicationTest, ArgumentsAreCapturedOnlyByTheArgcArgvConstructor )
{
    {
        CoreApplication app;
        EXPECT_TRUE( app.arguments().empty() );
    }

    char arg0[] = "app";
    char arg1[] = "--flag";
    char* argv[] = { arg0, arg1 };
    {
        CoreApplication app( 2, argv );
        ASSERT_EQ( app.arguments().size(), 2u );
        EXPECT_EQ( app.arguments()[0], "app" );
        EXPECT_EQ( app.arguments()[1], "--flag" );
    }
}

//! Regression test for R18: exec() after a quit() used to spin at 100% CPU.
//!
//! EventDispatcherDefault::mInterrupt was latched true by interrupt() and never cleared by
//! anything, so once quit() had interrupted the dispatcher every later processEvents() returned
//! instantly. A worker Thread hid this by building a fresh dispatcher on each start(); the main
//! thread reuses one, so its second exec() became a tight loop burning a core until something
//! else stopped it. Measured before the fix: cpu/wall = 100%.
//!
//! Asserted with std::clock(), which measures processor time and is standard C++ (no /proc, no
//! getrusage, no GetProcessTimes), so this test is portable. A correctly blocking loop consumes
//! almost no CPU over the interval; the threshold is deliberately loose -- the regression pins the
//! ratio at ~1.0, so anything below 0.5 distinguishes them with a wide margin.
//!
//! The second loop is stopped by a plain watchdog thread rather than by a Timer, and that choice
//! is load-bearing. With the defect present, processEvents() returns before dispatching anything,
//! so a Timer armed to call quit() would itself never fire and the test would hang forever instead
//! of failing -- confirmed by reverting the fix. A watchdog stops the loop either way, so a
//! regression reports a real assertion failure with numbers attached.
TEST( CoreApplicationTest, ReExecAfterQuitBlocksInsteadOfSpinning )
{
    CoreApplication app;

    // First cycle: run the loop briefly and quit. This is what latches the interrupt.
    EXPECT_EQ( execUntilQuit( app, 10 ), 0 );

    // Second cycle: the loop must block until the watchdog quits it, not spin.
    constexpr int kRunMs = 300;
    std::thread watchdog( [kRunMs]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( kRunMs ) );
            CoreApplication::quit();
        } );

    const std::clock_t cpuBefore = std::clock();
    const auto wallBefore = std::chrono::steady_clock::now();

    EXPECT_EQ( app.exec(), 0 );

    const double cpuSeconds
        = static_cast<double>( std::clock() - cpuBefore ) / CLOCKS_PER_SEC;
    const double wallSeconds
        = std::chrono::duration<double>( std::chrono::steady_clock::now() - wallBefore ).count();
    watchdog.join();

    ASSERT_GT( wallSeconds, 0.1 ) << "the loop returned far too early to have waited at all";
    EXPECT_LT( cpuSeconds / wallSeconds, 0.5 )
        << "exec() burned " << cpuSeconds << "s of CPU over " << wallSeconds
        << "s of wall time -- it is spinning rather than blocking, so the dispatcher's interrupt "
        "flag is latched again.";
}

//! Verifies the loop still dispatches work on a *second* exec(), not merely that it stops spinning.
//!
//! The CPU check above catches the symptom; this catches the more damaging half of the same defect.
//! A latched interrupt made processEvents() return before touching the queue, so after one
//! quit()/exec() cycle the application was silently inert -- timers never fired, queued slots never
//! ran -- while looking like a healthy running loop.
TEST( CoreApplicationTest, LoopStillDispatchesAfterAQuitExecCycle )
{
    CoreApplication app;

    EXPECT_EQ( execUntilQuit( app, 10 ), 0 );

    // A watchdog bounds the test: if dispatch is broken the timer below never fires, and without
    // this the loop would run until the harness killed the whole binary.
    std::atomic<bool> finished { false };
    std::thread watchdog( [&finished]()
        {
            for( int i = 0; i < 200 && !finished.load(); ++i )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
            }
            CoreApplication::quit();
        } );

    bool timerFired = false;
    Timer timer;
    timer.setSingleShot( true );
    Object::connect( timer.timeout, &timer, [&timerFired]()
        {
            timerFired = true;
            CoreApplication::quit();
        }, ConnectionType::DirectConnection );
    timer.start( 10 );

    EXPECT_EQ( app.exec(), 0 );
    finished.store( true );
    watchdog.join();

    EXPECT_TRUE( timerFired )
        << "after one quit()/exec() cycle the loop stopped dispatching entirely: the timer never "
        "fired, so processEvents() is returning before it reaches the queue.";
}

//! Verifies exec() is rejected from a thread other than the one the application adopted.
TEST( CoreApplicationTest, ExecFromAnotherThreadIsRejected )
{
    CoreApplication app;

    int result = 0;
    std::thread other( [&app, &result]()
        {
            result = app.exec();
        } );
    other.join();

    EXPECT_EQ( result, -1 ) << "exec() must refuse to run the main loop on a foreign thread.";
}

//! Verifies a nested exec() is rejected rather than starting a second loop on one thread.
TEST( CoreApplicationTest, NestedExecIsRejected )
{
    CoreApplication app;

    int nestedResult = 0;
    Timer stopper;
    stopper.setSingleShot( true );
    Object::connect( stopper.timeout, &stopper, [&app, &nestedResult]()
        {
            // Re-entering exec() from inside the running loop must be refused, not honoured.
            nestedResult = app.exec();
            CoreApplication::quit();
        }, ConnectionType::DirectConnection );
    stopper.start( 10 );

    EXPECT_EQ( app.exec(), 0 );
    EXPECT_EQ( nestedResult, -1 ) << "a nested exec() must be refused.";
}

//! Verifies queued work posted from a worker thread is delivered on the main thread's loop.
TEST( CoreApplicationTest, QueuedSignalFromWorkerIsDeliveredOnTheMainThread )
{
    CoreApplication app;

    Signal<int> sig;
    Object receiver;
    std::atomic<int> received { 0 };
    Thread* mainThread = Thread::currentThread();
    std::atomic<Thread*> ranOn { nullptr };

    Object::connect( sig, &receiver, [&received, &ranOn]( int aValue )
        {
            received.store( aValue );
            ranOn.store( Thread::currentThread() );
            CoreApplication::quit();
        }, ConnectionType::QueuedConnection );

    std::thread emitter( [&sig]()
        {
            sig.emit( 7 );
        } );

    EXPECT_EQ( app.exec(), 0 );
    emitter.join();

    EXPECT_EQ( received.load(), 7 );
    EXPECT_EQ( ranOn.load(), mainThread )
        << "a queued connection must run its slot on the receiver's own (main) thread.";
}

//! Verifies deleteLater() on the main thread is honoured by the running loop.
TEST( CoreApplicationTest, DeleteLaterIsProcessedByTheMainLoop )
{
    CoreApplication app;

    auto destroyed = std::make_shared<std::atomic<bool> >( false );
    Object* victim = new Object();
    victim->addCleanupCallback( [destroyed]()
        {
            destroyed->store( true );
        } );
    victim->deleteLater();

    EXPECT_EQ( execUntilQuit( app, 20 ), 0 );
    EXPECT_TRUE( destroyed->load() )
        << "deleteLater() on the main thread was never dispatched by exec().";
}

//! Verifies a deleteLater() still pending when the application shuts down is not leaked.
//!
//! Worker threads already drained deferred deletes on shutdown; the main thread had no equivalent,
//! so anything that called deleteLater() and never saw a loop run was leaked outright -- the
//! dispatcher's destructor frees the queued events but cannot free the objects they target.
//! Under AddressSanitizer a regression also shows up directly as a leak report.
TEST( CoreApplicationTest, PendingDeleteLaterIsProcessedWhenTheApplicationShutsDown )
{
    auto destroyed = std::make_shared<std::atomic<bool> >( false );
    {
        CoreApplication app;

        Object* victim = new Object();
        victim->addCleanupCallback( [destroyed]()
            {
                destroyed->store( true );
            } );
        victim->deleteLater();

        // Deliberately no exec() -- the deferred delete is still queued at destruction.
    }
    EXPECT_TRUE( destroyed->load() )
        << "a deleteLater() still pending at application shutdown was leaked instead of run.";
}

//! Verifies a Timer created on the main thread fires from the application's loop.
TEST( CoreApplicationTest, TimerFiresOnTheMainThreadLoop )
{
    CoreApplication app;

    int ticks = 0;
    Timer timer;
    Object::connect( timer.timeout, &timer, [&ticks]()
        {
            if( ++ticks >= 3 )
            {
                CoreApplication::quit();
            }
        }, ConnectionType::DirectConnection );
    timer.start( 5 );

    EXPECT_EQ( app.exec(), 0 );
    EXPECT_GE( ticks, 3 );
}

//! Verifies exit()/quit() are safe no-ops when no application exists.
TEST( CoreApplicationTest, StaticExitWithoutAnApplicationIsHarmless )
{
    ASSERT_EQ( CoreApplication::instance(), nullptr );
    CoreApplication::quit();
    CoreApplication::exit( 3 );
    SUCCEED();
}
