// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for QtLikeSignal::Object's parent-child relationship -- mission.txt stage 5.5.
//!
//! Step 1 of the plan in ForAI/mission-parent-child-relationship.md establishes the links and keeps
//! them correct, but does **not** yet make a parent own its children: a destroyed parent orphans
//! them. The ownership tests arrive with step 2, and these must keep passing when they do.
//!
//! Deliberately parallel to what QtLikeSignal's QtLikeSignal-test-parent-child.cpp will be -- same tests,
//! same order, same names -- so the two can be diffed against each other once the port happens.
//! See history/TEST-UNIFICATION-PLAN-20260810.md.

#include "gtest/gtest.h"
#include "QtLikeSignal-test-types.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <array>
#include <string>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! Collects a parent's children in iteration order, so a test can assert the whole list rather
    //! than only its length.
    std::vector<Object*> childrenOf
        (
        const Object& aParent  //!< The parent to walk.
        )
    {
        std::vector<Object*> children;
        for( Object* child = aParent.firstChild(); child != nullptr;
            child = child->nextSibling() )
        {
            children.push_back( child );
        }
        return children;
    }

    //! True when @p aChild appears exactly once among @p aParent's children.
    bool hasChild
        (
        const Object& aParent,  //!< The parent to walk.
        const Object* aChild    //!< The child to look for.
        )
    {
        const std::vector<Object*> children = childrenOf( aParent );
        return std::count( children.begin(), children.end(), aChild ) == 1;
    }

    //! A heap child that reports its own lifetime through a counter.
    class CountedChild : public Object
    {
    public:
        //! The parent is optional and comes second, so that createChild<CountedChild>( parent,
        //! alive ) forwards only the counter and lets the factory do the attaching.
        explicit CountedChild
            (
            int& aAliveCount,            //!< Incremented on construction, decremented on destruction.
            Object* aParent = nullptr    //!< Parent to attach to, or null to be attached later.
            )
            : mAliveCount( aAliveCount )
        {
            ++mAliveCount;
            if( aParent != nullptr )
            {
                EXPECT_TRUE( setParent( aParent ) );
            }
        }

        virtual ~CountedChild() override
        {
            --mAliveCount;
        }

    private:
        int& mAliveCount;
    };

    //! A child whose destructor deletes one of its siblings -- the case Qt needs
    //! currentChildBeingDeleted for.
    class SiblingKiller : public Object
    {
    public:
        SiblingKiller
            (
            Object* aParent,   //!< Parent to attach to; takes ownership.
            int& aAliveCount,  //!< Lifetime counter, as CountedChild.
            Object* aVictim    //!< The sibling to delete on the way out.
            )
            : mAliveCount( aAliveCount )
            , mVictim( aVictim )
        {
            ++mAliveCount;
            EXPECT_TRUE( setParent( aParent ) );
        }

        virtual ~SiblingKiller() override
        {
            delete mVictim;
            --mAliveCount;
        }

    private:
        int& mAliveCount;
        Object* mVictim;
    };

    //! A child whose destructor tries to give the dying parent a new child.
    class LateAttacher : public Object
    {
    public:
        LateAttacher
            (
            Object* aParent,    //!< Parent to attach to; takes ownership.
            int& aAliveCount,   //!< Lifetime counter, as CountedChild.
            bool& aWasRefused   //!< Set true when the late attach is refused, as it must be.
            )
            : mAliveCount( aAliveCount )
            , mWasRefused( aWasRefused )
            , mRememberedParent( aParent )
        {
            ++mAliveCount;
            EXPECT_TRUE( setParent( aParent ) );
        }

        virtual ~LateAttacher() override
        {
            // Remembered at construction, because parent() is already null here: deleteChildren()
            // unlinks a child before deleting it, so a child's destructor cannot reach its parent
            // through the tree at all. Reading parent() here made an earlier version of this test
            // pass without ever attempting the attach.
            EXPECT_EQ( parent(), nullptr )
                << "a child still names its parent inside its own destructor.";

            // The parent is mid-destruction, so this must fail. Were it to succeed, the new child
            // would be appended to a list deleteChildren() is consuming and its loop would not end.
            Object latecomer;
            mWasRefused = !latecomer.setParent( mRememberedParent );
            --mAliveCount;
        }

    private:
        int& mAliveCount;
        bool& mWasRefused;
        Object* mRememberedParent;
    };

    //! A child that takes constructor arguments, so createChild()'s forwarding is exercised with
    //! something other than a default constructor.
    class NamedChild : public Object
    {
    public:
        NamedChild
            (
            int& aAliveCount,          //!< Lifetime counter, as CountedChild.
            const std::string& aName,  //!< Copied, to pin forwarding of an lvalue.
            int aNumber                //!< Moved, to pin forwarding of an rvalue.
            )
            : mAliveCount( aAliveCount )
            , mName( aName )
            , mNumber( aNumber )
        {
            ++mAliveCount;
        }

        virtual ~NamedChild() override
        {
            --mAliveCount;
        }

        const std::string& name() const
        {
            return mName;
        }

        int number() const
        {
            return mNumber;
        }

    private:
        int& mAliveCount;
        std::string mName;
        int mNumber;
    };

    //! A child whose destructor tries to createChild() on the parent that is destroying it.
    //!
    //! The refused-attach path, reached through the factory rather than through setParent(). What
    //! matters is that the object createChild() allocated is freed rather than stranded, which is
    //! what its unique_ptr is there for.
    class DyingParentProbe : public Object
    {
    public:
        DyingParentProbe
            (
            int& aAliveCount,   //!< Lifetime counter handed to the child it will try to create.
            int& aAttempts      //!< Incremented when the destructor makes its attempt.
            )
            : mAliveCount( aAliveCount )
            , mAttempts( aAttempts )
            , mRememberedParent( nullptr )
        {
        }

        //! Called by the test once the factory has attached this, since the constructor runs before
        //! createChild() does the attaching and therefore cannot see the parent either.
        void rememberParent()
        {
            mRememberedParent = parent();
        }

        virtual ~DyingParentProbe() override
        {
            // Remembered rather than read from parent(), which is already null here; see
            // LateAttacher for why.
            ++mAttempts;
            EXPECT_EQ( Object::createChild<CountedChild>( mRememberedParent, mAliveCount ), nullptr
                     )
                << "createChild() attached to a parent that was being destroyed.";
        }

    private:
        int& mAliveCount;
        int& mAttempts;
        Object* mRememberedParent;
    };

    //! A child whose slot records the thread it ran on, for the queued-delivery test.
    class ThreadRecorder : public Object
    {
    public:
        //! Records @p aValue and the thread this ran on, then releases waitForOneCall().
        void record
            (
            int aValue  //!< The emitted value, kept so the call can be identified.
            )
        {
            {
                std::lock_guard<std::mutex> lock( mMutex );
                mRanOn = Thread::currentThread();
                mValue = aValue;
                mCalled = true;
            }
            mCv.notify_one();
        }

        //! Blocks until record() has run once. @return false on timeout.
        bool waitForOneCall()
        {
            std::unique_lock<std::mutex> lock( mMutex );
            return mCv.wait_for( lock, std::chrono::seconds( 5 ), [this]
                {
                    return mCalled;
                } );
        }

        Thread* ranOn() const
        {
            std::lock_guard<std::mutex> lock( mMutex );
            return mRanOn;
        }

        int value() const
        {
            std::lock_guard<std::mutex> lock( mMutex );
            return mValue;
        }

    private:
        mutable std::mutex mMutex;
        std::condition_variable mCv;
        Thread* mRanOn { nullptr };
        int mValue { 0 };
        bool mCalled { false };
    };

    //! A distinct Object subtype, so findChild<T>() has something to discriminate on.
    class OtherKind : public Object
    {
    public:
        explicit OtherKind
            (
            int& aAliveCount,            //!< Lifetime counter, as CountedChild.
            Object* aParent = nullptr    //!< Parent to attach to, or null to be attached later.
            )
            : mAliveCount( aAliveCount )
        {
            ++mAliveCount;
            if( aParent != nullptr )
            {
                EXPECT_TRUE( setParent( aParent ) );
            }
        }

        virtual ~OtherKind() override
        {
            --mAliveCount;
        }

    private:
        int& mAliveCount;
    };

    //! Thrown by ThrowingChild, so the test can catch something it recognises.
    struct ChildConstructionFailed
    {
    };

    //! A child whose constructor builds a subtree of its own and then throws.
    //!
    //! The Object base is fully constructed by the time the body runs, so ~Object() runs on the way
    //! out even though ~ThrowingChild() does not -- and that is what has to destroy the grandchildren
    //! already attached. Attach-as-you-build is only safe if that holds.
    class ThrowingChild : public Object
    {
    public:
        ThrowingChild
            (
            int& aAliveCount,      //!< Lifetime counter for the grandchildren it builds.
            int aChildrenBefore    //!< How many children to attach before throwing.
            )
        {
            for( int i = 0; i < aChildrenBefore; ++i )
            {
                ( void )Object::createChild<CountedChild>( this, aAliveCount );
            }
            throw ChildConstructionFailed();
        }

    };
}

//! A fresh object is in no tree at all, and asking costs nothing and creates nothing.
TEST( ParentChildTest, DefaultsToNoParentAndNoChildren )
{
    Object object;

    EXPECT_EQ( object.parent(), nullptr );
    EXPECT_EQ( object.firstChild(), nullptr );
    EXPECT_EQ( object.nextSibling(), nullptr );
    EXPECT_EQ( object.childCount(), 0u );
}

//! The basic link: setParent() puts the child in the parent's list and names the parent.
TEST( ParentChildTest, SetParentLinksBothDirections )
{
    Object parent;
    Object child;

    EXPECT_TRUE( child.setParent( &parent ) );

    EXPECT_EQ( child.parent(), &parent );
    EXPECT_EQ( parent.firstChild(), &child );
    EXPECT_EQ( parent.childCount(), 1u );
    EXPECT_EQ( child.childCount(), 0u );
}

//! Several children all appear, and the list is walkable end to end.
TEST( ParentChildTest, SeveralChildrenAreAllReachable )
{
    Object parent;
    Object first;
    Object second;
    Object third;

    ASSERT_TRUE( first.setParent( &parent ) );
    ASSERT_TRUE( second.setParent( &parent ) );
    ASSERT_TRUE( third.setParent( &parent ) );

    EXPECT_EQ( parent.childCount(), 3u );
    EXPECT_TRUE( hasChild( parent, &first ) );
    EXPECT_TRUE( hasChild( parent, &second ) );
    EXPECT_TRUE( hasChild( parent, &third ) );
}

//! Setting the parent an object already has is a no-op that succeeds, rather than a second link.
//!
//! The same shape as moveToThread() returning true for a move to the thread the object is already
//! in. A duplicate entry here would make childCount() wrong and detach leave half a link behind.
TEST( ParentChildTest, SettingTheSameParentTwiceDoesNotDuplicate )
{
    Object parent;
    Object child;

    ASSERT_TRUE( child.setParent( &parent ) );
    EXPECT_TRUE( child.setParent( &parent ) );

    EXPECT_EQ( parent.childCount(), 1u );
    EXPECT_TRUE( hasChild( parent, &child ) );
}

//! setParent(nullptr) detaches, and detaching twice is harmless.
TEST( ParentChildTest, SetParentNullDetaches )
{
    Object parent;
    Object child;

    ASSERT_TRUE( child.setParent( &parent ) );
    EXPECT_TRUE( child.setParent( nullptr ) );

    EXPECT_EQ( child.parent(), nullptr );
    EXPECT_EQ( parent.childCount(), 0u );
    EXPECT_EQ( parent.firstChild(), nullptr );

    EXPECT_TRUE( child.setParent( nullptr ) );
    EXPECT_EQ( child.parent(), nullptr );
}

//! Re-homing at runtime: the child leaves one list and joins the other, with neither left wrong.
//!
//! This is the strength QObject's design has over type-encoded ownership, so it gets a test of its
//! own rather than being implied by the two halves.
TEST( ParentChildTest, ReParentingMovesTheChildBetweenLists )
{
    Object oldParent;
    Object newParent;
    Object child;

    ASSERT_TRUE( child.setParent( &oldParent ) );
    EXPECT_TRUE( child.setParent( &newParent ) );

    EXPECT_EQ( child.parent(), &newParent );
    EXPECT_EQ( oldParent.childCount(), 0u );
    EXPECT_EQ( newParent.childCount(), 1u );
    EXPECT_TRUE( hasChild( newParent, &child ) );
}

//! Unlinking from the middle of the list repairs both neighbours.
//!
//! The case where mPrevSibling and mNextSibling are both non-null, which the first-child and
//! last-child cases above do not reach. An intrusive list gets this wrong quietly: the count stays
//! plausible while the chain is broken, so the walk is asserted, not just the length.
TEST( ParentChildTest, DetachingFromTheMiddleKeepsTheListIntact )
{
    Object parent;
    Object first;
    Object middle;
    Object last;

    ASSERT_TRUE( first.setParent( &parent ) );
    ASSERT_TRUE( middle.setParent( &parent ) );
    ASSERT_TRUE( last.setParent( &parent ) );
    ASSERT_EQ( parent.childCount(), 3u );

    ASSERT_TRUE( middle.setParent( nullptr ) );

    EXPECT_EQ( parent.childCount(), 2u );
    EXPECT_TRUE( hasChild( parent, &first ) );
    EXPECT_TRUE( hasChild( parent, &last ) );
    EXPECT_FALSE( hasChild( parent, &middle ) );
}

//! A destroyed child takes itself out of its parent's list.
//!
//! ~Object() unlinks from the parent as its second act, before anything else it does. Without that
//! the parent would keep walking into freed memory -- which ASan is what proves, so this test is
//! worth more under a sanitizer than under a plain build.
TEST( ParentChildTest, DestroyingAChildUnlinksItFromTheParent )
{
    Object parent;
    Object survivor;

    ASSERT_TRUE( survivor.setParent( &parent ) );
    {
        Object doomed;
        ASSERT_TRUE( doomed.setParent( &parent ) );
        ASSERT_EQ( parent.childCount(), 2u );
    }

    EXPECT_EQ( parent.childCount(), 1u );
    EXPECT_EQ( parent.firstChild(), &survivor );
    EXPECT_EQ( survivor.nextSibling(), nullptr );
}

//! A destroyed parent destroys its children. This is what the tree is for.
//!
//! Replaces DestroyingAParentOrphansItsChildrenForNow, which pinned step 1's interim behaviour --
//! that a parent merely let its children go -- and which could not survive step 2 in its old form:
//! it attached *stack* objects and let the parent die first, which now means `delete` on automatic
//! storage.
TEST( ParentChildTest, DestroyingAParentDestroysItsChildren )
{
    int aliveCount = 0;
    {
        Object parent;
        // Heap, and deliberately owned by nothing else: the parent owns them from here.
        ( void )new CountedChild( aliveCount, &parent );
        ( void )new CountedChild( aliveCount, &parent );
        ASSERT_EQ( parent.childCount(), 2u );
        ASSERT_EQ( aliveCount, 2 );
    }

    EXPECT_EQ( aliveCount, 0 ) << "the parent did not destroy its children.";
}

//! Ownership reaches the whole subtree, not only the immediate children.
TEST( ParentChildTest, DestroyingAParentDestroysAWholeSubtree )
{
    int aliveCount = 0;
    {
        Object root;
        Object* branch = new CountedChild( aliveCount, &root );
        Object* leaf   = new CountedChild( aliveCount, branch );
        ( void )new CountedChild( aliveCount, leaf );
        ASSERT_EQ( aliveCount, 3 );
        ASSERT_EQ( root.childCount(), 1u );
    }

    EXPECT_EQ( aliveCount, 0 ) << "destruction stopped before the bottom of the subtree.";
}

//! A child destructor may delete one of its siblings, and the teardown survives it.
//!
//! Qt needs QObjectPrivate::currentChildBeingDeleted for this, because its deleteChildren() walks a
//! QList by index and has to mark the slot it is inside. The intrusive list here has no index to
//! invalidate: deleteChildren() unlinks a child *before* deleting it, so the list is wholly
//! consistent while the destructor runs and the sibling unlinks itself in the ordinary way.
TEST( ParentChildTest, AChildDestructorMayDeleteASibling )
{
    int aliveCount = 0;
    {
        Object parent;
        Object* victim = new CountedChild( aliveCount, &parent );
        ( void )new SiblingKiller( &parent, aliveCount, victim );
        ( void )new CountedChild( aliveCount, &parent );
        ASSERT_EQ( parent.childCount(), 3u );
        ASSERT_EQ( aliveCount, 3 );
    }

    EXPECT_EQ( aliveCount, 0 ) << "a sibling-deleting destructor left something behind.";
}

//! A child destructor cannot attach a new child to the parent that is destroying it.
//!
//! ~Object() clears the life flag before deleteChildren() runs and setParent() refuses a dead
//! parent, so the attach fails rather than appending to a list that is being consumed. That refusal
//! is what makes deleteChildren()'s loop terminate. Qt tolerates the append instead, by re-reading
//! children.size() on every iteration.
TEST( ParentChildTest, AChildDestructorCannotAttachToTheDyingParent )
{
    int aliveCount = 0;
    bool attachRefused = false;
    {
        Object parent;
        ( void )new LateAttacher( &parent, aliveCount, attachRefused );
        ASSERT_EQ( parent.childCount(), 1u );
    }

    EXPECT_TRUE( attachRefused ) << "setParent() accepted a parent that was being destroyed.";
    EXPECT_EQ( aliveCount, 0 );
}

//! A stack child destroyed before its parent is still fine, and must stay fine.
//!
//! The tree calls `delete` on a child, so a child has to be heap-allocated for a parent to outlive
//! it -- and standard C++ cannot ask whether a pointer names automatic storage, so nothing can
//! check that (see Object::deleteChildren()). What remains legal is the reverse order: a stack
//! object may be attached as long as it dies first, because ~Object() unlinks it from its parent.
//! Most of the link tests above rely on exactly that, by declaring the child after the parent, and
//! this pins it so that reordering those declarations does not quietly become a delete of automatic
//! storage.
TEST( ParentChildTest, AStackChildDestroyedBeforeItsParentIsFine )
{
    Object parent;
    {
        Object stackChild;
        ASSERT_TRUE( stackChild.setParent( &parent ) );
        ASSERT_EQ( parent.childCount(), 1u );
    }

    EXPECT_EQ( parent.childCount(), 0u ) << "the stack child did not unlink itself on the way out.";
    EXPECT_EQ( parent.firstChild(), nullptr );
}

//! An object may not be its own parent, and the refusal changes nothing.
TEST( ParentChildTest, SelfParentIsRefused )
{
    Object object;

    EXPECT_FALSE( object.setParent( &object ) );
    EXPECT_EQ( object.parent(), nullptr );
    EXPECT_EQ( object.childCount(), 0u );
}

//! A cycle through a descendant is refused too, at any depth.
//!
//! Qt looks for this only in debug builds, only past depth 4096, and only warns
//! (CheckForParentChildLoopsWarnDepth). A cycle makes destruction recurse forever, so it is refused
//! here in every build.
TEST( ParentChildTest, CycleThroughADescendantIsRefused )
{
    Object grandParent;
    Object parent;
    Object child;

    ASSERT_TRUE( parent.setParent( &grandParent ) );
    ASSERT_TRUE( child.setParent( &parent ) );

    EXPECT_FALSE( grandParent.setParent( &child ) );

    EXPECT_EQ( grandParent.parent(), nullptr );
    EXPECT_EQ( parent.parent(), &grandParent );
    EXPECT_EQ( child.parent(), &parent );
}

//! A refused re-parent keeps the parent the object already had.
//!
//! The point of departure from Qt, which on a refused re-parent leaves the object with **no**
//! parent at all (qobject.cpp:2341) -- neither honouring the request nor keeping the previous
//! state, so a caller that does not watch stderr silently leaks the object.
TEST( ParentChildTest, RefusedReParentKeepsTheExistingParent )
{
    Object originalParent;
    Object object;
    Object descendant;

    ASSERT_TRUE( object.setParent( &originalParent ) );
    ASSERT_TRUE( descendant.setParent( &object ) );

    EXPECT_FALSE( object.setParent( &descendant ) );

    EXPECT_EQ( object.parent(), &originalParent );
    EXPECT_EQ( originalParent.childCount(), 1u );
    EXPECT_TRUE( hasChild( originalParent, &object ) );
}

//! Depth is not limited, and a deep chain unwinds without help.
TEST( ParentChildTest, DeepChainsLinkAndUnlink )
{
    constexpr int kDepth = 64;
    std::array<Object, kDepth> chain;

    for( int i = 1; i < kDepth; ++i )
    {
        ASSERT_TRUE( chain[i].setParent( &chain[i - 1] ) );
    }

    EXPECT_EQ( chain[0].parent(), nullptr );
    for( int i = 1; i < kDepth; ++i )
    {
        EXPECT_EQ( chain[i].parent(), &chain[i - 1] );
        EXPECT_EQ( chain[i - 1].childCount(), 1u );
    }

    // Every link must be refused, not only the immediate one: the walk is from the proposed parent
    // upwards, so the deepest node is the furthest the loop check has to travel.
    EXPECT_FALSE( chain[0].setParent( &chain[kDepth - 1] ) );
    EXPECT_EQ( chain[0].parent(), nullptr );
}

//! The name and the tree share one lazily-allocated box, so using either must not disturb the
//! other.
//!
//! The four tree pointers moved into Object::Extras alongside mObjectName and the timer list (D2 of
//! the plan), which is what keeps an object outside any tree paying nothing for one. That sharing
//! is invisible from outside and is exactly the kind of thing a later refactor breaks silently.
TEST( ParentChildTest, ObjectNameAndTreeShareTheExtrasBoxWithoutInterfering )
{
    Object parent;
    Object child;

    child.setObjectName( "child" );
    ASSERT_TRUE( child.setParent( &parent ) );
    parent.setObjectName( "parent" );

    EXPECT_EQ( child.objectName(), "child" );
    EXPECT_EQ( parent.objectName(), "parent" );
    EXPECT_EQ( child.parent(), &parent );

    ASSERT_TRUE( child.setParent( nullptr ) );
    EXPECT_EQ( child.objectName(), "child" );
    EXPECT_EQ( parent.objectName(), "parent" );
}

//! createChild() builds the child, attaches it, and hands back a usable pointer.
TEST( ParentChildTest, CreateChildBuildsAndAttaches )
{
    int aliveCount = 0;
    {
        Object parent;
        CountedChild* child = Object::createChild<CountedChild>( &parent, aliveCount );

        ASSERT_NE( child, nullptr );
        EXPECT_EQ( child->parent(), &parent );
        EXPECT_EQ( parent.childCount(), 1u );
        EXPECT_TRUE( hasChild( parent, child ) );
        EXPECT_EQ( aliveCount, 1 );
    }

    EXPECT_EQ( aliveCount, 0 ) << "the parent did not own what createChild() gave it.";
}

//! Constructor arguments are forwarded, lvalues and rvalues alike.
TEST( ParentChildTest, CreateChildForwardsConstructorArguments )
{
    int aliveCount = 0;
    Object parent;

    const std::string name = "widget";
    NamedChild* child = Object::createChild<NamedChild>( &parent, aliveCount, name, 42 );

    ASSERT_NE( child, nullptr );
    EXPECT_EQ( child->name(), "widget" );
    EXPECT_EQ( child->number(), 42 );
    EXPECT_EQ( name, "widget" ) << "the lvalue argument was moved from rather than copied.";
    EXPECT_EQ( child->parent(), &parent );
}

//! A subtree built entirely through createChild() is destroyed by destroying its root.
TEST( ParentChildTest, CreateChildBuildsASubtreeTheRootOwns )
{
    int aliveCount = 0;
    {
        Object root;
        Object* branch = Object::createChild<CountedChild>( &root, aliveCount );
        ASSERT_NE( branch, nullptr );
        ( void )Object::createChild<CountedChild>( branch, aliveCount );
        ( void )Object::createChild<CountedChild>( branch, aliveCount );
        ASSERT_EQ( aliveCount, 3 );
    }

    EXPECT_EQ( aliveCount, 0 );
}

//! A null parent is refused, and nothing is constructed.
//!
//! The name says child, so there is no parent to be a child of and the call is a misuse rather than
//! a request to build a free-standing object. Refused rather than silently allocating something
//! nobody owns.
TEST( ParentChildTest, CreateChildRefusesANullParent )
{
    int aliveCount = 0;
    CountedChild* child = Object::createChild<CountedChild>( nullptr, aliveCount );

    EXPECT_EQ( child, nullptr );
    EXPECT_EQ( aliveCount, 0 ) << "something was constructed for a parent that does not exist.";
}

//! A parent already being destroyed refuses the child, and the child is freed rather than leaked.
//!
//! The one refusal a correctly written call can actually hit: a brand new object cannot close a
//! cycle, so the dying-parent case is the whole of what the unique_ptr inside createChild() is for.
//! Worth more under AddressSanitizer than under a plain build, which is where the leak would show.
TEST( ParentChildTest, CreateChildOnADyingParentFreesTheChild )
{
    int aliveCount = 0;
    int attemptedDuringTeardown = 0;
    {
        Object parent;
        DyingParentProbe* probe = Object::createChild<DyingParentProbe>( &parent, aliveCount,
            attemptedDuringTeardown );
        ASSERT_NE( probe, nullptr );
        probe->rememberParent();
    }

    EXPECT_EQ( attemptedDuringTeardown, 1 ) << "the probe never ran.";
    EXPECT_EQ( aliveCount, 0 ) << "the refused child was leaked instead of freed.";
}

//! A constructor that throws part-way leaves nothing behind.
//!
//! The plan's exception-safety requirement, and the reason attach-as-you-build is safe. The Object
//! base is constructed before the body runs, so ~Object() runs on the way out and destroys the
//! grandchildren already attached, even though the derived destructor never runs.
TEST( ParentChildTest, CreateChildCleansUpWhenTheConstructorThrows )
{
    int aliveCount = 0;
    Object parent;

    EXPECT_THROW(
        ( void )Object::createChild<ThrowingChild>( &parent, aliveCount, 3 ),
        ChildConstructionFailed );

    EXPECT_EQ( aliveCount, 0 ) << "a throwing constructor stranded the children it had attached.";
    EXPECT_EQ( parent.childCount(), 0u ) << "a child that never finished constructing is attached.";
}

// =================================================================================================
// Thread affinity. Step 4: the tree is what carries affinity, and the invariant that a child lives
// in its parent's thread is enforced rather than merely documented.
// =================================================================================================

//! Moving a parent moves its whole subtree, in one call.
//!
//! This is the strength the tree buys: the invariant that makes queued delivery sound is maintained
//! by the same structure that expresses ownership. Qt recurses over children in
//! moveToThread_helper() for the same reason.
TEST( ParentChildTest, MovingAParentMovesTheWholeSubtree )
{
    int aliveCount = 0;
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Object root;
    Object* branch = Object::createChild<CountedChild>( &root, aliveCount );
    ASSERT_NE( branch, nullptr );
    Object* leaf = Object::createChild<CountedChild>( branch, aliveCount );
    ASSERT_NE( leaf, nullptr );

    ASSERT_TRUE( root.moveToThread( &worker ) );

    EXPECT_EQ( root.thread(), &worker );
    EXPECT_EQ( branch->thread(), &worker ) << "a child was left behind in the old thread.";
    EXPECT_EQ( leaf->thread(), &worker ) << "the move stopped before the bottom of the subtree.";

    // No move back: moveToThread() is push-only, so the test's thread cannot pull an object out
    // of the worker. It does not need to. Once the loop has stopped there is nothing left to race,
    // and ~Object() says so -- its cross-thread diagnostic is gated on the owning thread still
    // running, which is why quit()/wait()-then-destroy is the idiom the whole suite uses.
    worker.quit();
    worker.wait();
}

//! A child cannot be moved on its own; the parent has to be moved instead.
//!
//! Qt refuses this too (qobject.cpp:1715). Without it a parent and a child could end up in
//! different threads, and the child's links -- which are deliberately unguarded, because the tree is
//! thread-confined -- would be reachable from both.
TEST( ParentChildTest, AChildCannotBeMovedOnItsOwn )
{
    int aliveCount = 0;
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Object parent;
    Object* child = Object::createChild<CountedChild>( &parent, aliveCount );
    ASSERT_NE( child, nullptr );
    Thread* const before = child->thread();

    EXPECT_FALSE( child->moveToThread( &worker ) );

    EXPECT_EQ( child->thread(), before ) << "the refused move changed the affinity anyway.";
    EXPECT_EQ( child->parent(), &parent ) << "the refused move disturbed the tree.";
    EXPECT_EQ( parent.thread(), before );

    worker.quit();
    worker.wait();
}

//! Detaching first is what makes the move legal, and it works.
//!
//! The escape hatch the refusal above points at, pinned so the advice in that message stays true.
TEST( ParentChildTest, ADetachedChildMayBeMoved )
{
    int aliveCount = 0;
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Object parent;
    CountedChild* child = Object::createChild<CountedChild>( &parent, aliveCount );
    ASSERT_NE( child, nullptr );

    ASSERT_TRUE( child->setParent( nullptr ) );
    EXPECT_TRUE( child->moveToThread( &worker ) );
    EXPECT_EQ( child->thread(), &worker );

    // Safe once the loop has stopped; see MovingAParentMovesTheWholeSubtree.
    worker.quit();
    worker.wait();

    // Detached, so nothing owns it and the test has to.
    delete child;
    EXPECT_EQ( aliveCount, 0 );
}

//! A parent in another thread is refused, and the existing parent is kept.
//!
//! The point of departure from Qt, which refuses the same link (qobject.cpp:2341) but leaves the
//! object with **no** parent at all -- neither honouring the request nor keeping the previous state,
//! so a caller that does not read stderr silently leaks whatever the old parent was going to free.
TEST( ParentChildTest, ACrossThreadParentIsRefusedAndTheOldParentKept )
{
    int aliveCount = 0;
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Object originalParent;
    CountedChild* child = Object::createChild<CountedChild>( &originalParent, aliveCount );
    ASSERT_NE( child, nullptr );

    // A parent that lives somewhere else. Built here and pushed over, which is the only direction
    // moveToThread() allows.
    Object stranger;
    ASSERT_TRUE( stranger.moveToThread( &worker ) );
    ASSERT_NE( stranger.thread(), child->thread() );

    EXPECT_FALSE( child->setParent( &stranger ) );

    EXPECT_EQ( child->parent(), &originalParent ) << "the refused re-parent orphaned the child.";
    EXPECT_EQ( originalParent.childCount(), 1u );
    EXPECT_EQ( stranger.childCount(), 0u );

    worker.quit();
    worker.wait();
}

//! createChild() inherits the refusal, since it attaches through setParent().
TEST( ParentChildTest, CreateChildRefusesAParentInAnotherThread )
{
    int aliveCount = 0;
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Object stranger;
    ASSERT_TRUE( stranger.moveToThread( &worker ) );

    EXPECT_EQ( Object::createChild<CountedChild>( &stranger, aliveCount ), nullptr );
    EXPECT_EQ( aliveCount, 0 ) << "the refused child was leaked instead of freed.";
    EXPECT_EQ( stranger.childCount(), 0u );

    worker.quit();
    worker.wait();
}

//! A queued call to a moved child is delivered on the thread the subtree moved to.
//!
//! The affinity is not merely reported correctly, it is *used*: emit resolves the receiver's thread
//! at emit time, so a child carried along by its parent's move has to receive there and not on the
//! thread it was built in.
TEST( ParentChildTest, QueuedDeliveryToAMovedChildLandsOnTheNewThread )
{
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Object root;
    ThreadRecorder* child = Object::createChild<ThreadRecorder>( &root );
    ASSERT_NE( child, nullptr );

    Signal<int> signal;
    const Connection connection = Object::connect( signal, child, &ThreadRecorder::record );
    ASSERT_TRUE( connection.connected() );

    ASSERT_TRUE( root.moveToThread( &worker ) );

    signal.emit( 7 );
    ASSERT_TRUE( child->waitForOneCall() ) << "the queued call never arrived.";
    EXPECT_EQ( child->ranOn(), &worker ) << "the slot ran on the thread the child was built in.";
    EXPECT_EQ( child->value(), 7 );

    worker.quit();
    worker.wait();
}

// =================================================================================================
// Reflection. Step 5: findChild, findChildren, dumpObjectTree.
// =================================================================================================

//! findChild() searches the whole subtree, not just the immediate children.
TEST( ParentChildTest, FindChildSearchesTheWholeSubtree )
{
    int aliveCount = 0;
    Object root;
    Object* branch = Object::createChild<CountedChild>( &root, aliveCount );
    ASSERT_NE( branch, nullptr );
    OtherKind* deep = Object::createChild<OtherKind>( branch, aliveCount );
    ASSERT_NE( deep, nullptr );

    EXPECT_EQ( root.findChild<OtherKind>(), deep )
        << "findChild() stopped at the immediate children.";
    EXPECT_EQ( branch->findChild<OtherKind>(), deep );
    EXPECT_EQ( deep->findChild<OtherKind>(), nullptr ) << "a node found itself.";
}

//! findChild() discriminates on type.
TEST( ParentChildTest, FindChildDiscriminatesOnType )
{
    int aliveCount = 0;
    Object root;
    ( void )Object::createChild<CountedChild>( &root, aliveCount );
    OtherKind* other = Object::createChild<OtherKind>( &root, aliveCount );
    ASSERT_NE( other, nullptr );

    EXPECT_EQ( root.findChild<OtherKind>(), other );
    EXPECT_NE( root.findChild<CountedChild>(), nullptr );
    EXPECT_NE( root.findChild<Object>(), nullptr ) << "the base type should match anything.";
}

//! An empty name matches any name; a given name has to match.
//!
//! Same rule as QObject::findChild(), where a null QString matches anything.
TEST( ParentChildTest, FindChildMatchesOnName )
{
    int aliveCount = 0;
    Object root;
    CountedChild* wanted = Object::createChild<CountedChild>( &root, aliveCount );
    ASSERT_NE( wanted, nullptr );
    wanted->setObjectName( "wanted" );
    CountedChild* other = Object::createChild<CountedChild>( &root, aliveCount );
    ASSERT_NE( other, nullptr );
    other->setObjectName( "other" );

    EXPECT_EQ( root.findChild<CountedChild>( "wanted" ), wanted );
    EXPECT_EQ( root.findChild<CountedChild>( "other" ), other );
    EXPECT_EQ( root.findChild<CountedChild>( "absent" ), nullptr );
    EXPECT_NE( root.findChild<CountedChild>(), nullptr ) << "an empty name should match anything.";
}

//! findChildren() collects every match in the subtree.
TEST( ParentChildTest, FindChildrenCollectsEveryMatch )
{
    int aliveCount = 0;
    Object root;
    Object* branch = Object::createChild<CountedChild>( &root, aliveCount );
    ASSERT_NE( branch, nullptr );
    ( void )Object::createChild<CountedChild>( branch, aliveCount );
    ( void )Object::createChild<OtherKind>( &root, aliveCount );

    EXPECT_EQ( root.findChildren<CountedChild>().size(), 2u );
    EXPECT_EQ( root.findChildren<OtherKind>().size(), 1u );
    EXPECT_EQ( root.findChildren<Object>().size(), 3u );
    EXPECT_TRUE( root.findChildren<OtherKind>( "absent" ).empty() );
}

//! Both are empty on an object with no children, and neither creates an extras box to find out.
TEST( ParentChildTest, FindOnALeafIsEmpty )
{
    Object leaf;

    EXPECT_EQ( leaf.findChild<Object>(), nullptr );
    EXPECT_TRUE( leaf.findChildren<Object>().empty() );
    EXPECT_EQ( leaf.childCount(), 0u );
}

//! dumpObjectTree() walks the subtree without disturbing it.
//!
//! It writes to stderr, so what it prints is not asserted here; what matters is that it visits a
//! tree of some depth and leaves it exactly as it found it.
TEST( ParentChildTest, DumpObjectTreeLeavesTheTreeAlone )
{
    int aliveCount = 0;
    Object root;
    root.setObjectName( "root" );
    Object* branch = Object::createChild<CountedChild>( &root, aliveCount );
    ASSERT_NE( branch, nullptr );
    branch->setObjectName( "branch" );
    ( void )Object::createChild<CountedChild>( branch, aliveCount );

    root.dumpObjectTree();

    EXPECT_EQ( root.childCount(), 1u );
    EXPECT_EQ( branch->childCount(), 1u );
    EXPECT_EQ( aliveCount, 2 );
}

// =================================================================================================
// Ownership rules, pinned. These are the claims the README makes about what a caller may and may
// not do with a parented object, and they are tested rather than asserted.
// =================================================================================================

//! Deleting a child directly is safe: it unlinks itself, and the parent does not double-free it.
//!
//! Worth pinning because it is the opposite of what a reader might reasonably assume from "the
//! parent owns its children". ~Object() detaches from the parent before anything else, so by the
//! time the parent's own teardown runs the child is simply not in the list.
TEST( ParentChildTest, DeletingAChildDirectlyIsSafe )
{
    int aliveCount = 0;
    {
        Object parent;
        CountedChild* child = Object::createChild<CountedChild>( &parent, aliveCount );
        ASSERT_NE( child, nullptr );
        CountedChild* survivor = Object::createChild<CountedChild>( &parent, aliveCount );
        ASSERT_NE( survivor, nullptr );
        ASSERT_EQ( aliveCount, 2 );

        delete child;

        EXPECT_EQ( aliveCount, 1 ) << "the direct delete did not run.";
        EXPECT_EQ( parent.childCount(), 1u ) << "the deleted child is still in the parent's list.";
        EXPECT_EQ( parent.firstChild(), survivor );
    }

    EXPECT_EQ( aliveCount, 0 ) << "the parent double-freed a child that was already deleted.";
}

//! deleteLater() on a child is safe for the same reason, and the parent does not double-free it.
TEST( ParentChildTest, DeleteLaterOnAChildIsSafe )
{
    int aliveCount = 0;
    {
        Object parent;
        CountedChild* child = Object::createChild<CountedChild>( &parent, aliveCount );
        ASSERT_NE( child, nullptr );
        ASSERT_EQ( aliveCount, 1 );

        child->deleteLater();
        // Deferred deletes run from the event loop, so drain this thread's queue once.
        Thread* const here = Thread::currentThread();
        ASSERT_NE( here, nullptr );
        here->processEvents();

        EXPECT_EQ( aliveCount, 0 ) << "the deferred delete never ran.";
        EXPECT_EQ( parent.childCount(), 0u ) << "the deleted child is still in the parent's list.";
    }

    EXPECT_EQ( aliveCount, 0 );
}

//! A pending deleteLater() on a child the parent destroys first is dropped, not left dangling.
//!
//! The other order of the same race. ~Object() strips the object's queued events, so the deferred
//! delete does not fire afterwards against freed memory.
TEST( ParentChildTest, APendingDeleteLaterOnADestroyedChildIsDropped )
{
    int aliveCount = 0;
    {
        Object parent;
        CountedChild* child = Object::createChild<CountedChild>( &parent, aliveCount );
        ASSERT_NE( child, nullptr );
        child->deleteLater();
        // Parent goes out of scope with the deferred delete still queued.
    }
    EXPECT_EQ( aliveCount, 0 );

    // Drain with a marker in the queue rather than on its own. processEvents() blocks when there
    // is nothing to do -- it is one pass of an event loop, not a poll -- and the whole point of
    // this test is that the queue is now empty. The marker guarantees the pass returns, and a stale
    // deferred delete would have run in the same pass if one had survived.
    Thread* const here = Thread::currentThread();
    ASSERT_NE( here, nullptr );
    bool markerRan = false;
    ASSERT_TRUE( here->post( [&markerRan]()
        {
            markerRan = true;
        } ) );
    here->processEvents();
    ASSERT_TRUE( markerRan ) << "the drain pass did not run.";

    EXPECT_EQ( aliveCount, 0 ) << "a stale deferred delete ran against a destroyed child.";
}
