// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The event types the loop carries: the library's MetaCall, Timer and DeferredDelete, and the
//! base class an application derives its own from.

#ifndef QT_LIKE_SIGNAL_EVENT_HPP
#define QT_LIKE_SIGNAL_EVENT_HPP

#include "QtLikeSignal/EventPool.hpp"

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace QtLikeSignal
{
    //! Base class for every event the loop carries.
    //!
    //! Two populations, and the split is what keeps the queue safe. The three types below User are
    //! the library's own: each has a concrete subclass here whose constructor only Object can
    //! reach, and each is dispatched by casting to that subclass. Everything from User up belongs
    //! to the application, which defines its own types and its own subclasses to carry them.
    //!
    //! An application type is made by deriving from this class and passing a value of at least
    //! User:
    //!
    //! @code
    //!   struct BrakeWarningEvent : public QtLikeSignal::Event
    //!   {
    //!       BrakeWarningEvent( bool aOn )
    //!           : Event( static_cast<Type>( User + 1 ) )
    //!           , mOn( aOn )
    //!       {
    //!       }
    //!
    //!       bool mOn;
    //!   };
    //!
    //!   QtLikeSignal::Object::postEvent( &cluster, new BrakeWarningEvent( true ) );
    //! @endcode
    //!
    //! Deriving is the only way in, deliberately: there is no constructible plain Event, so an
    //! application cannot hand the queue something claiming to be a MetaCall that the dispatcher
    //! would then cast to MetaCallEvent. A payload-free notification is a one-line subclass, which
    //! is a small price for closing that hole by construction. postEvent() enforces the same rule a
    //! second time at the queue's edge, because a subclass is still free to pass an internal value
    //! to the constructor below.
    class Event
    {
    public:
        //! The core event types, and where the application's own begin.
        enum Type
        {
            MetaCall       = 1,
            Timer          = 2,
            DeferredDelete = 3,

            //! First value an application may use. Below this is the library's, and postEvent()
            //! refuses it. Matches QEvent::User, which is 1000 for the same reason: it leaves room
            //! for the library to add types without colliding with an application already shipped.
            User = 1000,

            //! Last value an application may use, as QEvent::MaxUser is.
            MaxUser = 65535
        };

        //! Virtual destructor.
        virtual ~Event() = default;

        //! Takes an event's block from the event pool rather than from the heap.
        //!
        //! **On the base class, so that every event type is pooled and none is an exception.** The
        //! pool reached only MetaCallEvent at first, which was the type a cross-thread emit
        //! allocates; a TimerEvent then paid a malloc and a free on every expiry of every timer,
        //! forever, on the thread a frame budget belongs to. Nothing about that was deliberate --
        //! it is simply where the pool stopped -- and leaving some event types pooled and some not
        //! would be a distinction carrying no meaning that a later reader has to rediscover.
        //!
        //! An application's own event types inherit this, which is the intent: `new
        //! BrakeWarningEvent( true )` is the same shape of allocation and wants the same treatment.
        //! A type too large for a pooled block still works -- EventPool::allocate() falls back to
        //! the heap and counts a miss -- so nothing here has to know which happened.
        //!
        //! Never returns null; it reports failure the way ::operator new does.
        static void* operator new
            (
            std::size_t aBytes  //!< Size of the event being built.
            )
        {
            return EventPool::allocate( aBytes );
        }

        //! Returns an event's block to the event pool.
        //!
        //! **Unsized on purpose.** ~Event() is virtual, so `delete` through an Event* finds the
        //! deallocation function in the *dynamic* type's scope; declaring the unsized member form
        //! here is what stops the compiler emitting a call to the C++14 sized form. That matters
        //! because a block is not always sizeof( the object ): MetaCallEvent's carries a callable
        //! past its own end. MetaCallEvent declares the same function again, and the note there
        //! says why it is worth restating where that is true.
        static void operator delete
            (
            void* aBlock  //!< Block from operator new; null is ignored.
            )
        {
            EventPool::release( aBlock );
        }

        //! Placement new, so an event can still be built into memory the caller already owns.
        //!
        //! Redeclared rather than inherited from the global scope: declaring any operator new in a
        //! class hides **all** of the global ones from lookup at that class, so without this the
        //! placement form MetaCallEvent::create() uses would no longer be found.
        static void* operator new
            (
            std::size_t,      //!< Size; unused, the caller has already sized the block.
            void* aPlace      //!< Where to build the event.
            )
        {
            return aPlace;
        }

        //! The deallocation function that pairs with placement new. Does nothing, as it must: the
        //! block belongs to whoever supplied it. Reached only if a constructor throws.
        static void operator delete
            (
            void*,  //!< The block; not ours to free.
            void*   //!< The place it was built at.
            )
        {
        }

        //! Gets the type of the event. Thread-safe.
        Type type() const
        {
            return mType;
        }

        //! @return true if @p aType is one an application may post. Thread-safe.
        //!
        //! The queue's admission test, exposed so a caller can ask the same question before
        //! building an event rather than discovering the answer from a rejected post.
        static bool isUserType
            (
            Type aType  //!< The type to test.
            )
        {
            return aType >= User && aType <= MaxUser;
        }

    protected:
        //! Constructs an event of the specified type.
        //!
        //! Protected rather than public, which is what makes "no plain Event exists" true: an
        //! application reaches this only from its own subclass. Qt's equivalent is public, and Qt
        //! therefore relies on documentation alone to stop `new QEvent( QEvent::MetaCall )`
        //! reaching a queue that would cast it to QMetaCallEvent.
        //!
        //! Passing an internal value from a subclass is still possible and still wrong; postEvent()
        //! is what catches it.
        Event
            (
            Type aType  //!< The type of the event.
            )
            : mType( aType )
        {
        }

    private:
        Type mType;
    };

    //! An event that encapsulates a function call across threads.
    //!
    //! Entirely internal: it wraps an arbitrary callable, so both creating one and firing one are
    //! restricted to Object, which is the only code that queues or dispatches metacalls.
    //!
    //! The callable lives in this event's own block, immediately past the end of this object,
    //! rather than behind a std::function member. It used to be a std::function, and every queued
    //! emit therefore cost two heap blocks: the box std::function needed for a closure far too big
    //! for its small-buffer optimisation, and the event carrying it. Both were allocated on the
    //! emitting thread and freed on the receiving one. Sizing this allocation to fit the concrete
    //! callable makes it one block.
    //!
    //! Two consequences worth knowing before editing this class:
    //!
    //! - **The block is bigger than sizeof(MetaCallEvent), so sized deallocation must not be used.**
    //!   ~Event() is virtual, so `delete` through an Event* reaches ~MetaCallEvent() and then calls
    //!   this class's deallocation function. Without the member operator delete below, C++14 sized
    //!   deallocation would pass sizeof(MetaCallEvent) instead of the size actually allocated.
    //!   glibc ignores that argument, so the bug would pass every test on Linux; jemalloc, tcmalloc
    //!   and mimalloc use it to choose the free list to return the block to.
    //! - **create() builds the callable before the header**, because ~MetaCallEvent() destroys
    //!   whatever the tail holds without asking whether it was ever constructed.
    //!
    //! The block comes from EventPool rather than from ::operator new, which is what lets a
    //! cross-thread emit cost no allocation at all once the pool has been reserved. That is a
    //! straight substitution -- allocate() for new, release() for delete -- and the pool falls back
    //! to the heap for a block too large or a pool too small, so nothing here has to know which
    //! happened.
    class MetaCallEvent : public Event
    {
    public:
        //! Destroys the callable in the tail. See create() for why it can assume one is there.
        virtual ~MetaCallEvent() override
        {
            mDestroy( storage() );
        }

        //! Deallocates the whole block, not just the header. See the class comment.
        //!
        //! Declaring the unsized member form is what stops the compiler emitting a call to the
        //! sized form with the wrong size.
        //!
        //! Kept even though Event now declares the identical function. To the compiler it is
        //! redundant -- lookup would find the base's unsized form and call that. To a reader it is
        //! not: this is the one type whose block is deliberately larger than the object inside it,
        //! and the rule that follows from that belongs where the reason for it lives. Deleting it
        //! would leave nothing at this class to stop the next person adding a sized deallocation
        //! and handing the allocator a size that was never allocated.
        static void operator delete
            (
            void* aBlock  //!< Block returned by create(); never null in practice.
            )
        {
            EventPool::release( aBlock );
        }

        //! The deallocation function that pairs with the placement new create() builds through.
        //!
        //! Declaring the unsized form above hides **every** operator delete in the base, the
        //! placement one included, and a placement new whose matching delete cannot be found is a
        //! diagnosed warning rather than a silent nothing. So it is redeclared here.
        //!
        //! Empty, and unreachable. It would run only if this class's constructor threw, and that
        //! constructor stores two function pointers. The block is create()'s to free either way:
        //! its own catch already covers the one step of building an event that can throw, which is
        //! the copy of the caller's callable into the tail.
        static void operator delete
            (
            void*,  //!< The block; create() owns it.
            void*   //!< The place the header was built at.
            )
        {
        }

    private:
        //! Calls the callable held in the tail.
        using Invoke = void ( * )( void* );

        //! Destroys the callable held in the tail.
        using Destroy = void ( * )( void* ) noexcept;

        //! Constructs the header. Private: only create() ever builds one, and only into a block it
        //! has already furnished with a callable.
        MetaCallEvent
            (
            Invoke aInvoke,     //!< Calls the tail callable.
            Destroy aDestroy    //!< Destroys the tail callable.
            )
            : Event( MetaCall )
            , mInvoke( aInvoke )
            , mDestroy( aDestroy )
        {
        }

        //! Allocates one block holding this event and @p aCallable, and returns the event.
        //!
        //! The caller owns the result and must dispose of it with `delete`.
        template <typename Callable>
        static MetaCallEvent* create
            (
            Callable&& aCallable  //!< The call to make on the receiving thread.
            )
        {
            using Body = std::decay_t<Callable>;

            // An over-aligned callable would need ::operator new( size, align_val_t ) and a
            // matching deallocation function. Nothing this library queues is over-aligned -- the
            // captures are pointers, shared_ptrs and the signal's own argument types -- so the case
            // is rejected at compile time rather than handled.
            static_assert( alignof( Body ) <= alignof( std::max_align_t ),
                "MetaCallEvent cannot carry an over-aligned callable." );

            void* block = EventPool::allocate( sizeof( MetaCallEvent ) + sizeof( Body ) );

            // The callable first, the header second. ~MetaCallEvent() destroys the tail
            // unconditionally, so a header must never exist over a tail that was not built: if the
            // copy below throws, releasing the raw block is the whole of the cleanup.
            //
            // **Compiled out when the including translation unit has exceptions disabled**, and
            // that is what lets an application build with -fno-exceptions at all. This is a
            // template, so it is instantiated in the *caller's* translation unit -- every queued
            // connect() a program makes puts a copy of this function in the program's own object
            // file. A bare try/catch here is therefore not a private detail of the library: it is a
            // hard error in any user file compiled with -fno-exceptions, which made the flag
            // impossible to use however the library itself was built.
            //
            // __cpp_exceptions rather than __EXCEPTIONS or _CPPUNWIND: it is the standard
            // feature-test macro and all three compilers here agree on it. MSVC defines it under
            // /EHsc and leaves it undefined without, which was measured rather than assumed.
            //
            // Nothing is lost by the guard. Without exceptions the copy cannot throw, so there is
            // no path on which the block would need releasing.
            #if defined( __cpp_exceptions )
                try
                {
                    new ( static_cast<char*>( block ) + sizeof( MetaCallEvent ) )
                    Body( std::forward<Callable>( aCallable ) );
                }
                catch( ... )
                {
                    EventPool::release( block );

                    throw;
                }
            #else
                new ( static_cast<char*>( block ) + sizeof( MetaCallEvent ) )
                Body( std::forward<Callable>( aCallable ) );
            #endif

            return new ( block ) MetaCallEvent(
                []( void* aBody )
                {
                    ( *static_cast<Body*>( aBody ) )();
                },
                []( void* aBody ) noexcept
                {
                    static_cast<Body*>( aBody )->~Body();
                } );
        }

        //! Where the callable lives: immediately past the end of this object, in the same block.
        void* storage() const
        {
            return const_cast<MetaCallEvent*>( this ) + 1;
        }

        //! Executes the stored function call.
        void placeMetaCall() const
        {
            mInvoke( storage() );
        }

        Invoke mInvoke;     //!< Never null: create() is the only way to build one of these.
        Destroy mDestroy;   //!< Never null, for the same reason.

        friend class Object;
    };

    //! Event sent when a timer expires.
    class TimerEvent : public Event
    {
    public:
        //! Constructs a timer event with a given timer ID.
        //!
        //! Left public, unlike the other two event types: timerEvent() is a supported override
        //! point, so synthesizing a TimerEvent to drive an override directly (as tests do) is
        //! legitimate. Constructing one grants no privileged capability -- it cannot be posted to
        //! any queue.
        TimerEvent
            (
            int aTimerId  //!< The unique identifier of the expired timer.
            )
            : Event( Timer )
            , mTimerId( aTimerId )
        {
        }

        //! Gets the timer ID associated with this event. Thread-safe.
        int timerId() const
        {
            return mTimerId;
        }

    private:
        int mTimerId;
    };

    //! Event sent to delete an object asynchronously.
    //!
    //! Internal: only Object::deleteLater() creates one. Delivering this event destroys the
    //! receiver, so it must not be constructible by outside code.
    class DeferredDeleteEvent : public Event
    {
    private:
        //! Constructs a deferred delete event.
        DeferredDeleteEvent()
            : Event( DeferredDelete )
        {
        }

        friend class Object;
    };
}

#endif // QT_LIKE_SIGNAL_EVENT_HPP
