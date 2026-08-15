//! @file
//!
//! The event types the loop carries: MetaCall, Timer and DeferredDelete.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_EVENT_HPP
#define QT_MIMIC_EVENT_HPP

#include <functional>
#include <utility>

namespace QtMimic
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
    class MetaCallEvent : public Event
    {
    private:
        //! Constructs a metacall event with the given callback.
        MetaCallEvent
            (
            std::function<void()> aCallback  //!< The function to execute.
            )
            : Event( MetaCall )
            , mCallback( std::move( aCallback ) )
        {
        }

        //! Executes the stored function call.
        void placeMetaCall() const
        {
            if( mCallback )
            {
                mCallback();
            }
        }

        std::function<void()> mCallback;

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

#endif // QT_MIMIC_EVENT_HPP
