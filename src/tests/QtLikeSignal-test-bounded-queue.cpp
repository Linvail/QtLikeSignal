// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for the bounded event queue -- capacity, the three overflow policies, the
//! exemptions that keep a bound from turning into a leak, and the drop counter.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/OverflowPolicy.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <atomic>
#include <memory>
#include <vector>

using namespace QtLikeSignal;
using namespace std::chrono_literals;

namespace
{
    //! Live instances of the event type below.
    //!
    //! A bound that refuses an event has to free it, and a bound that evicts one has to free that.
    //! Neither failure crashes -- they leak -- so counting construction against destruction is the
    //! only way a test can see them. Every case here brackets itself with this.
    std::atomic<int> gLiveBoundedEvents { 0 };

    //! Two application types, so a test can show that coalescing tells them apart.
    const Event::Type kTickType = static_cast<Event::Type>( Event::User + 11 );
    const Event::Type kOtherTickType = static_cast<Event::Type>( Event::User + 12 );

    //! An application event carrying one value, so eviction order can be checked.
    class TickEvent : public Event
    {
    public:
        explicit TickEvent
            (
            int aValue,                    //!< Payload the receiver records.
            Event::Type aType = kTickType  //!< Type to carry.
            )
            : Event( aType )
            , mValue( aValue )
        {
            ++gLiveBoundedEvents;
        }

        ~TickEvent() override
        {
            --gLiveBoundedEvents;
        }

        int value() const
        {
            return mValue;
        }

    private:
        int mValue;
    };

    //! A receiver that records the values its hook was handed, in order.
    class TickRecorder : public Object
    {
    public:
        bool event
            (
            Event* aEvent  //!< The event to handle.
            ) override
        {
            if( aEvent->type() == kTickType || aEvent->type() == kOtherTickType )
            {
                mValues.push_back( static_cast<TickEvent*>( aEvent )->value() );

                // Published after the push, and released, so a thread that sees the new count also
                // sees the value behind it. Every case here but the cross-thread one reads mValues
                // directly, on the one thread that writes it; the cross-thread case watches this
                // instead and only touches the vector once the worker has stopped.
                mSeen.fetch_add( 1, std::memory_order_release );
                return true;
            }
            return Object::event( aEvent );
        }

        //! Values seen, in arrival order. Only safe to read on the thread this object lives in.
        std::vector<int> mValues;

        //! How many of them there are, readable from any thread. See event().
        std::atomic<int> mSeen { 0 };
    };

    //! A receiver that records its own destruction, for the deferred-delete cases.
    class DeleteRecorder : public Object
    {
    public:
        explicit DeleteRecorder
            (
            std::atomic<int>& aCounter  //!< Incremented by this object's destructor.
            )
            : mCounter( aCounter )
        {
        }

        ~DeleteRecorder() override
        {
            ++mCounter;
        }

    private:
        std::atomic<int>& mCounter;
    };

    //! The calling thread's dispatcher, which is what a test here sets a capacity on.
    std::shared_ptr<AbstractEventDispatcher> currentDispatcher()
    {
        return Thread::currentThread()->eventDispatcher();
    }

    //! Runs the calling thread's loop over whatever is queued right now.
    void pump()
    {
        Thread::currentThread()->processEvents();
    }

    //! Sets a capacity for one test and puts it back afterwards.
    //!
    //! The dispatcher belongs to the thread GoogleTest runs every case on, so a capacity left
    //! behind would apply to every test that follows -- including ones in other files that know
    //! nothing about bounds. Restoring it is what keeps these cases from being order-dependent.
    class ScopedCapacity
    {
    public:
        explicit ScopedCapacity
            (
            std::size_t aCapacity  //!< Capacity to apply for the life of this object.
            )
            : mDispatcher( currentDispatcher() )
            , mPrevious( mDispatcher ? mDispatcher->eventQueueCapacity() : 0 )
        {
            if( mDispatcher )
            {
                mDispatcher->setEventQueueCapacity( aCapacity );
            }
        }

        ~ScopedCapacity()
        {
            if( mDispatcher )
            {
                mDispatcher->setEventQueueCapacity( mPrevious );
            }
        }

        ScopedCapacity
            (
            const ScopedCapacity&
            ) = delete;

        ScopedCapacity& operator=
            (
            const ScopedCapacity&
            ) = delete;

    private:
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;
        std::size_t mPrevious;
    };
}

//! Tests that the default queue is unbounded, so nothing about an existing program changed.
TEST( BoundedQueueTest, DefaultCapacityIsUnbounded )
{
    const int before = gLiveBoundedEvents.load();

    auto dispatcher = currentDispatcher();
    ASSERT_TRUE( dispatcher );
    EXPECT_EQ( dispatcher->eventQueueCapacity(), 0u ) << "a capacity leaked in from another test.";

    TickRecorder receiver;
    for( int i = 0; i < 5000; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( i ) ) )
            << "an unbounded queue refused an event at depth " << i << ".";
    }

    pump();
    EXPECT_EQ( receiver.mValues.size(), 5000u );
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that DropNewest refuses at the ceiling and leaves everything already queued alone.
TEST( BoundedQueueTest, DropNewestRefusesAtTheCeiling )
{
    const int before = gLiveBoundedEvents.load();

    ScopedCapacity capacity( 4 );
    TickRecorder receiver;

    for( int i = 0; i < 4; ++i )
    {
        EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( i ),
            OverflowPolicy::DropNewest ) ) << "refused below the ceiling, at depth " << i << ".";
    }

    EXPECT_FALSE( Object::postEvent( &receiver, new TickEvent( 99 ),
        OverflowPolicy::DropNewest ) ) << "the ceiling was not enforced.";

    pump();

    ASSERT_EQ( receiver.mValues.size(), 4u ) << "a refusal disturbed what was already queued.";
    for( int i = 0; i < 4; ++i )
    {
        EXPECT_EQ( receiver.mValues[static_cast<size_t>( i )], i );
    }
    EXPECT_EQ( gLiveBoundedEvents.load(), before ) << "the refused event was leaked.";
}

//! Tests that DropOldest evicts exactly one event, frees it, and admits the new one.
TEST( BoundedQueueTest, DropOldestEvictsTheFrontAndAdmits )
{
    const int before = gLiveBoundedEvents.load();

    ScopedCapacity capacity( 3 );
    TickRecorder receiver;

    for( int i = 0; i < 3; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( i ),
            OverflowPolicy::DropOldest ) );
    }

    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 3 ), OverflowPolicy::DropOldest ) )
        << "DropOldest refused instead of making room.";

    pump();

    // 0 was evicted; 1, 2 and 3 survive, still in order.
    ASSERT_EQ( receiver.mValues.size(), 3u );
    EXPECT_EQ( receiver.mValues[0], 1 );
    EXPECT_EQ( receiver.mValues[1], 2 );
    EXPECT_EQ( receiver.mValues[2], 3 );
    EXPECT_EQ( gLiveBoundedEvents.load(), before ) << "the evicted event was leaked.";
}

//! Tests that a deferred delete is admitted past a full queue.
//!
//! Dropping one leaks the object it names, outright and forever -- and under a flood that would
//! leak exactly when memory is scarce, which inverts the point of having a ceiling at all.
TEST( BoundedQueueTest, ADeferredDeleteIsAdmittedPastAFullQueue )
{
    const int before = gLiveBoundedEvents.load();
    std::atomic<int> destroyed { 0 };

    ScopedCapacity capacity( 2 );

    TickRecorder filler;
    ASSERT_TRUE( Object::postEvent( &filler, new TickEvent( 0 ) ) );
    ASSERT_TRUE( Object::postEvent( &filler, new TickEvent( 1 ) ) );

    // The queue is full, and an ordinary post confirms it before the interesting one.
    EXPECT_FALSE( Object::postEvent( &filler, new TickEvent( 2 ) ) );

    DeleteRecorder* doomed = new DeleteRecorder( destroyed );
    doomed->deleteLater();

    pump();

    EXPECT_EQ( destroyed.load(), 1 )
        << "deleteLater() was dropped by the bound, which leaks the object.";
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that DropOldest never chooses a deferred delete as its victim.
//!
//! The exemption has to hold on the eviction side as well as the admission side. A queue whose
//! oldest entry is a pending delete would otherwise leak that object the moment it filled up.
TEST( BoundedQueueTest, DropOldestSkipsADeferredDelete )
{
    const int before = gLiveBoundedEvents.load();
    std::atomic<int> destroyed { 0 };

    ScopedCapacity capacity( 3 );

    // The deferred delete goes in FIRST, so it is the entry a naive DropOldest would evict.
    DeleteRecorder* doomed = new DeleteRecorder( destroyed );
    doomed->deleteLater();

    TickRecorder receiver;
    ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( 0 ), OverflowPolicy::DropOldest ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( 1 ), OverflowPolicy::DropOldest ) );

    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 2 ), OverflowPolicy::DropOldest ) );

    pump();

    EXPECT_EQ( destroyed.load(), 1 ) <<
        "DropOldest evicted a deferred delete and leaked its object.";

    // 0 was the oldest droppable entry, so it is the one that went.
    ASSERT_EQ( receiver.mValues.size(), 2u );
    EXPECT_EQ( receiver.mValues[0], 1 );
    EXPECT_EQ( receiver.mValues[1], 2 );
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that Coalesce collapses an equivalent event well below the ceiling.
//!
//! The distinguishing property of this policy: the other two do nothing until the queue is full,
//! and a Coalesce that waited for the ceiling would let a hundred identical repaints accumulate and
//! call it healthy.
TEST( BoundedQueueTest, CoalesceActsBelowTheCeiling )
{
    const int before = gLiveBoundedEvents.load();

    ScopedCapacity capacity( 100 );
    TickRecorder receiver;

    for( int i = 0; i < 20; ++i )
    {
        EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( i ), OverflowPolicy::Coalesce ) )
            << "Coalesce reported failure; a collapse is success, not refusal.";
    }

    EXPECT_EQ( currentDispatcher()->eventQueueDepth(), 1u )
        << "twenty equivalent events did not collapse to one.";

    pump();

    // The first one survives, not the last: coalescing drops the newcomer rather than replacing
    // what is pending, so the receiver still sees the request in the position it was first made.
    ASSERT_EQ( receiver.mValues.size(), 1u );
    EXPECT_EQ( receiver.mValues[0], 0 );
    EXPECT_EQ( gLiveBoundedEvents.load(), before ) << "a coalesced event was leaked.";
}

//! Tests that coalescing is keyed on the event type, not just the receiver.
TEST( BoundedQueueTest, CoalesceDistinguishesTypesOnOneReceiver )
{
    const int before = gLiveBoundedEvents.load();

    TickRecorder receiver;

    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 1, kTickType ),
        OverflowPolicy::Coalesce ) );
    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 2, kOtherTickType ),
        OverflowPolicy::Coalesce ) );
    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 3, kTickType ),
        OverflowPolicy::Coalesce ) );

    pump();

    ASSERT_EQ( receiver.mValues.size(), 2u ) << "two distinct types were collapsed into one.";
    EXPECT_EQ( receiver.mValues[0], 1 );
    EXPECT_EQ( receiver.mValues[1], 2 );
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that coalescing is keyed on the receiver, not just the event type.
TEST( BoundedQueueTest, CoalesceDistinguishesReceiversOfOneType )
{
    const int before = gLiveBoundedEvents.load();

    TickRecorder first;
    TickRecorder second;

    EXPECT_TRUE( Object::postEvent( &first, new TickEvent( 1 ), OverflowPolicy::Coalesce ) );
    EXPECT_TRUE( Object::postEvent( &second, new TickEvent( 2 ), OverflowPolicy::Coalesce ) );
    EXPECT_TRUE( Object::postEvent( &first, new TickEvent( 3 ), OverflowPolicy::Coalesce ) );

    pump();

    ASSERT_EQ( first.mValues.size(), 1u );
    EXPECT_EQ( first.mValues[0], 1 );
    ASSERT_EQ( second.mValues.size(), 1u );
    EXPECT_EQ( second.mValues[0], 2 );
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that coalescing an application event never touches a queued signal.
//!
//! Every queued signal carries Event::MetaCall, so a coalescing scan that matched on type alone
//! would collapse unrelated slots into one call. Two emits and a coalescing post share this queue,
//! and both emits must still arrive.
TEST( BoundedQueueTest, CoalesceLeavesQueuedSignalsAlone )
{
    const int before = gLiveBoundedEvents.load();

    TickRecorder receiver;
    std::atomic<int> calls { 0 };

    Signal<> signal;
    Object::connect( signal, &receiver, [&calls]()
        {
            ++calls;
        }, ConnectionType::Queued );

    signal.emit();
    signal.emit();

    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 1 ), OverflowPolicy::Coalesce ) );
    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 2 ), OverflowPolicy::Coalesce ) );

    pump();

    EXPECT_EQ( calls.load(), 2 ) << "coalescing collapsed two unrelated queued signals.";
    EXPECT_EQ( receiver.mValues.size(), 1u ) << "the application events did not coalesce.";
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that the drop counter counts refusals and evictions, and that coalescing is not a drop.
TEST( BoundedQueueTest, DroppedEventsAreCounted )
{
    const int before = gLiveBoundedEvents.load();

    auto dispatcher = currentDispatcher();
    const unsigned long long baseline = dispatcher->droppedEventCount();

    ScopedCapacity capacity( 2 );
    TickRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( 0 ) ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( 1 ) ) );

    EXPECT_FALSE( Object::postEvent( &receiver, new TickEvent( 2 ) ) );
    EXPECT_FALSE( Object::postEvent( &receiver, new TickEvent( 3 ) ) );
    EXPECT_EQ( dispatcher->droppedEventCount(), baseline + 2 ) << "refusals were not counted.";

    // An eviction is a drop too: something the application handed over was destroyed.
    EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( 4 ), OverflowPolicy::DropOldest ) );
    EXPECT_EQ( dispatcher->droppedEventCount(), baseline + 3 ) << "an eviction was not counted.";

    pump();
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that a coalesced post is not counted as a drop.
//!
//! It reports success, because the caller's intent -- make sure this is pending -- is satisfied.
//! Counting it would make a healthy coalescing producer look like a queue in trouble.
TEST( BoundedQueueTest, CoalescingIsNotCountedAsADrop )
{
    const int before = gLiveBoundedEvents.load();

    auto dispatcher = currentDispatcher();
    const unsigned long long baseline = dispatcher->droppedEventCount();

    TickRecorder receiver;
    for( int i = 0; i < 10; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( i ), OverflowPolicy::Coalesce ) );
    }

    EXPECT_EQ( dispatcher->droppedEventCount(), baseline )
        << "coalescing was counted as dropping.";

    pump();
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that the ceiling is a depth rather than a lifetime total.
//!
//! A queue that refused everything after its first N events would be useless. Draining has to make
//! room again.
TEST( BoundedQueueTest, ABoundedQueueDrainsAndReadmits )
{
    const int before = gLiveBoundedEvents.load();

    ScopedCapacity capacity( 2 );
    TickRecorder receiver;

    for( int round = 0; round < 3; ++round )
    {
        EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( round * 10 ) ) );
        EXPECT_TRUE( Object::postEvent( &receiver, new TickEvent( round * 10 + 1 ) ) );
        EXPECT_FALSE( Object::postEvent( &receiver, new TickEvent( round * 10 + 2 ) ) )
            << "the ceiling was not enforced in round " << round << ".";

        pump();
    }

    EXPECT_EQ( receiver.mValues.size(), 6u ) << "draining did not make room again.";
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that lowering the capacity below the current depth strands nothing.
//!
//! Trimming on the way down would destroy events that were already accepted, which is a worse
//! surprise than refusing new ones. The queue simply drains back under the new ceiling.
TEST( BoundedQueueTest, LoweringCapacityBelowTheDepthKeepsWhatIsQueued )
{
    const int before = gLiveBoundedEvents.load();

    TickRecorder receiver;
    {
        ScopedCapacity capacity( 10 );
        for( int i = 0; i < 8; ++i )
        {
            ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( i ) ) );
        }

        currentDispatcher()->setEventQueueCapacity( 2 );

        EXPECT_FALSE( Object::postEvent( &receiver, new TickEvent( 99 ) ) )
            << "the lowered ceiling was not applied to new work.";
        EXPECT_EQ( currentDispatcher()->eventQueueDepth(), 8u )
            << "lowering the capacity discarded events that were already accepted.";

        pump();
    }

    EXPECT_EQ( receiver.mValues.size(), 8u ) << "events queued before the change were lost.";
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that a receiver destroyed with a bounded queue full frees everything aimed at it.
//!
//! The five walks that strip a destroyed receiver from the queue are the part of this change most
//! likely to be got wrong, and a bound adds a counter they all have to leave consistent.
TEST( BoundedQueueTest, ADestroyedReceiverReleasesABoundedQueue )
{
    const int before = gLiveBoundedEvents.load();

    ScopedCapacity capacity( 4 );

    TickRecorder* receiver = new TickRecorder();
    for( int i = 0; i < 4; ++i )
    {
        ASSERT_TRUE( Object::postEvent( receiver, new TickEvent( i ) ) );
    }
    EXPECT_FALSE( Object::postEvent( receiver, new TickEvent( 4 ) ) );

    delete receiver;

    EXPECT_EQ( gLiveBoundedEvents.load(), before )
        << "events queued for a destroyed receiver were leaked.";

    // The queue is empty again, so a new receiver gets the whole ceiling.
    TickRecorder replacement;
    for( int i = 0; i < 4; ++i )
    {
        EXPECT_TRUE( Object::postEvent( &replacement, new TickEvent( i ) ) )
            << "destroying a receiver did not free its place in the queue.";
    }

    pump();
    EXPECT_EQ( replacement.mValues.size(), 4u );
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}

//! Tests that moveToThread() carries every queued event across even when the destination is full.
//!
//! A move is not a post. Those events were admitted once already, on the dispatcher they are
//! leaving, so putting them through admission again would let moveToThread() silently destroy
//! accepted work whenever the destination happened to sit near its ceiling.
TEST( BoundedQueueTest, AMoveIgnoresTheDestinationCeiling )
{
    const int before = gLiveBoundedEvents.load();

    TickRecorder receiver;

    {
        ScopedCapacity capacity( 2 );

        // Two on this thread's queue, which is exactly its ceiling.
        ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( 1 ) ) );
        ASSERT_TRUE( Object::postEvent( &receiver, new TickEvent( 2 ) ) );

        Thread worker;
        worker.start();

        // Waiting on isRunning() is not enough: start() publishes it before the OS thread exists,
        // and the dispatcher is created by the run body afterwards. Wait for what is actually
        // needed.
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.eventDispatcher() != nullptr;
            } ) );

        // The destination is bounded, and tighter than the number of events about to arrive.
        auto workerDispatcher = worker.eventDispatcher();
        ASSERT_TRUE( workerDispatcher );
        workerDispatcher->setEventQueueCapacity( 1 );

        ASSERT_TRUE( receiver.moveToThread( &worker ) );

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.mSeen.load( std::memory_order_acquire ) == 2;
            } ) ) << "a move dropped queued events against the destination ceiling.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    ASSERT_TRUE( receiver.moveToThread( Thread::currentThread() ) );

    // Read after the join, when this thread is the only one that can still touch the vector.
    ASSERT_EQ( receiver.mValues.size(), 2u );
    EXPECT_EQ( receiver.mValues[0], 1 );
    EXPECT_EQ( receiver.mValues[1], 2 );
    EXPECT_EQ( gLiveBoundedEvents.load(), before );
}
