// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for loop liveness -- what a dispatcher reports about the handler it is running,
//! and the seqlock that lets another thread read it while it runs.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/LoopHealth.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"
#include <atomic>
#include <memory>
#include <thread>

using namespace QtLikeSignal;
using namespace std::chrono_literals;

namespace
{
    //! The application type these cases post.
    const Event::Type kProbeType = static_cast<Event::Type>( Event::User + 31 );

    //! A payload-free application event.
    class ProbeEvent : public Event
    {
    public:
        ProbeEvent()
            : Event( kProbeType )
        {
        }

    };

    //! A receiver that runs whatever the test gave it, from inside the dispatch.
    class ProbeReceiver : public Object
    {
    public:
        virtual bool event
            (
            Event* aEvent  //!< The event to handle.
            ) override
        {
            if( aEvent->type() == kProbeType )
            {
                ++mHandled;
                if( mDuringDispatch )
                {
                    mDuringDispatch();
                }
                return true;
            }
            return Object::event( aEvent );
        }

        virtual void timerEvent
            (
            TimerEvent* aEvent
            ) override
        {
            ( void )aEvent;
            ++mTicks;
            if( mDuringTimer )
            {
                mDuringTimer();
            }
        }

        std::atomic<int> mHandled { 0 };        //!< Application events handled.
        std::atomic<int> mTicks { 0 };          //!< Timer events handled.
        std::function<void()> mDuringDispatch;  //!< Run from inside event().
        std::function<void()> mDuringTimer;     //!< Run from inside timerEvent().
    };

    //! The calling thread's dispatcher.
    std::shared_ptr<AbstractEventDispatcher> currentDispatcher()
    {
        return Thread::currentThread()->eventDispatcher();
    }

    //! Runs the calling thread's loop over whatever is queued right now.
    void pump()
    {
        Thread::currentThread()->processEvents();
    }

    //! Turns tracking on for one test and puts it back afterwards.
    //!
    //! The dispatcher belongs to the thread every case runs on, so a setting left behind would
    //! apply to every test that follows -- including ones that measure allocations and would then
    //! be measuring this instead.
    class ScopedTracking
    {
    public:
        explicit ScopedTracking
            (
            bool aEnabled  //!< Setting to apply for the life of this object.
            )
            : mDispatcher( currentDispatcher() )
            , mPrevious( mDispatcher ? mDispatcher->isHealthTrackingEnabled() : false )
        {
            if( mDispatcher )
            {
                mDispatcher->setHealthTrackingEnabled( aEnabled );
            }
        }

        ~ScopedTracking()
        {
            if( mDispatcher )
            {
                mDispatcher->setHealthTrackingEnabled( mPrevious );
            }
        }

        ScopedTracking
            (
            const ScopedTracking&
            ) = delete;

        ScopedTracking& operator=
            (
            const ScopedTracking&
            ) = delete;

    private:
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;
        bool mPrevious;
    };
}

//! Tests that tracking is off unless it is asked for, and says so rather than lying.
TEST( LoopHealthTest, TrackingIsOffByDefault )
{
    auto dispatcher = currentDispatcher();
    ASSERT_TRUE( dispatcher );
    EXPECT_FALSE( dispatcher->isHealthTrackingEnabled() )
        << "tracking leaked in from another test.";

    ProbeReceiver receiver;
    ASSERT_TRUE( Object::postEvent( &receiver, new ProbeEvent() ) );
    pump();
    ASSERT_EQ( receiver.mHandled.load(), 1 );

    // A reading taken with tracking off must be empty rather than plausible: a caller that forgot
    // to switch it on should see nothing, not a stale value it might act on.
    const LoopHealth health = dispatcher->health();
    EXPECT_FALSE( health.mDispatching );
    EXPECT_EQ( health.mDispatchCount, 0u );
    EXPECT_EQ( health.mReceiver, nullptr );
    EXPECT_EQ( health.mEventType, 0 );
}

//! Tests that a finished dispatch is counted, and that only finished ones are.
TEST( LoopHealthTest, FinishedDispatchesAreCounted )
{
    ScopedTracking tracking( true );
    auto dispatcher = currentDispatcher();

    const unsigned long long before = dispatcher->health().mDispatchCount;

    ProbeReceiver receiver;
    for( int i = 0; i < 5; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new ProbeEvent() ) );
    }
    pump();

    const LoopHealth health = dispatcher->health();
    EXPECT_EQ( health.mDispatchCount, before + 5 );
    EXPECT_FALSE( health.mDispatching ) << "the loop still claims to be dispatching.";
}

//! Tests that a reading taken from inside a dispatch names the receiver and the type.
//!
//! Taken on the loop's own thread, which is the simplest way to be certain a dispatch really is in
//! flight at the moment of the reading. The cross-thread case is below.
TEST( LoopHealthTest, AReadingInsideADispatchNamesTheReceiver )
{
    ScopedTracking tracking( true );
    auto dispatcher = currentDispatcher();

    ProbeReceiver receiver;
    LoopHealth seen;

    receiver.mDuringDispatch = [&dispatcher, &seen]()
        {
            seen = dispatcher->health();
        };

    ASSERT_TRUE( Object::postEvent( &receiver, new ProbeEvent() ) );
    pump();

    EXPECT_TRUE( seen.mDispatching ) << "a reading taken mid-dispatch did not say so.";
    EXPECT_EQ( seen.mReceiver, &receiver ) << "the reading named the wrong receiver.";
    EXPECT_EQ( seen.mEventType, static_cast<int>( kProbeType ) );
    EXPECT_EQ( seen.mTimerId, 0 ) << "a queued event reported a timer id.";
    EXPECT_NE( seen.mDispatchStart, std::chrono::steady_clock::time_point {} );
}

//! Tests that a timer stall carries the timer id.
//!
//! "Stalled in timer 7" is a report someone can act on. "Stalled in a timer" usually is not.
TEST( LoopHealthTest, ATimerDispatchCarriesItsTimerId )
{
    ScopedTracking tracking( true );
    auto dispatcher = currentDispatcher();

    ProbeReceiver receiver;
    LoopHealth seen;

    receiver.mDuringTimer = [&dispatcher, &seen]()
        {
            seen = dispatcher->health();
        };

    const int timerId = receiver.startTimer( 1 );
    ASSERT_GT( timerId, 0 );

    EXPECT_TRUE( waitFor( [&receiver]()
        {
            pump();
            return receiver.mTicks.load() > 0;
        } ) );

    receiver.killTimer( timerId );

    EXPECT_TRUE( seen.mDispatching );
    EXPECT_EQ( seen.mReceiver, &receiver );
    EXPECT_EQ( seen.mEventType, static_cast<int>( Event::Timer ) );
    EXPECT_EQ( seen.mTimerId, timerId ) << "a timer dispatch did not report which timer.";
}

//! Tests that an idle loop does not read as stalled, however long it has been idle.
//!
//! The distinction the whole design turns on. A loop with nothing to do has not dispatched for as
//! long as it has had nothing to do, and that is health rather than a fault -- which is why
//! mDispatching exists and why there is no bare "time since the loop last ran" to threshold on.
TEST( LoopHealthTest, AnIdleLoopIsNotStalled )
{
    ScopedTracking tracking( true );
    auto dispatcher = currentDispatcher();

    ProbeReceiver receiver;
    ASSERT_TRUE( Object::postEvent( &receiver, new ProbeEvent() ) );
    pump();

    std::this_thread::sleep_for( 30ms );

    const LoopHealth health = dispatcher->health();
    EXPECT_FALSE( health.mDispatching )
        << "an idle loop reported itself as dispatching, which a watchdog would call a stall.";
}

//! Tests that a loop stuck in a handler is visible as such from another thread, with the receiver.
//!
//! The case the mission exists for. A watchdog on another thread has to be able to see both that
//! the loop is stuck and what it is stuck in, while it is still stuck.
TEST( LoopHealthTest, AStalledLoopIsVisibleFromAnotherThread )
{
    ProbeReceiver receiver;

    std::atomic<bool> inHandler { false };
    std::atomic<bool> release { false };
    LoopHealth observed;
    std::atomic<bool> observedTaken { false };

    {
        Thread worker;
        worker.start();
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.eventDispatcher() != nullptr;
            } ) );

        auto workerDispatcher = worker.eventDispatcher();
        workerDispatcher->setHealthTrackingEnabled( true );

        ASSERT_TRUE( receiver.moveToThread( &worker ) );

        // Blocks the worker inside its own handler until this thread lets it go, which is a stall
        // that can be observed rather than merely inferred.
        receiver.mDuringDispatch = [&inHandler, &release]()
            {
                inHandler.store( true, std::memory_order_release );
                while( !release.load( std::memory_order_acquire ) )
                {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
            };

        ASSERT_TRUE( Object::postEvent( &receiver, new ProbeEvent() ) );

        ASSERT_TRUE( waitFor( [&inHandler]()
            {
                return inHandler.load( std::memory_order_acquire );
            } ) ) << "the worker never entered the handler.";

        observed = workerDispatcher->health();
        observedTaken.store( true );

        release.store( true, std::memory_order_release );

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    ASSERT_TRUE( observedTaken.load() );
    EXPECT_TRUE( observed.mDispatching ) << "a wedged loop did not report itself as dispatching.";
    EXPECT_EQ( observed.mReceiver, &receiver ) << "the stall did not name what it was stuck in.";
    EXPECT_EQ( observed.mEventType, static_cast<int>( kProbeType ) );

    ASSERT_TRUE( receiver.moveToThread( Thread::currentThread() ) );
}

//! Tests that a nested pass reports the innermost dispatch, and the outer one resumes after it.
//!
//! A handler running its own processEvents() is ordinary, and the record has to survive it: the
//! innermost is what is actually running, and the outer one not making progress is what says it is
//! stuck.
TEST( LoopHealthTest, ANestedPassReportsTheInnermostDispatch )
{
    ScopedTracking tracking( true );
    auto dispatcher = currentDispatcher();

    ProbeReceiver outer;
    ProbeReceiver inner;

    LoopHealth duringInner;
    LoopHealth afterInner;

    inner.mDuringDispatch = [&dispatcher, &duringInner]()
        {
            duringInner = dispatcher->health();
        };

    outer.mDuringDispatch = [&dispatcher, &inner, &afterInner]()
        {
            ASSERT_TRUE( Object::postEvent( &inner, new ProbeEvent() ) );
            pump();
            afterInner = dispatcher->health();
        };

    ASSERT_TRUE( Object::postEvent( &outer, new ProbeEvent() ) );
    pump();

    EXPECT_EQ( duringInner.mReceiver, &inner )
        << "a nested dispatch did not replace the record while it ran.";
    EXPECT_EQ( afterInner.mReceiver, &outer )
        << "the outer dispatch was not restored when the nested pass returned.";
    EXPECT_TRUE( afterInner.mDispatching );
}

//! Tests that the record never tears while a busy loop writes it.
//!
//! The seqlock's whole reason for existing. A reader that paired the receiver of one dispatch with
//! the start time of another would name the wrong handler, which is worse than reporting nothing --
//! and it would be intermittent, so nothing but a test like this would find it.
TEST( LoopHealthTest, TheReadingNeverTears )
{
    ProbeReceiver first;
    ProbeReceiver second;

    std::atomic<bool> stop { false };
    std::atomic<int> torn { 0 };
    std::atomic<int> reads { 0 };

    {
        Thread worker;
        worker.start();
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.eventDispatcher() != nullptr;
            } ) );

        auto workerDispatcher = worker.eventDispatcher();
        workerDispatcher->setHealthTrackingEnabled( true );

        ASSERT_TRUE( first.moveToThread( &worker ) );
        ASSERT_TRUE( second.moveToThread( &worker ) );

        // Reads as fast as it can while the worker dispatches as fast as it can. Every reading has
        // to be self-consistent: dispatching implies a receiver that is one of the two, and a
        // start time that was actually set.
        std::thread observer( [&stop, &torn, &reads, workerDispatcher, &first, &second]()
            {
                while( !stop.load( std::memory_order_acquire ) )
                {
                    const LoopHealth health = workerDispatcher->health();
                    ++reads;

                    if( !health.mDispatching )
                    {
                        continue;
                    }

                    const bool consistent = ( health.mReceiver == &first
                    || health.mReceiver == &second )
                    && health.mEventType == static_cast<int>( kProbeType )
                    && health.mDispatchStart
                    != std::chrono::steady_clock::time_point {};

                    if( !consistent )
                    {
                        ++torn;
                    }
                }
            } );

        for( int i = 0; i < 4000; ++i )
        {
            ( void )Object::postEvent( ( i % 2 ) ? &first : &second, new ProbeEvent() );
        }

        EXPECT_TRUE( waitFor( [&first, &second]()
            {
                return first.mHandled.load() + second.mHandled.load() >= 4000;
            } ) ) << "the worker did not get through the events.";

        stop.store( true, std::memory_order_release );
        observer.join();

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    EXPECT_GT( reads.load(), 0 ) << "the observer never got a reading, so this proves nothing.";
    EXPECT_EQ( torn.load(), 0 ) << torn.load() << " of " << reads.load()
                                << " readings mixed fields from different dispatches.";

    ASSERT_TRUE( first.moveToThread( Thread::currentThread() ) );
    ASSERT_TRUE( second.moveToThread( Thread::currentThread() ) );
}

//! Tests that turning tracking off during a dispatch does not strand the dispatching flag.
//!
//! The guard restores the record from the flag it was handed rather than by re-reading the atomic,
//! so a setting changed underneath it cannot leave the loop permanently claiming to be busy -- a
//! watchdog would then report a fault that had already ended, and go on doing so forever.
TEST( LoopHealthTest, TurningTrackingOffMidDispatchDoesNotStrandTheFlag )
{
    auto dispatcher = currentDispatcher();
    ProbeReceiver receiver;

    {
        ScopedTracking tracking( true );

        receiver.mDuringDispatch = [&dispatcher]()
            {
                dispatcher->setHealthTrackingEnabled( false );
            };

        ASSERT_TRUE( Object::postEvent( &receiver, new ProbeEvent() ) );
        pump();

        // Back on, so the reading below can be taken at all.
        dispatcher->setHealthTrackingEnabled( true );
    }

    ScopedTracking tracking( true );
    const LoopHealth health = dispatcher->health();
    EXPECT_FALSE( health.mDispatching )
        << "the loop was left claiming to dispatch after tracking was switched off mid-handler.";
}
