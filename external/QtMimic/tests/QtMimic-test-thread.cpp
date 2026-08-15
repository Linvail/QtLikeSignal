//! @file
//!
//! GoogleTest suite for QtMimic::Thread -- lifecycle, exit codes, post() and the subclassing
//! idiom.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic-test-types.hpp"

#include "gtest/gtest.h"
#include "QtMimic/Thread.hpp"
#include "QtMimic/Signal.hpp"
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace QtMimic;

//! Custom Thread subclass for testing thread execution.
class CustomTestThread : public Thread
{
protected:
    virtual void run() override
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        mExecuted = true;
    }

public:
    //! Checks if run() executed. Returns true if run() completed.
    bool wasExecuted() const
    {
        return mExecuted;
    }

private:
    bool mExecuted { false };
};

//! Thread subclass for testing Thread::currentThread() pointer.
class ThreadPointerCheckThread : public Thread
{
protected:
    virtual void run() override
    {
        mSelfPointer = Thread::currentThread();
    }

public:
    //! Gets the captured currentThread pointer. Returns the captured thread pointer.
    Thread* selfPointer() const
    {
        return mSelfPointer;
    }

private:
    Thread* mSelfPointer { nullptr };
};

//! Thread subclass for testing exit code signaling.
class ExitCodeTestThread : public Thread
{
protected:
    virtual void run() override
    {
        exit( 123 );
    }

};

//! Thread subclass for testing wait timeout logic.
class SlowTestThread : public Thread
{
protected:
    virtual void run() override
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
    }

};

//! Tests thread lifecycle methods and lifecycle signals. Verifies Thread::start(),
//! Thread::wait(), Thread::isRunning(), Thread::isFinished(), and emission of
//! Thread::started and Thread::finished signals.
TEST( ThreadTest, LifecycleAndSignals )
{
    CustomTestThread thread;
    bool startedFired  = false;
    bool finishedFired = false;

    Object context;
    Object::connect(
        thread.getStarted(), &context, [&startedFired]()
        {
            startedFired = true;
        }, ConnectionType::Direct );

    Object::connect(
        thread.getFinished(),
        &context,
        [&finishedFired]()
        {
            finishedFired = true;
        },
        ConnectionType::Direct );

    thread.start();
    thread.wait();

    EXPECT_TRUE( thread.isFinished() );
    EXPECT_FALSE( thread.isRunning() );
    EXPECT_TRUE( thread.wasExecuted() );
    EXPECT_TRUE( startedFired );
    EXPECT_TRUE( finishedFired );
}

//! Tests static thread factory creation. Verifies static function Thread::create()
//! instantiates a Thread that executes a functor once started.
//!
//! create() deliberately does not start what it returns -- see its declaration -- so start() here
//! is part of the contract, not boilerplate.
TEST( ThreadTest, CreateStaticFactory )
{
    bool funcExecuted = false;
    Thread* threadObj    = Thread::create( [&funcExecuted]()
        {
            funcExecuted = true;
        } );

    threadObj->start();
    threadObj->wait();
    EXPECT_TRUE( funcExecuted );
    delete threadObj;
}

//! Verifies Thread::create() hands back an unstarted thread, and why that matters.
//!
//! create() used to start the thread before returning, which closed the only window in which a
//! caller can connect to started, re-home Objects onto the thread, or set its priority --
//! setPriority() refuses on a thread that is not running, so a self-starting thread could never be
//! given one without a race. Qt's QThread::create() leaves it unstarted for exactly these reasons:
//! "The new thread is not started -- it must be started by an explicit call to start(). This allows
//! you to connect to its signals, move QObjects to the thread, choose the new thread's priority and
//! so on."
//!
//! Pins the whole window, not just the flag: a started signal connected before start() must
//! actually fire, which is the thing that was impossible before.
TEST( ThreadTest, CreateReturnsAnUnstartedThread )
{
    std::atomic<bool> bodyRan { false };
    Thread* threadObj = Thread::create( [&bodyRan]()
        {
            bodyRan.store( true );
        } );
    ASSERT_NE( threadObj, nullptr );

    EXPECT_FALSE( threadObj->isRunning() ) << "create() must not start the thread";
    EXPECT_FALSE( threadObj->isFinished() );

    // The window create() exists to preserve: wire up the thread before it runs.
    std::atomic<bool> startedFired { false };
    Object context;
    Object::connect( threadObj->getStarted(), &context, [&startedFired]()
        {
            startedFired.store( true );
        }, ConnectionType::Direct );

    // Nothing should have happened yet.
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    EXPECT_FALSE( bodyRan.load() ) << "the body ran without start() ever being called";

    threadObj->start();
    threadObj->wait();

    EXPECT_TRUE( bodyRan.load() );
    EXPECT_TRUE( startedFired.load() )
        << "started was connected before start() and still did not fire -- the window create() "
        "leaves open is not usable.";

    delete threadObj;
}

//! Tests retrieval of current thread pointer. Verifies static function
//! Thread::currentThread() returns the pointer to the active Thread instance inside its
//! execution context.
TEST( ThreadTest, CurrentThreadPointer )
{
    ThreadPointerCheckThread thread;
    thread.start();
    thread.wait();

    EXPECT_EQ( thread.selfPointer(), &thread );
}

//! Tests thread exit signal and completion status. Verifies Thread::exit() requests thread
//! termination and transitions Thread to finished state.
TEST( ThreadTest, ThreadExitAndReturnCode )
{
    ExitCodeTestThread thread;
    thread.start();
    thread.wait();
    EXPECT_TRUE( thread.isFinished() );
}

//! Tests timed waiting on thread execution. Verifies Thread::wait(ms) returns false when
//! thread execution exceeds timeout, and true once finished.
TEST( ThreadTest, WaitTimeout )
{
    SlowTestThread thread;
    thread.start();

    bool finishedInShortTime = thread.wait( 20 );
    EXPECT_FALSE( finishedInShortTime );
    EXPECT_TRUE( thread.isRunning() );

    bool finishedEventually = thread.wait( 1000 );
    EXPECT_TRUE( finishedEventually );
    EXPECT_TRUE( thread.isFinished() );
}

//! Tests concurrent multi-thread execution. Verifies launching multiple Thread instances in
//! parallel, joining each via Thread::wait(), and ensuring thread safety.
TEST( ThreadTest, MultipleThreadsExecution )
{
    constexpr int count = 5;
    std::atomic<int>      completedCount { 0 };
    std::vector<Thread*> threads;

    for( int i = 0; i < count; ++i )
    {
        Thread* t = Thread::create(
            [&completedCount]()
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
                completedCount.fetch_add( 1 );
            } );
        t->start();
        threads.push_back( t );
    }

    for( auto* t : threads )
    {
        t->wait();
        delete t;
    }

    EXPECT_EQ( completedCount.load(), count );
}

//! Tests event dispatcher lifetime across a thread's own start/finish cycle. Replaces the
//! former EventDispatcherSetAndGet test, which drove the removed Thread::setEventDispatcher().
//! That setter could delete a dispatcher a running exec()/processEvents() loop was still calling
//! into, so a thread now creates and owns its dispatcher itself and eventDispatcher() is
//! read-only. This verifies that contract: none before start(), one owned while running, and
//! cleaned up on exit (the last part relies on AddressSanitizer/LeakSanitizer in the debug build
//! to catch a leak or double free).
TEST( ThreadTest, EventDispatcherOwnedAcrossThreadLifecycle )
{
    Thread thread;
    EXPECT_EQ( thread.eventDispatcher(), nullptr ) << "no dispatcher should exist before start()";

    thread.start();
    while( !thread.eventDispatcher() )
    {
        std::this_thread::yield();
    }
    EXPECT_NE( thread.eventDispatcher(), nullptr ) <<
        "start() should create the thread's dispatcher";

    thread.quit();
    thread.wait();
    EXPECT_TRUE( thread.isFinished() );
}

//! Tests Thread::post() runs the task on the target thread, from another thread.
TEST( ThreadTest, PostRunsTaskOnTargetThread )
{
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    std::promise<Thread*> ranOnPromise;
    auto ranOnFuture = ranOnPromise.get_future();

    EXPECT_TRUE( worker.post( [&ranOnPromise]()
        {
            ranOnPromise.set_value( Thread::currentThread() );
        } ) );

    ASSERT_EQ( ranOnFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "posted task never ran.";
    EXPECT_EQ( ranOnFuture.get(), &worker );

    worker.quit();
    worker.wait();
}

//! Tests Thread::post() always defers, even when called from the target thread itself.
//! Regression coverage for the reason post() explicitly requests ConnectionType::Queued rather
//! than ConnectionType::Auto: Auto would resolve to a same-thread direct call and run the task
//! inline, before post() returns, instead of on a later loop iteration.
TEST( ThreadTest, PostFromOwnThreadStillDefers )
{
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    std::promise<void> orderPromise;
    auto orderFuture = orderPromise.get_future();
    std::atomic<bool> postReturnedBeforeTaskRan { false };

    // Lives in the test body, not inside the outer task, and that is the whole point: the inner
    // task runs *after* the outer one returns -- which is exactly what this test asserts -- so a
    // variable local to the outer task would already be destroyed by the time the inner task wrote
    // to it. It was a local, and AddressSanitizer caught the resulting stack-use-after-return.
    // The test body outlives everything here, since quit()/wait() below stop the loop before any of
    // these locals go out of scope.
    std::atomic<bool> innerRan { false };

    // Ask the worker to post a task to itself, and observe whether post() returns before or
    // after that inner task actually executes.
    ASSERT_TRUE( worker.post(
        [&worker, &orderPromise, &postReturnedBeforeTaskRan, &innerRan]()
        {
            worker.post( [&innerRan]()
            {
                innerRan.store( true );
            } );
            // If post() deferred correctly, innerRan is still false immediately after the call.
            postReturnedBeforeTaskRan.store( !innerRan.load() );
            orderPromise.set_value();
        } ) );

    ASSERT_EQ( orderFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );
    EXPECT_TRUE( postReturnedBeforeTaskRan.load() )
        << "post() ran the task inline instead of deferring it.";

    worker.quit();
    worker.wait();
}

//! Tests Thread::post() reports failure and drops the task when there is no dispatcher.
TEST( ThreadTest, PostBeforeStartFails )
{
    Thread thread;
    bool ran = false;
    EXPECT_FALSE( thread.post( [&ran]()
        {
            ran = true;
        } ) )
        << "post() should fail before start() -- there is no dispatcher yet.";
    EXPECT_FALSE( ran );
}

//! Tests Thread::post() rejects an empty std::function without touching the dispatcher.
TEST( ThreadTest, PostRejectsEmptyTask )
{
    Thread worker;
    worker.start();
    waitUntilRunning( worker );

    EXPECT_FALSE( worker.post( std::function<void()>() ) );

    worker.quit();
    worker.wait();
}

//! Verifies isRunning()/isFinished() report the states Qt reports, at the moments Qt reports them.
//!
//! Three claims, same as Qt's behavior.
//!
//!   - An adopted thread is *running*. Qt's adopting QThread constructor sets threadState =
//!     Running outright, commenting that the thread "should be running and not finished for the
//!     lifetime of the application". Thread::currentThread()->isRunning() answering false on the
//!     main thread was simply a lie.
//!   - A finished() handler sees isRunning() false and isFinished() true. Qt sets Finishing inside
//!     finish() and emits finished() immediately afterwards, in that order.
//!   - wait() returns only once the thread is fully done, which is strictly later than the point
//!     isFinished() starts reporting true. Qt separates Finishing from Finished for this reason.
TEST( ThreadTest, RunningAndFinishedFollowQtStateTransitions )
{
    Thread* const adopted = Thread::currentThread();
    ASSERT_NE( adopted, nullptr );
    EXPECT_TRUE( adopted->isRunning() ) << "an adopted thread is running -- it is executing now";
    EXPECT_FALSE( adopted->isFinished() );

    Thread worker( "state-transitions" );
    EXPECT_FALSE( worker.isRunning() ) << "not started yet";
    EXPECT_FALSE( worker.isFinished() );

    std::atomic<bool> runningInHandler { true };
    std::atomic<bool> finishedInHandler { false };
    Object context;
    Object::connect( worker.getFinished(), &context,
        [&worker, &runningInHandler, &finishedInHandler]()
        {
            runningInHandler.store( worker.isRunning() );
            finishedInHandler.store( worker.isFinished() );
        }, ConnectionType::Direct );

    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );
    EXPECT_TRUE( worker.isRunning() );
    EXPECT_FALSE( worker.isFinished() );

    worker.quit();

    // Called as a statement, not asserted on: the untimed call cannot fail, and what matters here
    // is what it guarantees on return, which the assertions below check. The return value is
    // exercised by WaitTimeout instead.
    worker.wait();

    EXPECT_FALSE( runningInHandler.load() )
        << "finished() ran while the thread still reported itself running";
    EXPECT_TRUE( finishedInHandler.load() )
        << "finished() ran before the thread reported itself finished";

    EXPECT_FALSE( worker.isRunning() );
    EXPECT_TRUE( worker.isFinished() );
}
