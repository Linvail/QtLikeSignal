// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The event types the loop carries: MetaCall, Timer and DeferredDelete.

#ifndef QT_LIKE_SIGNAL_EVENT_HPP
#define QT_LIKE_SIGNAL_EVENT_HPP

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace QtLikeSignal
{
    //! Base class for all events in the event loop.
    //!
    //! The event set is closed: these three types are the only ones the queue ever carries, and all of
    //! them are posted by Object's own internals. There is deliberately no user/custom event type and
    //! no way to post an arbitrary event for an arbitrary receiver -- this is a queued signal-slot
    //! mechanism, not a general event system.
    class Event
    {
    public:
        //! The core event types.
        enum Type
        {
            MetaCall       = 1,
            Timer          = 2,
            DeferredDelete = 3
        };
        //! Virtual destructor.
        virtual ~Event() = default;

        //! Gets the type of the event. Thread-safe.
        Type type() const
        {
            return mType;
        }

    protected:
        //! Constructs an event of the specified type.
        //!
        //! Protected: Event is only ever instantiated through one of the concrete subclasses below.
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
        static void operator delete
            (
            void* aBlock  //!< Block returned by create(); never null in practice.
            )
        {
            ::operator delete( aBlock );
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

            void* block = ::operator new( sizeof( MetaCallEvent ) + sizeof( Body ) );

            // The callable first, the header second. ~MetaCallEvent() destroys the tail
            // unconditionally, so a header must never exist over a tail that was not built: if the
            // copy below throws, releasing the raw block is the whole of the cleanup.
            try
            {
                new ( static_cast<char*>( block ) + sizeof( MetaCallEvent ) )
                Body( std::forward<Callable>( aCallable ) );
            }
            catch( ... )
            {
                ::operator delete( block );

                throw;
            }

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
        //! Left public, unlike the other two event types: timerEvent() is a supported override point,
        //! so synthesizing a TimerEvent to drive an override directly (as tests do) is legitimate.
        //! Constructing one grants no privileged capability -- it cannot be posted to any queue.
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
    //! Internal: only Object::deleteLater() creates one. Delivering this event destroys the receiver,
    //! so it must not be constructible by outside code.
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
