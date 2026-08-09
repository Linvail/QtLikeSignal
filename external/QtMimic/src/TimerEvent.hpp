//! @file
//!
//! The one event type QtMimic has.
//!
//! QtMimic's mailbox carries std::function<void()>, not events: a queued slot invocation, a
//! deleteLater() and a posted task are all just callables, so there has never been anything for an
//! Event base class or a QObject::event() router to dispatch on. Timers are the exception. A timer
//! is not a callable the poster hands over -- it is a schedule the loop owns, and what the loop
//! delivers when the schedule elapses is a notification carrying the id of the timer that fired.
//! Object::timerEvent() is the override point that receives it, so it needs a parameter, and this
//! is that parameter.
//!
//! There is deliberately no Event base class and no way to post one of these: TimerEvent is created
//! by the event loop, on the stack, immediately before the call it is an argument to.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_TIMEREVENT_HPP
#define QT_MIMIC_TIMEREVENT_HPP

namespace QtMimic
{

    //----------------------------------------------------------------
    //! @class TimerEvent
    //!
    //! Tells Object::timerEvent() which of the object's timers expired, mirroring Qt's
    //! QTimerEvent. An object may run several timers at once, so the id is what an override
    //! dispatches on -- see Timer::timerEvent(), which ignores any id that is not its own.
    //!
    //! Constructible by anyone, exactly as QTimerEvent is: synthesizing one to drive an override
    //! directly (as the tests do) is legitimate, and grants no privileged capability because it
    //! cannot be handed to a loop.
    //----------------------------------------------------------------
    class TimerEvent
    {
    public:
        explicit TimerEvent
            (
            int aTimerId  //!< Id of the timer that expired.
            )
            : mTimerId( aTimerId )
        {
        }

        //! @return the id of the timer that expired, as returned by Object::startTimer().
        int timerId() const
        {
            return mTimerId;
        }

    private:
        int mTimerId;
    };

} // namespace QtMimic

#endif // QT_MIMIC_TIMEREVENT_HPP
