// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for QtLikeSignal::Property: change notification, the equal-write rule, and the
//! coalesced mode that defers a notification to the end of the loop pass.

#include "QtLikeSignal/Property.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <string>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! Records every value a property reported, so a test can assert on how many arrived.
    class Watcher : public Object
    {
    public:
        //! Records one reported value.
        void onChanged
            (
            int aValue  //!< The value the property reported.
            )
        {
            mSeen.push_back( aValue );
        }

        std::vector<int> mSeen;   //!< Every value reported, in order.
    };

    //! Runs one pass of the calling thread's event loop, so a deferred notification is delivered.
    void drainOnePass()
    {
        Thread* const current = Thread::currentThread();
        ASSERT_NE( current, nullptr );
        current->processEvents( AbstractEventDispatcher::ProcessEventsFlag::AllEvents );
    }
}

//! Verifies a change is reported, once, with the new value.
TEST( PropertyTest, AChangeIsReportedOnce )
{
    CoreApplication app;

    Property<int> score;
    Watcher watcher;
    Object::connect( score.getChanged(), &watcher, &Watcher::onChanged );

    score.set( 10 );

    ASSERT_EQ( watcher.mSeen.size(), 1u );
    EXPECT_EQ( watcher.mSeen[0], 10 );
    EXPECT_EQ( score.get(), 10 );
}

//! Verifies a write of the value it already holds reports nothing.
//!
//! What makes a property safe to write from a loop that recomputes the same answer every pass,
//! which is most loops that write a property at all.
TEST( PropertyTest, AnEqualWriteIsNotAChange )
{
    CoreApplication app;

    Property<int> score( 7 );
    Watcher watcher;
    Object::connect( score.getChanged(), &watcher, &Watcher::onChanged );

    score.set( 7 );
    EXPECT_TRUE( watcher.mSeen.empty() ) << "writing the value it already had was reported.";

    score.set( 8 );
    score.set( 8 );
    ASSERT_EQ( watcher.mSeen.size(), 1u );
    EXPECT_EQ( watcher.mSeen[0], 8 );
}

//! Verifies the value is stored before the notification, not after.
//!
//! A slot generally reads more than the one value it was handed -- a label redrawing itself reads
//! every property it shows. If the write happened after the emission, that slot would see one new
//! value and the rest stale, which is the hardest kind of fault to find.
TEST( PropertyTest, TheValueIsVisibleWhileTheChangeIsBeingReported )
{
    CoreApplication app;

    Property<int> score;
    Object context;

    int seenDuringNotification = -1;
    Object::connect( score.getChanged(), &context, [&]( int )
        {
            seenDuringNotification = score.get();
        } );

    score.set( 42 );
    EXPECT_EQ( seenDuringNotification, 42 );
}

//! Verifies a property nobody has subscribed to still works, and reports no subscribers.
//!
//! hasSubscribers() is the observable form of "this property has not allocated its signal": the
//! signal is created by the first getChanged(), so a property no one watches carries a null
//! pointer and nothing else. That is the whole reason the signal is not held by value.
TEST( PropertyTest, APropertyNobodyWatchesStillWorks )
{
    CoreApplication app;

    Property<int> score;
    EXPECT_FALSE( score.hasSubscribers() );

    score.set( 3 );
    EXPECT_EQ( score.get(), 3 );
    EXPECT_FALSE( score.hasSubscribers() );

    Watcher watcher;
    Object::connect( score.getChanged(), &watcher, &Watcher::onChanged );
    EXPECT_TRUE( score.hasSubscribers() );
}

//! Verifies asking twice for the change signal gives the same signal.
//!
//! It is created on demand, so the obvious defect is creating a second one and leaving the first
//! set of subscribers connected to something nothing emits.
TEST( PropertyTest, TheChangeSignalIsCreatedOnceAndKept )
{
    CoreApplication app;

    Property<int> score;
    SignalView<int>& first  = score.getChanged();
    SignalView<int>& second = score.getChanged();
    EXPECT_EQ( &first, &second );

    Watcher watcher;
    Object::connect( first, &watcher, &Watcher::onChanged );
    score.set( 5 );
    EXPECT_EQ( watcher.mSeen.size(), 1u )
        << "the second getChanged() handed out a different signal.";
}

//! Verifies coalesced mode reports once for many writes, carrying the last value.
//!
//! The mode's whole purpose. A thousand writes in one pass of the loop are one notification, and
//! the value it carries is the one the property ended up with rather than the one it started at.
TEST( PropertyTest, CoalescedReportsOncePerPassWithTheLastValue )
{
    CoreApplication app;
    Object owner;

    Property<int> speed( &owner );
    speed.setNotifyMode( PropertyNotifyMode::Coalesced );
    ASSERT_EQ( speed.notifyMode(), PropertyNotifyMode::Coalesced );

    Watcher watcher;
    Object::connect( speed.getChanged(), &watcher, &Watcher::onChanged );

    for( int i = 1; i <= 1000; ++i )
    {
        speed.set( i );
    }

    EXPECT_TRUE( watcher.mSeen.empty() )
        << "a coalesced write reported before the loop had a chance to run.";
    EXPECT_EQ( speed.get(), 1000 ) << "the value itself must be written immediately.";

    drainOnePass();

    ASSERT_EQ( watcher.mSeen.size(), 1u );
    EXPECT_EQ( watcher.mSeen[0], 1000 )
        << "the deferred notification carried a value the property no longer had.";
}

//! Verifies coalesced mode is refused, rather than half-honoured, without an owner.
//!
//! The deduplication defers through Object::callLater(), which keys on a context object. A
//! property with no owner has none, and accepting the mode anyway would leave it notifying
//! immediately while reporting that it was coalescing.
TEST( PropertyTest, CoalescedIsRefusedWithoutAnOwner )
{
    CoreApplication app;

    Property<int> score;
    score.setNotifyMode( PropertyNotifyMode::Coalesced );

    EXPECT_EQ( score.notifyMode(), PropertyNotifyMode::Immediate )
        << "a mode the property cannot honour was accepted.";

    Watcher watcher;
    Object::connect( score.getChanged(), &watcher, &Watcher::onChanged );
    score.set( 1 );
    EXPECT_EQ( watcher.mSeen.size(), 1u ) << "the refusal did not leave the property immediate.";
}

//! Verifies a property holding a non-arithmetic type works, since T is not constrained to numbers.
TEST( PropertyTest, APropertyHoldsAnyEqualityComparableType )
{
    CoreApplication app;

    Property<std::string> title( std::string( "start" ) );
    Object context;

    std::string seen;
    int count = 0;
    Object::connect( title.getChanged(), &context, [&]( const std::string& aValue )
        {
            seen = aValue;
            ++count;
        } );

    title.set( "start" );
    EXPECT_EQ( count, 0 );

    title.set( "changed" );
    EXPECT_EQ( count, 1 );
    EXPECT_EQ( seen, "changed" );
}

//! Verifies the owner is remembered and reported.
TEST( PropertyTest, AnOwnerIsRememberedAndReported )
{
    CoreApplication app;
    Object owner;

    Property<int> withOwner( &owner, 3 );
    EXPECT_EQ( withOwner.owner(), &owner );
    EXPECT_EQ( withOwner.get(), 3 );

    Property<int> withoutOwner;
    EXPECT_EQ( withoutOwner.owner(), nullptr );
}
