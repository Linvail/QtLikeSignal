#ifndef ABSTRACTEVENTDISPATCHER_H
#define ABSTRACTEVENTDISPATCHER_H

#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <vector>

namespace QtLikeSignal
{
    class Event;
    class Object;

    //! Abstract base class for event dispatchers managing event queues and timer dispatching.
    //!
    //! All public methods must be thread-safe as they can be invoked across threads.
    class AbstractEventDispatcher
    {
    public:
        //! A timer registration, as handed between dispatchers when an object changes thread.
        struct TimerRegistration
        {
            int mTimerId;     //!< The timer's unique id, preserved across the move.
            int mIntervalMs;  //!< The interval the timer was registered with, in milliseconds.
        };
        //! Constructs an event dispatcher.
        AbstractEventDispatcher();

        //! Destroys the event dispatcher and cleans up pending events.
        virtual ~AbstractEventDispatcher();

        //! Processes pending events and expired timers once, without infinite looping. Returns
        //! true if events were processed, false otherwise.
        //!
        //! Thread-safe invocation within the event loop of the owning thread.
        virtual bool processEvents() = 0;

        //! Wakes up the event loop if it is waiting for events. Thread-safe.
        virtual void wakeUp() = 0;

        //! Interrupts the event loop execution. Thread-safe.
        virtual void interrupt() = 0;

        //! Installs a callback invoked whenever work is queued for this dispatcher's thread.
        //!
        //! For a thread that runs its own native loop instead of exec(): the callback runs on
        //! whichever thread posted the work and should nudge that native loop so it knows to call
        //! processEvents(). Pass nullptr to remove it.
        //!
        //! Loop-level like wakeUp() and interrupt(), so it is public for the same reason -- it
        //! cannot be aimed at a particular receiver.
        //!
        //! The callback may run with this dispatcher's internal lock held, so it must not block or
        //! call back into the dispatcher; signal the native loop and return. Thread-safe.
        virtual void setWakeCallback
            (
            std::function<void()> aCallback  //!< Invoked on post; nullptr clears.
            ) = 0;

        //! Stops the dispatcher accepting further events, before its final drain.
        //!
        //! Closes the shutdown race: a thread finishing used to drain its deferred deletes and only
        //! then release the dispatcher, so a deleteLater() landing between those two steps was
        //! accepted by a queue nothing would drain again. The event was freed with the dispatcher
        //! and its receiver never deleted -- neither run nor deleted, i.e. leaked. Refusing posts
        //! first makes postEvent() report failure instead, and deleteLater() then falls back to
        //! deleting synchronously.
        //!
        //! One-way: there is no reopen. Thread-safe.
        virtual void close() = 0;

        //! Dispatches any pending deferred-delete events, destroying their receivers.
        //!
        //! Called when an event loop is shutting down, before the dispatcher itself goes away. Without
        //! it, every object that called deleteLater() before the loop stopped is leaked: the
        //! destructor can free the queued events but has no way to free the objects they target.
        //! Mirrors Qt, which drains DeferredDelete in QThreadPrivate::finish() for the same reason.
        //!
        //! Public rather than protected because, like processEvents()/wakeUp()/interrupt(), it drives
        //! the loop as a whole and cannot be aimed at a particular receiver.
        //!
        //! Thread-safe, but intended to run on the dispatcher's own thread -- it destroys
        //! objects, and their destructors expect to run there.
        virtual void processDeferredDeletes() = 0;

    protected:
        // The methods below all target a *specific* receiver object, so exposing them publicly would
        // let any caller inject events or timers on another object's behalf. They exist solely for
        // Object's own internals (deleteLater(), startTimer()/killTimer(), dispatchMetaCall(), and
        // ~Object()), which reach them through the friend declaration at the end of this class.
        // processEvents()/wakeUp()/interrupt() stay public: they drive or stop the loop as a whole
        // and cannot be aimed at a particular object.
        //! Registers a timer for the given object. Thread-safe.
        virtual void registerTimer
            (
            int aTimerId,       //!< Unique timer identifier.
            int aInterval,      //!< Interval in milliseconds.
            Object* aObject    //!< Target object to receive TimerEvent.
            ) = 0;

        //! Unregisters a timer by ID. Returns true if the timer was found and removed. Thread-safe.
        virtual bool unregisterTimer
            (
            int aTimerId  //!< Unique timer identifier.
            ) = 0;

        //! Thread-safely posts an event to the dispatcher's queue.
        //! @return true if the event was queued; false if the dispatcher is closed, in which case
        //!         the event is deleted and the caller must handle the failure.
        virtual bool postEvent
            (
            Object* aReceiver,  //!< The object that will receive the event.
            Event* aEvent       //!< The event to be processed.
            ) = 0;

        //! Removes and deletes all pending events for the specified receiver. Thread-safe.
        virtual void removeEventsForReceiver
            (
            Object* aReceiver  //!< The receiver whose events should be removed.
            ) = 0;

        //! Unregisters the receiver's timers and returns them so they can be re-registered.
        //!
        //! Used by Object::moveToThread() to carry active timers across to the destination thread's
        //! dispatcher. The ids are handed back rather than released, so the same timer id stays valid
        //! after the move and a Timer's cached id still matches the events it receives -- the same
        //! reason Qt notes "do not release our timer ids back to the pool" when it does this. Returns
        //! the removed registrations, empty if the receiver had none. Thread-safe.
        virtual std::vector<TimerRegistration> takeTimersForReceiver
            (
            Object* aReceiver  //!< The receiver whose timers should be taken.
            ) = 0;

        friend class Object;
    };
}

#endif // ABSTRACTEVENTDISPATCHER_H
