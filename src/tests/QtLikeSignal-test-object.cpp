// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for QtLikeSignal::Object -- connections, thread affinity, lifetime and the
//! callLater() family.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <future>
#include <thread>

using namespace QtLikeSignal;
using namespace std::chrono_literals;

//! Helper test receiver class for verifying Object slot invocations.
class ObjectTestReceiver : public Object
{
public:
    //! Slot for integer parameter signals.
    void onValueReceived
        (
        int aVal  //!< Received value.
        )
    {
        mLastValue = aVal;
        mCallCount++;
        mExecutedThread = Thread::currentThread();
    }

    //! Used to test overloaded slots.
    void onValueReceived
        (
        int aVal1,  //!< First value.
        int aVal2   //!< Second value.
        )
    {
        mLastValue = aVal1 + aVal2;
        mCallCount++;
        mExecutedThread = Thread::currentThread();
    }

    //! Slot with non-void return type. Returns value plus 1.
    int onValueNonVoidReturn
        (
        int aVal  //!< Value to return.
        )
    {
        mLastValue = aVal;
        mCallCount++;
        mExecutedThread = Thread::currentThread();
        return mLastValue;
    }

    //! Slot taking const reference.
    void onStringConstReference
        (
        const std::string& aStr  //!< String value.
        )
    {
        mLastString = aStr;
        mCallCount++;
        mExecutedThread = Thread::currentThread();
    }

    //! Slot taking multiple arguments.
    void onMultiArg
        (
        int aA,          //!< Int argument.
        std::string aB,  //!< String argument.
        double aC        //!< Double argument.
        )
    {
        mLastInt    = aA;
        mLastString = aB;
        mLastDouble = aC;
        mCallCount++;
        mExecutedThread = Thread::currentThread();
    }

    //! Gets the total call count.
    int callCount() const
    {
        return mCallCount;
    }

    //! Gets the last received value.
    int lastValue() const
    {
        return mLastValue;
    }

    //! Gets the last received int.
    int lastInt() const
    {
        return mLastInt;
    }

    //! Gets the last received string.
    const std::string& lastString() const
    {
        return mLastString;
    }

    //! Gets the last received double.
    double lastDouble() const
    {
        return mLastDouble;
    }

    //! Gets the thread on which the slot was executed. Returns a pointer to the thread.
    Thread* executedThread() const
    {
        return mExecutedThread;
    }

private:
    int mCallCount { 0 };
    int mLastValue { 0 };
    int mLastInt { 0 };
    std::string mLastString;
    double mLastDouble { 0.0 };
    Thread*    mExecutedThread { nullptr };
};

//! Tests direct signal-slot connection and emission.
//!
//! Verifies that Object::connect() correctly binds a Signal to a Object member function slot
//! using ConnectionType::Direct, and that Signal::emit() properly invokes the receiver slot.
TEST( ObjectTest, DirectSignalSlotConnection )
{
    Signal<int>        sig;
    ObjectTestReceiver receiver;

    Object::connect( sig, &receiver, &ObjectTestReceiver::onValueReceived, ConnectionType::
        Direct );

    sig.emit( 42 );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 42 );
}

//! Tests signal disconnection via connection handle.
//!
//! Verifies Object::disconnect() successfully disconnects a previously connected signal handle,
//! preventing subsequent signal emissions from invoking the slot.
TEST( ObjectTest, SignalDisconnection )
{
    Signal<int>        sig;
    ObjectTestReceiver receiver;

    auto handle = Object::connect( sig, &receiver, &ObjectTestReceiver::onValueReceived );
    sig.emit( 10 );
    EXPECT_EQ( receiver.callCount(), 1 );

    Object::disconnect( handle );
    sig.emit( 20 );
    EXPECT_EQ( receiver.callCount(), 1 );
}

//! Tests object naming and thread affinity functions.
//!
//! Verifies Object::setObjectName(), Object::objectName(), Object::thread(), and
//! Object::moveToThread().
TEST( ObjectTest, ObjectNameAndThreadAffinity )
{
    Object obj;
    EXPECT_EQ( obj.objectName(), "" );

    obj.setObjectName( "TestObject" );
    EXPECT_EQ( obj.objectName(), "TestObject" );

    EXPECT_EQ( obj.thread(), Thread::currentThread() );

    Thread dummyThread;
    obj.moveToThread( &dummyThread );
    EXPECT_EQ( obj.thread(), &dummyThread );
}

//! Tests weak life token tracking for object destruction.
//!
//! Verifies Object::objectLife() returns a token that reports the object alive during its
//! lifetime and expired once it has been destroyed, including after the Object itself is gone.
TEST( ObjectTest, ObjectLifeToken )
{
    ObjectLife lifeToken;
    EXPECT_TRUE( lifeToken.expired() ) << "a token naming no object reports expired";
    {
        Object obj;
        lifeToken = obj.objectLife();
        EXPECT_FALSE( lifeToken.expired() );
    }
    EXPECT_TRUE( lifeToken.expired() );
}

//! Tests connecting a signal to a functor/lambda with context object.
//!
//! Verifies Object::connect() overload for functor and lambda slots associated with a context
//! object, testing Signal::emit() and Signal::operator().
TEST( ObjectTest, LambdaSlotConnection )
{
    Signal<int> sig;
    Object context;
    int receivedValue = 0;
    int callCount     = 0;

    Object::connect(
        sig,
        &context,
        [&receivedValue, &callCount]( int aVal )
        {
            receivedValue = aVal;
            callCount++;
        },
        ConnectionType::Direct );

    sig( 100 );
    EXPECT_EQ( callCount, 1 );
    EXPECT_EQ( receivedValue, 100 );

    sig.emit( 200 );
    EXPECT_EQ( callCount, 2 );
    EXPECT_EQ( receivedValue, 200 );
}

//! Tests multi-argument signal template instantiation and slot invocation.
//!
//! Verifies Signal<int, std::string, double> correctly passes diverse primitive and object
//! arguments to a receiver slot.
TEST( ObjectTest, MultiArgumentSignal )
{
    Signal<int, std::string, double> sig;
    ObjectTestReceiver receiver;

    Object::connect( sig, &receiver, &ObjectTestReceiver::onMultiArg, ConnectionType::
        Direct );

    sig.emit( 7, "Hello Signals", 3.14159 );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastInt(), 7 );
    EXPECT_EQ( receiver.lastString(), "Hello Signals" );
    EXPECT_DOUBLE_EQ( receiver.lastDouble(), 3.14159 );
}

//! Tests null receiver and context safety when connecting signals.
//!
//! Both libraries refuse the connection outright and hand back a dead handle, as Qt does: a
//! connection with no context has no life token, no affinity and no cleanup token, so it would
//! fire forever, on whichever thread emitted, with nothing able to stop it.
TEST( ObjectTest, NullReceiverOrContextConnection )
{
    Signal<int>         sig;
    ObjectTestReceiver* nullReceiver = nullptr;

    auto handle1 = Object::connect( sig, nullReceiver, &ObjectTestReceiver::onValueReceived );
    EXPECT_FALSE( handle1.connected() );

    Object* nullContext = nullptr;
    auto handle2     = Object::connect( sig, nullContext, []( int )
        {
        } );
    EXPECT_FALSE( handle2.connected() );

    // And emitting with those handles outstanding must be safe.
    sig.emit( 7 );
}

//! Tests low-level timer registration and cleanup.
//!
//! Verifies Object::startTimer() returns valid timer IDs and Object::killTimer() stops active
//! timers.
TEST( ObjectTest, TimerStartAndKill )
{
    Thread* thread = Thread::create(
        []()
        {
            Object obj;
            int timerId1 = obj.startTimer( 100 );
            int timerId2 = obj.startTimer( 200 );

            EXPECT_GT( timerId1, 0 );
            EXPECT_GT( timerId2, 0 );
            EXPECT_NE( timerId1, timerId2 );

            obj.killTimer( timerId1 );
            obj.killTimer( timerId2 );
        } );
    thread->start();
    thread->wait();
    delete thread;
}

//! Counts timer expiries, so a test can tell a refused killTimer() from an honoured one.
class TimerTickCounter : public Object
{
public:
    explicit TimerTickCounter
        (
        Thread* aThread = nullptr
        )
        : Object()
    {
        // Built here and pushed, rather than constructed on that
        // thread's affinity: Object no longer takes a Thread*.
        if( aThread != nullptr )
        {
            ( void )moveToThread( aThread );
        }
    }

    //! @return how many expiries have been delivered.
    int ticks() const
    {
        return mTicks.load();
    }

protected:
    void timerEvent
        (
        TimerEvent* aEvent
        ) override
    {
        ( void )aEvent;
        mTicks.fetch_add( 1 );
    }

private:
    // Atomic because the test thread polls it while the timer's own thread writes it.
    std::atomic<int> mTicks { 0 };
};

//! Tests the ways startTimer() and killTimer() refuse a request.
//!
//! Each refusal writes a diagnostic to stderr and otherwise does nothing. None of them had been
//! exercised, so the guards were only ever as good as the code reading them -- and a guard that
//! silently stopped guarding would have looked exactly the same from outside.
TEST( ObjectTest, TimerRequestsThatAreRefused )
{
    Thread worker( "timer-refusals" );
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    TimerTickCounter owned( &worker );

    // A negative interval is refused outright rather than corrected. Timer::start() clamps it to
    // 1 ms instead; startTimer() is the low-level entry point and simply says no.
    int negativeResult = 0;
    runOnThread( worker, [&owned, &negativeResult]()
        {
            negativeResult = owned.startTimer( -1 );
        } );
    EXPECT_EQ( negativeResult, -1 ) << "startTimer() accepted a negative interval.";

    // Timers are thread-confined, as in Qt: only the object's own thread may start one, because
    // only that thread's loop can ever deliver the resulting timerEvent().
    EXPECT_EQ( owned.startTimer( 5 ), -1 )
        << "startTimer() was honoured from a thread the object does not live in.";

    int liveId = -1;
    runOnThread( worker, [&owned, &liveId]()
        {
            liveId = owned.startTimer( 5 );
        } );
    ASSERT_GT( liveId, 0 );

    // Stopping one from the wrong thread is refused for the same reason, and -- the part worth
    // asserting -- leaves the timer running rather than half-killing it.
    owned.killTimer( liveId );
    EXPECT_TRUE( waitFor( [&owned]()
        {
            return owned.ticks() >= 2;
        } ) ) << "killTimer() from a foreign thread stopped the timer instead of refusing.";

    runOnThread( worker, [&owned, &liveId]()
        {
            // An id this object never owned is not its to forget. Handing it back to the shared
            // id pool would let a later startTimer() reissue an id another object is still using,
            // which is the recycling hazard the pool's FIFO reuse exists to keep narrow.
            owned.killTimer( liveId + 100000 );
            owned.killTimer( liveId );
        } );

    const int settled = owned.ticks();
    std::this_thread::sleep_for( 40ms );
    EXPECT_EQ( owned.ticks(), settled )
        << "the timer kept firing after being killed from its own thread.";

    drainQueuedTasks( worker );
    worker.quit();
    worker.wait();
}

//! Tests connect() with member function slot when receiver lives in another thread.
//!
//! Verifies that Object::connect() with ConnectionType::Auto or ConnectionType::Queued routes
//! signal emissions across thread boundaries into the receiver's thread event loop for execution.
TEST( ObjectTest, CrossThreadMemberFunctionConnection )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    Signal<int>        sig;
    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    Object::connect( sig, &receiver, &ObjectTestReceiver::onValueReceived, ConnectionType::
        Auto );
    Object::connect(
        sig, &receiver, [&workerThread]( int )
        {
            workerThread.quit();
        }, ConnectionType::Auto );

    sig.emit( 100 );

    workerThread.wait();

    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 100 );
    EXPECT_EQ( receiver.executedThread(), &workerThread );
}

//! Tests connect() with a lambda slot and context living in another thread.
//!
//! Verifies that Object::connect() with a context object living in a worker thread executes
//! the lambda slot inside the worker thread context.
TEST( ObjectTest, CrossThreadLambdaConnection )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    Signal<std::string> sig;
    Object context;
    context.moveToThread( &workerThread );

    std::string receivedMsg;
    Thread*    executedInThread = nullptr;

    Object::connect(
        sig,
        &context,
        [&receivedMsg, &executedInThread, &workerThread]( std::string aMsg )
        {
            receivedMsg      = aMsg;
            executedInThread = Thread::currentThread();
            workerThread.quit();
        },
        ConnectionType::Auto );

    sig.emit( "hello cross thread" );

    workerThread.wait();

    EXPECT_EQ( receivedMsg, "hello cross thread" );
    EXPECT_EQ( executedInThread, &workerThread );
}

//! Tests signal emission when receiver is destroyed before event processing.
//!
//! Verifies that when a receiver object connected via ConnectionType::Queued is destroyed prior to
//! the event dispatcher processing the pending MetaCallEvent, the event is safely removed/ignored
//! and no slot function is invoked or use-after-free error occurs.
TEST( ObjectTest, ReceiverDestroyedBeforeQueuedEventHandled )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    Object dummyContext;
    dummyContext.moveToThread( &workerThread );

    Signal<> blockSig;
    Object::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        ConnectionType::Queued );

    blockSig.emit();
    blockEnteredFuture.get();

    Signal<int> sig;

    {
        auto receiver = std::make_unique<ObjectTestReceiver>();
        receiver->moveToThread( &workerThread );

        Object::connect(
            sig, receiver.get(), &ObjectTestReceiver::onValueReceived, ConnectionType::
            Queued );

        sig.emit( 55 );
        // Receiver is deleted here before workerThread event loop processes the queued event
    }

    blockReleasePromise.set_value();

    // Make another event to make sure the `emit(55)` is processed before the thread terminates.
    Signal<> quitSig;
    Object::connect(
        quitSig, &dummyContext, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_TRUE( workerThread.isFinished() );
}

//! Tests lambda slot execution when context object is destroyed before event handling.
//!
//! Verifies that when a context Object associated with a lambda connection is destroyed before
//! the queued metacall is processed, the lambda slot is not executed.
TEST( ObjectTest, ContextDestroyedBeforeQueuedLambdaHandled )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    Object dummyContext;
    dummyContext.moveToThread( &workerThread );

    Signal<> blockSig;
    Object::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        ConnectionType::Queued );

    blockSig.emit();
    blockEnteredFuture.get();

    Signal<int> sig;
    bool lambdaExecuted = false;

    {
        Object context;
        context.moveToThread( &workerThread );

        Object::connect(
            sig, &context, [&lambdaExecuted]( int )
            {
                lambdaExecuted = true;
            }, ConnectionType::Queued );

        sig.emit( 77 );
        // context goes out of scope and is destroyed here
    }

    blockReleasePromise.set_value();

    // Make another event to make sure the `emit(77)` is processed before the thread terminates.
    Signal<> quitSig;
    Object::connect(
        quitSig, &dummyContext, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_FALSE( lambdaExecuted );
}

//! Tests connect() with explicit ConnectionType::Direct when receiver lives in another thread.
//!
//! Verifies that when ConnectionType::Direct is explicitly requested, the slot function is invoked
//! synchronously on the emitting thread, even if the receiver belongs to a different thread.
TEST( ObjectTest, CrossThreadDirectConnection )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    Signal<int>        sig;
    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    Object::connect( sig, &receiver, &ObjectTestReceiver::onValueReceived, ConnectionType::
        Direct );

    sig.emit( 888 );

    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 888 );
    EXPECT_EQ( receiver.executedThread(), Thread::currentThread() );
    EXPECT_NE( receiver.executedThread(), &workerThread );

    workerThread.quit();
    workerThread.wait();
}

static int g_testFreeFuncCount   = 0;
static int g_testFreeFuncLastVal = 0;

//! Free function used for testing Object::callLater free function overload.
static void testCallLaterFreeFunc
    (
    int aVal  //!< Received test value.
    )
{
    g_testFreeFuncLastVal = aVal;
    g_testFreeFuncCount++;
}

//! Tests Object::callLater with a member function slot.
TEST( ObjectTest, CallLaterMemberFunction )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    Object::callLater( &receiver, &ObjectTestReceiver::onValueReceived, 42 );

    Signal<> quitSig;
    Object::connect(
        quitSig, &receiver, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 42 );
    EXPECT_EQ( receiver.executedThread(), &workerThread );
}

//! Tests Object::callLater deduplication and parameter overwriting.
//!
//! Verifies that invoking callLater multiple times in the same cycle collapses to a single
//! execution using the arguments of the last call.
TEST( ObjectTest, CallLaterDeduplicationAndLastArgs )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    // Pause workerThread's event processing so all callLater invocations land in the same event
    // loop cycle
    std::mutex blockMutex;
    std::condition_variable blockCv;
    bool canProceed = false;

    Signal<> blockSig;
    Object::connect(
        blockSig,
        &receiver,
        [&blockMutex, &blockCv, &canProceed]()
        {
            std::unique_lock<std::mutex> lock( blockMutex );
            blockCv.wait( lock, [&canProceed]()
            {
                return canProceed;
            } );
        },
        ConnectionType::Queued );

    blockSig.emit();

    // Ensure workerThread has entered the block slot before issuing callLater
    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

    Object::callLater( &receiver, &ObjectTestReceiver::onValueReceived, 10 );
    Object::callLater( &receiver, &ObjectTestReceiver::onValueReceived, 20 );
    Object::callLater( &receiver, &ObjectTestReceiver::onValueReceived, 30 );

    // Release workerThread to process pending event loop queue
    {
        std::lock_guard<std::mutex> lock( blockMutex );
        canProceed = true;
    }
    blockCv.notify_all();

    Signal<> quitSig;
    Object::connect(
        quitSig, &receiver, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 30 );
}


//! Tests Object::callLater with a free function.
TEST( ObjectTest, CallLaterFreeFunction )
{
    g_testFreeFuncCount   = 0;
    g_testFreeFuncLastVal = 0;

    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    Object context;
    context.moveToThread( &workerThread );

    Object::callLater( &context, &testCallLaterFreeFunc, 99 );

    Signal<> quitSig;
    Object::connect(
        quitSig, &context, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ( g_testFreeFuncCount, 1 );
    EXPECT_EQ( g_testFreeFuncLastVal, 99 );
}

//! Tests Object::callLater with a Signal instance.
TEST( ObjectTest, CallLaterSignal )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    Signal<int> sig;
    Object::connect( sig, &receiver, &ObjectTestReceiver::onValueReceived, ConnectionType::
        Direct );

    Object::callLater( &receiver, sig, 777 );

    Signal<> quitSig;
    Object::connect(
        quitSig, &receiver, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 777 );
}

//! Tests Object::callLater execution across multiple event loop cycles.
TEST( ObjectTest, CallLaterMultipleCycles )
{
    Thread workerThread;
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    // Cycle 1
    Object::callLater( &receiver, &ObjectTestReceiver::onValueReceived, 100 );

    Signal<> syncSig1;
    // Atomic: written on the worker thread and spun on here. A plain bool is a data race, which
    // ThreadSanitizer flags (and which a compiler is free to hoist out of the loop below).
    std::atomic<bool> sync1Done { false };
    Object::connect(
        syncSig1, &receiver, [&sync1Done]()
        {
            sync1Done = true;
        }, ConnectionType::Queued );
    syncSig1.emit();

    while( !sync1Done )
    {
        std::this_thread::yield();
    }

    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 100 );

    // Cycle 2
    Object::callLater( &receiver, &ObjectTestReceiver::onValueReceived, 200 );

    Signal<> quitSig;
    Object::connect(
        quitSig, &receiver, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ( receiver.callCount(), 2 );
    EXPECT_EQ( receiver.lastValue(), 200 );
}

//! Tests that every callLater() overload tolerates a null receiver or context.
//!
//! Each overload opens with the same guard and each guard was unreached: connect() had a null
//! test (NullReceiverOrContextConnection above) and callLater() did not, even though it is the
//! easier of the two to reach with a null -- the pointer is usually a `this` from a caller that
//! has just checked something else.
TEST( ObjectTest, CallLaterWithNullReceiverOrContextIsANoOp )
{
    g_testFreeFuncCount   = 0;
    g_testFreeFuncLastVal = 0;

    ObjectTestReceiver* nullReceiver = nullptr;
    Object*             nullContext  = nullptr;

    // One per overload, since each carries its own copy of the guard.
    Object::callLater( nullReceiver, &ObjectTestReceiver::onStringConstReference,
        std::string( "ignored" ) );                                            // unambiguous slot
    Object::callLater( nullReceiver, &ObjectTestReceiver::onValueNonVoidReturn, 1 ); // non-void
    Object::callLater( nullReceiver, &ObjectTestReceiver::onValueReceived, 2 );      // overloaded
    Object::callLater( nullContext, &testCallLaterFreeFunc, 3 );                     // free func

    ConsumerDerived* nullDerived = nullptr;
    Object::callLater( nullDerived, &Consumer::onProducedReturnInt, 5 );   // inherited non-void

    Signal<int> sig;
    Object::callLater( nullContext, sig, 4 );                                        // signal

    // Nothing was scheduled, so nothing can arrive later either.
    EXPECT_EQ( g_testFreeFuncCount, 0 );
    EXPECT_EQ( g_testFreeFuncLastVal, 0 );
}

//! Tests the callLater() overloads that resolve by return type and by base class.
//!
//! The suite reached the plain and the overloaded-slot forms and nothing else. A slot with a
//! non-void return selects a different overload, and one inherited from a base class selects
//! another again -- each with its own copy of the dispatch, and neither had ever been reached by
//! a call that went through. The eleven overloads exist so that the awkward cases compile; a test
//! per shape is what says they also run.
TEST( ObjectTest, CallLaterResolvesNonVoidAndInheritedSlots )
{
    Thread workerThread( "calllater-overloads" );
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    ObjectTestReceiver receiver;
    receiver.moveToThread( &workerThread );

    ConsumerDerived derived;
    derived.moveToThread( &workerThread );

    // An unambiguous void slot: the plainest overload, resolved by deducing the member pointer.
    Object::callLater( &receiver, &ObjectTestReceiver::onStringConstReference,
        std::string( "deferred" ) );

    // A non-void slot. The return value is discarded, which is the whole reason for the separate
    // overload: a value-returning slot does not match the void-returning ones.
    Object::callLater( &receiver, &ObjectTestReceiver::onValueNonVoidReturn, 5 );

    // A non-void slot inherited from a base class, so the receiver's type and the slot's class
    // differ -- which selects yet another overload.
    Object::callLater( &derived, &Consumer::onProducedReturnInt, 11 );

    Signal<> quitSig;
    Object::connect(
        quitSig, &receiver, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::Queued );
    quitSig.emit();

    workerThread.wait();

    // Queued behind the three calls above, so reaching the quit means all three had their turn.
    EXPECT_EQ( receiver.callCount(), 2 );
    EXPECT_EQ( receiver.lastString(), "deferred" );
    EXPECT_EQ( receiver.lastValue(), 5 );
    EXPECT_EQ( derived.mCount.load(), 1 );
    EXPECT_EQ( derived.mLast.load(), 11 );
}

//! Tests that destroying a context cancels the callLater() still pending for it.
//!
//! Two things have to happen in ~Object() for this to hold: the queued metacall is stripped from
//! the dispatcher, and the entry is dropped from the process-wide callLater registry. The second
//! is the one this test is really about -- leaving it behind would keep a dead pointer in a table
//! every later callLater() consults, and the registry is guarded by a set-once flag
//! (mUsedCallLater) precisely so most objects never pay for the scan. Nothing had ever destroyed
//! an object with that flag set and a call outstanding.
TEST( ObjectTest, CallLaterIsCancelledWhenItsContextIsDestroyed )
{
    g_testFreeFuncCount   = 0;
    g_testFreeFuncLastVal = 0;

    Thread workerThread( "calllater-cancel" );
    workerThread.start();
    ASSERT_TRUE( waitUntilRunning( workerThread ) );

    // Hold the worker inside a posted task so the queued metacall cannot be dispatched while the
    // test arranges the destruction it means to test. Without the block this is a race that
    // usually resolves the uninteresting way.
    std::mutex blockMutex;
    std::condition_variable blockCv;
    bool release = false;
    std::promise<void>      blocked;
    auto blockedFuture = blocked.get_future();

    ASSERT_TRUE( workerThread.post( [&blockMutex, &blockCv, &release, &blocked]()
        {
            blocked.set_value();
            std::unique_lock<std::mutex> lock( blockMutex );
            blockCv.wait( lock, [&release]()
            {
                return release;
            } );
        } ) );
    ASSERT_EQ( blockedFuture.wait_for( kPatience ), std::future_status::ready )
        << "the worker never reached the blocking task.";

    // A second context that outlives the first, so the registry sweep has to be selective:
    // dropping every pending entry would be the opposite defect, and a test with only one context
    // in the table could not tell the two apart.
    ObjectTestReceiver survivor;
    survivor.moveToThread( &workerThread );
    Object::callLater( &survivor, &ObjectTestReceiver::onValueReceived, 77 );

    {
        Object context;
        // Built here and pushed: Object takes a parent now, not a thread.
        ASSERT_TRUE( context.moveToThread( &workerThread ) );
        Object::callLater( &context, &testCallLaterFreeFunc, 123 );

        // context is destroyed here, with its metacall still queued on the blocked worker and the
        // survivor's entry sitting next to it in the registry.
    }

    {
        std::lock_guard<std::mutex> lock( blockMutex );
        release = true;
    }
    blockCv.notify_one();

    drainQueuedTasks( workerThread );
    workerThread.quit();
    workerThread.wait();

    EXPECT_EQ( g_testFreeFuncCount, 0 )
        << "a callLater() ran after the context that owned it had been destroyed.";
    EXPECT_EQ( survivor.callCount(), 1 )
        << "destroying one context cancelled another context's pending callLater() too.";
    EXPECT_EQ( survivor.lastValue(), 77 );
}

//! Tests Object::connect with overloaded slots.
TEST( ObjectTest, ConnectOverloadedSlot )
{
    Signal<int, int>   sig;
    ObjectTestReceiver receiver;

    // onValueReceived has two overload 1-arg and 2-arg. Try if we can connect to the 2-arg one.
    Object::connect( sig, &receiver, &ObjectTestReceiver::onValueReceived );

    sig.emit( 3, 4 );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 7 );
}

//! Tests Object::connect with non-void-return slot.
TEST( ObjectTest, ConneectNonVoidReturnSlot )
{
    Signal<int>        sig;
    ObjectTestReceiver receiver;

    Object::connect( sig, &receiver, &ObjectTestReceiver::onValueNonVoidReturn );

    sig.emit( 42 );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 42 );
}

//! Tests Object::connect with const reference argument slot.
TEST( ObjectTest, ConnectConstReferenceSlot )
{
    Signal<std::string> sig;
    ObjectTestReceiver receiver;

    Object::connect( sig, &receiver, &ObjectTestReceiver::onStringConstReference );

    sig.emit( "Hello, Object!" );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastString(), "Hello, Object!" );
}

//! Detects whether T has a callable emit(), for the static assertions below.
template <typename T, typename = void>
struct HasEmit : std::false_type
{
};

template <typename T>
struct HasEmit<T, std::void_t<decltype( std::declval<T&>().emit() )> >: std::true_type
{
};

// The whole point of handing out a view instead of the Signal: a subscriber can connect but
// cannot fire it. Asserted here rather than trusted, because nothing else in the suite would
// notice if emit() were ever added to SignalView -- every test would still pass.
static_assert( HasEmit<Signal<> >::value, "Signal<> must be emittable." );
static_assert( !HasEmit<SignalView<> >::value, "SignalView<> must not be emittable." );

//! Tests Object::connect through a SignalView rather than the Signal itself.
//!
//! A view is what a class hands out when it wants callers to subscribe but not emit (see
//! Thread::getStarted(), Timer::getTimeout()), so every connect() overload has to accept one.
//! Overloads 2-9 name their source as a template-template parameter for exactly this reason;
//! before that they spelled out Signal<Args...> and a view would not bind.
TEST( ObjectTest, ConnectThroughSignalView )
{
    Signal<int>        sig;
    ObjectTestReceiver receiver;

    // Overload 1: deduced source, non-overloaded member slot.
    Object::connect( sig.view(), &receiver, &ObjectTestReceiver::onValueNonVoidReturn );

    sig.emit( 11 );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 11 );
}

//! Tests Object::connect through a SignalView with an overloaded slot and with a lambda.
//!
//! The overloaded slot is the case that actually exercises the template-template change: it
//! cannot use overload 1, because the compiler needs the signal's argument types to pick which
//! onValueReceived is meant.
TEST( ObjectTest, ConnectThroughSignalViewWithOverloadedSlotAndLambda )
{
    Signal<int, int>   sig;
    ObjectTestReceiver receiver;
    Object context;
    int lambdaSum = 0;

    Object::connect( sig.view(), &receiver, &ObjectTestReceiver::onValueReceived );

    // Overload 10: functor with a context object.
    Object::connect( sig.view(), &context, [&lambdaSum]( int aFirst, int aSecond )
        {
            lambdaSum += aFirst + aSecond;
        } );

    sig.emit( 3, 4 );
    EXPECT_EQ( receiver.callCount(), 1 );
    EXPECT_EQ( receiver.lastValue(), 7 );
    EXPECT_EQ( lambdaSum, 7 );
}


//! Polls an atomic counter until it reaches 1 or a bounded number of tries elapses, so a broken
//! delivery fails the test instead of hanging the suite.
static void waitForOneDelivery
    (
    const std::atomic<int>& aCount  //!< Counter the receiving slot bumps.
    )
{
    for( int i = 0; i < 200 && aCount.load() == 0; ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
}

// =================================================================================================
// Tests that originated in a second, independently written object suite.
//
// The two suites shared not one test name, so this is the union
// rather than a reconciliation: nothing was dropped from either side on a judgment that some other
// test "already covers it". Several of these do overlap a test above -- DirectConnectionSameThread
// with DirectSignalSlotConnection, for instance -- and pruning the genuine duplicates is a separate
// pass that should be done by reading the bodies, not the names.
// =================================================================================================

/*----------------------------------------------------------
   Verify that the current thread is represented by a Thread.
   ----------------------------------------------------------*/
TEST( ObjectTest, CurrentThreadIsAvailable )
{
    EXPECT_NE( nullptr, Thread::currentThread() );
}

/*----------------------------------------------------------
   Verify direct delivery when sender and receiver are on the
   same thread.
   ----------------------------------------------------------*/
TEST( ObjectTest, DirectConnectionSameThread )
{
    Producer p;
    Consumer c;
    Object::connect( p.produced, &c, &Consumer::onProduced );

    // Sender and receiver share this thread, so the Auto connection resolves to a
    // direct call at emit time: the slot runs inline, on this thread, before
    // emit() returns (mirror of the queued cross-thread case below).
    p.produced.emit( 42 );

    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 42 );
    EXPECT_EQ( c.mSlotThread, std::this_thread::get_id() );
}

/*----------------------------------------------------------
   Verify Auto connection queues delivery to receiver affinity
   thread when sender emits from a different thread.
   ----------------------------------------------------------*/
TEST( ObjectTest, QueuedConnectionCrossThread )
{
    Thread worker( "worker" );
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    // c's affinity is the worker; p lives on the main thread. With an Auto
    // connection the delivery type is decided at emit time: because we emit from
    // a different thread than c's affinity, the slot is queued into the worker's
    // event loop instead of running inline here.
    Consumer c( &worker );
    Producer p;
    Object::connect( p.produced, &c, &Consumer::onProduced );

    p.produced.emit( 7 );

    // The worker runs the slot asynchronously, so poll until it has processed the
    // queued invocation (bounded so a failure cannot hang the suite).
    for( int i = 0; i < 100 && c.mCount.load() == 0; ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }

    // mCount is atomic; observing it as 1 (a seq_cst RMW/acquire pair with the
    // worker's ++mCount) also publishes the preceding non-atomic writes to
    // mLast/mSlotThread, so reading them here is safe without extra locking.
    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 7 );
    EXPECT_NE( c.mSlotThread, std::this_thread::get_id() );

    worker.quit();
    worker.wait();
}

/*----------------------------------------------------------
   Verify queued slots are dropped safely when the receiver
   is destroyed before delivery.
   ----------------------------------------------------------*/
TEST( ObjectTest, ReceiverDestroyedBeforeDeliveryNoCrash )
{
    Thread worker( "worker2" );
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    std::atomic<int> invocations { 0 };

    // Synchronization primitives to block the worker.
    std::promise<void> releaseWorkerPromise;
    std::future<void> releaseWorkerFuture = releaseWorkerPromise.get_future();

    std::promise<void> workerBlockedPromise;
    std::future<void> workerBlockedFuture = workerBlockedPromise.get_future();

    // Post a task that will hang the worker's event loop.
    ASSERT_TRUE( worker.post( [&]()
        {
            workerBlockedPromise.set_value(); // Tell main thread: "I am now blocked"
            releaseWorkerFuture.wait();   // Block here until main thread says go
        } ) );

    // Wait until the worker thread is stuck on the task above.
    workerBlockedFuture.wait();

    Producer p;
    {
        ExternalCounter c( &worker, invocations );
        Object::connect( p.produced, &c, &ExternalCounter::onProduced );
        // Safe to emit! Worker is blocked, so this event is merely queued, not executed.
        p.produced.emit( 1 );
        // Yield to let the emitter fire a few signals into the queue
        std::this_thread::yield();
    } // 'c' is destroyed here!

    // Unblock the worker so it can finally process the queued emit(1).
    releaseWorkerPromise.set_value();

    // Set a barrier to wait until the worker has processed the queued emit(1) (or at least
    // attempted to).
    std::mutex barrierMutex;
    std::condition_variable barrierCv;
    bool barrierDone = false;
    ASSERT_TRUE( worker.post( [&]()
        {
            {
                std::lock_guard<std::mutex> lock( barrierMutex );
                barrierDone = true;
            }
            barrierCv.notify_one();
        } ) );
    {
        std::unique_lock<std::mutex> lock( barrierMutex );
        ASSERT_TRUE( barrierCv.wait_for( lock, 2s, [&]
            {
                return barrierDone;
            } ) );
    }

    // Slot targeted a destroyed receiver, so it must not have executed.
    EXPECT_EQ( invocations.load(), 0 );

    worker.quit();
    worker.wait();
}

/*----------------------------------------------------------
   Verify incoming connection bookkeeping is pruned on manual
   disconnect and disconnected slots are not invoked.
   ----------------------------------------------------------*/
TEST( ObjectTest, IncomingPrunedOnDisconnect )
{
    Producer p;
    Consumer c;

    EXPECT_EQ( c.incomingConnectionCount(), 0U );

    Connection a = Object::connect( p.produced, &c, &Consumer::onProduced );
    Connection b = Object::connect( p.produced, &c, &Consumer::onProduced );
    EXPECT_EQ( c.incomingConnectionCount(), 2U );

    a.disconnect();
    EXPECT_EQ( c.incomingConnectionCount(), 1U );

    b.disconnect();
    EXPECT_EQ( c.incomingConnectionCount(), 0U );

    // The observable half, which holds on both libraries: a disconnected slot is not invoked.
    const int before = c.mCount.load();
    p.produced.emit( 99 );
    EXPECT_EQ( c.mCount.load(), before );
}

/*----------------------------------------------------------
   Verify the connect template can resolve to an overloaded slot.
   ----------------------------------------------------------*/
TEST( ObjectTest, ConnectToOverloadedSlot )
{
    Producer p;
    Consumer c;

    // produced signal is 1-arg, but Consumer has two onProduced overloads (1-arg and 2-arg).
    // The connect template must resolve the correct one, which is done here by wrapping the
    // pointer-to-member in an overload helper that disambiguates the overload set to the 1-arg
    // version.
    Connection a = Object::connect( p.produced, &c, overload<int>( &Consumer::onProduced ) );
    EXPECT_TRUE( a.connected() );
    a.disconnect();

    a = Object::connect( p.produced2Args, &c, overload<int, int>( &Consumer::onProduced ) );
    EXPECT_TRUE( a.connected() );
    p.produced2Args.emit( 3, 4 );
    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 7 );
}

/*----------------------------------------------------------
   Verify the connect template can accept derived receivers and resolve base's slot.
   ----------------------------------------------------------*/
TEST( ObjectTest, ConnectToBaseSlotOnDerivedReceiver )
{
    Producer p;
    ConsumerDerived c;

    // OnProduced is defined in the base class, but the receiver is a derived type.
    Connection a = Object::connect( p.produced, &c, &ConsumerDerived::onProduced );
    EXPECT_TRUE( a.connected() );
}

/*----------------------------------------------------------
   Verify outsiders can connect a private signal through its
   subscription-only view while only its owner can emit it.
   ----------------------------------------------------------*/
TEST( ObjectTest, ConnectToPrivateSignalView )
{
    ConsumerDerived sender;
    Consumer receiver;

    Connection connection = Object::connect( sender.getSignalView(), &receiver,
        &Consumer::onProduced );
    EXPECT_TRUE( connection.connected() );

    sender.emitPrivateSignal( 73 );
    EXPECT_EQ( receiver.mCount.load(), 1 );
    EXPECT_EQ( receiver.mLast.load(), 73 );
}

/*----------------------------------------------------------
   Verify the connect template can resolve to a const slot.
   ----------------------------------------------------------*/
TEST( ObjectTest, ConnectToConstSlot )
{
    Producer p;
    Consumer c;

    Connection a = Object::connect( p.produced, &c, &Consumer::onProducedConst );
    EXPECT_TRUE( a.connected() );

    p.produced.emit( 123 );
    EXPECT_EQ( c.mLastConst.load(), 123 );
}

/*----------------------------------------------------------
   Verify the connect template can resolve to a non-void return type slot.
   ----------------------------------------------------------*/
TEST( ObjectTest, ConnectToNonVoidReturnTypeSlot )
{
    Producer p;
    Consumer c;

    Connection a = Object::connect( p.produced, &c, &Consumer::onProducedReturnInt );
    EXPECT_TRUE( a.connected() );

    p.produced.emit( 456 );
    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 456 );
}

/*----------------------------------------------------------
   Verify a lambda can be connected to a signal that carries
   arguments and receives the emitted value. Regression guard
   for the connectImpl<Args...> bug, which bound the signal's
   argument types onto connectImpl's SignalType parameter and
   so failed to compile for any non-empty Signal<...>; only
   Signal<> slipped through.
   ----------------------------------------------------------*/
TEST( ObjectTest, ConnectLambdaToSignalWithArgs )
{
    Producer p;
    Consumer c; // context object for affinity/lifetime

    std::atomic<int> received { 0 };
    Connection a = Object::connect( p.produced, &c, [&]( int aValue )
        {
            received = aValue;
        } );
    EXPECT_TRUE( a.connected() );

    p.produced.emit( 321 );
    EXPECT_EQ( received.load(), 321 );

    std::atomic<int> receivedSum { 0 };
    Connection b = Object::connect( p.produced2Args, &c, [&]( int aFirst, int aSecond )
        {
            receivedSum = aFirst + aSecond;
        } );
    EXPECT_TRUE( b.connected() );

    p.produced2Args.emit( 20, 22 );
    EXPECT_EQ( receivedSum.load(), 42 );
}

/*----------------------------------------------------------
   Verify deleteLater posts destruction to affinity thread and
   multiple deleteLater calls coalesce into a single delete.
   ----------------------------------------------------------*/
TEST( ObjectTest, DeleteLaterCrossThreadAndSameThreadCoalesce )
{
    Thread worker( "deleteLaterWorker" );
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    std::thread::id workerId;
    std::mutex workerIdMutex;
    std::condition_variable workerIdCv;
    bool workerIdReady = false;

    ASSERT_TRUE( worker.post( [&]()
        {
            {
                std::lock_guard<std::mutex> lock( workerIdMutex );
                workerId = std::this_thread::get_id();
                workerIdReady = true;
            }
            workerIdCv.notify_one();
        } ) );

    {
        std::unique_lock<std::mutex> lock( workerIdMutex );
        workerIdCv.wait_for( lock, std::chrono::seconds( 1 ), [&]()
            {
                return workerIdReady;
            } );
    }
    ASSERT_TRUE( workerIdReady );

    std::mutex dtorMutex;
    std::condition_variable dtorCv;
    bool dtorDone = false;
    std::thread::id dtorThread;
    std::atomic<int> dtorCount { 0 };

    DeleteProbe* obj = new DeleteProbe( &worker, dtorMutex, dtorCv, dtorDone, dtorThread,
        dtorCount );

    // deleteLater() posts the actual delete to the object's affinity thread (the
    // worker). The second call must be a no-op: an internal atomic guard ensures
    // only one delete is ever posted, so dtorCount is expected to be exactly 1.
    obj->deleteLater();
    obj->deleteLater();

    {
        std::unique_lock<std::mutex> lock( dtorMutex );
        dtorCv.wait_for( lock, std::chrono::seconds( 1 ), [&]()
            {
                return dtorDone;
            } );
    }

    EXPECT_TRUE( dtorDone );
    EXPECT_EQ( dtorCount.load(), 1 );
    EXPECT_EQ( dtorThread, workerId );

    worker.quit();
    worker.wait();

    std::mutex mainDtorMutex;
    std::condition_variable mainDtorCv;
    bool mainDtorDone = false;
    std::thread::id mainDtorThread;
    std::atomic<int> mainDtorCount { 0 };

    DeleteProbe* mainObj = new DeleteProbe( nullptr, mainDtorMutex, mainDtorCv, mainDtorDone,
        mainDtorThread, mainDtorCount );

    mainObj->deleteLater();
    mainObj->deleteLater();

    // mainObj was constructed with a null thread, so its affinity resolved to the
    // Thread of the thread that built it - i.e. this test thread's adopted
    // Thread (the same object Thread::currentThread() returns here). That thread has
    // no running exec() loop, so nothing drains its queue automatically; we pump
    // it by hand, which is where the deferred delete actually runs.
    Thread::currentThread()->processEvents();

    EXPECT_TRUE( mainDtorDone );
    EXPECT_EQ( mainDtorCount.load(), 1 );
    EXPECT_EQ( mainDtorThread, std::this_thread::get_id() );
}

/*----------------------------------------------------------
   A valid push (from the object's own thread) redirects a
   connection made BEFORE the move to the new thread.
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadPushRedirectsExistingConnection )
{
    Thread worker( "push-redirect-worker" );
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Consumer c; // built on and living in this (main) thread
    Producer p;

    // Connected BEFORE the move; delivery is resolved at emit time, so it must
    // follow c to the worker.
    Object::connect( p.produced, &c, &Consumer::onProduced );

    // Valid push: the caller (this thread) is c's current affinity thread.
    EXPECT_TRUE( c.moveToThread( &worker ) );
    EXPECT_EQ( c.thread(), &worker );

    // Emit from this thread (different from the worker), so the Auto connection
    // queues into the worker's event loop.
    p.produced.emit( 5 );
    waitForOneDelivery( c.mCount );

    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 5 );
    EXPECT_EQ( c.mSlotThread, worker.id() );
    EXPECT_NE( c.mSlotThread, std::this_thread::get_id() );

    worker.quit();
    worker.wait();
}

/*----------------------------------------------------------
   Moving to the thread the object already lives in is a
   successful no-op that returns true (as in Qt6).
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadReturnsTrueWhenAlreadyInTargetThread )
{
    Consumer c; // lives in this thread

    EXPECT_TRUE( c.moveToThread( Thread::currentThread() ) );
    EXPECT_EQ( c.thread(), Thread::currentThread() );
}

/*----------------------------------------------------------
   Push-only protection: a pull (caller is not on the object's
   affinity thread) is refused, returns false, and leaves the
   affinity untouched.
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadRejectsPullFromAnotherThread )
{
    Thread worker( "pull-source-worker" );
    Thread other( "pull-target-worker" );
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );
    other.start();
    ASSERT_TRUE( waitUntilRunning( other ) );

    // c's affinity is the worker, but we call moveToThread() from this thread.
    Consumer c( &worker );

    EXPECT_FALSE( c.moveToThread( &other ) );
    EXPECT_EQ( c.thread(), &worker ) << "a refused move must not change affinity";

    EXPECT_FALSE( c.moveToThread( nullptr ) );
    EXPECT_EQ( c.thread(), &worker );

    worker.quit();
    worker.wait();
    other.quit();
    other.wait();
}

/*----------------------------------------------------------
   Qt's one exception to push-only: an object with no affinity
   may be pulled to the calling thread.
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadAllowsNoAffinityPullToCallingThread )
{
    Consumer c; // lives in this thread

    // Dissociating from our own thread is a valid push to "no thread".
    EXPECT_TRUE( c.moveToThread( nullptr ) );
    EXPECT_EQ( c.thread(), nullptr );

    // Now thread-less: pulling it to the calling thread is the allowed exception.
    EXPECT_TRUE( c.moveToThread( Thread::currentThread() ) );
    EXPECT_EQ( c.thread(), Thread::currentThread() );
}

/*----------------------------------------------------------
   Pulling a thread-less object to a thread that is NOT the
   caller is still refused (only the caller is exempt).
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadRefusesNoAffinityMoveToOtherThread )
{
    Thread other( "no-affinity-other-worker" );
    other.start();
    ASSERT_TRUE( waitUntilRunning( other ) );

    Consumer c;
    ASSERT_TRUE( c.moveToThread( nullptr ) ); // now thread-less
    EXPECT_EQ( c.thread(), nullptr );

    EXPECT_FALSE( c.moveToThread( &other ) );
    EXPECT_EQ( c.thread(), nullptr );

    other.quit();
    other.wait();
}

/*----------------------------------------------------------
   moveToThread(nullptr) from the object's own thread
   dissociates it: thread() reports null and, as in Qt6, all
   event processing for it stops -- an Auto connection emitted
   afterwards is dropped, not delivered directly on the emitter.
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadNullStopsEventProcessing )
{
    Consumer c; // lives in this thread
    Producer p;
    Object::connect( p.produced, &c, &Consumer::onProduced );

    EXPECT_TRUE( c.moveToThread( nullptr ) );
    EXPECT_EQ( c.thread(), nullptr );

    // Detached: Qt parks the object on an orphan thread-data whose loop never runs, so the
    // slot is not invoked at all -- deliberately NOT a direct-call fallback on the emitter.
    p.produced.emit( 8 );

    EXPECT_EQ( c.mCount.load(), 0 );
}

/*----------------------------------------------------------
   A Direct connection ignores thread affinity, so it still
   fires on a detached object (unlike an Auto one, above).
   ----------------------------------------------------------*/
TEST( ObjectTest, DirectConnectionStillFiresOnDetachedObject )
{
    Consumer c; // lives in this thread
    Producer p;
    Object::connect( p.produced, &c, &Consumer::onProduced, ConnectionType::Direct );

    EXPECT_TRUE( c.moveToThread( nullptr ) );
    EXPECT_EQ( c.thread(), nullptr );

    p.produced.emit( 9 );

    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 9 );
    EXPECT_EQ( c.mSlotThread, std::this_thread::get_id() );
}

/*----------------------------------------------------------
   A second push performed FROM the object's (new) own thread
   is honored, and the latest affinity is the one used.
   ----------------------------------------------------------*/
TEST( ObjectTest, MoveToThreadSecondPushFromOwningThread )
{
    Thread workerB( "second-push-b" );
    Thread workerC( "second-push-c" );
    workerB.start();
    ASSERT_TRUE( waitUntilRunning( workerB ) );
    workerC.start();
    ASSERT_TRUE( waitUntilRunning( workerC ) );

    Consumer c; // lives in this thread
    Producer p;
    Object::connect( p.produced, &c, &Consumer::onProduced );

    // First push (main -> B) is valid from this thread.
    ASSERT_TRUE( c.moveToThread( &workerB ) );

    // Second push (B -> C) must run ON B, since c now lives there. Doing it from
    // here would be a pull and be refused.
    std::promise<bool> movedPromise;
    std::future<bool> movedFuture = movedPromise.get_future();
    ASSERT_TRUE( workerB.post( [&]()
        {
            movedPromise.set_value( c.moveToThread( &workerC ) );
        } ) );
    EXPECT_TRUE( movedFuture.get() );
    EXPECT_EQ( c.thread(), &workerC );

    p.produced.emit( 11 );
    waitForOneDelivery( c.mCount );

    EXPECT_EQ( c.mCount.load(), 1 );
    EXPECT_EQ( c.mLast.load(), 11 );
    EXPECT_EQ( c.mSlotThread, workerC.id() );
    EXPECT_NE( c.mSlotThread, workerB.id() );

    workerB.quit();
    workerB.wait();
    workerC.quit();
    workerC.wait();
}
