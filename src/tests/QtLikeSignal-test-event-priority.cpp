// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for event priority -- queue ordering, preemption of a pass already running, and
//! how priority composes with a bounded queue.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/EventPriority.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/OverflowPolicy.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <atomic>
#include <memory>
#include <vector>

using namespace QtLikeSignal;
using namespace std::chrono_literals;

namespace
{
    //! Live instances of the event type below, so a dropped event that was not freed shows up.
    std::atomic<int> gLivePriorityEvents { 0 };

    //! The application type these cases post.
    const Event::Type kMarkType = static_cast<Event::Type>( Event::User + 21 );

    //! A second type, for the coalescing case.
    const Event::Type kOtherMarkType = static_cast<Event::Type>( Event::User + 22 );

    //! An application event carrying the value the receiver records.
    class MarkEvent : public Event
    {
    public:
        explicit MarkEvent
            (
            int aValue,                    //!< Payload the receiver records.
            Event::Type aType = kMarkType  //!< Type to carry.
            )
            : Event( aType )
            , mValue( aValue )
        {
            ++gLivePriorityEvents;
        }

        ~MarkEvent() override
        {
            --gLivePriorityEvents;
        }

        int value() const
        {
            return mValue;
        }

    private:
        int mValue;
    };

    //! Records the order its hook was handed values in.
    class MarkRecorder : public Object
    {
    public:
        bool event
            (
            Event* aEvent  //!< The event to handle.
            ) override
        {
            if( aEvent->type() == kMarkType || aEvent->type() == kOtherMarkType )
            {
                mValues.push_back( static_cast<MarkEvent*>( aEvent )->value() );

                // Run while the value is being dispatched, which is how the preemption cases post
                // from inside a pass. Empty for every other case.
                if( mDuringDispatch )
                {
                    mDuringDispatch( static_cast<MarkEvent*>( aEvent )->value() );
                }
                return true;
            }
            return Object::event( aEvent );
        }

        std::vector<int> mValues;  //!< Values seen, in dispatch order.

        //! Called from inside event(), so a test can post while a pass is running.
        std::function<void( int )> mDuringDispatch;
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

    //! Sets a capacity for one test and puts it back afterwards.
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

//! Tests that a higher priority overtakes lower-priority events already queued.
TEST( EventPriorityTest, HigherPriorityRunsFirst )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ) ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ) ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 99 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 3 ) ) );

    pump();

    ASSERT_EQ( receiver.mValues.size(), 4u );
    EXPECT_EQ( receiver.mValues[0], 99 ) << "a high-priority event did not overtake the queue.";
    EXPECT_EQ( receiver.mValues[1], 1 );
    EXPECT_EQ( receiver.mValues[2], 2 );
    EXPECT_EQ( receiver.mValues[3], 3 );
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that a lower priority runs after everything at the default.
TEST( EventPriorityTest, LowerPriorityRunsLast )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 50 ),
        OverflowPolicy::DropNewest, EventPriority::kLow ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ) ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ) ) );

    pump();

    ASSERT_EQ( receiver.mValues.size(), 3u );
    EXPECT_EQ( receiver.mValues[0], 1 );
    EXPECT_EQ( receiver.mValues[1], 2 );
    EXPECT_EQ( receiver.mValues[2], 50 ) << "a demoted event did not sink to the back.";
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that equal priorities keep posting order, at several priorities at once.
//!
//! The property everything else depends on. Insertion uses upper_bound rather than lower_bound
//! precisely for this: lower would place a newcomer before the entries it ties with and quietly
//! reverse same-priority FIFO, which every queued signal relies on.
TEST( EventPriorityTest, EqualPrioritiesKeepPostingOrder )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;

    // Interleaved on purpose, so a stable sort is the only thing that produces the answer below.
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 10 ), OverflowPolicy::DropNewest, 0 )
               );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 20 ), OverflowPolicy::DropNewest, 5 )
               );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 11 ), OverflowPolicy::DropNewest, 0 )
               );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 21 ), OverflowPolicy::DropNewest, 5 )
               );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 30 ), OverflowPolicy::DropNewest, 9 )
               );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 12 ), OverflowPolicy::DropNewest, 0 )
               );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 22 ), OverflowPolicy::DropNewest, 5 )
               );

    pump();

    const std::vector<int> expected { 30, 20, 21, 22, 10, 11, 12 };
    EXPECT_EQ( receiver.mValues, expected )
        << "priorities are not descending, or equal priorities lost their posting order.";
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that a queue nobody gives a priority to behaves exactly as it did before priorities.
TEST( EventPriorityTest, TheDefaultIsAPlainFifo )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;
    for( int i = 0; i < 32; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( i ) ) );
    }

    pump();

    ASSERT_EQ( receiver.mValues.size(), 32u );
    for( int i = 0; i < 32; ++i )
    {
        EXPECT_EQ( receiver.mValues[static_cast<size_t>( i )], i );
    }
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that a higher-priority event posted mid-pass preempts the rest of that pass.
//!
//! **The case sorted insertion alone does not answer.** A pass takes the whole queue in one swap
//! and dispatches it with the mutex released, so an event posted during the pass is invisible to
//! it. Without the preemption check a telltale posted while a long batch is in flight would wait
//! out the whole batch however high its priority -- the feature would look implemented and not do
//! its job.
TEST( EventPriorityTest, AHighPriorityPostPreemptsARunningPass )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;

    // A batch of ten, all at the default.
    for( int i = 0; i < 10; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( i ) ) );
    }

    // Posted from inside the dispatch of the first of them, so it lands while the pass is running
    // and the batch it would otherwise have queued behind has already been taken.
    receiver.mDuringDispatch = [&receiver]( int aValue )
        {
            if( aValue == 0 )
            {
                receiver.mDuringDispatch = nullptr;
                ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 99 ),
                    OverflowPolicy::DropNewest, EventPriority::kHigh ) );
            }
        };

    pump();

    ASSERT_EQ( receiver.mValues.size(), 11u ) << "something was lost.";
    EXPECT_EQ( receiver.mValues[0], 0 );
    EXPECT_EQ( receiver.mValues[1], 99 )
        << "a high-priority event posted during a pass waited for the rest of the batch.";
    EXPECT_EQ( receiver.mValues[2], 1 ) << "the batch did not resume where it left off.";
    EXPECT_EQ( receiver.mValues[10], 9 );
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that an equal-priority post does not preempt a running pass.
//!
//! The preemption test is strictly greater, deliberately: letting an equal priority cut in would
//! let a late arrival overtake one already waiting at the same rank, which is the same-priority
//! FIFO guarantee everything else is built on.
TEST( EventPriorityTest, AnEqualPriorityPostDoesNotPreempt )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;
    for( int i = 0; i < 5; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( i ) ) );
    }

    receiver.mDuringDispatch = [&receiver]( int aValue )
        {
            if( aValue == 0 )
            {
                receiver.mDuringDispatch = nullptr;
                ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 99 ) ) );
            }
        };

    pump();

    // 99 was posted during the pass at the same priority, so it waits for the next one.
    ASSERT_GE( receiver.mValues.size(), 5u );
    const std::vector<int> firstFive( receiver.mValues.begin(), receiver.mValues.begin() + 5 );
    const std::vector<int> expected { 0, 1, 2, 3, 4 };
    EXPECT_EQ( firstFive, expected ) << "an equal-priority post cut into a running pass.";

    pump();
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that a pass ends even when every handler posts a higher-priority event.
//!
//! Preemption is bounded by the queue depth at the moment the pass took its batch. Without that
//! bound a producer posting high-priority events faster than they dispatch would keep one pass
//! running forever, and processEvents() would never return to its caller.
TEST( EventPriorityTest, APassEndsUnderAFloodOfHighPriorityPosts )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;
    for( int i = 0; i < 4; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( i ) ) );
    }

    // Every dispatch posts another high-priority event, for a while. If the pass were unbounded
    // this would not return.
    std::atomic<int> posted { 0 };
    receiver.mDuringDispatch = [&receiver, &posted]( int )
        {
            if( posted.fetch_add( 1 ) < 200 )
            {
                ( void )Object::postEvent( &receiver, new MarkEvent( 1000 ),
                    OverflowPolicy::DropNewest, EventPriority::kHigh );
            }
        };

    pump();

    // It returned, which is the assertion. Drain the rest so nothing is left for another test.
    receiver.mDuringDispatch = nullptr;
    for( int i = 0; i < 300; ++i )
    {
        pump();
    }

    EXPECT_EQ( gLivePriorityEvents.load(), before ) << "events were left queued or leaked.";
}

//! Tests that a full queue sheds its lowest priority rather than its front.
//!
//! The two features have to compose. A bound that evicted the front of a priority-ordered queue
//! would throw away the most important thing in it, and each feature would undo the other.
TEST( EventPriorityTest, DropOldestShedsTheLowestPriority )
{
    const int before = gLivePriorityEvents.load();

    ScopedCapacity capacity( 4 );
    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 99 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ) ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 50 ),
        OverflowPolicy::DropNewest, EventPriority::kLow ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 51 ),
        OverflowPolicy::DropNewest, EventPriority::kLow ) );

    // Full. This one has to displace something, and the something must be the oldest of the lowest
    // band -- 50 -- not the high-priority 99 at the front.
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ),
        OverflowPolicy::DropOldest, EventPriority::kNormal ) );

    pump();

    const std::vector<int> expected { 99, 1, 2, 51 };
    EXPECT_EQ( receiver.mValues, expected )
        << "DropOldest did not shed the oldest of the lowest priority band.";
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that an event too unimportant for the queue is refused rather than evicting its betters.
//!
//! **The case DropOldestShedsTheLowestPriority does not reach**, and the defect it hid: that test
//! only ever posts a newcomer at or above the band being shed, so the eviction always looks
//! correct. When the newcomer is *below* everything queued, the least important thing in play is
//! the newcomer, and it is the newcomer that must go -- otherwise a flood of unimportant work sheds
//! the important work it arrived behind, which inverts the ordering the policy exists to protect.
TEST( EventPriorityTest, DropOldestRefusesAnEventBeneathTheWholeQueue )
{
    const int before = gLivePriorityEvents.load();

    ScopedCapacity capacity( 2 );
    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 91 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 92 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );

    // Full, and every entry outranks what is arriving. DropOldest must refuse rather than make
    // room.
    EXPECT_FALSE( Object::postEvent( &receiver, new MarkEvent( 1 ),
        OverflowPolicy::DropOldest, EventPriority::kLow ) )
        << "a low-priority event was admitted by evicting a high-priority one.";

    pump();

    const std::vector<int> expected { 91, 92 };
    EXPECT_EQ( receiver.mValues, expected ) << "the queue lost an event it should have kept.";
    EXPECT_EQ( gLivePriorityEvents.load(), before ) << "the refused event was leaked.";
}

//! Tests that an equal priority may still displace, which is DropOldest doing its job.
//!
//! The boundary of the rule above: refusing at equal priority as well would turn DropOldest into
//! DropNewest for every queue whose entries share one band, which is most of them.
TEST( EventPriorityTest, DropOldestStillDisplacesWithinOneBand )
{
    const int before = gLivePriorityEvents.load();

    ScopedCapacity capacity( 2 );
    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );

    EXPECT_TRUE( Object::postEvent( &receiver, new MarkEvent( 3 ),
        OverflowPolicy::DropOldest, EventPriority::kHigh ) )
        << "DropOldest refused a newcomer of the same rank as the band it should shed.";

    pump();

    const std::vector<int> expected { 2, 3 };
    EXPECT_EQ( receiver.mValues, expected );
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that a high-priority event survives eviction while lower-priority ones are present.
TEST( EventPriorityTest, AHighPriorityEventIsNotEvicted )
{
    const int before = gLivePriorityEvents.load();

    ScopedCapacity capacity( 3 );
    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 99 ),
        OverflowPolicy::DropNewest, EventPriority::kHigh ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ) ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ) ) );

    for( int i = 0; i < 10; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 100 + i ),
            OverflowPolicy::DropOldest ) );
    }

    pump();

    ASSERT_FALSE( receiver.mValues.empty() );
    EXPECT_EQ( receiver.mValues[0], 99 )
        << "ten evictions took the one event that was marked as important.";
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that a deferred delete is still exempt from eviction whatever the priorities are.
TEST( EventPriorityTest, ADeferredDeleteIsStillExemptWithPriorities )
{
    const int before = gLivePriorityEvents.load();
    std::atomic<int> destroyed { 0 };

    ScopedCapacity capacity( 3 );

    //! A receiver whose destructor records that it ran.
    class DeleteProbe : public Object
    {
    public:
        explicit DeleteProbe
            (
            std::atomic<int>& aCounter
            )
            : mCounter( aCounter )
        {
        }

        ~DeleteProbe() override
        {
            ++mCounter;
        }

    private:
        std::atomic<int>& mCounter;
    };

    DeleteProbe* doomed = new DeleteProbe( destroyed );
    doomed->deleteLater();

    MarkRecorder receiver;
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ),
        OverflowPolicy::DropNewest, EventPriority::kLow ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ),
        OverflowPolicy::DropNewest, EventPriority::kLow ) );

    // Full, and the deferred delete shares the queue with two low-priority events. The eviction
    // must take one of those rather than the delete, whose priority is the default and therefore
    // higher than theirs.
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 3 ), OverflowPolicy::DropOldest ) );

    pump();

    EXPECT_EQ( destroyed.load(), 1 ) << "the deferred delete was evicted, which leaks its object.";
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that coalescing still matches on receiver and type, not on priority.
//!
//! Two posts of the same type for the same receiver coalesce even when their priorities differ:
//! the question Coalesce asks is "is this already pending", and a priority does not make it a
//! different request.
TEST( EventPriorityTest, CoalesceIgnoresPriority )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder receiver;

    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 1 ),
        OverflowPolicy::Coalesce, EventPriority::kNormal ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 2 ),
        OverflowPolicy::Coalesce, EventPriority::kHigh ) );
    ASSERT_TRUE( Object::postEvent( &receiver, new MarkEvent( 3, kOtherMarkType ),
        OverflowPolicy::Coalesce, EventPriority::kHigh ) );

    pump();

    ASSERT_EQ( receiver.mValues.size(), 2u ) << "coalescing was defeated by a priority.";

    // 3 first, because it was posted at kHigh and priority orders the queue. Coalescing decides
    // *whether* an event is queued; priority decides where. The second post of kMarkType was
    // collapsed into the first even though it asked for a higher priority -- a promotion does not
    // make it a different request, and it does not lift the event already pending either.
    EXPECT_EQ( receiver.mValues[0], 3 );
    EXPECT_EQ( receiver.mValues[1], 1 );
    EXPECT_EQ( gLivePriorityEvents.load(), before );
}

//! Tests that high-priority work queued mid-pass is cancelled when its receiver is destroyed.
//!
//! **Named for what it does, which is less than it first appears.** ~Object() calls
//! removeEventsForReceiver(), and that strips the live queue immediately -- so these entries are
//! gone before the dispatch loop can steal any of them, and the steal path is never entered. What
//! this checks is that posting high-priority work and destroying its receiver from inside a pass
//! leaks nothing and disturbs nothing, which is worth having and is not the same thing.
//!
//! Reaching the steal path *and then* cancelling would need the destruction to land between the
//! loop reading mEventQueue.front() and dispatching it, which is a window inside one mutex
//! acquisition and cannot be driven from a test. It is closed by construction rather than by
//! coverage: the entry is taken out of the queue under the same lock that a canceller needs.
TEST( EventPriorityTest, HighPriorityWorkDiesWithItsReceiver )
{
    const int before = gLivePriorityEvents.load();

    MarkRecorder keeper;
    MarkRecorder* doomed = new MarkRecorder();

    for( int i = 0; i < 6; ++i )
    {
        ASSERT_TRUE( Object::postEvent( &keeper, new MarkEvent( i ) ) );
    }

    // From inside the pass: queue high-priority work for an object, then destroy that object. The
    // preemption would otherwise pick those entries up after it had died.
    keeper.mDuringDispatch = [&keeper, &doomed]( int aValue )
        {
            if( aValue == 0 )
            {
                for( int i = 0; i < 4; ++i )
                {
                    ( void )Object::postEvent( doomed, new MarkEvent( 500 + i ),
                        OverflowPolicy::DropNewest, EventPriority::kHigh );
                }
                delete doomed;
                doomed = nullptr;
                keeper.mDuringDispatch = nullptr;
            }
        };

    pump();

    // The keeper's own batch still ran in full; the destroyed receiver's events were freed.
    ASSERT_EQ( keeper.mValues.size(), 6u ) << "destroying another receiver disturbed this batch.";
    EXPECT_EQ( gLivePriorityEvents.load(), before )
        << "events for a destroyed receiver were leaked by the preemption path.";
}
