//! @file
//!
//! Dispatch-overhead benchmarks for Qt 6 itself, run alongside the QtLikeSignal ones so
//! all three appear in a single comparison table.
//!
//! Qt is the thing this project is imitating, so it is the reference the other two should be read
//! against. The scenarios, iteration counts and timing code are shared through PerfHarness.h, and
//! the slot bodies do the same trivial work, so the only difference between rows is the dispatch
//! machinery.
//!
//! Built only where Qt 6 is installed; see src/tests/wscript. This is its own translation unit
//! because Qt defines `emit` as an empty macro, which would turn every `sig.emit( 1 )` in the other
//! benchmarks into a syntax error if the headers met.

#include "PerfHarness.h"
#include "Qt6PerfObjects.h"

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
