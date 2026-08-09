//! @file
//!
//! GoogleTest suite for QtMimic::Timer and the Object/ThreadData timer plumbing behind it
//! (Object::startTimer()/killTimer()/timerEvent(), the timer list in ThreadData, and the event
//! loop's timer-aware wait).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Object.hpp"
#include "Thread.hpp"
#include "Timer.hpp"
#include "TimerEvent.hpp"

#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using namespace QtMimic;

    //! How long a test waits for something that should happen within a few timer intervals. Long
    //! enough that a loaded CI box does not fail spuriously, short enough that a genuine hang is
    //! still a test failure rather than a timeout at a higher level.
    constexpr auto kPatience = 5s;

    //! Posts a task and blocks until it has run, which can only happen once the thread's loop is
    //! actually draining its mailbox. QtMimic::Thread exposes no isRunning(), so a round-trip
    //! through post() is the idiomatic way to know the loop is up -- used throughout this suite.
    //! @return true if the thread drained the marker task in time.
    bool waitUntilRunning
        (
        Thread& aThread  //!< The thread to wait for.
        )
    {
        std::promise<void> running;
        auto runningFuture = running.get_future();
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while( !aThread.post( [&running]()
            {
                running.set_value();
            } ) )
        {
            if( std::chrono::steady_clock::now() > deadline )
            {
                return false;
            }
            std::this_thread::yield();
        }
        return runningFuture.wait_for( kPatience ) == std::future_status::ready;
    }

    //! Runs @p aBody on @p aThread and blocks until it has finished. Timer start()/stop() are
    //! thread-confined, so nearly every test here needs to get onto the worker before touching one.
    template <typename Body> void runOnThread
        (
        Thread& aThread,  //!< The thread to run on.
        Body aBody        //!< The work to run there.
        )
    {
        std::promise<void> done;
        auto doneFuture = done.get_future();
        ASSERT_TRUE( aThread.post( [&aBody, &done]()
            {
                aBody();
                done.set_value();
            } ) ) << "worker refused the task.";
        ASSERT_EQ( doneFuture.wait_for( kPatience ), std::future_status::ready )
            << "worker never ran the task.";
    }

    //! Blocks until everything currently queued on @p aThread has been drained.
    //!
    //! Needed before quitting a worker that may still hold a single-shot helper's own
    //! deleteLater(): quitting while that delete is queued strands the object, which the leak
    //! checker then reports.
    void drainQueuedTasks
        (
        Thread& aThread  //!< The thread to drain.
        )
    {
        std::promise<void> drained;
        auto drainedFuture = drained.get_future();
        if( !aThread.post( [&drained]()
            {
                drained.set_value();
            } ) )
        {
            return;  // loop already stopped, so there is nothing left to drain
        }
        EXPECT_EQ( drainedFuture.wait_for( kPatience ), std::future_status::ready )
            << "worker event loop did not drain.";
    }

    //! Counts how often it is timed out, and on which thread.
    class CountingReceiver : public Object
    {
    public:
        explicit CountingReceiver
            (
            Thread* aThread = nullptr
            )
            : Object( aThread )
        {
        }

        //! Slot for Timer::timeout, and target for the singleShot member-function overload.
        void onTimeout()
        {
            mFiringThread.store( Thread::current() );
            mFireCount.fetch_add( 1 );
        }

        //! @return how many times onTimeout() has run.
        int fireCount() const
        {
            return mFireCount.load();
        }

        //! @return the thread onTimeout() last ran on, or nullptr if it never has.
        Thread* firingThread() const
        {
            return mFiringThread.load();
        }

    private:
        // Atomic because the test thread polls these while the worker writes them.
        std::atomic<int> mFireCount { 0 };
        std::atomic<Thread*> mFiringThread { nullptr };
    };

    //! Records the timer ids delivered to it, for the raw Object::startTimer() tests.
    class RecordingObject : public Object
    {
    public:
        explicit RecordingObject
            (
            Thread* aThread = nullptr
            )
            : Object( aThread )
        {
        }

        //! @return how many expiries have been delivered for @p aTimerId.
        int countFor
            (
            int aTimerId  //!< The timer id to look up.
            ) const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            int count = 0;
            for( const int id : mDelivered )
            {
                count += ( id == aTimerId ) ? 1 : 0;
            }
            return count;
        }

        //! @return the total number of expiries delivered.
        std::size_t total() const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            return mDelivered.size();
        }

        //! Runs on the next expiry, before it is recorded. Set from the timer's own thread.
        std::function<void( int aTimerId )> mOnTimer;

    protected:
        virtual void timerEvent
            (
            TimerEvent* aEvent
            ) override
        {
            if( mOnTimer )
            {
                mOnTimer( aEvent->timerId() );
            }
            std::lock_guard<std::mutex> locker( mMutex );
            mDelivered.push_back( aEvent->timerId() );
        }

    private:
        mutable std::mutex mMutex;
        std::vector<int> mDelivered;
    };

    //! Takes queued slot invocations and timer expiries on the same object at the same time, and
    //! records enough about them to show that neither kind was lost, reordered, overlapped, or run
    //! on the wrong thread.
    class MixedLoadReceiver : public Object
    {
    public:
        explicit MixedLoadReceiver
            (
            Thread* aThread  //!< The thread this receiver lives in.
            )
            : Object( aThread )
            , mExpectedThread( aThread )
        {
        }

        //! Queued slot: one metacall carrying emitter @p aSender's @p aSequence counter.
        void onMetaCall
            (
            int aSender,   //!< Which emitter sent it.
            int aSequence  //!< Its position in that emitter's stream, counting from 0.
            )
        {
            const Marker marker( *this );

            // Burn the configured amount of wall-clock time before recording anything, so a test
            // can make the worker the bottleneck and keep its mailbox genuinely saturated.
            const auto workUntil = std::chrono::steady_clock::now()
                + std::chrono::microseconds( mWorkMicros.load() );
            while( std::chrono::steady_clock::now() < workUntil )
            {
            }

            std::lock_guard<std::mutex> locker( mMutex );

            // Each emitter counts up from 0 and posts in program order under the mailbox mutex, so
            // anything other than the next value means a metacall was lost, duplicated or
            // reordered.
            int& expected = mNextExpected[aSender];
            if( aSequence != expected )
            {
                ++mOutOfOrder;
            }
            expected = aSequence + 1;
            ++mMetaCalls;

            // Sampled on every metacall, so after the run it holds the count as of the LAST one --
            // i.e. how many expiries got through while there was still queued work outstanding.
            mTimerFiresAtLastMetaCall = mTimerFires.load();
        }

        //! Make each metacall take @p aMicros of wall-clock time. Set before the load starts.
        void setMetaCallWorkMicros
            (
            int aMicros  //!< Microseconds to spend in each metacall.
            )
        {
            mWorkMicros.store( aMicros );
        }

        //! @return the timer count as of the last metacall delivered.
        int timerFiresAtLastMetaCall() const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            return mTimerFiresAtLastMetaCall;
        }

        //! @return how many metacalls have been delivered.
        int metaCalls() const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            return mMetaCalls;
        }

        //! @return how many metacalls arrived out of their emitter's order.
        int outOfOrder() const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            return mOutOfOrder;
        }

        //! @return how many timer expiries have been delivered.
        int timerFires() const
        {
            return mTimerFires.load();
        }

        //! @return true if two handlers were ever running at once.
        bool overlapped() const
        {
            return mOverlapped.load();
        }

        //! @return true if any handler ran somewhere other than this object's thread.
        bool ranOnWrongThread() const
        {
            return mWrongThread.load();
        }

    protected:
        virtual void timerEvent
            (
            TimerEvent* aEvent
            ) override
        {
            ( void )aEvent;
            const Marker marker( *this );
            mTimerFires.fetch_add( 1 );
        }

    private:
        //! Marks a handler as running for as long as it is on the stack.
        //!
        //! Everything the loop runs -- posted tasks, queued slots and timer expiries alike -- is
        //! serialized on the one thread, so the depth here must never exceed 1. Worth checking
        //! explicitly now that the timer list shares its mutex and its loop pass with the mailbox.
        struct Marker
        {
            explicit Marker
                (
                MixedLoadReceiver& aOwner
                )
                : mOwner( aOwner )
            {
                if( mOwner.mDepth.fetch_add( 1 ) != 0 )
                {
                    mOwner.mOverlapped.store( true );
                }
                if( Thread::current() != mOwner.mExpectedThread )
                {
                    mOwner.mWrongThread.store( true );
                }
            }

            ~Marker()
            {
                mOwner.mDepth.fetch_sub( 1 );
            }

            MixedLoadReceiver& mOwner;
        };

        Thread* const mExpectedThread;          //!< The only thread any handler may run on.
        std::atomic<int> mWorkMicros { 0 };     //!< Wall-clock time each metacall must take.
        mutable std::mutex mMutex;              //!< Guards the four counters below.
        std::map<int, int> mNextExpected;       //!< Per emitter, the sequence number due next.
        int mMetaCalls { 0 };                   //!< Total metacalls delivered.
        int mOutOfOrder { 0 };                  //!< Metacalls that broke their emitter's order.
        int mTimerFiresAtLastMetaCall { 0 };    //!< Timer count as of the most recent metacall.
        std::atomic<int> mTimerFires { 0 };     //!< Total timer expiries delivered.
        std::atomic<int> mDepth { 0 };          //!< Handlers currently on the stack.
        std::atomic<bool> mOverlapped { false };  //!< Set if mDepth ever exceeded 1.
        std::atomic<bool> mWrongThread { false }; //!< Set if a handler ran off mExpectedThread.
    };

    //! Exposes the protected timerEvent() so a synthesized TimerEvent can drive it directly.
    class ManualTimer : public Timer
    {
    public:
        //! Invokes the protected handler, as the event loop would.
        void deliver
            (
            TimerEvent* aEvent  //!< The expiry to deliver.
            )
        {
            timerEvent( aEvent );
        }

    };

    //! Spins until @p aPredicate holds or kPatience runs out.
    //! @return the final value of the predicate.
    template <typename Predicate> bool waitFor
        (
        Predicate aPredicate  //!< Condition to wait for.
        )
    {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while( !aPredicate() )
        {
            if( std::chrono::steady_clock::now() > deadline )
            {
                return false;
            }
            std::this_thread::sleep_for( 1ms );
        }
        return true;
    }

    //================================================================
    // Timer configuration
    //================================================================

    //! Property accessors report what was set, and an unstarted timer is inactive with no id.
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

    //! start() makes the timer active with a positive id, stop() takes it back to inactive/-1.
    TEST( TimerTest, StartAndStopTrackState )
    {
        Thread worker( "timer-state" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        runOnThread( worker, [&worker]()
            {
                Timer timer( &worker );
                EXPECT_FALSE( timer.isActive() );

                timer.start( 150 );
                EXPECT_EQ( timer.interval(), 150 );
                EXPECT_TRUE( timer.isActive() );
                EXPECT_GT( timer.timerId(), 0 );

                timer.stop();
                EXPECT_FALSE( timer.isActive() );
                EXPECT_EQ( timer.timerId(), -1 );
            } );

        worker.quit();
        worker.join();
    }

    //! A repeating timer keeps emitting timeout until it is stopped.
    TEST( TimerTest, RepeatingTimerFiresRepeatedly )
    {
        Thread worker( "timer-repeat" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        CountingReceiver receiver( &worker );
        Timer* timer = nullptr;

        runOnThread( worker, [&]()
            {
                timer = new Timer( &worker );
                Object::connect( timer->getTimeout(), &receiver, &CountingReceiver::onTimeout );
                timer->start( 5 );
            } );

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.fireCount() >= 3;
            } ) ) << "repeating timer fired " << receiver.fireCount() << " times, expected 3+.";
        EXPECT_EQ( receiver.firingThread(), &worker ) << "timeout was emitted on the wrong thread.";

        runOnThread( worker, [&timer]()
            {
                timer->stop();
                delete timer;
            } );

        worker.quit();
        worker.join();
    }

    //! A single-shot timer fires exactly once and reports itself inactive afterwards.
    TEST( TimerTest, SingleShotTimerFiresOnce )
    {
        Thread worker( "timer-single" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        CountingReceiver receiver( &worker );
        Timer* timer = nullptr;

        runOnThread( worker, [&]()
            {
                timer = new Timer( &worker );
                timer->setSingleShot( true );
                Object::connect( timer->getTimeout(), &receiver, &CountingReceiver::onTimeout );
                timer->start( 5 );
            } );

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.fireCount() >= 1;
            } ) ) << "single-shot timer never fired.";

        // Long enough that a repeating timer of this interval would have fired many more times.
        std::this_thread::sleep_for( 60ms );
        EXPECT_EQ( receiver.fireCount(), 1 ) << "single-shot timer fired more than once.";

        runOnThread( worker, [&timer]()
            {
                EXPECT_FALSE( timer->isActive() ) << "single-shot timer still active after firing.";
                delete timer;
            } );

        worker.quit();
        worker.join();
    }

    //! stop() actually stops it: no further timeout is emitted.
    TEST( TimerTest, StoppedTimerStopsFiring )
    {
        Thread worker( "timer-stop" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        CountingReceiver receiver( &worker );
        Timer* timer = nullptr;

        runOnThread( worker, [&]()
            {
                timer = new Timer( &worker );
                Object::connect( timer->getTimeout(), &receiver, &CountingReceiver::onTimeout );
                timer->start( 5 );
            } );

        ASSERT_TRUE( waitFor( [&receiver]()
            {
                return receiver.fireCount() >= 1;
            } ) );

        int countAtStop = 0;
        runOnThread( worker, [&]()
            {
                timer->stop();
                countAtStop = receiver.fireCount();
            } );

        std::this_thread::sleep_for( 60ms );
        EXPECT_EQ( receiver.fireCount(), countAtStop ) << "timer fired after stop().";

        runOnThread( worker, [&timer]()
            {
                delete timer;
            } );

        worker.quit();
        worker.join();
    }

    //! timerEvent() emits timeout for its own id and ignores any other, without an event loop
    //! involved at all.
    TEST( TimerTest, TimerEventIgnoresForeignIds )
    {
        Thread worker( "timer-manual" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        runOnThread( worker, [&worker]()
            {
                ManualTimer timer;
                timer.start( 10000 );  // long enough that the loop will never deliver it itself
                const int id = timer.timerId();
                ASSERT_GT( id, 0 );

                Object context( &worker );
                int emitted = 0;
                Object::connect( timer.getTimeout(), &context, [&emitted]()
                {
                    ++emitted;
                }, ConnectionType::Direct );

                TimerEvent foreign( id + 1000 );
                timer.deliver( &foreign );
                EXPECT_EQ( emitted, 0 ) << "timeout emitted for another timer's id.";

                TimerEvent own( id );
                timer.deliver( &own );
                EXPECT_EQ( emitted, 1 ) << "timeout not emitted for the timer's own id.";

                timer.stop();
            } );

        worker.quit();
        worker.join();
    }

    //================================================================
    // Object::startTimer()/killTimer()
    //================================================================

    //! startTimer() hands out distinct positive ids, and each one is delivered to timerEvent().
    TEST( ObjectTimerTest, ConcurrentTimersDeliverDistinctIds )
    {
        Thread worker( "obj-timer-ids" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );
        int idA = -1;
        int idB = -1;

        runOnThread( worker, [&]()
            {
                idA = receiver.startTimer( 5 );
                idB = receiver.startTimer( 5 );
            } );

        EXPECT_GT( idA, 0 );
        EXPECT_GT( idB, 0 );
        EXPECT_NE( idA, idB ) << "two live timers were given the same id.";

        EXPECT_TRUE( waitFor( [&]()
            {
                return receiver.countFor( idA ) >= 1 && receiver.countFor( idB ) >= 1;
            } ) ) << "not every timer was delivered.";

        runOnThread( worker, [&]()
            {
                receiver.killTimer( idA );
                receiver.killTimer( idB );
            } );

        worker.quit();
        worker.join();
    }

    //! startTimer() from a thread other than the object's own is refused, as in Qt.
    TEST( ObjectTimerTest, StartTimerFromForeignThreadIsRefused )
    {
        Thread worker( "obj-timer-foreign" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );

        // This test's own thread is not the worker, so this must be rejected rather than silently
        // installing a timer whose events this thread is not positioned to receive.
        EXPECT_EQ( receiver.startTimer( 5 ), -1 );

        std::this_thread::sleep_for( 40ms );
        EXPECT_EQ( receiver.total(), 0u ) << "a refused timer was delivered anyway.";

        worker.quit();
        worker.join();
    }

    //! An object detached with moveToThread(nullptr) has no mailbox to schedule against.
    TEST( ObjectTimerTest, StartTimerWithoutAThreadIsRefused )
    {
        RecordingObject receiver;
        ASSERT_TRUE( receiver.moveToThread( nullptr ) );
        EXPECT_EQ( receiver.startTimer( 5 ), -1 );
    }

    //! A timer killed from inside a sibling's handler does not still fire in that same batch.
    //!
    //! Both timers come due together and are collected into one batch before any of them is
    //! delivered, so this only works if killTimer() reaches into the batch being dispatched.
    TEST( ObjectTimerTest, KillFromHandlerCancelsSiblingInSameBatch )
    {
        Thread worker( "obj-timer-batch" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );
        std::atomic<int> idA { -1 };
        std::atomic<int> idB { -1 };

        // Whichever of the two is delivered first kills both: the sibling, which is the entry
        // already sitting in this batch behind us, and itself, so that these repeating timers
        // cannot come due again and confuse the count below with a second round.
        receiver.mOnTimer = [&receiver, &idA, &idB]( int )
            {
                receiver.killTimer( idA.load() );
                receiver.killTimer( idB.load() );
            };

        runOnThread( worker, [&]()
            {
                // Registered back to back with the same interval, so they come due in the same pass.
                idA.store( receiver.startTimer( 20 ) );
                idB.store( receiver.startTimer( 20 ) );
            } );

        ASSERT_TRUE( waitFor( [&receiver]()
            {
                return receiver.total() >= 1;
            } ) ) << "neither timer was delivered.";

        std::this_thread::sleep_for( 80ms );
        EXPECT_EQ( receiver.total(), 1u )
            << "the killed sibling was delivered anyway (" << receiver.total() << " deliveries).";

        worker.quit();
        worker.join();

        // Cleared only once the loop is joined, so nothing can be reading it.
        receiver.mOnTimer = nullptr;
    }

    //! Destroying an object with a running timer stops it, rather than leaving the loop to call
    //! into freed memory.
    TEST( ObjectTimerTest, DestroyingReceiverStopsItsTimers )
    {
        Thread worker( "obj-timer-destroy" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject* receiver = nullptr;
        runOnThread( worker, [&]()
            {
                receiver = new RecordingObject( &worker );
                receiver->startTimer( 5 );
            } );

        ASSERT_TRUE( waitFor( [&receiver]()
            {
                return receiver->total() >= 1;
            } ) );

        runOnThread( worker, [&receiver]()
            {
                delete receiver;  // must strip the registration on the way out
            } );

        // Nothing to assert beyond surviving: a stale registration would have the loop calling
        // timerEvent() on freed memory, which the sanitizer build reports.
        std::this_thread::sleep_for( 60ms );

        worker.quit();
        worker.join();
    }

    //! moveToThread() carries a running timer to the destination, keeping its id, and stops
    //! delivering it on the thread the object left. Qt documents exactly this.
    TEST( ObjectTimerTest, MoveToThreadCarriesRunningTimers )
    {
        Thread source( "obj-timer-src" );
        Thread destination( "obj-timer-dst" );
        source.start();
        destination.start();
        ASSERT_TRUE( waitUntilRunning( source ) );
        ASSERT_TRUE( waitUntilRunning( destination ) );

        CountingReceiver receiver( &source );
        Timer* timer = nullptr;

        runOnThread( source, [&]()
            {
                timer = new Timer( &source );
                Object::connect( timer->getTimeout(), &receiver, &CountingReceiver::onTimeout,
                ConnectionType::Direct );
                timer->start( 5 );
            } );

        ASSERT_TRUE( waitFor( [&receiver]()
            {
                return receiver.fireCount() >= 1;
            } ) );
        ASSERT_EQ( receiver.firingThread(), &source );

        int idBeforeMove = -1;
        runOnThread( source, [&]()
            {
                idBeforeMove = timer->timerId();
                // Push-only, so the move has to be made from the thread the object is leaving.
                ASSERT_TRUE( timer->moveToThread( &destination ) );
            } );

        EXPECT_TRUE( waitFor( [&receiver, &destination]()
            {
                return receiver.firingThread() == &destination;
            } ) ) << "the timer never resumed on the destination thread.";

        runOnThread( destination, [&]()
            {
                EXPECT_EQ( timer->timerId(), idBeforeMove )
                    << "the timer id changed across the move, so cached ids would stop matching.";
                timer->stop();
                delete timer;
            } );

        source.quit();
        source.join();
        destination.quit();
        destination.join();
    }

    //================================================================
    // Timer::singleShot()
    //================================================================

    //! singleShot(int, Functor) runs its functor on the calling thread's loop.
    TEST( TimerSingleShotTest, PlainFunctorRuns )
    {
        Thread worker( "single-plain" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        std::promise<Thread*> fired;
        auto firedFuture = fired.get_future();

        ASSERT_TRUE( worker.post( [&fired]()
            {
                Timer::singleShot( 5, [&fired]()
                {
                    fired.set_value( Thread::current() );
                } );
            } ) );

        ASSERT_EQ( firedFuture.wait_for( kPatience ), std::future_status::ready )
            << "singleShot(int, Functor) never ran its functor.";
        EXPECT_EQ( firedFuture.get(), &worker ) << "the functor ran on the wrong thread.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.join();
    }

    //! singleShot(int, context, Functor) hops to the context's thread and runs there exactly once.
    TEST( TimerSingleShotTest, ContextFunctorRunsOnContextThread )
    {
        Thread worker( "single-context" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        Object context( &worker );

        std::promise<Thread*> fired;
        auto firedFuture = fired.get_future();
        std::atomic<int> runs { 0 };

        // Called from this test's thread, not the worker's, so this exercises the cross-thread hop.
        Timer::singleShot( 5, &context, [&fired, &runs]()
            {
                if( runs.fetch_add( 1 ) == 0 )
                {
                    fired.set_value( Thread::current() );
                }
            } );

        Object* nullContext = nullptr;
        Timer::singleShot( 5, nullContext, []()
            {
            } );

        ASSERT_EQ( firedFuture.wait_for( kPatience ), std::future_status::ready )
            << "singleShot(int, context, Functor) never ran its functor.";
        EXPECT_EQ( firedFuture.get(), &worker ) << "the functor did not run on the context thread.";

        std::this_thread::sleep_for( 60ms );
        EXPECT_EQ( runs.load(), 1 ) << "the single shot ran more than once.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.join();
    }

    //! singleShot(int, receiver, MemberFunc) calls the member function once, on the receiver's
    //! thread, and tolerates a null receiver.
    TEST( TimerSingleShotTest, MemberFunctionRunsOnce )
    {
        Thread worker( "single-member" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        CountingReceiver receiver( &worker );

        CountingReceiver* nullReceiver = nullptr;
        Timer::singleShot( 5, nullReceiver, &CountingReceiver::onTimeout );

        Timer::singleShot( 5, &receiver, &CountingReceiver::onTimeout );

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.fireCount() >= 1;
            } ) ) << "singleShot(int, receiver, MemberFunc) never called the member function.";
        EXPECT_EQ( receiver.firingThread(), &worker ) << "it ran on the wrong thread.";

        std::this_thread::sleep_for( 60ms );
        EXPECT_EQ( receiver.fireCount(), 1 ) << "the single shot fired more than once.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.join();
    }

    //! A single shot aimed at a thread whose loop has already stopped is dropped, not leaked and
    //! not run somewhere else.
    TEST( TimerSingleShotTest, ContextOnStoppedThreadIsDropped )
    {
        Thread worker( "single-stopped" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        Object context( &worker );

        worker.quit();
        worker.join();

        std::atomic<int> runs { 0 };
        Timer::singleShot( 5, &context, [&runs]()
            {
                runs.fetch_add( 1 );
            } );

        std::this_thread::sleep_for( 40ms );
        EXPECT_EQ( runs.load(), 0 ) << "the functor ran even though its thread had stopped.";
    }

    //================================================================
    // Event loop interaction
    //================================================================

    //! The loop wakes for a timer deadline on its own, with nothing posted to it.
    //!
    //! The wait is otherwise unbounded, so a timer that did not shorten it would never be delivered
    //! at all -- this is what the timeout plumbed through loop() buys.
    TEST( ThreadTimerTest, IdleLoopWakesForTimerDeadline )
    {
        Thread worker( "loop-idle" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );
        int id = -1;
        runOnThread( worker, [&]()
            {
                id = receiver.startTimer( 30 );
            } );
        ASSERT_GT( id, 0 );

        // Nothing is posted from here on, so only the timer deadline can end the loop's wait.
        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.total() >= 2;
            } ) ) << "the idle loop did not wake for its timer.";

        runOnThread( worker, [&]()
            {
                receiver.killTimer( id );
            } );

        worker.quit();
        worker.join();
    }

    //! A timer registered while the loop is already asleep on a longer wait still fires on time.
    //!
    //! Covers the mTimersChanged handshake: without it the loop would sleep out the deadline it
    //! computed before the timer existed.
    TEST( ThreadTimerTest, TimerAddedWhileLoopSleepsIsHonoured )
    {
        Thread worker( "loop-resleep" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );

        // Let the loop settle into its unbounded wait before anything is scheduled.
        std::this_thread::sleep_for( 30ms );

        int id = -1;
        const auto armed = std::chrono::steady_clock::now();
        runOnThread( worker, [&]()
            {
                id = receiver.startTimer( 20 );
            } );

        ASSERT_TRUE( waitFor( [&receiver]()
            {
                return receiver.total() >= 1;
            } ) ) << "a timer added to a sleeping loop never fired.";

        const auto elapsed = std::chrono::steady_clock::now() - armed;
        EXPECT_LT( elapsed, 2s ) << "the timer fired, but far too late to have been scheduled.";

        runOnThread( worker, [&]()
            {
                receiver.killTimer( id );
            } );

        worker.quit();
        worker.join();
    }

    //! Thread::processEvents() services timers too, so an adopted thread pumping from its own loop
    //! is not the one place where timers silently never fire.
    TEST( ThreadTimerTest, ProcessEventsDeliversTimers )
    {
        // Adopts this test's thread; no loop of its own, we pump it by hand below.
        Thread* self = Thread::current();
        ASSERT_NE( self, nullptr );

        RecordingObject receiver( self );
        const int id = receiver.startTimer( 5 );
        ASSERT_GT( id, 0 );

        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while( receiver.total() < 1 && std::chrono::steady_clock::now() < deadline )
        {
            std::this_thread::sleep_for( 1ms );
            self->processEvents();
        }

        EXPECT_GE( receiver.countFor( id ), 1 ) << "processEvents() never delivered the timer.";
        receiver.killTimer( id );
    }

    //================================================================
    // Metacalls and timers at the same time
    //================================================================

    //! A thread carrying metacalls and timers simultaneously loses neither, and keeps them ordered.
    //!
    //! Timers and the mailbox now share one mutex, one condition variable and one pass of the loop,
    //! so the two can interfere in ways neither can on its own: a timer batch collected mid-pass
    //! could displace queued work, a saturated mailbox could starve the timers, and either could in
    //! principle be delivered from the wrong place. Four threads emit into one queued connection
    //! while three timers of unrelated intervals run on the same object, so expiries land both
    //! alone and alongside metacalls.
    TEST( ThreadTimerTest, MetaCallsAndTimersInterleaveWithoutLoss )
    {
        constexpr int kEmitters = 4;
        constexpr int kPerEmitter = 250;
        constexpr int kExpectedMetaCalls = kEmitters * kPerEmitter;

        Thread worker( "mixed-load" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        MixedLoadReceiver receiver( &worker );

        Signal<int, int> metaCall;
        Object::connect( metaCall, &receiver, &MixedLoadReceiver::onMetaCall,
            ConnectionType::Queued );

        // Intervals that do not divide into one another, so the three come due in varying
        // combinations rather than always as one batch.
        std::vector<int> timerIds;
        runOnThread( worker, [&]()
            {
                timerIds.push_back( receiver.startTimer( 1 ) );
                timerIds.push_back( receiver.startTimer( 3 ) );
                timerIds.push_back( receiver.startTimer( 7 ) );
            } );
        for( const int id : timerIds )
        {
            ASSERT_GT( id, 0 );
        }

        std::vector<std::thread> emitters;
        for( int sender = 0; sender < kEmitters; ++sender )
        {
            emitters.emplace_back( [&metaCall, sender]()
                {
                    for( int sequence = 0; sequence < kPerEmitter; ++sequence )
                    {
                        metaCall.emit( sender, sequence );
                    }
                } );
        }
        for( auto& emitter : emitters )
        {
            emitter.join();
        }

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.metaCalls() >= kExpectedMetaCalls;
            } ) ) << "only " << receiver.metaCalls() << " of " << kExpectedMetaCalls
                  << " metacalls were delivered.";

        EXPECT_EQ( receiver.metaCalls(), kExpectedMetaCalls )
            << "the mailbox delivered a metacall more than once.";
        EXPECT_EQ( receiver.outOfOrder(), 0 )
            << "metacalls from one emitter arrived out of the order it sent them.";
        EXPECT_GT( receiver.timerFires(), 0 )
            << "the timers were starved for the whole run.";
        EXPECT_FALSE( receiver.overlapped() )
            << "a timer expiry and a metacall ran at the same time.";
        EXPECT_FALSE( receiver.ranOnWrongThread() )
            << "something was delivered off the receiver's own thread.";

        runOnThread( worker, [&]()
            {
                for( const int id : timerIds )
                {
                    receiver.killTimer( id );
                }
            } );

        worker.quit();
        worker.join();
    }

    //! Timers keep firing while the mailbox never empties, not only once it has drained.
    //!
    //! A different claim from the test above, and the one that catches real starvation: a loop that
    //! serviced its timers only on passes with nothing queued would still deliver every metacall,
    //! and would still fire its timers -- just never while there was work outstanding.
    //!
    //! Note what is *not* claimed. One pass of the loop takes the whole mailbox in a single swap and
    //! runs it before looking at the timers, so a timer cannot interleave *within* a batch -- queue
    //! 400 slow metacalls in one go and exactly one expiry gets through, however long the batch
    //! takes. That is the same granularity Qt has (sendPostedEvents drains the list, then timers are
    //! processed) and what QtLikeSignal's dispatcher does. The guarantee is per pass, so the load
    //! here arrives in rounds: each round is queued while the previous is still being chewed, which
    //! keeps every pass non-empty while still giving the loop many passes to be measured over.
    TEST( ThreadTimerTest, TimersKeepFiringWhileMailboxNeverEmpties )
    {
        constexpr int kRounds = 60;
        constexpr int kPerRound = 10;
        constexpr int kWorkMicros = 200;  // 2 ms of work per round, fed every 1 ms

        Thread worker( "mixed-saturated" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        MixedLoadReceiver receiver( &worker );
        receiver.setMetaCallWorkMicros( kWorkMicros );

        Signal<int, int> metaCall;
        Object::connect( metaCall, &receiver, &MixedLoadReceiver::onMetaCall,
            ConnectionType::Queued );

        int timerId = -1;
        runOnThread( worker, [&]()
            {
                timerId = receiver.startTimer( 5 );
            } );
        ASSERT_GT( timerId, 0 );

        // Fed twice as fast as the worker can retire it, so the backlog only grows and the loop
        // never reaches a pass with an empty batch.
        for( int round = 0; round < kRounds; ++round )
        {
            for( int i = 0; i < kPerRound; ++i )
            {
                metaCall.emit( 0, round * kPerRound + i );
            }
            std::this_thread::sleep_for( 1ms );
        }

        ASSERT_TRUE( waitFor( [&receiver]()
            {
                return receiver.metaCalls() >= kRounds * kPerRound;
            } ) ) << "the timer starved the mailbox: only " << receiver.metaCalls() << " of "
                  << ( kRounds * kPerRound ) << " metacalls were delivered.";

        // ~120 ms of queued work against a 5 ms timer, spread over many passes. Asserting on a
        // handful rather than the couple of dozen expected leaves room for a slow machine.
        EXPECT_GE( receiver.timerFiresAtLastMetaCall(), 3 )
            << "only " << receiver.timerFiresAtLastMetaCall()
            << " expiries got through while the mailbox was backed up, so the timers were being "
            "deferred until it drained.";
        EXPECT_EQ( receiver.outOfOrder(), 0 );
        EXPECT_FALSE( receiver.overlapped() );
        EXPECT_FALSE( receiver.ranOnWrongThread() );

        runOnThread( worker, [&]()
            {
                receiver.killTimer( timerId );
            } );

        worker.quit();
        worker.join();
    }

    //! A timer due on every pass of the loop still leaves the mailbox drained.
    //!
    //! An interval of 0 means the loop never blocks and always has an expiry waiting, which is the
    //! shape most likely to spin on timers and never get back to the queued work.
    TEST( ThreadTimerTest, ZeroIntervalTimerDoesNotStarveMetaCalls )
    {
        constexpr int kPosts = 200;

        Thread worker( "mixed-zero" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        MixedLoadReceiver receiver( &worker );

        Signal<int, int> metaCall;
        Object::connect( metaCall, &receiver, &MixedLoadReceiver::onMetaCall,
            ConnectionType::Queued );

        int timerId = -1;
        runOnThread( worker, [&]()
            {
                timerId = receiver.startTimer( 0 );
            } );
        ASSERT_GT( timerId, 0 );

        for( int sequence = 0; sequence < kPosts; ++sequence )
        {
            metaCall.emit( 0, sequence );
        }

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.metaCalls() >= kPosts;
            } ) ) << "a zero-interval timer starved the mailbox: only " << receiver.metaCalls()
                  << " of " << kPosts << " metacalls were delivered.";
        EXPECT_EQ( receiver.outOfOrder(), 0 );
        EXPECT_GT( receiver.timerFires(), 0 ) << "the zero-interval timer never fired.";
        EXPECT_FALSE( receiver.overlapped() );

        runOnThread( worker, [&]()
            {
                receiver.killTimer( timerId );
            } );

        worker.quit();
        worker.join();
    }

    //! A timer handler may post tasks and touch the timer list without deadlocking.
    //!
    //! Expiries are collected under the mailbox mutex and delivered with it released, so a handler
    //! that posts a task, starts a timer or kills one re-enters ThreadData in the middle of a
    //! delivery pass. Holding the mutex across delivery would deadlock here rather than fail an
    //! assertion, which is why the test is written to finish or hang rather than to compare
    //! numbers.
    TEST( ThreadTimerTest, HandlersMayPostAndRetimeDuringDelivery )
    {
        Thread worker( "mixed-reentrant" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );

        std::atomic<int> postedFromTimer { 0 };
        std::atomic<int> secondTimerId { -1 };
        std::atomic<bool> secondReported { false };
        std::promise<void> secondFired;
        auto secondFiredFuture = secondFired.get_future();
        int firstTimerId = -1;

        // Installed before the timer exists, and published to the worker by the post() inside
        // runOnThread() below, so no handler can be reading it while it is written.
        receiver.mOnTimer = [&]( int aFiredId )
            {
                if( aFiredId == firstTimerId )
                {
                    // Post from inside a timer handler...
                    worker.post( [&postedFromTimer]()
                        {
                            postedFromTimer.fetch_add( 1 );
                        } );

                    // ...arm another timer from inside one...
                    if( secondTimerId.load() < 0 )
                    {
                        secondTimerId.store( receiver.startTimer( 5 ) );
                    }

                    // ...and kill the one currently being delivered.
                    receiver.killTimer( firstTimerId );
                }
                else if( aFiredId == secondTimerId.load() && !secondReported.exchange( true ) )
                {
                    receiver.killTimer( aFiredId );
                    secondFired.set_value();
                }
            };

        runOnThread( worker, [&]()
            {
                firstTimerId = receiver.startTimer( 5 );
            } );
        ASSERT_GT( firstTimerId, 0 );

        ASSERT_EQ( secondFiredFuture.wait_for( kPatience ), std::future_status::ready )
            << "the timer armed from inside a timer handler never fired.";

        EXPECT_EQ( postedFromTimer.load(), 1 )
            << "the task posted from inside a timer handler did not run exactly once.";
        EXPECT_GT( secondTimerId.load(), 0 );

        worker.quit();
        worker.join();

        // Cleared only once the loop is joined, so nothing can be reading it.
        receiver.mOnTimer = nullptr;
    }

} // namespace
