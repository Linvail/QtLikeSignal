// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! What an event loop is doing right now, readable from another thread.

#ifndef QT_LIKE_SIGNAL_LOOPHEALTH_HPP
#define QT_LIKE_SIGNAL_LOOPHEALTH_HPP

#include <chrono>

namespace QtLikeSignal
{
    class Object;

    //! One reading of an event loop's liveness, taken from any thread.
    //!
    //! **What this is for.** Nothing else can tell a wedged loop from a busy one.
    //! Thread::isRunning() is true of a thread stuck in a handler that will never return, and
    //! eventQueueDepth() says work is waiting without saying whether anything is taking it. This
    //! says what the loop is doing, and -- the half that matters in a field return -- what it is
    //! stuck in.
    //!
    //! Taken with AbstractEventDispatcher::health(), which fills every field from one consistent
    //! moment. Reading it costs nothing on the loop being observed.
    //!
    //! **The test a watchdog performs** is the two fields together, because an idle loop is not a
    //! stalled one:
    //!
    //! @code
    //!   const QtLikeSignal::LoopHealth health = worker.eventDispatcher()->health();
    //!   const auto stuckFor = std::chrono::steady_clock::now() - health.mDispatchStart;
    //!   if( health.mDispatching && stuckFor > std::chrono::seconds( 2 ) )
    //!   {
    //!       qCCritical( gLogApp )
    //!           << "loop stalled in receiver" << static_cast<const void*>( health.mReceiver )
    //!           << "event type" << health.mEventType;
    //!   }
    //! @endcode
    //!
    //! Nothing here is filled in unless the dispatcher was told to track it -- see
    //! AbstractEventDispatcher::setHealthTrackingEnabled(), and the reason it is not on by default.
    struct LoopHealth
    {
        //! True while a handler is running on the loop's own thread.
        //!
        //! The field that separates stalled from idle, and the reason there is no "time since the
        //! loop last ran" figure to compare against a threshold on its own: a loop with nothing to
        //! do has not run for as long as it has had nothing to do, and that is health rather than
        //! a fault.
        bool mDispatching { false };

        //! When the in-flight dispatch began. Meaningless unless mDispatching.
        //!
        //! Subtract from steady_clock::now() for how long it has been running. A steady clock, so
        //! the answer survives the wall clock jumping when the vehicle gets a GPS fix -- the same
        //! reason a log record carries one.
        std::chrono::steady_clock::time_point mDispatchStart;

        //! When the last dispatch that *finished* began.
        //!
        //! "When did this loop last do anything", for a report rather than for a stall test. It is
        //! the start rather than the end of that dispatch because recording the end would cost a
        //! second clock read per event, and nothing needs the difference.
        std::chrono::steady_clock::time_point mLastProgress;

        //! Events this loop has finished dispatching since it started.
        //!
        //! Never resets, so two readings subtract to a rate -- which is the cheapest liveness test
        //! there is, and the one that works even when every individual dispatch is short.
        unsigned long long mDispatchCount { 0 };

        //! The receiver of the in-flight event. Null when nothing is running.
        //!
        //! **Compare it; never dereference it.** The loop may be stuck inside this object's
        //! destructor, which is one of the cases most worth diagnosing, and reading a name off it
        //! from another thread would be a use-after-free exactly then. The library follows the same
        //! rule for the Thread* in Affinity::namesOtherRunningThread().
        //!
        //! An application that knows the object is alive -- because it owns it -- can look up
        //! whatever it likes from the pointer itself.
        const Object* mReceiver { nullptr };

        //! Event::Type of the in-flight event, or 0 when nothing is running.
        //!
        //! An int rather than Event::Type so this header does not have to include Event.hpp, which
        //! would drag the whole event hierarchy into anything that only wants to check on a loop.
        int mEventType { 0 };

        //! Timer id of the in-flight event when it is a timer, otherwise 0.
        //!
        //! "Stalled in timer 7" is a report someone can act on; "stalled in a timer" usually is
        //! not.
        int mTimerId { 0 };
    };
}

#endif // QT_LIKE_SIGNAL_LOOPHEALTH_HPP
