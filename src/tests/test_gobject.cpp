#include <gtest/gtest.h>
#include "Object.h"
#include "Signal.h"
#include "Event.h"
#include "CoreApplication.h"
#include "Thread.h"
#include "EventDispatcherDefault.h"
#include "Timer.h"
#include <atomic>
#include <string>
#include <future>
#include <thread>

using namespace QtLikeSignal;

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
//! Verifies Object::objectLife() returns a valid weak pointer during the lifetime of Object
//! and expires upon object destruction.
TEST( ObjectTest, ObjectLifeToken )
{
    std::weak_ptr<int> lifeToken;
    {
        Object obj;
        lifeToken = obj.objectLife();
        EXPECT_FALSE( lifeToken.expired() );
    }
    EXPECT_TRUE( lifeToken.expired() );
}

//! Tests destruction cleanup callback execution.
//!
//! Verifies Object::addCleanupCallback() registers callbacks that execute when Object is
//! destroyed.
TEST( ObjectTest, CleanupCallbacks )
{
    bool callbackFired1 = false;
    bool callbackFired2 = false;

    {
        Object obj;
        obj.addCleanupCallback( [&callbackFired1]()
            {
                callbackFired1 = true;
            } );
        obj.addCleanupCallback( [&callbackFired2]()
            {
                callbackFired2 = true;
            } );
        EXPECT_FALSE( callbackFired1 );
        EXPECT_FALSE( callbackFired2 );
    }

    EXPECT_TRUE( callbackFired1 );
    EXPECT_TRUE( callbackFired2 );
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
//! Verifies Object::connect() safely returns an invalid ConnectionHandle when passed nullptr
//! for receiver or context pointers.
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

//! Tests connect() with member function slot when receiver lives in another thread.
//!
//! Verifies that Object::connect() with ConnectionType::Auto or ConnectionType::Queued routes signal
//! emissions across thread boundaries into the receiver's thread event loop for execution.
TEST( ObjectTest, CrossThreadMemberFunctionConnection )
{
    Thread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
