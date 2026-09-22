// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for QtLikeSignal::Timer and the Object/dispatcher timer plumbing behind it
//! (Object::startTimer()/killTimer()/timerEvent(), the timer list in EventDispatcherDefault, and
//! the event loop's timer-aware wait).

#include "QtLikeSignal-test-types.hpp"
#include "TestCpuTime.hpp"

#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"

#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using namespace QtLikeSignal;

    //! Counts how often it is timed out, and on which thread.
    class CountingReceiver : public Object
    {
    public:
        explicit CountingReceiver
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

        //! Slot for Timer::timeout, and target for the singleShot member-function overload.
        void onTimeout()
        {
            mFiringThread.store( Thread::currentThread() );
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
            : Object()
        {
            // Built here and pushed, rather than constructed on that
            // thread's affinity: Object no longer takes a Thread*.
            if( aThread != nullptr )
            {
                ( void )moveToThread( aThread );
            }
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

        //! @return every id delivered so far, in the order it was delivered.
        //!
        //! For the tests that are about *order* rather than count: the dispatcher collects a pass's
        //! expiries into one batch and delivers that batch in one go, so the order within it is
        //! observable and has to stay put.
        std::vector<int> delivered() const
        {
            std::lock_guard<std::mutex> locker( mMutex );
            return mDelivered;
        }

        //! Runs on the next expiry, before it is recorded. Set from the timer's own thread.
        std::function<void( int aTimerId )> mOnTimer;

    protected:
        void timerEvent
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
            : Object()
            , mExpectedThread( aThread )
        {
            // Built here and pushed: Object no longer takes a Thread*.
            if( aThread != nullptr )
            {
                ( void )moveToThread( aThread );
            }
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
        void timerEvent
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
                if( Thread::currentThread() != mOwner.mExpectedThread )
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
                Timer timer;
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
        worker.wait();
    }

    //! QTimer restarts an active timer when setInterval() is called, even for the same interval.
    TEST( TimerTest, SetIntervalRestartsActiveTimerWithNewId )
    {
        Thread worker( "timer-change-interval" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        runOnThread( worker, [&worker]()
            {
                Timer timer;
                timer.start( 1000 );
                const int oldId = timer.timerId();

                timer.setInterval( 1000 );

                EXPECT_EQ( timer.interval(), 1000 );
                EXPECT_TRUE( timer.isActive() );
                EXPECT_GT( timer.timerId(), 0 );
                EXPECT_NE( timer.timerId(), oldId );
                timer.stop();
            } );

        worker.quit();
        worker.wait();
    }

    //! Qt 6.10 and later clamp negative timer intervals to one millisecond.
    TEST( TimerTest, NegativeIntervalIsClampedToOneMillisecond )
    {
        Timer timer;
        timer.setInterval( -1 );
        EXPECT_EQ( timer.interval(), 1 );
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
                timer = new Timer();
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
        worker.wait();
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
                timer = new Timer();
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
        worker.wait();
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
                timer = new Timer();
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
        worker.wait();
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

                // Constructed on the worker, because this lambda runs there -- an Object takes
                // its affinity from the thread that builds it. No move is needed and none is made:
                // an earlier version called moveToThread( &worker ) here, which was a self-move
                // that did nothing, under a comment claiming it was a push from elsewhere.
                Object context;
                ASSERT_EQ( context.thread(), &worker );
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
        worker.wait();
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
        worker.wait();
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
        worker.wait();
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
                // Registered back to back with the same interval, so they come due in the same
                // pass.
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
        worker.wait();

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
        worker.wait();
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
                timer = new Timer();
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
        source.wait();
        destination.quit();
        destination.wait();
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
                    fired.set_value( Thread::currentThread() );
                } );
            } ) );

        ASSERT_EQ( firedFuture.wait_for( kPatience ), std::future_status::ready )
            << "singleShot(int, Functor) never ran its functor.";
        EXPECT_EQ( firedFuture.get(), &worker ) << "the functor ran on the wrong thread.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    //! singleShot(int, context, Functor) hops to the context's thread and runs there exactly once.
    TEST( TimerSingleShotTest, ContextFunctorRunsOnContextThread )
    {
        Thread worker( "single-context" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        Object context;

        // Built here and pushed: Object takes a parent now, not a thread.

        ASSERT_TRUE( context.moveToThread( &worker ) );
        std::promise<Thread*> fired;
        auto firedFuture = fired.get_future();
        std::atomic<int> runs { 0 };

        // Called from this test's thread, not the worker's, so this exercises the cross-thread hop.
        Timer::singleShot( 5, &context, [&fired, &runs]()
            {
                if( runs.fetch_add( 1 ) == 0 )
                {
                    fired.set_value( Thread::currentThread() );
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
        worker.wait();
    }

    //! singleShot(int, context, Functor) arms the timer in place when the caller is already on the
    //! context's thread, instead of posting a hop to get there.
    //!
    //! The sibling test above deliberately calls from a foreign thread and so only ever takes the
    //! posted-hop branch. This is the other half of the same `if`, and it is the commoner way to
    //! use the overload -- Qt splits it the same way in
    //! QSingleShotTimer::startTimerForReceiver(). Nothing exercised the in-place arm, so a fault
    //! there would only ever have surfaced in user code.
    TEST( TimerSingleShotTest, ContextFunctorArmsInPlaceWhenAlreadyOnTheContextThread )
    {
        Thread worker( "single-same-thread" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        Object context;

        // Built here and pushed: Object takes a parent now, not a thread.

        ASSERT_TRUE( context.moveToThread( &worker ) );
        std::promise<Thread*> fired;
        auto firedFuture = fired.get_future();
        std::atomic<int> runs { 0 };

        // Issued from the worker itself, so the context is on the calling thread and singleShot()
        // takes the arm-here branch rather than posting to reach it.
        runOnThread( worker, [&context, &fired, &runs]()
            {
                Timer::singleShot( 5, &context, [&fired, &runs]()
                {
                    if( runs.fetch_add( 1 ) == 0 )
                    {
                        fired.set_value( Thread::currentThread() );
                    }
                } );
            } );

        ASSERT_EQ( firedFuture.wait_for( kPatience ), std::future_status::ready )
            << "a single shot armed on the context's own thread never ran its functor.";
        EXPECT_EQ( firedFuture.get(), &worker ) << "the functor did not run on the context thread.";

        std::this_thread::sleep_for( 60ms );
        EXPECT_EQ( runs.load(), 1 ) << "the single shot ran more than once.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    //! singleShot(int, context, Functor) drops the call when the context has no thread.
    //!
    //! moveToThread(nullptr) leaves nothing that could ever deliver the timer, so the overload has
    //! to return before it allocates its helper. The alternative is a helper nobody will fire and
    //! nobody will free, which is a leak rather than a missed call -- run under AddressSanitizer,
    //! a regression here shows up as the leak directly.
    TEST( TimerSingleShotTest, ContextFunctorIsDroppedForADetachedContext )
    {
        Object context;
        ASSERT_TRUE( context.moveToThread( nullptr ) );
        ASSERT_EQ( context.thread(), nullptr );

        std::atomic<int> runs { 0 };
        Timer::singleShot( 5, &context, [&runs]()
            {
                runs.fetch_add( 1 );
            } );

        std::this_thread::sleep_for( 40ms );
        EXPECT_EQ( runs.load(), 0 )
            << "a single shot was armed against a context with no thread to deliver it.";
    }

    //! singleShot(int, context, Functor) reclaims its helper when the context dies before the arm
    //! reaches the context's thread.
    //!
    //! The cross-thread form checks the context is alive, then posts a task that arms the timer on
    //! the context's thread. Between those two steps the context can be destroyed, so arm() checks
    //! again and throws the helper away if it has been -- otherwise the helper is left holding a
    //! timer for an object that no longer exists, and the functor runs against freed memory.
    //!
    //! Deterministic rather than racy: the worker is held inside a posted task, which is what puts
    //! the destruction squarely inside the window. Worth running under AddressSanitizer, where a
    //! regression is a use-after-free rather than a wrong count.
    TEST( TimerSingleShotTest, ContextDestroyedBeforeTheArmReachesItsThread )
    {
        Thread worker( "single-armed-too-late" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        std::atomic<int> runs { 0 };
        std::atomic<bool> blocked { false };
        std::atomic<bool> release { false };

        ASSERT_TRUE( worker.post( [&blocked, &release]()
            {
                blocked.store( true );
                while( !release.load() )
                {
                    std::this_thread::sleep_for( 1ms );
                }
            } ) );
        ASSERT_TRUE( waitFor( [&blocked]()
            {
                return blocked.load();
            } ) ) << "the worker never reached the blocking task.";

        {
            Object context;
            // Built here and pushed: Object takes a parent now, not a thread.
            ASSERT_TRUE( context.moveToThread( &worker ) );
            // Queued behind the blocking task, so the arm cannot have run yet.
            Timer::singleShot( 5, &context, [&runs]()
                {
                    runs.fetch_add( 1 );
                } );

            // context dies here, inside the window between the check and the arm.
        }

        release.store( true );

        std::this_thread::sleep_for( 60ms );
        EXPECT_EQ( runs.load(), 0 )
            << "a single shot fired for a context that was destroyed before the timer was armed.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
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
        worker.wait();
    }

    //! A single shot aimed at a thread whose loop has already stopped is dropped, not leaked and
    //! not run somewhere else.
    TEST( TimerSingleShotTest, ContextOnStoppedThreadIsDropped )
    {
        Thread worker( "single-stopped" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        Object context;

        // Built here and pushed: Object takes a parent now, not a thread.

        ASSERT_TRUE( context.moveToThread( &worker ) );
        worker.quit();
        worker.wait();

        std::atomic<int> runs { 0 };
        Timer::singleShot( 5, &context, [&runs]()
            {
                runs.fetch_add( 1 );
            } );

        std::this_thread::sleep_for( 40ms );
        EXPECT_EQ( runs.load(), 0 ) << "the functor ran even though its thread had stopped.";
    }

    //! A context-bound single shot is cancelled when its context is destroyed before expiry.
    //!
    //! The counter deliberately lives outside the context and is owned by the functor, not the
    //! context, so the cancellation is observable without touching freed memory: if the liveness
    //! check in the helper were removed, this counter would simply reach 1 and the test would fail
    //! outright rather than relying on a sanitizer to notice.
    TEST( TimerSingleShotTest, DestroyedContextCancelsFunctor )
    {
        Thread worker( "single-destroyed-context" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        std::atomic<int> runs { 0 };

        // Created and destroyed on the worker, which is the only thread allowed to do either.
        Object* context = nullptr;
        runOnThread( worker, [&]()
            {
                context = new Object();
            } );
        ASSERT_NE( context, nullptr );

        Timer::singleShot( 100, context, [&runs]()
            {
                runs.fetch_add( 1 );
            } );

        runOnThread( worker, [&]()
            {
                delete context;
                context = nullptr;
            } );

        std::this_thread::sleep_for( 150ms );
        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();

        EXPECT_EQ( runs.load(), 0 )
            << "the functor ran even though its context was destroyed before the delay elapsed.";
    }

    //! The member-function overload takes the same cancellation path, on the receiver it would
    //! otherwise have called into.
    //!
    //! What this adds over the functor test above is the bound-member closure, which captures a raw
    //! receiver pointer and would dereference it if the liveness check ever stopped running. The
    //! counter is an ExternalCounter's, owned here and not by the receiver, so it survives the
    //! receiver and can be asserted on afterwards -- the same technique
    //! ObjectTest.ReceiverDestroyedBeforeDeliveryNoCrash uses to observe a slot that must not have
    //! run. A regression bumps it to 1 and fails here outright, rather than only under a sanitizer.
    TEST( TimerSingleShotTest, DestroyedContextDoesNotCallIntoFreedReceiver )
    {
        Thread worker( "single-destroyed-receiver" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        std::atomic<int> invocations { 0 };

        ExternalCounter* receiver = nullptr;
        runOnThread( worker, [&]()
            {
                receiver = new ExternalCounter( &worker, invocations );
            } );
        ASSERT_NE( receiver, nullptr );

        Timer::singleShot( 100, receiver, &ExternalCounter::onTimeout );

        runOnThread( worker, [&]()
            {
                delete receiver;
                receiver = nullptr;
            } );

        std::this_thread::sleep_for( 150ms );
        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();

        EXPECT_EQ( invocations.load(), 0 )
            << "the member function was called on a receiver that had already been destroyed.";
    }

    //! Every entry point that takes an interval corrects a negative one to 1 ms rather than
    //! dropping the call, so singleShot() does not silently do nothing where start() would run.
    TEST( TimerSingleShotTest, NegativeIntervalStillRuns )
    {
        Thread worker( "single-negative" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        Object context;

        // Built here and pushed: Object takes a parent now, not a thread.

        ASSERT_TRUE( context.moveToThread( &worker ) );
        std::atomic<int> runs { 0 };
        Timer::singleShot( -1, &context, [&runs]()
            {
                runs.fetch_add( 1 );
            } );

        EXPECT_TRUE( waitFor( [&runs]()
            {
                return runs.load() >= 1;
            } ) ) << "a negative interval dropped the single shot instead of clamping it to 1 ms.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
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
        worker.wait();
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
        worker.wait();
    }

    //! Thread::processEvents() services timers too, so an adopted thread pumping from its own loop
    //! is not the one place where timers silently never fire.
    TEST( ThreadTimerTest, ProcessEventsDeliversTimers )
    {
        // Adopts this test's thread; no loop of its own, we pump it by hand below.
        Thread* self = Thread::currentThread();
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

        // Enough work per metacall that the load provably outlasts the timer intervals below.
        //
        // Without it the assertions here are a race rather than a test. The load is what sets the
        // observation window: the timers are armed, the metacalls are delivered, and timerFires()
        // is sampled the moment the last one lands. Trivial metacalls make that window ~0.9-2.8 ms
        // against a 1 ms shortest interval -- roughly 280 us of margin at the median -- so a few
        // percent of runs finish before any timer is due and correctly report zero expiries. That
        // reads as "the timers were starved" when nothing was starved at all; they were not yet
        // owed anything. Measured at 11 failures in 300 runs before this, none in 300 after.
        //
        // 5 us x 1000 metacalls puts the window at ~5-7 ms, several times the shortest interval,
        // so any expiry that fails to get through really is one the mailbox crowded out.
        constexpr int kWorkMicros = 5;

        Thread worker( "mixed-load" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        MixedLoadReceiver receiver( &worker );
        receiver.setMetaCallWorkMicros( kWorkMicros );

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
            emitters.emplace_back( [&metaCall, sender, kPerEmitter]()
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

        EXPECT_TRUE( waitFor( [&receiver, kExpectedMetaCalls]()
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
        worker.wait();
    }

    //! Timers keep firing while the mailbox never empties, not only once it has drained.
    //!
    //! A different claim from the test above, and the one that catches real starvation: a loop that
    //! serviced its timers only on passes with nothing queued would still deliver every metacall,
    //! and would still fire its timers -- just never while there was work outstanding.
    //!
    //! Note what is *not* claimed. One pass of the loop takes the whole mailbox in a single swap
    //! and runs it before looking at the timers, so a timer cannot interleave *within* a batch --
    //! queue 400 slow metacalls in one go and exactly one expiry gets through, however long the
    //! batch takes. That is the same granularity Qt has (sendPostedEvents drains the list, then
    //! timers are processed). The guarantee is per pass, so the load here arrives in rounds: each
    //! round is queued while the previous is still being chewed, which keeps every pass non-empty
    //! while still giving the loop many passes to be measured over.
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

        ASSERT_TRUE( waitFor( [&receiver, kRounds, kPerRound]()
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
        worker.wait();
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

        EXPECT_TRUE( waitFor( [&receiver, kPosts]()
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
        worker.wait();
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
                    ASSERT_TRUE( worker.post( [&postedFromTimer]()
                        {
                            postedFromTimer.fetch_add( 1 );
                        } ) );

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
        worker.wait();

        // Cleared only once the loop is joined, so nothing can be reading it.
        receiver.mOnTimer = nullptr;
    }

    //================================================================
    // Cadence and ordering when the loop has been away
    //================================================================

    //! A pass that finds several timers due fires each exactly once, in the order they were
    //! started.
    //!
    //! **Both halves of this are properties of the timer list, not of any one timer.**
    //!
    //! *Order.* The dispatcher keeps its timers ordered by deadline so that a pass which finds
    //! nothing due costs one comparison instead of a walk. The order a *batch* is delivered in is a
    //! separate thing, and it is the order the timers were started in -- so the three intervals
    //! below are deliberately out of that order, and a delivery sorted by deadline would read
    //! `a, c, b` and fail here.
    //!
    //! *Cadence.* Each timer is due several times over during the stall, and a loop that worked
    //! off that backlog would deliver the shortest one four times in this pass and go on delivering
    //! it for as long as it took to catch up -- for an animation tick, a burst of frames nobody
    //! asked for. It resynchronises instead, which is what the second pass proves: every timer is
    //! re-armed a full interval into the future, so there is nothing left to deliver.
    TEST( ThreadTimerTest, StalledLoopFiresEachTimerOnceInStartOrder )
    {
        // Adopts this test's thread and is pumped by hand below, so the stall is exact rather than
        // something a worker's loop has to be persuaded into.
        Thread* const self = Thread::currentThread();
        ASSERT_NE( self, nullptr );

        RecordingObject receiver( self );

        // Anything an earlier test left queued would land in the first batch below and be counted
        // as one of ours.
        self->processEvents();
        ASSERT_EQ( receiver.total(), 0u );

        // Started in this order, with deadlines deliberately in a different one.
        const int first  = receiver.startTimer( 100 );
        const int second = receiver.startTimer( 300 );
        const int third  = receiver.startTimer( 200 );
        ASSERT_GT( first, 0 );
        ASSERT_GT( second, 0 );
        ASSERT_GT( third, 0 );

        // Longer than every interval, and more than four times the shortest, so a loop that
        // delivered a backlog would be unmistakable.
        std::this_thread::sleep_for( 500ms );

        self->processEvents();

        const std::vector<int> firstBatch = receiver.delivered();
        EXPECT_EQ( firstBatch, ( std::vector<int> { first, second, third } ) )
            << "a stalled pass delivered " << firstBatch.size()
            << " expiries, and not one of each in the order the timers were started. Ordering the "
            "timer list by deadline must not change the order a batch is delivered in.";

        // Immediately, with the shortest interval a further 100 ms away. Every timer was re-armed
        // from now rather than from the deadline it missed, so this pass has nothing to do.
        self->processEvents();
        EXPECT_EQ( receiver.total(), 3u )
            << "the loop worked off a backlog of missed deadlines instead of resynchronising.";

        receiver.killTimer( first );
        receiver.killTimer( second );
        receiver.killTimer( third );
    }

    //================================================================
    // A thread running its own native loop
    //================================================================

    //! A timer on a thread with its own native loop fires on schedule, with no other traffic.
    //!
    //! **This is the case setWakeCallback() alone cannot serve.** A wake callback says "something
    //! was posted", which is everything a posted event needs; a deadline is the opposite question
    //! -- how long may this loop sleep before it must call processEvents() again -- and a loop with
    //! no answer to it runs its timers whenever some unrelated event happens to wake it. On a
    //! thread with nothing else going on, as here, that is never. It fails as jitter rather than as
    //! a missing feature, which is the worst way for it to fail.
    //!
    //! So the loop below is written the way a host would write one: it blocks indefinitely when
    //! nothing is scheduled, and for exactly as long as it is told otherwise. Nothing is posted to
    //! it after the timer starts, so every wake it gets past that point comes from the deadline.
    TEST( ThreadTimerTest, NativeLoopRunsTimersOnItsOwnDeadline )
    {
        constexpr int kIntervalMs = 50;
        constexpr int kWantedFires = 4;

        std::mutex mutex;
        std::condition_variable cv;

        // Guarded by mutex. -1 means nothing is scheduled, so the loop may sleep until woken --
        // which is exactly the state that used to strand a timer forever.
        int sleepMs = -1;
        bool woken = false;
        bool rearm = false;
        bool stop = false;

        std::atomic<bool> ready { false };
        RecordingObject* receiver = nullptr;
        int timerId = -1;

        std::thread nativeLoop( [&]()
            {
                // Adopts this raw thread, which is the whole premise: it has a QtLikeSignal
                // dispatcher but never calls exec().
                Thread* const self = Thread::currentThread();

                self->setWakeCallback( [&]()
                {
                    {
                        std::lock_guard<std::mutex> lock( mutex );
                        woken = true;
                    }
                    cv.notify_all();
                } );

                self->setDeadlineCallback( [&]( int aMsFromNow )
                {
                    {
                        std::lock_guard<std::mutex> lock( mutex );
                        sleepMs = aMsFromNow;

                        // The wait below has to end on this as well as on a post. A loop parked
                        // with no deadline at all is waiting on a predicate, and a new deadline
                        // that does not appear in that predicate would leave it parked -- the same
                        // silence this whole test is about, moved into the host.
                        rearm = true;
                    }
                    cv.notify_all();
                } );

                // Born here, so it lives on this thread and its timer can be started here.
                RecordingObject local;
                receiver = &local;
                timerId  = local.startTimer( kIntervalMs );
                ready.store( true );

                for( ;; )
                {
                    {
                        std::unique_lock<std::mutex> lock( mutex );
                        auto readyToRun = [&]()
                        {
                            return woken || rearm || stop;
                        };

                        if( sleepMs < 0 )
                        {
                            // No deadline: sleep until somebody says otherwise. A poll with a
                            // timeout here would hide the very defect this test is about, by
                            // delivering the timer on the next poll rather than on its deadline.
                            cv.wait( lock, readyToRun );
                        }
                        else
                        {
                            cv.wait_for( lock, std::chrono::milliseconds( sleepMs ), readyToRun );
                        }
                        woken = false;
                        rearm = false;
                        if( stop )
                        {
                            break;
                        }
                    }

                    self->processEvents();
                }

                local.killTimer( timerId );
                self->setDeadlineCallback( nullptr );
                self->setWakeCallback( nullptr );
            } );

        ASSERT_TRUE( waitFor( [&ready]()
            {
                return ready.load();
            } ) ) << "the native loop never started.";
        ASSERT_GT( timerId, 0 ) << "the timer could not be started on the adopted thread.";

        const bool fired = waitFor( [&receiver]()
            {
                return receiver->total() >= static_cast<std::size_t>( kWantedFires );
            } );

        // Read before the loop is stopped. The receiver is a local of that thread, so it is gone
        // by the time join() returns and the failure message below would be reading freed memory.
        const std::size_t observed = receiver->total();

        {
            std::lock_guard<std::mutex> lock( mutex );
            stop = true;
        }
        cv.notify_all();
        nativeLoop.join();

        EXPECT_TRUE( fired )
            << "a " << kIntervalMs << " ms timer produced only " << observed
            << " expiries on a thread running its own loop with no other traffic. The loop is "
            "never told when the next deadline is, so it sleeps through it.";
    }

    //! A thread whose only timers are far in the future blocks rather than polling for them.
    //!
    //! The counterpart to the test above, and the reason its loop is allowed to sleep
    //! indefinitely: a deadline that is hours away must produce a wait that is hours long, not a
    //! wake ten times a second to ask again. Measured as CPU time against wall time, because that
    //! is the only difference between the two that is visible from outside.
    //!
    //! CPU time comes from TestSupport::processCpuSeconds() rather than std::clock(); see
    //! TestCpuTime.hpp for why the difference matters on Windows.
    TEST( ThreadTimerTest, FarFutureTimersLeaveTheLoopBlocked )
    {
        constexpr int kRunMs = 400;

        Thread worker( "idle-timers" );
        worker.start();
        ASSERT_TRUE( waitUntilRunning( worker ) );

        RecordingObject receiver( &worker );

        // An hour away, so nothing can come due inside the window measured below. Several of them,
        // because the cost being ruled out is per-timer as well as per-pass.
        std::vector<int> ids;
        runOnThread( worker, [&]()
            {
                for( int i = 0; i < 32; ++i )
                {
                    ids.push_back( receiver.startTimer( 3600000 + i ) );
                }
            } );
        ASSERT_EQ( ids.size(), 32u );

        const double cpuBefore = TestSupport::processCpuSeconds();
        const auto wallBefore = std::chrono::steady_clock::now();

        std::this_thread::sleep_for( std::chrono::milliseconds( kRunMs ) );

        const double cpuSeconds = TestSupport::processCpuSeconds() - cpuBefore;
        const double wallSeconds
            = std::chrono::duration<double>( std::chrono::steady_clock::now() - wallBefore ).count()
            ;

        ASSERT_GT( wallSeconds, 0.1 ) << "the measured window was too short to say anything.";

        // Process-wide CPU over a window in which this test's own threads are the only ones with
        // anything to do, so the bar is loose enough to survive whatever else the process is
        // finishing and far below the one core a spinning loop would burn.
        EXPECT_LT( cpuSeconds / wallSeconds, 0.5 )
            << "the process burned " << cpuSeconds << "s of CPU over " << wallSeconds
            << "s of wall time (ratio " << ( cpuSeconds / wallSeconds )
            << ") while its only timers were an hour away -- the loop is waking to look at them "
            "rather than waiting for the earliest deadline.";

        EXPECT_EQ( receiver.total(), 0u ) << "a timer an hour away fired.";

        runOnThread( worker, [&]()
            {
                for( const int id : ids )
                {
                    receiver.killTimer( id );
                }
            } );

        worker.quit();
        worker.wait();
    }

    //================================================================
    // Remaining time
    //================================================================

    //! remainingTime() counts down while the timer runs, and reports -1 while it does not.
    //!
    //! QTimer::remainingTime() with the same two answers. Driven on this test's own thread, so the
    //! reading is taken between passes rather than racing a loop that might deliver the expiry
    //! half-way through the assertion.
    TEST( TimerTest, RemainingTimeCountsDownAndIsMinusOneWhenStopped )
    {
        Thread* const self = Thread::currentThread();
        ASSERT_NE( self, nullptr );

        Timer timer;
        EXPECT_EQ( timer.remainingTime(), -1 ) << "an unstarted timer reported a remaining time.";

        timer.start( 1000 );
        const int atStart = timer.remainingTime();
        EXPECT_GT( atStart, 0 );
        EXPECT_LE( atStart, 1000 );

        std::this_thread::sleep_for( 50ms );

        const int later = timer.remainingTime();
        EXPECT_LT( later, atStart ) << "remainingTime() did not fall as the deadline approached.";
        EXPECT_GE( later, 0 );

        timer.stop();
        EXPECT_EQ( timer.remainingTime(), -1 ) << "a stopped timer still reported a deadline.";
    }

    //! The dispatcher reports the *earliest* deadline of all its timers, and -1 when it has none.
    //!
    //! The loop-level question, as against Timer::remainingTime()'s per-timer one: this is what a
    //! thread with its own native loop feeds to whatever it blocks in, so "earliest" and "-1 means
    //! sleep until woken" are the two things it has to get right.
    TEST( ThreadTimerTest, RemainingTimeMsReportsTheEarliestDeadline )
    {
        Thread* const self = Thread::currentThread();
        ASSERT_NE( self, nullptr );

        auto dispatcher = self->eventDispatcher();
        ASSERT_NE( dispatcher, nullptr );

        // Anything an earlier test left running would be an earlier deadline than the two below.
        ASSERT_EQ( dispatcher->remainingTimeMs(), -1 )
            << "this thread already had a timer, so nothing here would be the earliest.";

        RecordingObject receiver( self );

        const int distant = receiver.startTimer( 100000 );
        ASSERT_GT( distant, 0 );
        const int distantMs = dispatcher->remainingTimeMs();
        EXPECT_GT( distantMs, 90000 );
        EXPECT_LE( distantMs, 100000 );

        // Started second and due first, so it must take over the answer -- which is the whole
        // point: the front of the heap, not the most recent registration and not the first one.
        const int soon = receiver.startTimer( 5000 );
        ASSERT_GT( soon, 0 );
        const int soonMs = dispatcher->remainingTimeMs();
        EXPECT_GT( soonMs, 0 );
        EXPECT_LE( soonMs, 5000 );

        receiver.killTimer( soon );
        EXPECT_GT( dispatcher->remainingTimeMs(), 90000 )
            << "killing the earliest timer did not hand the answer back to the one behind it.";

        receiver.killTimer( distant );
        EXPECT_EQ( dispatcher->remainingTimeMs(), -1 )
            << "a dispatcher with no timers left still reported a deadline, so a native loop "
            "would keep waking for one that is not there.";
    }

    //! Asking about a timer the dispatcher does not have answers -1, whatever the id.
    //!
    //! The per-id query's other answer, and the one the rest of this suite never asks for.
    //! `Timer::remainingTime()` cannot reach it: that class returns -1 from its own `mActive` flag
    //! and never puts the question to the dispatcher at all, so a test has to ask through
    //! `Object::remainingTime()` directly. Both ways of being absent are covered, because they
    //! arrive at the same line from opposite directions -- an id that was never issued, and one
    //! that was issued and then killed.
    TEST( ObjectTimerTest, RemainingTimeIsMinusOneForATimerTheDispatcherDoesNotHave )
    {
        Thread* const self = Thread::currentThread();
        ASSERT_NE( self, nullptr );

        RecordingObject receiver( self );

        // An id no startTimer() ever returned. Negative, so it cannot collide with a real one
        // however the pool is behaving by the time this test runs.
        EXPECT_EQ( receiver.remainingTime( -12345 ), -1 )
            << "an id that was never issued reported a deadline.";

        const int id = receiver.startTimer( 60000 );
        ASSERT_GT( id, 0 );
        EXPECT_GT( receiver.remainingTime( id ), 0 )
            << "a running timer reported no deadline, so the -1 below would prove nothing.";

        receiver.killTimer( id );
        EXPECT_EQ( receiver.remainingTime( id ), -1 )
            << "a killed timer still reported a deadline, so its registration outlived it.";
    }

    //! A detached object has no dispatcher to ask, so every id answers -1.
    //!
    //! The guard in front of the query rather than the query itself: `Object::remainingTime()`
    //! reaches the dispatcher through this object's thread, and an object detached with
    //! moveToThread(nullptr) has neither. Same shape as
    //! `ObjectTimerTest.StartTimerWithoutAThreadIsRefused`.
    TEST( ObjectTimerTest, RemainingTimeIsMinusOneWithoutAThread )
    {
        RecordingObject receiver;
        ASSERT_TRUE( receiver.moveToThread( nullptr ) );
        EXPECT_EQ( receiver.remainingTime( 1 ), -1 );
    }

} // namespace
