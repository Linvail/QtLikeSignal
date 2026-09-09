// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for the application-facing event system -- Object::postEvent(), the
//! Object::event() hook, and the boundary that keeps the library's own event types out of an
//! application's hands.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"
#include <atomic>
#include <thread>
#include <vector>

using namespace QtLikeSignal;
using namespace std::chrono_literals;

namespace
{
    //! Live instances of every event type defined in this file.
    //!
    //! The whole ownership contract is "postEvent() takes the event on every path", and a leak is
    //! the only way to break it that does not also crash. Counting construction against destruction
    //! is what turns that contract into something a test can assert, on the paths where the event
    //! is refused as much as on the path where it is delivered.
    std::atomic<int> gLiveEvents { 0 };

    //! The type an application would define for itself. Any value from Event::User up will do.
    const Event::Type kOrderType = static_cast<Event::Type>( Event::User + 1 );

    //! A second application type, so a test can prove the hook tells them apart.
    const Event::Type kOtherType = static_cast<Event::Type>( Event::User + 2 );

    //! An ordinary application event carrying one value.
    class OrderEvent : public Event
    {
    public:
        explicit OrderEvent
            (
            int aValue,                     //!< The payload the receiver reads back.
            Event::Type aType = kOrderType  //!< Type to carry; overridden by the boundary tests.
            )
            : Event( aType )
            , mValue( aValue )
        {
            ++gLiveEvents;
        }

        virtual ~OrderEvent() override
        {
            --gLiveEvents;
        }

        int value() const
        {
            return mValue;
        }

    private:
        int mValue;
    };

    //! An application event built with one of the library's own type values.
    //!
    //! Exists only to be refused. Event's constructor is protected rather than public precisely so
    //! that this takes a deliberate subclass to write -- but a subclass *can* write it, which is
    //! why postEvent() checks as well.
    class ForgedEvent : public Event
    {
    public:
        explicit ForgedEvent
            (
            Event::Type aType  //!< One of MetaCall, Timer or DeferredDelete.
            )
            : Event( aType )
        {
            ++gLiveEvents;
        }

        virtual ~ForgedEvent() override
        {
            --gLiveEvents;
        }

    };

    //! A receiver that records what its event() hook was handed, and delegates the rest.
    //!
    //! The delegation is the part worth copying into real code: everything this class does not
    //! recognise goes to Object::event(), which is what keeps timers and deleteLater() working on
    //! an object that overrides the hook. Queued signals never arrive here at all -- see
    //! AQueuedSignalDoesNotReachTheEventHook for why that is deliberate.
    class EventRecorder : public Object
    {
    public:
        virtual bool event
            (
            Event* aEvent  //!< The event to handle.
            ) override
        {
            if( aEvent->type() == kOrderType )
            {
                mValues.push_back( static_cast<OrderEvent*>( aEvent )->value() );
                mThread = std::this_thread::get_id();
                ++mHandled;
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
        }

        //! Values seen by event(), in the order they arrived. Only touched on mThread.
        std::vector<int> mValues;

        std::atomic<int> mHandled { 0 };   //!< Events this class claimed.
        std::atomic<int> mTicks { 0 };     //!< Timer events that reached timerEvent().

        //! The thread event() last ran on, so a cross-thread test can check where delivery landed.
        std::thread::id mThread;
    };

    //! A receiver whose hook claims everything, including the library's own types.
    //!
    //! The mistake the Object::event() documentation warns about, made deliberately so a test can
    //! show what it costs: a timer that never reaches timerEvent().
    class GreedyRecorder : public Object
    {
    public:
        virtual bool event
            (
            Event* aEvent
            ) override
        {
            ( void )aEvent;
            ++mSeen;
            return true;
        }

        virtual void timerEvent
            (
            TimerEvent* aEvent
            ) override
        {
            ( void )aEvent;
            ++mTicks;
        }

        std::atomic<int> mSeen { 0 };    //!< Everything the hook swallowed.
        std::atomic<int> mTicks { 0 };   //!< Stays zero, which is the point.
    };

    //! Runs the calling thread's event loop over whatever is queued right now.
    //!
    //! One pass drains everything queued at the moment of the call, which is all any test here
    //! needs: every event is posted before the pump, and no handler in this file posts another.
    void pumpCurrentThread()
    {
        Thread::currentThread()->processEvents();
    }
}

//! Tests that a posted application event reaches the receiver's event() hook with its payload.
TEST( EventTest, PostedUserEventReachesTheReceiverHook )
{
    const int before = gLiveEvents.load();

    EventRecorder receiver;
    EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 42 ) ) );

    pumpCurrentThread();

    ASSERT_EQ( receiver.mValues.size(), 1u ) << "the event never reached event().";
    EXPECT_EQ( receiver.mValues[0], 42 );
    EXPECT_EQ( gLiveEvents.load(), before ) << "the delivered event was not deleted.";
}

//! Tests that events are delivered in the order they were posted.
TEST( EventTest, PostedEventsArriveInPostingOrder )
{
    EventRecorder receiver;
    for( int i = 0; i < 8; ++i )
    {
        EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( i ) ) );
    }

    pumpCurrentThread();

    ASSERT_EQ( receiver.mValues.size(), 8u );
    for( int i = 0; i < 8; ++i )
    {
        EXPECT_EQ( receiver.mValues[static_cast<size_t>( i )], i )
            << "delivery order differs from posting order at index " << i << ".";
    }
}

//! Tests that the base hook reports an application type it was never taught as unhandled.
TEST( EventTest, BaseHookReturnsFalseForAnUnknownUserType )
{
    const int before = gLiveEvents.load();

    EventRecorder receiver;

    // Posted as the *other* application type, which EventRecorder::event() does not claim and
    // therefore hands to Object::event().
    EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 7, kOtherType ) ) );

    pumpCurrentThread();

    EXPECT_EQ( receiver.mHandled.load(), 0 ) << "the recorder claimed a type it does not handle.";
    EXPECT_EQ( gLiveEvents.load(), before )
        << "an unhandled event was leaked rather than deleted after delivery.";
}

//! Tests that every type below Event::User is refused, and the event deleted with it.
//!
//! The check that keeps Object::event()'s three casts sound: each of those branches casts to a
//! concrete subclass an application cannot construct, so a hand-built event carrying one of their
//! type values must never reach the queue.
TEST( EventTest, PostEventRefusesEveryReservedType )
{
    EventRecorder receiver;

    const Event::Type reserved[] = { Event::MetaCall, Event::Timer, Event::DeferredDelete };
    for( const Event::Type type : reserved )
    {
        const int before = gLiveEvents.load();

        EXPECT_FALSE( Object::postEvent( &receiver, new ForgedEvent( type ) ) )
            << "a reserved type was accepted: " << static_cast<int>( type );
        EXPECT_EQ( gLiveEvents.load(), before )
            << "a refused event was not deleted: " << static_cast<int>( type );
    }

    pumpCurrentThread();
    EXPECT_EQ( receiver.mHandled.load(), 0 );
}

//! Tests that isUserType() draws the line exactly where postEvent() does.
//!
//! Two answers that must agree, since the whole point of exposing the predicate is that a caller
//! can ask it instead of discovering the answer from a rejected post.
TEST( EventTest, IsUserTypeAgreesWithWhatPostEventAccepts )
{
    EXPECT_FALSE( Event::isUserType( Event::MetaCall ) );
    EXPECT_FALSE( Event::isUserType( Event::Timer ) );
    EXPECT_FALSE( Event::isUserType( Event::DeferredDelete ) );
    EXPECT_FALSE( Event::isUserType( static_cast<Event::Type>( Event::User - 1 ) ) );
    EXPECT_TRUE( Event::isUserType( Event::User ) );
    EXPECT_TRUE( Event::isUserType( Event::MaxUser ) );
    EXPECT_FALSE( Event::isUserType( static_cast<Event::Type>( Event::MaxUser + 1 ) ) );

    EventRecorder receiver;

    const int before = gLiveEvents.load();
    EXPECT_FALSE( Object::postEvent( &receiver,
        new OrderEvent( 1, static_cast<Event::Type>( Event::User - 1 ) ) ) );
    EXPECT_FALSE( Object::postEvent( &receiver,
        new OrderEvent( 2, static_cast<Event::Type>( Event::MaxUser + 1 ) ) ) );
    EXPECT_EQ( gLiveEvents.load(), before ) << "a refused event was not deleted.";

    EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 3, Event::User ) ) );
    EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 4, Event::MaxUser ) ) );

    pumpCurrentThread();
    EXPECT_EQ( gLiveEvents.load(), before ) << "an accepted event was not deleted after delivery.";
}

//! Tests that a null receiver is refused and the event deleted rather than stranded.
TEST( EventTest, PostEventRefusesANullReceiver )
{
    const int before = gLiveEvents.load();

    EXPECT_FALSE( Object::postEvent( nullptr, new OrderEvent( 1 ) ) );
    EXPECT_EQ( gLiveEvents.load(), before ) << "the event was leaked when the receiver was null.";
}

//! Tests that a null event is refused without a crash.
TEST( EventTest, PostEventRefusesANullEvent )
{
    EventRecorder receiver;
    EXPECT_FALSE( Object::postEvent( &receiver, nullptr ) );
    EXPECT_FALSE( Object::postEvent( nullptr, nullptr ) );
}

//! Tests that an object with no thread affinity refuses the post rather than running it inline.
//!
//! Delivering on the posting thread would be the surprise moveToThread( nullptr ) exists to avoid:
//! an object detached from every loop must stop receiving, not start receiving somewhere else.
TEST( EventTest, PostEventRefusesAnObjectWithNoAffinity )
{
    const int before = gLiveEvents.load();

    EventRecorder receiver;
    ASSERT_TRUE( receiver.moveToThread( nullptr ) );

    EXPECT_FALSE( Object::postEvent( &receiver, new OrderEvent( 1 ) ) );
    EXPECT_EQ( gLiveEvents.load(), before ) << "the event was leaked when there was no affinity.";
    EXPECT_EQ( receiver.mHandled.load(), 0 ) << "the event ran on the posting thread.";
}

//! Tests that a posted event is delivered on the thread the receiver lives in, not the poster's.
TEST( EventTest, PostedEventIsDeliveredOnTheReceiversThread )
{
    EventRecorder receiver;

    {
        Thread worker;
        worker.start();
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.isRunning();
            } ) );

        ASSERT_TRUE( receiver.moveToThread( &worker ) );

        EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 99 ) ) );

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.mHandled.load() == 1;
            } ) ) << "the event never reached the worker.";

        EXPECT_NE( receiver.mThread, std::this_thread::get_id() )
            << "the event was handled on the posting thread.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    // Only after ~Thread() has nulled the back-pointer: while the worker object is alive the
    // receiver still names it, and one thread may not re-home another thread's object. An affinity
    // whose Thread is gone reads as none, which is the case moveToThread() lets the caller adopt.
    ASSERT_TRUE( receiver.moveToThread( Thread::currentThread() ) );
}

//! Tests that events still queued for an object are dropped when that object is destroyed.
//!
//! This is what makes postEvent() safe to call on an object that may go away before delivery, and
//! it is the reason the documentation can promise the receiver need not outlive the post.
TEST( EventTest, QueuedEventsForADestroyedReceiverAreDropped )
{
    const int before = gLiveEvents.load();

    EventRecorder* receiver = new EventRecorder();
    for( int i = 0; i < 4; ++i )
    {
        EXPECT_TRUE( Object::postEvent( receiver, new OrderEvent( i ) ) );
    }

    // Destroyed with all four still queued: nothing has pumped this thread since the posts.
    delete receiver;

    pumpCurrentThread();

    EXPECT_EQ( gLiveEvents.load(), before )
        << "events queued for a destroyed receiver were leaked rather than freed.";
}

//! Tests that an event posted before moveToThread() is delivered on the destination thread.
//!
//! The event is already in one dispatcher's queue when the move happens, so this covers the
//! migration path rather than the ordinary post.
TEST( EventTest, AQueuedEventFollowsItsReceiverToAnotherThread )
{
    EventRecorder receiver;

    {
        Thread worker;
        worker.start();
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.isRunning();
            } ) );

        EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 5 ) ) );

        // Moved with the event still queued on this thread, and never pumped here, so the only way
        // it can be delivered at all is if the move carried it across.
        ASSERT_TRUE( receiver.moveToThread( &worker ) );

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.mHandled.load() == 1;
            } ) ) << "the queued event did not follow its receiver to the new thread.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    ASSERT_TRUE( receiver.moveToThread( Thread::currentThread() ) );
}

//! Tests that an override delegating to the base still receives timers and deleteLater().
//!
//! The contract the Object::event() documentation states, checked rather than merely written down.
TEST( EventTest, AnOverrideThatDelegatesStillGetsTimers )
{
    EventRecorder receiver;
    EXPECT_TRUE( Object::postEvent( &receiver, new OrderEvent( 1 ) ) );

    const int timerId = receiver.startTimer( 5 );
    ASSERT_GT( timerId, 0 );

    EXPECT_TRUE( waitFor( [&receiver]()
        {
            pumpCurrentThread();
            return receiver.mTicks.load() > 0;
        } ) ) << "an override that delegates stopped its own timers.";

    receiver.killTimer( timerId );
    EXPECT_EQ( receiver.mHandled.load(), 1 ) << "the application event was lost.";
}

//! Tests that an override claiming everything stops the timers it did not mean to claim.
//!
//! The failure mode the delegation rule exists to prevent, pinned so that the documentation and the
//! behaviour cannot drift apart.
TEST( EventTest, AnOverrideThatClaimsEverythingStarvesItsOwnTimers )
{
    GreedyRecorder receiver;

    const int timerId = receiver.startTimer( 5 );
    ASSERT_GT( timerId, 0 );

    EXPECT_TRUE( waitFor( [&receiver]()
        {
            pumpCurrentThread();
            return receiver.mSeen.load() > 0;
        } ) ) << "the hook never ran at all, so this test proves nothing.";

    EXPECT_EQ( receiver.mTicks.load(), 0 )
        << "a hook that answered true for every type still reached timerEvent().";

    receiver.killTimer( timerId );
}

//! Tests that a queued signal is delivered without going through the event() hook.
//!
//! Not a detail of routing: it is what lets a receiver be destroyed on one thread while another
//! still holds queued emits for it. A queued emit names its receiver only as the key the queue is
//! stripped by, and delivering one never follows that pointer -- so making the delivery a virtual
//! call on the receiver, as Qt does, would mean reading the vtable of an object whose destructor
//! may be running concurrently. GreedyRecorder claims every type it is handed, so if a metacall
//! ever starts arriving through the hook, this test sees it.
TEST( EventTest, AQueuedSignalDoesNotReachTheEventHook )
{
    GreedyRecorder receiver;

    Signal<int> signal;
    Object::connect( signal, &receiver, []( int )
        {
        }, ConnectionType::Queued );

    signal.emit( 1 );
    pumpCurrentThread();

    EXPECT_EQ( receiver.mSeen.load(), 0 )
        << "a queued signal was delivered through event(), which dereferences the receiver.";
}

//! Tests that posting to an object whose thread has already stopped is refused, not stranded.
//!
//! Accepting it would put the event in a queue nothing will drain again, which is the leak the
//! dispatcher's close() exists to prevent.
TEST( EventTest, PostEventRefusesAThreadThatHasStopped )
{
    const int before = gLiveEvents.load();

    EventRecorder receiver;

    {
        Thread worker;
        worker.start();
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.isRunning();
            } ) );

        ASSERT_TRUE( receiver.moveToThread( &worker ) );
        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();

        EXPECT_FALSE( Object::postEvent( &receiver, new OrderEvent( 1 ) ) )
            << "a stopped thread accepted an event nothing would ever dispatch.";
        EXPECT_EQ( gLiveEvents.load(), before ) << "the refused event was leaked.";
    }

    ASSERT_TRUE( receiver.moveToThread( Thread::currentThread() ) );
}
