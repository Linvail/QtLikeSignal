//! @file
//!
//! Implementation of QtMimic::Timer.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Timer.hpp"

#include <cstdio>

namespace QtMimic
{

    //! @brief Constructor - creates an inactive timer living in @p aThread.
    //! @param aThread The Thread this timer lives in; null (the default) means the calling thread,
    //!        exactly as for Object.
    Timer::Timer
        (
        Thread* aThread
        )
        : Object( aThread )
    {
    }

    //! @brief Destructor - stops the timer if it is still running.
    //!
    //! Matches QTimer::~QTimer(), and shares its consequence: killTimer() is thread-confined, so
    //! destroying an *active* timer from a thread other than its own warns. That is a genuine misuse
    //! signal, not noise -- destroy the timer on the thread it lives in. Nothing leaks either way,
    //! because ~Object() strips this object's timer registrations regardless.
    Timer::~Timer()
    {
        if( mActive )
        {
            stop();
        }
    }

    //! @brief Correct a caller-supplied interval to one a timer can actually run at.
    //!
    //! Mirrors Qt's checkInterval() in qtimer.cpp, including its choice to correct rather than
    //! reject: Qt treats a negative interval as a caller mistake worth reporting but not worth
    //! cancelling the call over, so the timer still runs, just at the shortest interval there is.
    //! Shared by every entry point that takes one so none of them can drift from the others.
    //! @return the interval to use, in milliseconds.
    int Timer::checkInterval
        (
        const char* aCaller,  //!< Name of the calling function, for the warning text.
        int aMsec             //!< The requested interval, in milliseconds.
        )
    {
        if( aMsec < 0 )
        {
            std::fprintf( stderr,
                "%s: negative intervals aren't allowed; the interval will be set to 1 ms\n",
                aCaller );
            return 1;
        }
        return aMsec;
    }

    //! @return the interval in milliseconds.
    int Timer::interval() const
    {
        return mInterval;
    }

    //! @brief Set the interval, restarting a running timer so the new value takes effect at once.
    //!
    //! Matches QTimer::setInterval(), which likewise kills and restarts an active timer rather than
    //! waiting for the next start(). It restarts unconditionally, even when the value is unchanged.
    //!
    //! **Must be called from this timer's own thread whenever the timer is active**, because the
    //! restart goes through Object::startTimer()/killTimer(); see start(). Calling it off-thread on
    //! an active timer warns and leaves the old timer registered while this object reports itself
    //! stopped. It is safe from any thread only while the timer is inactive, where it is a plain
    //! assignment.
    void Timer::setInterval
        (
        int aMsec  //!< Interval in milliseconds. Negative values become 1 ms, with a warning.
        )
    {
        mInterval = checkInterval( "Timer::setInterval", aMsec );
        if( mActive )
        {
            start();
        }
    }

    //! @return true while the timer is running.
    bool Timer::isActive() const
    {
        return mActive;
    }

    //! @return true if the timer stops itself after firing once.
    bool Timer::isSingleShot() const
    {
        return mSingleShot;
    }

    //! @brief Set whether the timer stops itself after firing once.
    void Timer::setSingleShot
        (
        bool aSingleShot  //!< True for single-shot, false for repeating.
        )
    {
        mSingleShot = aSingleShot;
    }

    //! @return the underlying Object timer id, or -1 while inactive.
    int Timer::timerId() const
    {
        return mTimerId;
    }

    //! @brief Return a view of the signal emitted each time the interval elapses (Qt-like
    //! QTimer::timeout()). A view can be connected to but not emitted.
    SignalView<>& Timer::getTimeout() const
    {
        return mTimeout.view();
    }

    //! @brief Start or restart the timer with a new interval.
    void Timer::start
        (
        int aMsec  //!< Interval in milliseconds.
        )
    {
        mInterval = checkInterval( "Timer::start", aMsec );
        start();
    }

    //! @brief Start or restart the timer using the interval already configured.
    //!
    //! **Must be called from this timer's own thread**; see start(int).
    void Timer::start()
    {
        const int oldTimerId = mActive ? mTimerId : -1;
        const int newTimerId = startTimer( mInterval );

        // Take the replacement id before handing the old one back, so a restart is guaranteed an id
        // it did not just have. Killing first would put the old id in an otherwise-empty free pool
        // and get it straight back, and timerEvent() tells expiries apart by id alone: an id reused
        // across a restart lets an expiry belonging to the timer we just killed be accepted as the
        // new one's first fire.
        //
        // This is deliberately stricter than Qt, which kills before it starts
        // (QTimer::setInterval()) and whose QFreeList commonly does return the same id. Qt promises
        // nothing about ids across a restart; we promise a fresh one, because that is what makes
        // timerEvent()'s id check sound rather than merely usually right.
        if( oldTimerId != -1 )
        {
            killTimer( oldTimerId );
        }

        mTimerId = newTimerId;
        mActive = ( mTimerId != -1 );
    }

    //! @brief Stop the timer. Does nothing if it is not running.
    //!
    //! **Must be called from this timer's own thread**, because it goes through
    //! Object::killTimer(). Calling it from elsewhere warns and leaves the timer running.
    void Timer::stop()
    {
        if( mActive && mTimerId != -1 )
        {
            killTimer( mTimerId );
            mTimerId = -1;
            mActive = false;
        }
    }

    //! @brief Deliver one expiry: emit timeout, having first stopped a single-shot timer.
    void Timer::timerEvent
        (
        TimerEvent* aEvent  //!< The expiry being delivered.
        )
    {
        if( !aEvent || aEvent->timerId() != mTimerId )
        {
            return;
        }

        // Stop before emitting, matching QTimer::timerEvent()'s ordering: a slot must not observe a
        // single-shot timer as still active, and emitting last means none of our own code touches
        // this object after user code has run.
        //
        // Note this narrows -- but does not eliminate -- the hazard of a directly-connected slot
        // deleting the timer: emit() is still executing inside the Signal member of the object being
        // destroyed. Use deleteLater() from a timeout slot; deleting outright is not supported.
        if( mSingleShot )
        {
            stop();
        }

        mTimeout.emit();
    }

} // namespace QtMimic
