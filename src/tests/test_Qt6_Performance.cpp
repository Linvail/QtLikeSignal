// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Dispatch-overhead benchmarks for Qt 6 itself, run alongside the QtLikeSignal and QtMimic ones so
//! all three appear in a single comparison table.
//!
//! Qt is the thing this project is imitating, so it is the reference the other two should be read
//! against. The scenarios, iteration counts and timing code are shared through PerfHarness.hpp, and
//! the slot bodies do the same trivial work, so the only difference between rows is the dispatch
//! machinery.
//!
//! Built only where Qt 6 is installed; see src/tests/wscript. This is its own translation unit
//! because Qt defines `emit` as an empty macro, which would turn every `sig.emit( 1 )` in the other
//! benchmarks into a syntax error if the headers met.

#include "PerfHarness.hpp"
#include "Qt6PerfObjects.hpp"

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    //! Creates the QCoreApplication the queued benchmark needs, once, for the whole run.
    //!
    //! Qt can post events without an application instance, but an event loop with no `qApp` is not
    //! the configuration anyone actually ships, and several code paths check for it. Constructing
    //! one keeps the measurement representative of real Qt use.
    class Qt6Environment : public ::testing::Environment
    {
    public:
        virtual void SetUp() override
        {
            if( QCoreApplication::instance() == nullptr )
            {
                mApp = new QCoreApplication( mArgc, mArgv );
            }
        }

        virtual void TearDown() override
        {
            delete mApp;
            mApp = nullptr;
        }

    private:
        // Storage must outlive the QCoreApplication, which keeps the pointers it is handed.
        char mArg0[5] { 'p', 'e', 'r', 'f', '\0' };
        char* mArgv[2] { mArg0, nullptr };
        int mArgc { 1 };
        QCoreApplication* mApp { nullptr };
    };

    //! Registers the environment before main() runs, so this file needs nothing from main().
    //!
    //! Keeps the shared main() free of any Qt reference, which is what lets it compile unchanged in
    //! builds where Qt is not present.
    const bool gQt6EnvironmentRegistered = []()
        {
            ::testing::AddGlobalTestEnvironment( new Qt6Environment() );
            return true;
        }();
}

//! Measures establishing a connection.
TEST( Performance, Qt6_Connect )
{
    Qt6PerfSender sender;
    Qt6PerfReceiver receiver;

    const double ns = PerfHarness::timeLoop( PerfHarness::kConnectOps, [&]( int )
        {
            QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, []( int )
            {
            }, Qt::DirectConnection );
        } );
    PerfHarness::record( "connect()", "Qt6", ns );
}

//! Measures emit -> receive on one thread with an explicit direct connection.
TEST( Performance, Qt6_DirectEmit )
{
    Qt6PerfSender sender;
    Qt6PerfReceiver receiver;

    long long received = 0;
    QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, Qt::DirectConnection );

    sender.fire( 1 );   // warm up
    const double ns = PerfHarness::timeLoop( PerfHarness::kDirectOps, [&]( int )
        {
            sender.fire( 1 );
            PerfHarness::keep( received );
        } );
    PerfHarness::record( "emit->receive, direct", "Qt6", ns );
    EXPECT_GT( received, 0 );
}

//! Measures emit -> receive on one thread through Qt::AutoConnection.
//!
//! Same delivery as the direct row; the difference is what Qt spends deciding that sender and
//! receiver share a thread. Qt resolves this from a QThreadData pointer cached on the connection
//! itself, which is why the gap here is small.
TEST( Performance, Qt6_AutoEmitSameThread )
{
    Qt6PerfSender sender;
    Qt6PerfReceiver receiver;

    long long received = 0;
    QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, [&received]( int aValue )
        {
            received += aValue;
        }, Qt::AutoConnection );

    sender.fire( 1 );
    const double ns = PerfHarness::timeLoop( PerfHarness::kDirectOps, [&]( int )
        {
            sender.fire( 1 );
            PerfHarness::keep( received );
        } );
    PerfHarness::record( "emit->receive, auto same-thread", "Qt6", ns );
    EXPECT_GT( received, 0 );
}

//! Measures end-to-end cross-thread throughput: emit on this thread, receive on a worker's loop.
//!
//! Timed until the last message has actually been *received*, not merely posted, so this is the full
//! queue-and-dispatch round trip rather than the cost of enqueueing.
TEST( Performance, Qt6_QueuedEmitCrossThread )
{
    ASSERT_NE( QCoreApplication::instance(), nullptr )
        << "the queued path needs an application instance";

    QThread worker;
    Qt6PerfSender sender;
    Qt6PerfReceiver receiver;

    // Move before starting: moveToThread() is push-only in Qt, so the receiver has to be handed
    // over from the thread that currently owns it, which is this one.
    receiver.moveToThread( &worker );
    worker.start();

    // isRunning() only says the thread exists, not that its event loop is dispatching. Bounce a
    // queued call off the receiver and wait for it, which proves both.
    std::atomic<bool> loopReady { false };
    QMetaObject::invokeMethod( &receiver, [&loopReady]()
        {
            loopReady.store( true );
        }, Qt::QueuedConnection );
    while( !loopReady.load() )
    {
        std::this_thread::yield();
    }

    std::atomic<int> received { 0 };
    QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, [&received]( int )
        {
            received.fetch_add( 1, std::memory_order_relaxed );
        }, Qt::QueuedConnection );

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < PerfHarness::kQueuedOps; ++i )
    {
        sender.fire( 1 );
    }
    while( received.load( std::memory_order_relaxed ) < PerfHarness::kQueuedOps )
    {
        std::this_thread::yield();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    PerfHarness::record( "emit->receive, queued x-thread", "Qt6",
        std::chrono::duration<double, std::nano>( elapsed ).count() / PerfHarness::kQueuedOps );

    EXPECT_EQ( received.load(), PerfHarness::kQueuedOps );

    // Stop the worker before the receiver goes out of scope. It cannot be moved back -- only its
    // owning thread may hand it on -- so the safe order is to end that thread first and destroy the
    // receiver afterwards, with no loop left that could be dispatching to it.
    worker.quit();
    worker.wait();
}

// -------------------------------------------------------------------------------------------
// Timing guards
//
// Expressed as a ratio against Qt 6 measured in the same process, on the same machine, in the same
// run. That is what makes a timing guard usable at all: an absolute nanosecond threshold has to be
// recalibrated for every machine and fails on a busy one, while a ratio moves only when our cost
// moves relative to a reference that is not changing. Qt 6 is a good reference precisely because we
// do not maintain it.
//
// Each guard measures both libraries itself rather than reading the table the benchmarks above
// build, so it does not depend on test order and survives --gtest_shuffle.
//
// The bars are set at roughly twice the current ratio. Wide enough that a loaded machine never
// fails one, tight enough that the regressions this project has actually had -- a per-emit
// allocation, an extra shared_ptr copy on the auto path -- would trip them.
// -------------------------------------------------------------------------------------------

namespace
{
    //! Nanoseconds per direct emit, Qt 6.
    double qt6DirectEmitNs()
    {
        Qt6PerfSender sender;
        Qt6PerfReceiver receiver;
        long long received = 0;
        QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, [&received]( int aValue )
            {
                received += aValue;
            }, Qt::DirectConnection );
        sender.fire( 1 );
        const double ns = PerfHarness::timeLoop( PerfHarness::kDirectOps, [&]( int )
            {
                sender.fire( 1 );
                PerfHarness::keep( received );
            } );
        return ns;
    }

    //! Nanoseconds per same-thread auto emit, Qt 6.
    double qt6AutoEmitNs()
    {
        Qt6PerfSender sender;
        Qt6PerfReceiver receiver;
        long long received = 0;
        QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, [&received]( int aValue )
            {
                received += aValue;
            }, Qt::AutoConnection );
        sender.fire( 1 );
        const double ns = PerfHarness::timeLoop( PerfHarness::kDirectOps, [&]( int )
            {
                sender.fire( 1 );
                PerfHarness::keep( received );
            } );
        return ns;
    }

    //! Nanoseconds per connect(), Qt 6.
    double qt6ConnectNs()
    {
        Qt6PerfSender sender;
        Qt6PerfReceiver receiver;
        return PerfHarness::timeLoop( PerfHarness::kConnectOps, [&]( int )
            {
                QObject::connect( &sender, &Qt6PerfSender::fired, &receiver, []( int )
                {
                }, Qt::DirectConnection );
            } );
    }
}

//! True when this build is optimised, which the timing guards below require.
//!
//! In a debug build our code is `-O0` while Qt 6 is a prebuilt optimised library, so every ratio
//! against it is meaningless and would fail. The shape and allocation guards next door have no such
//! problem -- a ratio between two sizes, or a count, is the same at any optimisation level -- which
//! is another reason to prefer them.
constexpr bool kOptimisedBuild =
#ifdef NDEBUG
        true;
#else
        false;
#endif

    //! Fails if our direct emit falls a long way behind Qt 6's.
    //!
    //! We are currently **faster** than Qt here -- about 24 ns against 29 -- having been 2.3x slower
    //! before the in-house Signal replaced boost. The bar is 2x Qt, which is where we were when that
    //! was considered a problem worth a document entry.
    TEST( PerformanceRegression, DirectEmitKeepsUpWithQt6 )
{
    if( !kOptimisedBuild )
    {
        GTEST_SKIP() << "timing guards need an optimised build; Qt 6 is always optimised";
    }

    const double qt = PerfHarness::bestOf( 3, qt6DirectEmitNs );
    const double ours = PerfHarness::bestOf( 3, PerfHarness::Measure::qtLikeSignalDirectEmitNs );

    ASSERT_GT( qt, 0.0 );
    const double ratio = ours / qt;
    EXPECT_LT( ratio, 2.0 )
        << "a direct emit costs " << ratio << "x Qt 6's (" << ours << " ns against " << qt
        << " ns). It was faster than Qt when this guard was written. Anything approaching 2x means "
        "the emit path is allocating or locking again -- check the allocation guards first, they "
        "say which.";
}

//! Fails if our same-thread auto emit falls a long way behind Qt 6's.
//!
//! The widest gap we still have, and the one with a known cause: resolving the receiver's affinity
//! takes a mutex where Qt takes an atomic load, which is P2 and is accepted. About 47 ns against
//! Qt's 28, so 1.7x; the bar is 3.5x.
TEST( PerformanceRegression, SameThreadAutoEmitKeepsUpWithQt6 )
{
    if( !kOptimisedBuild )
    {
        GTEST_SKIP() << "timing guards need an optimised build; Qt 6 is always optimised";
    }

    const double qt = PerfHarness::bestOf( 3, qt6AutoEmitNs );
    const double ours = PerfHarness::bestOf( 3, PerfHarness::Measure::qtLikeSignalAutoEmitNs );

    ASSERT_GT( qt, 0.0 );
    const double ratio = ours / qt;
    EXPECT_LT( ratio, 3.5 )
        << "a same-thread auto emit costs " << ratio << "x Qt 6's (" << ours << " ns against " << qt
        << " ns), against 1.7x when this guard was written. The known part of that gap is the "
        "affinity mutex (P2); a jump beyond it means something new was added to the path every "
        "emit takes.";
}

//! Fails if establishing a connection falls a long way behind Qt 6's.
//!
//! The loosest bar in the file, because this row is the noisiest: Qt 6's own measurement moves
//! between 76 and 142 ns depending on what ran before it. About 2.9x today, driven entirely by
//! allocation count (P10); the bar is 6x, and the allocation guard next door is the sharper
//! instrument for this path.
TEST( PerformanceRegression, ConnectKeepsUpWithQt6 )
{
    if( !kOptimisedBuild )
    {
        GTEST_SKIP() << "timing guards need an optimised build; Qt 6 is always optimised";
    }

    const double qt = PerfHarness::bestOf( 3, qt6ConnectNs );
    const double ours = PerfHarness::bestOf( 3, PerfHarness::Measure::qtLikeSignalConnectNs );

    ASSERT_GT( qt, 0.0 );
    const double ratio = ours / qt;
    EXPECT_LT( ratio, 6.0 )
        << "connect() costs " << ratio << "x Qt 6's (" << ours << " ns against " << qt
        << " ns), against 2.9x when this guard was written. This row is noisy, so a failure here "
        "means a large change; OneConnectionCostsAtMostThreeHeapBlocks will say what allocated.";
}
