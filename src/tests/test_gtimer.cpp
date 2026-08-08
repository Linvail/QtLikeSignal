#include <gtest/gtest.h>
#include "Timer.h"
#include "Event.h"
#include "Signal.h"
#include "Thread.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace QtLikeSignal;

//! Helper test receiver class for verifying Timer singleShot member function invocations.
class TimerTestReceiver : public Object
{
public:
    //! Slot called when timer expires.
    void onTimeout()
    {
        mFired = true;
        mFireCount++;
    }

    //! Checks if slot was called. Returns true if fired.
    bool wasFired() const
    {
        return mFired;
    }

    //! Gets total fire count. Returns the fire count.
    int fireCount() const
    {
        return mFireCount;
    }

private:
    // Atomic so this test thread can poll them while the worker thread writes them.
    std::atomic<bool> mFired { false };
    std::atomic<int>  mFireCount { 0 };
};

//! Test timer subclass to verify manual event handling.
class ManualTestTimer : public Timer
{
public:
    //! Invokes protected timerEvent for test verification.
    void triggerTimerEvent
        (
        TimerEvent* aEv  //!< Timer event.
        )
    {
        timerEvent( aEv );
    }

};

//! Tests Timer configuration and property accessors.
//!
//! Verifies Timer::setInterval(), Timer::interval(), Timer::setSingleShot(),
//! Timer::isSingleShot(), Timer::isActive(), and Timer::timerId() initial states.
TEST( TimerTest, ConfigurationAndProperties )
{
    Timer timer;
    timer.setInterval( 100 );
    EXPECT_EQ( timer.interval(), 100 );

    timer.setSingleShot( true );
    EXPECT_TRUE( timer.isSingleShot() );
    EXPECT_FALSE( timer.isActive() );
    EXPECT_EQ( timer.timerId(), -1 );
}

//! Tests Timer start and stop lifecycle.
//!
//! Verifies Timer::start(ms), Timer::stop(), Timer::isActive(), and Timer::timerId() state
//! transitions.
TEST( TimerTest, StartAndStop )
{
    Thread* thread = Thread::create(
        []()
        {
            Timer timer;
            EXPECT_FALSE( timer.isActive() );

            timer.start( 150 );
            EXPECT_EQ( timer.interval(), 150 );
            EXPECT_TRUE( timer.isActive() );
            EXPECT_GT( timer.timerId(), 0 );

            timer.stop();
            EXPECT_FALSE( timer.isActive() );
            EXPECT_EQ( timer.timerId(), -1 );

            timer.setInterval( 200 );
            EXPECT_EQ( timer.interval(), 200 );
        } );
    thread->start();
    thread->wait();
    delete thread;
}

//! Starts a worker thread running an event loop and blocks until its dispatcher exists.
static void startWorkerAndWaitForDispatcher
    (
    Thread& aThread  //!< The thread to start.
    )
{
    aThread.start();
    while( !aThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }
}

//! Blocks until a queued slot has run on the context object's thread.
//!
//! Used before quitting a worker so that anything already sitting in its queue is processed first
//! -- in particular a single-shot helper's own deleteLater(), which the helper posts immediately
//! after invoking its functor. Quitting without this can stop the loop while that delete is still
//! queued, and the object leaks: the dispatcher's destructor frees the pending event but has no
//! way to delete its receiver. ASan/LSan reports it, so the tests must not race it.
static void drainQueuedEvents
    (
    Object& aContext  //!< Object whose thread's event loop should be drained.
    )
{
    std::promise<void> syncPromise;
    auto syncFuture = syncPromise.get_future();
    Signal<>          syncSignal;
    Object::connect(
        syncSignal,
        &aContext,
        [&syncPromise]()
        {
            syncPromise.set_value();
        },
        ConnectionType::QueuedConnection );
    syncSignal.emit();
    EXPECT_EQ( syncFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "worker event loop did not drain.";
}

//! Tests static Timer::singleShot overload for standalone functors.
//!
//! Verifies Timer::singleShot(int, Functor) actually runs the functor. The call is made from
//! inside a queued slot so it executes on a thread that both owns a dispatcher and is running an
//! event loop -- the helper object registers its timer against the calling thread, so invoking
//! this from a thread without a running loop (as this test previously did from the main thread of
//! a binary with no CoreApplication) silently does nothing: startTimer() returns -1 and the
//! helper is destroyed immediately.
TEST( TimerTest, SingleShotStaticLambdaFires )
{
    Thread worker;
    startWorkerAndWaitForDispatcher( worker );

    Object context;
    context.moveToThread( &worker );

    std::promise<void> firedPromise;
    auto firedFuture = firedPromise.get_future();

    Signal<> trigger;
    Object::connect(
        trigger,
        &context,
        [&firedPromise]()
        {
            Timer::singleShot( 10, [&firedPromise]()
            {
                firedPromise.set_value();
            } );
        },
        ConnectionType::QueuedConnection );
    trigger.emit();

    EXPECT_EQ( firedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "singleShot(int, Functor) never invoked its functor.";

    drainQueuedEvents( context );
    worker.quit();
    worker.wait();
}

//! Tests static Timer::singleShot overload with target Object context.
//!
//! Verifies Timer::singleShot(int, const Object*, Functor) runs the functor on the context
//! object's thread, and that a null context is handled safely.
TEST( TimerTest, SingleShotStaticWithContextFires )
{
    Thread worker;
    startWorkerAndWaitForDispatcher( worker );

    Object context;
    context.moveToThread( &worker );

    std::promise<Thread*> firedPromise;
    auto firedFuture = firedPromise.get_future();

    Timer::singleShot( 10,
        &context,
        [&firedPromise]()
        {
            firedPromise.set_value( Thread::currentThread() );
        } );

    Object* nullContext = nullptr;
    Timer::singleShot( 10, nullContext, []()
        {
        } );

    ASSERT_EQ( firedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "singleShot(int, context, Functor) never invoked its functor.";
    EXPECT_EQ( firedFuture.get(), &worker ) <<
        "functor did not run on the context object's thread.";

    drainQueuedEvents( context );
    worker.quit();
    worker.wait();
}

//! Tests static Timer::singleShot overload with receiver object member function.
//!
//! Verifies Timer::singleShot(int, const Receiver*, MemberFunc) invokes the member function, and
//! that a null receiver is handled safely.
TEST( TimerTest, SingleShotStaticWithReceiverFires )
{
    Thread worker;
    startWorkerAndWaitForDispatcher( worker );

    TimerTestReceiver receiver;
    receiver.moveToThread( &worker );

    TimerTestReceiver* nullReceiver = nullptr;
    Timer::singleShot( 10, nullReceiver, &TimerTestReceiver::onTimeout );

    Timer::singleShot( 10, &receiver, &TimerTestReceiver::onTimeout );

    // wasFired() is atomic, so polling it from this thread while the worker writes it is safe.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( !receiver.wasFired() && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    drainQueuedEvents( receiver );
    worker.quit();
    worker.wait();

    EXPECT_TRUE( receiver.wasFired() )
        << "singleShot(int, receiver, MemberFunc) never invoked the member function.";
    EXPECT_EQ( receiver.fireCount(), 1 ) << "the single-shot fired more than once.";
}

//! Tests timer event handling and timeout signal emission.
//!
//! Verifies Timer::timerEvent() processes matching TimerEvent IDs and emits the Timer::timeout
//! signal.
TEST( TimerTest, ManualTimerEventTriggering )
{
    Thread* thread = Thread::create(
        []()
        {
            ManualTestTimer timer;
            timer.start( 100 );
            int tid = timer.timerId();
            EXPECT_GT( tid, 0 );

            bool timeoutSignaled = false;

            Object context;
            Object::connect(
            timer.timeout,
            &context,
            [&timeoutSignaled]()
            {
                timeoutSignaled = true;
            },
            ConnectionType::DirectConnection );

            TimerEvent event( tid );
            timer.triggerTimerEvent( &event );

            EXPECT_TRUE( timeoutSignaled );
            timer.stop();
        } );
    thread->start();
    thread->wait();
    delete thread;
}
