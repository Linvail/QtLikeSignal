//! @file
//!
//! GoogleTest suite for the QtMimic framework (Object affinity/connections,
//! Thread event loops, and CoreApplication)
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic-test-types.hpp"

#include "CoreApplication.hpp"

#include "gtest/gtest.h"
#include <chrono>
#include <future>

namespace
{

    using namespace std::chrono_literals;
    using namespace QtMimic;

    /*----------------------------------------------------------
       Verify that the current thread is represented by a Thread.
       ----------------------------------------------------------*/
    TEST( ObjectTest, CurrentThreadIsAvailable )
    {
        EXPECT_NE( nullptr, Thread::current() );
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
        worker.join();
    }

    /*----------------------------------------------------------
       Verify queued slots are dropped safely when the receiver
       is destroyed before delivery.
       ----------------------------------------------------------*/
    TEST( ObjectTest, ReceiverDestroyedBeforeDeliveryNoCrash )
    {
        Thread worker( "worker2" );
        worker.start();

        std::atomic<int> invocations { 0 };

        // Synchronization primitives to block the worker.
        std::promise<void> releaseWorkerPromise;
        std::future<void> releaseWorkerFuture = releaseWorkerPromise.get_future();

        std::promise<void> workerBlockedPromise;
        std::future<void> workerBlockedFuture = workerBlockedPromise.get_future();

        // Post a task that will hang the worker's event loop.
        worker.post( [&]()
            {
                workerBlockedPromise.set_value(); // Tell main thread: "I am now blocked"
                releaseWorkerFuture.wait();   // Block here until main thread says go
            } );

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
        worker.post( [&]()
            {
                {
                    std::lock_guard<std::mutex> lock( barrierMutex );
                    barrierDone = true;
                }
                barrierCv.notify_one();
            } );
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
        worker.join();
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

        int before = c.mCount.load();
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

        std::thread::id workerId;
        std::mutex workerIdMutex;
        std::condition_variable workerIdCv;
        bool workerIdReady = false;

        worker.post( [&]()
            {
                {
                    std::lock_guard<std::mutex> lock( workerIdMutex );
                    workerId = std::this_thread::get_id();
                    workerIdReady = true;
                }
                workerIdCv.notify_one();
            } );

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
        worker.join();

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
        // Thread (the same object Thread::current() returns here). That thread has
        // no running exec() loop, so nothing drains its queue automatically; we pump
        // it by hand, which is where the deferred delete actually runs.
        Thread::current()->processEvents();

        EXPECT_TRUE( mainDtorDone );
        EXPECT_EQ( mainDtorCount.load(), 1 );
        EXPECT_EQ( mainDtorThread, std::this_thread::get_id() );
    }

    /*----------------------------------------------------------
       moveToThread(): mirror Qt6's contract. Events posted after a
       successful move are delivered to the new thread -- including
       through connections established BEFORE the move -- and the
       move is push-only: it returns false and changes nothing when
       the caller is not on the object's affinity thread.

       QtMimic has no object parent/child hierarchy and no timers, so
       the "children" and "timer reset" clauses of Qt's contract do
       not apply; the delivery-thread, return-value, and push-only
       clauses do.
       ----------------------------------------------------------*/

    //! Poll an atomic counter until it reaches 1 or a bounded number of tries
    //! elapses, so a broken delivery fails the test instead of hanging the suite.
    static void waitForOneDelivery
        (
        const std::atomic<int>& aCount
        )
    {
        for( int i = 0; i < 200 && aCount.load() == 0; ++i )
        {
            std::this_thread::sleep_for( 5ms );
        }
    }

    /*----------------------------------------------------------
       A valid push (from the object's own thread) redirects a
       connection made BEFORE the move to the new thread.
       ----------------------------------------------------------*/
    TEST( ObjectTest, MoveToThreadPushRedirectsExistingConnection )
    {
        Thread worker( "push-redirect-worker" );
        worker.start();

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
        worker.join();
    }

    /*----------------------------------------------------------
       Moving to the thread the object already lives in is a
       successful no-op that returns true (as in Qt6).
       ----------------------------------------------------------*/
    TEST( ObjectTest, MoveToThreadReturnsTrueWhenAlreadyInTargetThread )
    {
        Consumer c; // lives in this thread

        EXPECT_TRUE( c.moveToThread( Thread::current() ) );
        EXPECT_EQ( c.thread(), Thread::current() );
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
        other.start();

        // c's affinity is the worker, but we call moveToThread() from this thread.
        Consumer c( &worker );

        EXPECT_FALSE( c.moveToThread( &other ) );
        EXPECT_EQ( c.thread(), &worker ) << "a refused move must not change affinity";

        EXPECT_FALSE( c.moveToThread( nullptr ) );
        EXPECT_EQ( c.thread(), &worker );

        worker.quit();
        worker.join();
        other.quit();
        other.join();
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
        EXPECT_TRUE( c.moveToThread( Thread::current() ) );
        EXPECT_EQ( c.thread(), Thread::current() );
    }

    /*----------------------------------------------------------
       Pulling a thread-less object to a thread that is NOT the
       caller is still refused (only the caller is exempt).
       ----------------------------------------------------------*/
    TEST( ObjectTest, MoveToThreadRefusesNoAffinityMoveToOtherThread )
    {
        Thread other( "no-affinity-other-worker" );
        other.start();

        Consumer c;
        ASSERT_TRUE( c.moveToThread( nullptr ) ); // now thread-less
        EXPECT_EQ( c.thread(), nullptr );

        EXPECT_FALSE( c.moveToThread( &other ) );
        EXPECT_EQ( c.thread(), nullptr );

        other.quit();
        other.join();
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
        workerC.start();

        Consumer c; // lives in this thread
        Producer p;
        Object::connect( p.produced, &c, &Consumer::onProduced );

        // First push (main -> B) is valid from this thread.
        ASSERT_TRUE( c.moveToThread( &workerB ) );

        // Second push (B -> C) must run ON B, since c now lives there. Doing it from
        // here would be a pull and be refused.
        std::promise<bool> movedPromise;
        std::future<bool> movedFuture = movedPromise.get_future();
        workerB.post( [&]()
            {
                movedPromise.set_value( c.moveToThread( &workerC ) );
            } );
        EXPECT_TRUE( movedFuture.get() );
        EXPECT_EQ( c.thread(), &workerC );

        p.produced.emit( 11 );
        waitForOneDelivery( c.mCount );

        EXPECT_EQ( c.mCount.load(), 1 );
        EXPECT_EQ( c.mLast.load(), 11 );
        EXPECT_EQ( c.mSlotThread, workerC.id() );
        EXPECT_NE( c.mSlotThread, workerB.id() );

        workerB.quit();
        workerB.join();
        workerC.quit();
        workerC.join();
    }

    /*----------------------------------------------------------
       Verify CoreApplication executes posted work on the main
       thread and exits with code 0 when quit is posted.
       ----------------------------------------------------------*/
    TEST( CoreApplicationTest, PostAndExec )
    {
        int argc = 1;
        char arg0[] = "core-application-test";
        char* argv[] = { arg0, nullptr };

        CoreApplication app( argc, argv );
        EXPECT_EQ( CoreApplication::instance(), &app );

        std::thread::id mainId = std::this_thread::get_id();
        std::thread::id taskId;

        CoreApplication::post( [&]()
            {
                taskId = std::this_thread::get_id();
                CoreApplication::quit();
            } );

        int rc = app.exec();
        EXPECT_EQ( rc, 0 );
        EXPECT_EQ( taskId, mainId );
    }

    /*----------------------------------------------------------
       Verify Thread lifecycle signals are emitted for both
       startup and shutdown transitions.
       ----------------------------------------------------------*/
    TEST( ThreadTest, StartedAndFinishedSignalsAreEmitted )
    {
        Thread worker( "thread-lifecycle-worker" );
        Thread localThread;
        Object local( &localThread ); // context for the signal connections

        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool finished = false;

        localThread.start();

        // Connect BEFORE start(): the started signal is emitted from inside the
        // worker's loop the moment it begins, so a connection made afterwards could
        // miss it.
        auto startedConnection = Object::connect( worker.getStarted(), &local, [&]()
            {
                std::lock_guard<std::mutex> locker( mutex );
                started = true;
                cv.notify_all();
            } );

        auto finishedConnection = Object::connect( worker.getFinished(), &local, [&]()
            {
                std::lock_guard<std::mutex> locker( mutex );
                finished = true;
                cv.notify_all();
            } );

        worker.start();

        {
            std::unique_lock<std::mutex> locker( mutex );
            EXPECT_TRUE( cv.wait_for( locker, 2s, [&]
                {
                    return started;
                } ) );
        }

        worker.quit();
        worker.join();

        {
            std::unique_lock<std::mutex> locker( mutex );
            EXPECT_TRUE( cv.wait_for( locker, 2s, [&]
                {
                    return finished;
                } ) );
        }

        startedConnection.disconnect();
        finishedConnection.disconnect();

        localThread.quit();
        localThread.join();
    }

} // namespace
