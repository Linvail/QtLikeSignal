// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for QtLikeSignal::Thread -- lifecycle, exit codes, post() and the subclassing
//! idiom.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>

#if defined( __linux__ )
    // For pthread_setname_np() only, which one test below uses to clear a thread's name. There is
    // no portable way to do that -- Thread::setNativeName() deliberately ignores an empty name --
    // and the test that needs it says why.
    #include <pthread.h>
#endif

using namespace QtLikeSignal;

//! Custom Thread subclass for testing thread execution.
class CustomTestThread : public Thread
{
protected:
    void run() override
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
    void run() override
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
    void run() override
    {
        exit( 123 );
    }

};

//! Thread subclass for testing wait timeout logic.
class SlowTestThread : public Thread
{
protected:
    void run() override
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
    }

};

//! Tests thread lifecycle methods and lifecycle signals. Verifies Thread::start(),
//! Thread::wait(), Thread::isRunning(), Thread::isFinished(), and emission of
//! Thread::started and Thread::finished signals.
//! A Thread can be given a parent at construction, and the parent deletes it -- while running.
//!
//! **The point is the teardown, not the linkage.** Qt allows a QThread to have a parent too, but
//! deleting that parent while the thread runs aborts: ~QThread() refuses to destroy a running
//! thread. ~Thread() calls quit() and wait() instead, so the parent's destructor stops and joins
//! it. That difference is the whole reason parenting a Thread is usable here.
//!
//! The thread is confirmed to be *inside* its loop before the parent is destroyed, because that is
//! the case being tested. Destroying it before the loop starts proves nothing and is a different
//! race -- see the note in NoWorkIsPromisedAfterQuit.
TEST( ThreadTest, ParentDeletesAThreadThatIsStillRunning )
{
    struct FlaggedThread : public Thread
    {
        FlaggedThread
            (
            bool* aDestroyed,
            Object* aParent
            )
            : Thread( "parented", aParent )
            , mDestroyed( aDestroyed )
        {
        }

        ~FlaggedThread() override
        {
            *mDestroyed = true;
        }

        bool* mDestroyed;
    };

    Object* const owner = new Object();

    bool destroyed = false;
    Thread* const worker = new FlaggedThread( &destroyed, owner );

    EXPECT_EQ( owner, worker->parent() );
    EXPECT_EQ( worker, owner->firstChild() );

    worker->start();

    // Waited for, not assumed: the loop has to be running for this to be testing what it claims.
    std::promise<void> insideLoop;
    auto reached = insideLoop.get_future();
    ASSERT_TRUE( worker->post( [&insideLoop]()
        {
            insideLoop.set_value();
        } ) );
    ASSERT_EQ( reached.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "the worker never entered its loop, so the deletion below would not be testing a "
        "running thread.";

    // No quit(), no wait(), no delete of the thread: the parent is asked to go and everything else
    // follows from that. Under Qt's destructor this aborts.
    delete owner;

    EXPECT_TRUE( destroyed )
        << "the parent did not delete the thread it owned.";
}

//! quit() ends the loop; it does not promise to drain what is already queued.
//!
//! **Written down because the opposite is easy to assume and usually appears to be true.** A task
//! posted to a thread that is sitting in processEvents() wakes it, gets dispatched, and only then
//! does the loop notice it has been asked to stop -- so post-then-quit looks reliable. It is not:
//! exec() tests its exiting flag before calling into the dispatcher, so a quit() that lands before
//! the loop's first pass drops everything queued behind it. Qt makes the same non-promise.
//!
//! Code that must run on a thread before it stops has to post the work and *observe* it, not post
//! it and quit.
TEST( ThreadTest, NoWorkIsPromisedAfterQuit )
{
    Thread thread( "quitting" );
    thread.start();

    std::atomic<bool> ran { false };
    static_cast<void>( thread.post( [&ran]()
        {
            ran.store( true );
        } ) );

    thread.quit();
    thread.wait();

    // Deliberately no assertion on `ran`: both outcomes are correct, and pinning either one would
    // be pinning a race. What is asserted is that the thread stopped and nothing came apart.
    EXPECT_TRUE( thread.isFinished() );
    static_cast<void>( ran.load() );
}

//! A Thread with no parent still works exactly as before.
//!
//! The parameter is defaulted and additive; this is the guard that says so.
TEST( ThreadTest, ThreadWithoutAParentIsUnowned )
{
    Thread thread( "unparented" );

    EXPECT_EQ( nullptr, thread.parent() );
    EXPECT_EQ( "unparented", thread.objectName() );
}

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

//! Tests that the constructor name is the object name, including the empty default.
//!
//! A Thread has one name and it is Object::objectName(). The constructor argument is a shorthand
//! for setObjectName() and not a second field beside it, which is what this pins down: before the
//! change that removed Thread::mName, a thread could answer two different questions with two
//! different answers depending on which getter was asked.
TEST( ThreadTest, NameIsWhatTheConstructorWasGiven )
{
    Thread named( "worker-with-a-name" );
    EXPECT_EQ( named.objectName(), "worker-with-a-name" );

    Thread unnamed;
    EXPECT_TRUE( unnamed.objectName().empty() )
        << "a thread constructed with no name reported one.";

    // Starting and finishing does not disturb it: nothing in the run body touches the name.
    named.start();
    ASSERT_TRUE( waitUntilRunning( named ) );
    EXPECT_EQ( named.objectName(), "worker-with-a-name" );
    named.quit();
    named.wait();
    EXPECT_EQ( named.objectName(), "worker-with-a-name" );
}

//! Tests that setObjectName() is what renames a thread.
//!
//! There is no Thread::setName() and no Thread::name() to disagree with it. Renaming through the
//! Object interface is the only way, and it has to work on a Thread exactly as on anything else.
TEST( ThreadTest, SetObjectNameRenamesAThread )
{
    Thread thread( "from-the-constructor" );
    ASSERT_EQ( thread.objectName(), "from-the-constructor" );

    thread.setObjectName( "renamed" );
    EXPECT_EQ( thread.objectName(), "renamed" );
}

//! Tests that an adopted thread takes the name the operating system already had for it.
//!
//! This is what makes a thread the library did not create identify itself. The thread exists
//! before QtLikeSignal ever sees it, and whoever created it has usually named it, so adoption reads
//! that name instead of labelling every adopted thread in the process "adopted".
//!
//! Skipped where the platform reports no thread name -- Windows before version 1607 -- rather than
//! failed, because the absence is the system's and not this library's.
TEST( ThreadTest, AnAdoptedThreadTakesItsNameFromTheOperatingSystem )
{
    std::atomic<bool> supported { false };
    std::string seenName;

    std::thread worker( [&supported, &seenName]()
        {
            Thread::setNativeName( "named-by-os" );
            supported.store( !Thread::nativeName().empty() );

            // Adoption happens inside currentThread(), and this is the first call that reaches it
            // on this thread -- so the name above is already in place when it looks.
            Thread* const adopted = Thread::currentThread();
            if( adopted != nullptr )
            {
                seenName = adopted->objectName();
            }
        } );
    worker.join();

    if( !supported.load() )
    {
        GTEST_SKIP() << "this platform reports no thread name, so there is nothing to adopt.";
    }

    EXPECT_EQ( seenName, "named-by-os" );
}

//! Tests that every adopted thread ends up with some name, whatever the platform reports.
//!
//! The weaker of the two halves, and deliberately so: what it pins is that no adopted thread is
//! ever nameless, which holds whether the name came from the operating system or from the
//! fallback. The fallback's own value is the next test's job.
TEST( ThreadTest, AnAdoptedThreadIsAlwaysNamed )
{
    std::string seenName;

    std::thread worker( [&seenName]()
        {
            Thread* const adopted = Thread::currentThread();
            if( adopted != nullptr )
            {
                seenName = adopted->objectName();
            }
        } );
    worker.join();

    EXPECT_FALSE( seenName.empty() )
        << "an adopted thread reported no name at all, so nothing in a log can identify it.";
}

//! Tests that an adopted thread the OS never named is called "adopted".
//!
//! **The name has to be cleared first, or this test does not reach the branch it is written for.**
//! Linux gives a new thread the creating thread's `comm`, which for a test binary is the
//! executable's own name -- so an ordinary std::thread here is already called "QtLikeSignal-test"
//! and the fallback never runs. Measured on 2026-09-08: without the clearing below this test saw
//! "QtLikeSignal-test" on Linux and "adopted" on Windows, and asserting only that the name was
//! non-empty passed in both cases and would have passed for any wrong value too.
//!
//! pthread_setname_np() is called directly rather than through Thread::setNativeName(), which
//! ignores an empty name on purpose: clearing a thread's name is not something the library should
//! offer, and it is only wanted here to force a state the platform will not otherwise produce.
//! Windows needs no such setup, because a thread has no description until somebody sets one.
//!
//! Skips rather than fails where the name cannot be emptied, so a platform that refuses is
//! reported as untested instead of broken.
TEST( ThreadTest, AnAdoptedThreadWithNoNativeNameIsCalledAdopted )
{
    std::string seenName;
    std::string nativeAfterClearing;

    std::thread worker( [&seenName, &nativeAfterClearing]()
        {
            #if defined( __linux__ )
                pthread_setname_np( pthread_self(), "" );
            #endif

            // Read before the adoption below, because that is the value the adoption will see.
            nativeAfterClearing = Thread::nativeName();

            Thread* const adopted = Thread::currentThread();
            if( adopted != nullptr )
            {
                seenName = adopted->objectName();
            }
        } );
    worker.join();

    if( !nativeAfterClearing.empty() )
    {
        GTEST_SKIP() << "this thread could not be left unnamed -- the OS still calls it \""
                     << nativeAfterClearing << "\" -- so the fallback was not reached.";
    }

    EXPECT_EQ( seenName, "adopted" )
        << "a thread the OS does not name has to get the fallback, and that exact fallback: an "
        "empty or unexpected name in a log reads as a fault in the log.";
}

//! Tests that a name given before start() reaches the operating system.
//!
//! The half of the contract `ps -L` reads. Checked from inside the thread rather than from
//! outside, because nativeName() is deliberately about the caller: there is no portable way to ask
//! another thread what it is called.
//!
//! **The name is deliberately under 16 characters.** Linux keeps a thread name in a fixed 16-byte
//! field, so a longer one comes back cut and this test would be asserting the truncation rule
//! rather than the delivery. That rule has a test of its own below.
TEST( ThreadTest, AThreadNameReachesTheOperatingSystem )
{
    //! A thread that reports, from inside itself, what the OS calls it.
    class NamingThread : public Thread
    {
    public:
        //! Constructs the thread with the name to give it and somewhere to report back to.
        NamingThread
            (
            const std::string& aName,       //!< Name to give the thread.
            std::atomic<bool>& aSupported,  //!< Set true if the platform reports any name.
            std::string& aSeen,             //!< Receives what the OS says this thread is called.
            std::mutex& aMutex              //!< Guards aSeen across the join.
            )
            : Thread( aName )
            , mSupported( aSupported )
            , mSeen( aSeen )
            , mMutex( aMutex )
        {
        }

    protected:
        //! Reads this thread's own name back from the operating system.
        void run() override
        {
            const std::string reported = Thread::nativeName();
            std::lock_guard<std::mutex> lock( mMutex );
            mSupported.store( !reported.empty() );
            mSeen = reported;
        }

    private:
        std::atomic<bool>& mSupported;   //!< True if the platform reports any name at all.
        std::string& mSeen;              //!< What the OS says this thread is called.
        std::mutex& mMutex;              //!< Guards mSeen.
    };

    std::atomic<bool> supported { false };
    std::string seenName;
    std::mutex seenMutex;

    NamingThread thread( "os-visible", supported, seenName, seenMutex );
    thread.start();
    thread.wait();

    if( !supported.load() )
    {
        GTEST_SKIP() << "this platform reports no thread name, so there is nothing to check.";
    }

    std::lock_guard<std::mutex> lock( seenMutex );
    EXPECT_EQ( seenName, "os-visible" );
}

//! Tests that a name too long for the platform is cut rather than dropped.
//!
//! Linux keeps 15 characters and pthread_setname_np() fails with ERANGE on a longer one, so a
//! caller that handed the string over untrimmed would end up with no name at all. Cutting it is
//! the deliberate choice, and this pins it down: what comes back is a prefix of what went in, and
//! it is not empty.
//!
//! Written as a prefix test rather than an exact one, so it holds on a platform with a different
//! limit or with none.
TEST( ThreadTest, AnOverlongThreadNameIsCutRatherThanRefused )
{
    const std::string longName = "a-name-far-longer-than-any-kernel-keeps";

    std::atomic<bool> supported { false };
    std::string seenName;

    std::thread worker( [&supported, &seenName, &longName]()
        {
            Thread::setNativeName( longName );
            seenName = Thread::nativeName();
            supported.store( !seenName.empty() );
        } );
    worker.join();

    if( !supported.load() )
    {
        GTEST_SKIP() << "this platform reports no thread name, so there is nothing to check.";
    }

    EXPECT_FALSE( seenName.empty() );
    EXPECT_EQ( longName.compare( 0, seenName.size(), seenName ), 0 )
        << "what the OS kept is not a prefix of the name given; it reported " << seenName;
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
            ASSERT_TRUE( worker.post( [&innerRan]()
                {
                    innerRan.store( true );
                } ) );
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

//! A post made in the window between start() and the loop coming up is delivered, not dropped.
//!
//! start() publishes isThreadRunning() before the OS thread exists, and the dispatcher is not
//! created until run() executes on that new thread. A post landing in between used to find no
//! dispatcher and be discarded outright -- Thread::post() returned false to a caller with no way to
//! retry, and the task simply never ran.
//!
//! It was not a narrow race. Posting immediately after start() lost the task on roughly two runs in
//! three of a win64-msvc release build, and the task stayed lost: instrumented, it had still not
//! been delivered thirty seconds later. What made it look like a flake rather than a dropped call
//! was that anything which happened to wait first -- a slower debug build, another test running
//! ahead of it -- closed the window before the post.
//!
//! Deliberately no waitUntilRunning() here: waiting is exactly what hides the defect. Repeated,
//! because a single pass can win the race by luck.
// See ObjectTest.DeepArgumentCopying_QueuedEventsMinimizeCopies, which is where this surfaced.
TEST( ThreadTest, PostImmediatelyAfterStartIsDeliveredNotDropped )
{
    constexpr int kAttempts = 20;
    for( int attempt = 0; attempt < kAttempts; ++attempt )
    {
        Thread worker;
        worker.start();

        std::mutex mutex;
        std::condition_variable cv;
        bool ran = false;

        EXPECT_TRUE( worker.post( [&]()
            {
                {
                    std::lock_guard<std::mutex> lock( mutex );
                    ran = true;
                }
                cv.notify_one();
            } ) ) << "post() refused the task on attempt " << attempt;

        {
            std::unique_lock<std::mutex> lock( mutex );
            EXPECT_TRUE( cv.wait_for( lock, std::chrono::seconds( 5 ), [&]
                {
                    return ran;
                } ) ) << "the task never ran on attempt " << attempt;
        }

        worker.quit();
        worker.wait();
    }
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
