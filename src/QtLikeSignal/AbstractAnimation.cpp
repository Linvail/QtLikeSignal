// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::AbstractAnimation, and the one clock that advances every
//! animation on a thread.

#include "QtLikeSignal/AbstractAnimation.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Timer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace QtLikeSignal
{
    //! The one clock that advances every running animation on its thread.
    //!
    //! **One of these for each thread, created when that thread first starts an animation.** It is
    //! what makes twenty animations cost one timer rather than twenty, and what keeps them in step
    //! with each other: they all see the same step, taken from one reading of the clock.
    //!
    //! Not public. An animation registers itself by starting, and the class documentation on
    //! AbstractAnimation describes the only two things a caller can do to the clock --
    //! tickExternally() and advanceAll().
    class AnimationClock
    {
    public:
        //! Constructs the clock with its timer stopped and nothing registered.
        AnimationClock() = default;

        //! Gets the calling thread's clock, creating it if this is the first animation on it.
        //!
        //! A thread_local rather than a member of Thread, because animation is an optional part of
        //! this library and a thread that never animates should not carry a vector and a Timer for
        //! it. Never destroyed before the thread ends.
        static AnimationClock& forCurrentThread()
        {
            static thread_local AnimationClock sClock;
            return sClock;
        }

        //! Adds @p aAnimation to the set this clock advances, and starts ticking if it was idle.
        void add
            (
            AbstractAnimation* aAnimation  //!< The animation to advance. Must not be null.
            )
        {
            if( std::find( mAnimations.begin(), mAnimations.end(), aAnimation ) !=
                mAnimations.end() )
            {
                return;
            }

            // A slot remove() emptied is filled again rather than appended past, which is what
            // bounds this vector by the most animations ever registered at once. compact() runs
            // only from tick(), so a program that starts and stops animations without advancing
            // the clock -- one driving animation from its own frame loop, between frames -- would
            // otherwise grow it by one on every cycle and never give the space back.
            if( mHasHoles )
            {
                const auto hole = std::find( mAnimations.begin(), mAnimations.end(),
                    static_cast<AbstractAnimation*>( nullptr ) );
                if( hole != mAnimations.end() )
                {
                    if( !anyLeft() )
                    {
                        resetStep();
                    }

                    *hole = aAnimation;
                    ensureTicking();
                    return;
                }

                // Every hole has been filled, so the next tick has nothing to compact.
                mHasHoles = false;
            }

            // **Restarted here, when the clock goes from idle to busy, and not inside
            // ensureTicking().** It used to live there, which meant it was skipped entirely
            // whenever the caller was driving the clock itself -- ensureTicking() returns at once
            // in that mode. The step is measured from the last tick, so an animation started after
            // a quiet period was charged the whole of that period on its first advance and jumped
            // straight to its end. A demo repainting once a second made every 900 ms animation
            // finish in one step.
            if( !anyLeft() )
            {
                resetStep();
            }

            mAnimations.push_back( aAnimation );
            ensureTicking();
        }

        //! Removes @p aAnimation, and stops ticking if it was the last one.
        void remove
            (
            AbstractAnimation* aAnimation  //!< The animation to stop advancing.
            )
        {
            const auto it = std::find( mAnimations.begin(), mAnimations.end(), aAnimation );
            if( it == mAnimations.end() )
            {
                return;
            }

            // Cleared rather than erased when a tick is in progress. tick() walks a copy, but the
            // live list is what the next tick reads, and erasing from under an in-progress walk
            // would still be a surprise for anything added between the two.
            *it = nullptr;
            mHasHoles = true;

            if( !anyLeft() )
            {
                mTimer.stop();
            }
        }

        //! Advances every registered animation by the time since the last tick.
        //!
        //! **The mark moves by what was consumed, not to now, and that is the whole of it.** The
        //! step is whole milliseconds, so a caller ticking faster than once a millisecond -- a
        //! render loop on a fast machine, or one that is not paced at all -- produces a step of
        //! zero every time. Moving the mark to `now` anyway would throw the remainder away on
        //! every call, and the animation would never accumulate a single millisecond however long
        //! it ran. Keeping the remainder means sixty sub-millisecond frames add up to a
        //! millisecond, exactly as they should.
        void tickFromClock()
        {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mLastTick );

            if( elapsed.count() <= 0 )
            {
                return;
            }

            mLastTick += elapsed;
            tick( static_cast<int>( elapsed.count() ) );
        }

        //! Advances every registered animation by exactly @p aMsec.
        void tick
            (
            int aMsec  //!< Step in milliseconds. Zero and negative do nothing.
            )
        {
            if( aMsec <= 0 )
            {
                return;
            }

            // Walked over a copy, because an animation that finishes inside advance() emits
            // finished(), and a slot on that signal is free to start another animation, stop this
            // one, or destroy either. Any of those would otherwise be a write to the vector being
            // walked.
            const std::vector<AbstractAnimation*> running = mAnimations;
            for( AbstractAnimation* const animation : running )
            {
                if( animation == nullptr )
                {
                    continue;
                }

                // Re-checked against the live list: a slot earlier in this same tick may have
                // stopped it, and advancing an animation that has been stopped would run it past
                // its own end.
                if( std::find( mAnimations.begin(), mAnimations.end(), animation ) ==
                    mAnimations.end() )
                {
                    continue;
                }

                animation->advance( aMsec );
            }

            compact();

            if( !anyLeft() )
            {
                mTimer.stop();
            }
        }

        //! Restarts the step measurement, so the next tick does not charge a long pause to it.
        //!
        //! Called when the first animation on an idle clock starts. Without it, a program that
        //! animates once a minute would hand the first tick a step of a minute and every animation
        //! would jump straight to its end.
        void resetStep()
        {
            mLastTick = std::chrono::steady_clock::now();
        }

    private:
        //! @return true if any registered slot still holds an animation.
        bool anyLeft() const
        {
            for( AbstractAnimation* const animation : mAnimations )
            {
                if( animation != nullptr )
                {
                    return true;
                }
            }
            return false;
        }

        //! Drops the emptied slots left by remove().
        void compact()
        {
            if( !mHasHoles )
            {
                return;
            }

            mAnimations.erase( std::remove( mAnimations.begin(), mAnimations.end(),
                static_cast<AbstractAnimation*>( nullptr ) ), mAnimations.end() );
            mHasHoles = false;
        }

        //! Starts the internal timer, unless the caller has taken the driving over.
        void ensureTicking();

        //! Every animation this clock advances. Holes are left by remove() and dropped by
        //! compact().
        std::vector<AbstractAnimation*> mAnimations;

        //! True while mAnimations holds a slot remove() emptied.
        bool mHasHoles { false };

        //! The internal clock. Not started until the first animation, and stopped after the last.
        Timer mTimer;

        //! True once mTimer has been connected, so it is wired exactly once.
        bool mTimerConnected { false };

        //! When the last tick happened, which is what the step is measured from.
        std::chrono::steady_clock::time_point mLastTick { std::chrono::steady_clock::now() };
    };

    namespace
    {
        //! How often the internal clock ticks, in milliseconds.
        //!
        //! About sixty times a second, which is what a display usually does and what an animation
        //! with no better information should assume. A program that knows better should switch the
        //! internal clock off and drive from its frame clock -- see
        //! AbstractAnimation::tickExternally().
        const int kInternalTickMs = 16;

        //! True when the caller has taken over the driving. See tickExternally().
        //!
        //! Process-wide rather than per-thread, because it describes how the *program* is built --
        //! a program driving animation from its render loop does so everywhere -- and because a
        //! per-thread flag would have to be set again on every thread that animates.
        std::atomic<bool> gExternalTick { false };
    }

    //! Starts the internal timer, unless the caller has taken the driving over.
    void AnimationClock::ensureTicking()
    {
        if( gExternalTick.load() )
        {
            return;
        }

        if( !mTimerConnected )
        {
            Object::connect( mTimer.getTimeout(), &mTimer, [this]()
                {
                    tickFromClock();
                }, ConnectionType::Direct );
            mTimerConnected = true;
        }

        if( !mTimer.isActive() )
        {
            mTimer.start( kInternalTickMs );
        }
    }

    //! Constructs a stopped animation of zero duration.
    AbstractAnimation::AbstractAnimation
        (
        Object* aParent  //!< Owner that will delete this animation; none by default.
        )
        : Object( aParent )
    {
    }

    //! Destroys the animation, taking it off its thread's clock first.
    //!
    //! The order matters and is the reason this destructor exists at all: a clock still holding
    //! the pointer would advance an object whose subclass has already been destroyed.
    AbstractAnimation::~AbstractAnimation()
    {
        AnimationClock::forCurrentThread().remove( this );
    }

    //! Sets how long one run takes. See the declaration.
    void AbstractAnimation::setDuration
        (
        int aMsec  //!< Length of one run in milliseconds; negative is treated as zero.
        )
    {
        mDuration = aMsec > 0 ? aMsec : 0;
    }

    //! Sets how many times the animation runs. See the declaration.
    void AbstractAnimation::setLoopCount
        (
        int aCount  //!< Number of runs, or -1 for indefinitely.
        )
    {
        if( aCount == 0 || aCount < -1 )
        {
            qCWarning( gLogTimer )
                << "AbstractAnimation::setLoopCount: a count of" << aCount
                << "is refused; use 1 for one run or -1 to run until stopped";
            return;
        }

        mLoopCount = aCount;
    }

    //! Sets the named curve the progress is shaped by, replacing any custom function.
    void AbstractAnimation::setEasing
        (
        Easing aCurve  //!< The curve to use.
        )
    {
        mEasing = aCurve;
        mEasingFunction = nullptr;
    }

    //! Sets a curve of the caller's own. An empty function puts the named curve back in charge.
    void AbstractAnimation::setEasingFunction
        (
        std::function<double( double )> aCurve  //!< Linear progress in, shaped progress out.
        )
    {
        mEasingFunction = std::move( aCurve );
    }

    //! Starts the animation from its beginning.
    //!
    //! **Starting one that is already running or paused restarts it**, which is what a caller who
    //! calls start() twice means. Qt differs here: QAbstractAnimation::start() returns at once when
    //! the animation is already running, and a program retargeting one has to stop it first. This
    //! is the more forgiving of the two, and the difference is worth knowing when porting.
    //!
    //! Taken to Stopped before it is taken to Running, and that is not decoration. changeState()
    //! reports only differences, so a restart would otherwise be a state change the subclass never
    //! hears about -- and a subclass reading its starting value from whatever it drives, which is
    //! what makes an animation interruptible, learns that a fresh run is beginning from the
    //! Stopped-to-Running move and from nothing else. Without the first step, a bar retargeted
    //! halfway across snapped back to the edge it started from.
    void AbstractAnimation::start()
    {
        changeState( AnimationState::Stopped );

        mCurrentTime = 0;
        mCurrentLoop = 0;

        AnimationClock& clock = AnimationClock::forCurrentThread();
        changeState( AnimationState::Running );
        clock.add( this );

        // The starting value is shown now rather than at the first tick, which is up to 16 ms
        // away. Without this a fade from transparent would be visible at its old opacity for a
        // frame, which is exactly the flash the fade existed to avoid.
        updateCurrentValue( shaped( 0.0 ) );
    }

    //! Stops the animation where it is. Does not emit finished(); see that signal.
    void AbstractAnimation::stop()
    {
        if( mState == AnimationState::Stopped )
        {
            return;
        }

        AnimationClock::forCurrentThread().remove( this );
        changeState( AnimationState::Stopped );
    }

    //! Holds the animation at its current position.
    void AbstractAnimation::pause()
    {
        if( mState != AnimationState::Running )
        {
            return;
        }

        // Taken off the clock, so that pausing the last running animation lets its timer stop.
        // Leaving it registered cost sixty wakeups a second on a thread where nothing was moving:
        // the clock keeps ticking while any slot is filled, and a paused animation fills one.
        //
        // Nothing is charged to the step after the pause, which is what leaving it registered used
        // to be for. resume() puts it back through add(), and add() restarts the step measurement
        // whenever the clock was idle -- so the pause is measured out rather than handed to the
        // first advance.
        AnimationClock::forCurrentThread().remove( this );
        changeState( AnimationState::Paused );
    }

    //! Continues a paused animation from where it stopped.
    void AbstractAnimation::resume()
    {
        if( mState != AnimationState::Paused )
        {
            return;
        }

        AnimationClock& clock = AnimationClock::forCurrentThread();
        changeState( AnimationState::Running );
        clock.add( this );
    }

    //! Switches the internal clock off or on for the whole process. See the declaration.
    void AbstractAnimation::tickExternally
        (
        bool aExternal  //!< True to drive animation yourself with advanceAll().
        )
    {
        gExternalTick.store( aExternal );
    }

    //! @return true if the internal clock has been switched off.
    bool AbstractAnimation::isTickingExternally()
    {
        return gExternalTick.load();
    }

    //! Advances every running animation on this thread by the time since the last tick.
    void AbstractAnimation::advanceAll()
    {
        AnimationClock::forCurrentThread().tickFromClock();
    }

    //! Advances every running animation on this thread by exactly @p aMsec.
    void AbstractAnimation::advanceAll
        (
        int aMsec  //!< Step in milliseconds. Zero and negative do nothing.
        )
    {
        AnimationClock::forCurrentThread().tick( aMsec );
    }

    //! The default reaction to a state change: nothing. See the declaration.
    void AbstractAnimation::updateState
        (
        AnimationState aNewState,  //!< The state now in force.
        AnimationState aOldState   //!< The state it was in before.
        )
    {
        static_cast<void>( aNewState );
        static_cast<void>( aOldState );
    }

    //! Advances this animation by @p aMsec. See the declaration.
    bool AbstractAnimation::advance
        (
        int aMsec  //!< Step in milliseconds.
        )
    {
        if( mState != AnimationState::Running )
        {
            return false;
        }

        // A duration of zero cannot be divided into, and means "arrive immediately". Handled here
        // rather than guarded at every division below.
        if( mDuration <= 0 )
        {
            // finishOrLoop() reports the end value itself when the last loop ends, so reporting it
            // here as well would send the same value twice within one step -- once to a subclass
            // that may be writing to a device. Only a run with another loop still to go needs it
            // from here.
            if( finishOrLoop() )
            {
                updateCurrentValue( shaped( 1.0 ) );
            }
            return mState == AnimationState::Running;
        }

        mCurrentTime += aMsec;

        while( mCurrentTime >= mDuration )
        {
            mCurrentTime -= mDuration;
            if( !finishOrLoop() )
            {
                // The last loop ended. The end value is reported by finishOrLoop() before
                // finished() is emitted, so there is nothing left to show here.
                return false;
            }
        }

        updateCurrentValue( shaped( static_cast<double>( mCurrentTime ) /
            static_cast<double>( mDuration ) ) );
        return true;
    }

    //! Ends one loop: starts the next, or finishes. Returns true if the animation is still running.
    bool AbstractAnimation::finishOrLoop()
    {
        ++mCurrentLoop;

        if( mLoopCount < 0 || mCurrentLoop < mLoopCount )
        {
            return true;
        }

        // Shown at its end before anything is told, so a finished() slot reading whatever this
        // animation drives sees the value it ended on rather than the last one before the step
        // that took it past the end.
        updateCurrentValue( shaped( 1.0 ) );

        mCurrentTime = mDuration;
        AnimationClock::forCurrentThread().remove( this );
        changeState( AnimationState::Stopped );

        // Emitted last. A slot is free to delete this animation, so nothing may touch it after.
        mFinished.emit();
        return false;
    }

    //! Shapes @p aProgress with whichever curve is in force.
    double AbstractAnimation::shaped
        (
        double aProgress  //!< Linear progress through the current run.
        ) const
    {
        if( mEasingFunction )
        {
            return mEasingFunction( aProgress );
        }
        return easingValue( mEasing, aProgress );
    }

    //! Moves to @p aState, telling the subclass if it is a change.
    void AbstractAnimation::changeState
        (
        AnimationState aState  //!< The state to move to.
        )
    {
        if( mState == aState )
        {
            return;
        }

        const AnimationState previous = mState;
        mState = aState;
        updateState( aState, previous );
    }
}
