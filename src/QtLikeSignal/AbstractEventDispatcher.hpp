// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Abstract interface every event dispatcher implements.

#ifndef QT_LIKE_SIGNAL_ABSTRACTEVENTDISPATCHER_HPP
#define QT_LIKE_SIGNAL_ABSTRACTEVENTDISPATCHER_HPP

#include "QtLikeSignal/LoopHealth.hpp"
#include "QtLikeSignal/OverflowPolicy.hpp"

#include <functional>

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

        //! Whether a pass may block when it finds nothing to do.
        //!
        //! **The distinction only matters to a thread whose loop is not ours.** A thread running
        //! exec() wants WaitForMoreEvents: with nothing queued there is nothing to do but sleep
        //! until something arrives, and spinning instead would burn a core. A thread with its own
        //! native loop wants AllEvents: it is borrowing a moment to drain our queue and needs its
        //! thread back, and a pass that blocked would strand it inside our condition variable where
        //! only another QtLikeSignal call could release it -- which is a hang, not a wait, because
        //! the loop that owns the thread has no reason to make one.
        //!
        //! The same distinction as Qt's QEventLoop::ProcessEventsFlags, and for the same reason.
        enum class ProcessEventsFlag
        {
            //! Dispatch whatever is ready and return, even if that is nothing.
            AllEvents,

            //! Block until there is work, a timer expires, or the loop is woken or interrupted.
            WaitForMoreEvents
        };

        //! Processes pending events and expired timers once, without infinite looping. Returns
        //! true if events were processed, false otherwise.
        //!
        //! Thread-safe invocation within the event loop of the owning thread.
        virtual bool processEvents
            (
            ProcessEventsFlag aFlag
            ) = 0;

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
        //! The callback runs with no dispatcher lock held and may call back in. It should not
        //! block: it is on the critical path of every post. Signal the native loop and return.
        //! Thread-safe.
        virtual void setWakeCallback
            (
            std::function<void()> aCallback  //!< Invoked on post; nullptr clears.
            ) = 0;

        //! @return milliseconds until the earliest timer deadline, or -1 if no timer is scheduled.
        //!
        //! **The other half of what a thread with its own native loop needs.** setWakeCallback()
        //! tells such a thread that something was *posted*, which is all a posted event needs. A
        //! timer needs the opposite question answered -- how long may this loop sleep before it
        //! must call processEvents() again -- and without an answer to it a timer on such a thread
        //! fires whenever the next unrelated event happens to wake the loop, and on an idle thread
        //! never at all. That failure reads as jitter rather than as a missing feature, which is
        //! the worst way for it to fail.
        //!
        //! Zero means a deadline has already passed, so the next pass will deliver it. Feed the
        //! result to whatever the native loop blocks in -- poll(), MsgWaitForMultipleObjectsEx(),
        //! an OS timer -- and call processEvents() when it returns. Waking early is always safe:
        //! a pass with nothing due delivers nothing and simply recomputes the deadline.
        //!
        //! A sample, not a promise: another thread may start a timer the moment this returns. Pair
        //! it with setDeadlineCallback() rather than polling it in a loop.
        //!
        //! QAbstractEventDispatcher::remainingTime() is the same idea, and Qt's own platform
        //! integrations arm their native timers out of the timer list in exactly this way.
        //! Thread-safe.
        virtual int remainingTimeMs() const = 0;

        //! Installs the callback invoked whenever the earliest timer deadline moves.
        //!
        //! Polling remainingTimeMs() defeats the purpose of asking, so this is how a native loop
        //! learns that it must re-arm: the callback is handed the same number remainingTimeMs()
        //! would return, and a host normally does nothing with it but re-arm its own wait. Pass
        //! nullptr to remove it.
        //!
        //! Called once, on the calling thread, from inside this function, so a host learns the
        //! current deadline on installation rather than having to wait for the first change.
        //!
        //! Runs with no dispatcher lock held and may call straight back in. It runs on whichever
        //! thread moved the deadline -- which is the loop's own thread for an expiry, and the
        //! caller's thread for a timer started or stopped from elsewhere -- so it must not block,
        //! exactly as the wake callback must not.
        //!
        //! Nothing guarantees two reports made at once arrive in the order they were computed; a
        //! host that must be certain re-reads remainingTimeMs(). In practice they cannot race,
        //! because starting and stopping a timer is confined to the timer's own thread.
        //! Thread-safe.
        virtual void setDeadlineCallback
            (
            std::function<void( int aMsFromNow )> aCallback  //!< Invoked on a move; nullptr clears.
            ) = 0;

        //! Stops the dispatcher accepting further events, before its final drain.
        //!
        //! Closes the shutdown race: a thread finishing drains its deferred deletes and only
        //! then release the dispatcher, so a deleteLater() landing between those two steps was
        //! accepted by a queue nothing would drain again. The event was freed with the dispatcher
        //! and its receiver never deleted -- neither run nor deleted, i.e. leaked. Refusing posts
        //! first makes postEvent() report failure instead, and deleteLater() then falls back to
        //! deleting synchronously.
        //!
        //! One-way: there is no reopen. Thread-safe.
        virtual void close() = 0;

        //! Sets how many events this dispatcher's queue will hold. Thread-safe.
        //!
        //! Zero is unbounded, and is the default -- so a program that never calls this behaves
        //! exactly as it did before bounds existed. Public for the same reason
        //! processEvents()/wakeUp()/interrupt() are: it configures the loop as a whole rather than
        //! aiming at one receiver.
        //!
        //! Lowering the capacity below the current depth is allowed and drops nothing. The queue
        //! simply refuses new work until it has drained under the new ceiling, because throwing
        //! away events that were already accepted is a worse surprise than a few refusals.
        //!
        //! One ceiling covers every producer feeding this thread, which means a flood on one path
        //! can still crowd out another. Deciding *which* events survive that is what an event
        //! priority would answer, and there is not one yet.
        virtual void setEventQueueCapacity
            (
            std::size_t aCapacity  //!< Maximum queued events; 0 for unbounded.
            ) = 0;

        //! @return the capacity set by setEventQueueCapacity(); 0 if unbounded. Thread-safe.
        virtual std::size_t eventQueueCapacity() const = 0;

        //! @return how many events are queued right now. Thread-safe.
        //!
        //! A sample, not a promise: it can change before the caller reads the answer. Useful for a
        //! watchdog or a health report, not for deciding whether the next post will be accepted.
        virtual std::size_t eventQueueDepth() const = 0;

        //! @return how many events this dispatcher has refused or evicted since it was created.
        //! Thread-safe.
        //!
        //! A queue that silently discards work is worse to debug than one that grows, because the
        //! growth is at least visible in the process's memory. This is what makes a bound a
        //! diagnosable condition. Never resets, so two readings subtract to give a rate.
        virtual unsigned long long droppedEventCount() const = 0;

        //! **A slot, an event handler and a timer handler must not throw.**
        //!
        //! This library is written for applications built with `-fno-exceptions`, and the dispatch
        //! loop takes that as a precondition rather than defending against it. An exception that
        //! escapes a handler produces one of two outcomes, neither of them recoverable:
        //!
        //! - on a worker thread, it unwinds into the platform's thread-entry callback and calls
        //!   std::terminate;
        //! - on the thread running processEvents() directly, it leaks the event being dispatched
        //!   and every event still queued in that pass.
        //!
        //! The second is a deliberate omission, not an oversight -- see the note at the delete in
        //! EventDispatcherDefault::processEvents(). Making it safe would put ownership machinery on
        //! the hottest path in the library to protect a case the build flags exclude.
        //!
        //! Nothing here is compiled with -fno-exceptions today: the library's own build keeps
        //! exceptions on so the test suite can use them, and the three try/catch sites that remain
        //! in src/QtLikeSignal guard against a *sink* or a *callable copy* throwing rather than a
        //! slot. The precondition is on what an application hands the loop.

        //! Turns liveness tracking on or off for this loop. Thread-safe.
        //!
        //! Off by default, because it costs one steady_clock::now() per dispatched event -- a few
        //! tens of nanoseconds against roughly 350 for a queued metacall, which is a fraction a
        //! library should not spend on programs that never read it. Off, the cost is one relaxed
        //! load and a branch that is not taken.
        //!
        //! A watchdog you have to remember to switch on is off when you need it, so turn it on in
        //! main() and leave it on. The cost is stated rather than hidden precisely so that decision
        //! can be made rather than guessed at.
        virtual void setHealthTrackingEnabled
            (
            bool aEnabled  //!< True to record what this loop is dispatching.
            ) = 0;

        //! @return whether liveness tracking is on. Thread-safe.
        virtual bool isHealthTrackingEnabled() const = 0;

        //! @return what this loop is doing, from one consistent moment. Thread-safe.
        //!
        //! Every field comes from the same instant: a reader never pairs the receiver of one
        //! dispatch with the start time of another, which would name the wrong handler and be worse
        //! than reporting nothing. Costs the observed loop nothing.
        //!
        //! Returns a default-constructed reading when tracking is off, so a caller that forgot to
        //! enable it sees "not dispatching, nothing counted" rather than plausible stale values.
        virtual LoopHealth health() const = 0;

        //! Dispatches any pending deferred-delete events, destroying their receivers.
        //!
        //! Called when an event loop is shutting down, before the dispatcher itself goes away.
        //! Without it, every object that called deleteLater() before the loop stopped is leaked:
        //! the destructor can free the queued events but has no way to free the objects they
        //! target. Mirrors Qt, which drains DeferredDelete in QThreadPrivate::finish() for the same
        //! reason.
        //!
        //! Public rather than protected because, like processEvents()/wakeUp()/interrupt(), it
        //! drives the loop as a whole and cannot be aimed at a particular receiver.
        //!
        //! Thread-safe, but intended to run on the dispatcher's own thread -- it destroys
        //! objects, and their destructors expect to run there.
        virtual void processDeferredDeletes() = 0;

    protected:
        // The methods below all target a *specific* receiver object, so exposing them publicly
        // would let any caller inject events or timers on another object's behalf. They exist
        // solely for Object's own internals (deleteLater(), startTimer()/killTimer(),
        // dispatchMetaCall(), and ~Object()), which reach them through the friend declaration at
        // the end of this class. processEvents()/wakeUp()/interrupt() stay public: they drive or
        // stop the loop as a whole and cannot be aimed at a particular object.
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

        //! @return milliseconds until @p aTimerId next fires; 0 if it is already due, -1 if it is
        //! not registered here. Thread-safe.
        //!
        //! Protected, and named apart from the no-argument remainingTimeMs(), because it names one
        //! timer rather than driving the loop: it belongs with registerTimer() and
        //! unregisterTimer() on the Object-only side of this class. Object::remainingTime() is the
        //! way in, and Timer::remainingTime() is what an application calls.
        //!
        //! A sample. The answer is stale the moment it is returned, and a timer whose loop is
        //! blocked will read 0 for as long as the block lasts.
        virtual int timerRemainingTimeMs
            (
            int aTimerId  //!< Unique timer identifier.
            ) const = 0;

        //! Thread-safely posts an event to the dispatcher's queue.
        //! @return true if the event was queued; false if it was not, in which case the event is
        //!         deleted and the caller must handle the failure. A post fails when the dispatcher
        //!         is closed, and when the queue is at capacity and @p aPolicy does not make room.
        //!
        //! A DeferredDelete is admitted whatever the depth: dropping one leaks the object it names,
        //! which under a flood would leak exactly when memory is scarce.
        //!
        //! @p aPriority orders the queue: higher runs first, and equal priorities keep posting
        //! order. It also decides what a full queue sheds -- DropOldest evicts from the lowest
        //! priority present, so a bound cannot undo a priority.
        virtual bool postEvent
            (
            Object* aReceiver,        //!< The object that will receive the event.
            Event* aEvent,            //!< The event to be processed.
            OverflowPolicy aPolicy,   //!< What to do if the queue is full.
            int aPriority             //!< Higher runs first; EventPriority::kNormal is the default.
            ) = 0;

        //! Appends an event without consulting the capacity. Takes ownership either way.
        //!
        //! For moveToThread() alone. The events it carries across were admitted once already, on
        //! the dispatcher they are leaving, so putting them through admission a second time would
        //! let a move silently destroy accepted work whenever the destination happened to be near
        //! its ceiling -- a surprising place to lose an event. A move may therefore leave a queue
        //! briefly over its ceiling, which is the right trade: the ceiling exists to stop unbounded
        //! growth, and a move adds a fixed number of events already counted somewhere else.
        //!
        //! Deliberately not a fourth OverflowPolicy. "Ignore the bound" is not a choice an
        //! application should be able to make, and keeping it out of the enum keeps it out of
        //! reach.
        //!
        //! @return true if the event was queued; false only if the dispatcher is closed.
        virtual bool postEventUnconditionally
            (
            Object* aReceiver,  //!< The object that will receive the event.
            Event* aEvent,      //!< The event to be processed.
            int aPriority       //!< Rank to keep it at; higher runs first.
            ) = 0;

        //! Removes and deletes all pending events for the specified receiver. Thread-safe.
        virtual void removeEventsForReceiver
            (
            Object* aReceiver  //!< The receiver whose events should be removed.
            ) = 0;

        //! Unregisters the receiver's timers and returns them so they can be re-registered.
        //!
        //! Used by Object::moveToThread() to carry active timers across to the destination thread's
        //! dispatcher. The ids are handed back rather than released, so the same timer id stays
        //! valid after the move and a Timer's cached id still matches the events it receives -- the
        //! same reason Qt notes "do not release our timer ids back to the pool" when it does this.
        //! Returns the removed registrations, empty if the receiver had none. Thread-safe.
        virtual std::vector<TimerRegistration> takeTimersForReceiver
            (
            Object* aReceiver  //!< The receiver whose timers should be taken.
            ) = 0;

        //! Removes the receiver's pending events and hands them over, still alive.
        //!
        //! The counterpart of removeEventsForReceiver() for a move rather than a destruction: the
        //! events are detached from this dispatcher but not deleted, so Object::moveToThread() can
        //! post them onto the destination thread. Ownership passes to the caller, which must post
        //! or delete every one of them. Thread-safe.
        virtual std::vector<Event*> takeEventsForReceiver
            (
            Object* aReceiver  //!< The receiver whose events should be taken.
            ) = 0;

        friend class Object;

        //! Grants ThreadData the ability to hand over events parked before a dispatcher existed.
        //! See ThreadData::mParkedEvents.
        friend struct ThreadData;
    };
}

#endif // QT_LIKE_SIGNAL_ABSTRACTEVENTDISPATCHER_HPP
