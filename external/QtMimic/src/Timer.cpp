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

    //! @return the interval in milliseconds.
    int Timer::interval() const
    {
        return mInterval;
    }

    //! @brief Set the interval. A running timer is stopped and restarted with a new timer id,
    //! matching QTimer.
    void Timer::setInterval
        (
        int aMsec  //!< Interval in milliseconds.
        )
    {
        if( aMsec < 0 )
        {
            std::fprintf( stderr,
                "Timer::setInterval: negative intervals are not allowed; using 1 ms\n" );
            aMsec = 1;
        }

        mInterval = aMsec;
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
        if( aMsec < 0 )
        {
            std::fprintf( stderr,
                "Timer::start: negative intervals are not allowed; using 1 ms\n" );
            aMsec = 1;
        }
        mInterval = aMsec;
        start();
    }

    //! @brief Start or restart the timer using the interval already configured.
    //!
    //! **Must be called from this timer's own thread**; see start(int).
    void Timer::start()
    {
        const int oldTimerId = mActive ? mTimerId : -1;
        const int newTimerId = startTimer( mInterval );

        // Keep the old id reserved until its replacement has been allocated. Otherwise the shared
        // FIFO pool can immediately hand the same id back, violating QTimer's restart contract.
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
