// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::AbstractAnimation -- something that changes over time, advanced by one clock for
//! each thread.

#ifndef QT_LIKE_SIGNAL_ABSTRACTANIMATION_HPP
#define QT_LIKE_SIGNAL_ABSTRACTANIMATION_HPP

#include "QtLikeSignal/AnimationState.hpp"
#include "QtLikeSignal/Easing.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"

#include <functional>

namespace QtLikeSignal
{
    //----------------------------------------------------------------
    //! @class AbstractAnimation
    //!
    //! Something that changes over time. Subclass it and implement updateCurrentValue().
    //!
    //! PropertyAnimation is the subclass this library ships, and it is what most programs want.
    //! This exists separately because plenty of things want to be driven by the same clock without
    //! being a property -- a sprite's frame index, a camera path, a shader parameter that lives in
    //! a renderer.
    //!
    //! **One clock advances every animation on a thread, not one timer each.** Twenty animations
    //! would otherwise be twenty timers in the dispatcher's heap, all expiring at about the same
    //! moment and each waking the loop. Qt reaches the same conclusion with QUnifiedTimer, and for
    //! the same reason.
    //!
    //! **That clock can be replaced**, which is the point of tickExternally(). By default it is a
    //! Timer at about 60 ticks a second, which works on any thread including one with no window.
    //! A renderer that presents a frame every time round its loop -- a game, a simulation -- should
    //! hand the driving over to that loop, so animation advances in step with what is shown rather
    //! than beating against it:
    //!
    //! @code
    //!   AbstractAnimation::tickExternally( true );          // stop the internal timer
    //!   for( ;; )                                           // the program's own frame loop
    //!   {
    //!       AbstractAnimation::advanceAll();                // ...which drives animation
    //!       renderer.drawAndPresent();
    //!   }
    //! @endcode
    //!
    //! **Do not do this in a program that draws only when something changed.** It is the obvious
    //! thing to try and it fails in two ways, both measured rather than imagined:
    //!
    //!   - On a window with immediate update delivery, a paint that asks for the next frame while
    //!     an animation runs is a loop with nothing throttling it. It repaints thousands of times
    //!     a second.
    //!   - On a window paced by the display it deadlocks instead. requestUpdate() arms a frame
    //!     callback that fires only after the surface is next presented, and such a program
    //!     presents only from inside its paint, which runs only when the callback fires. An
    //!     animation started from outside a paint arms a callback nothing will trigger.
    //!
    //! For that shape of program, leave the internal clock alone and let the animation ask for a
    //! repaint by changing the value, which is what a Property already does. The Wayland demo is
    //! written that way and says so.
    //!
    //! **An animation belongs to the thread it was constructed on**, like every other Object, and
    //! start(), stop() and the rest must be called from there.
    //----------------------------------------------------------------
    class AbstractAnimation : public Object
    {
    public:
        //! Constructs a stopped animation of zero duration.
        explicit AbstractAnimation
            (
            Object* aParent = nullptr  //!< Owner that will delete this animation; none by default.
            );

        //! Stops the animation, so it is off its thread's clock before anything of it is destroyed.
        ~AbstractAnimation() override;

        AbstractAnimation
            (
            const AbstractAnimation&
            ) = delete;

        AbstractAnimation& operator=
            (
            const AbstractAnimation&
            ) = delete;

        //! Sets how long one run takes, in milliseconds. Negative is treated as zero.
        //!
        //! A duration of zero is legal and useful: the animation jumps to its end value on the
        //! next tick and finishes, which is what a program wants when it has a general animation
        //! path and one case that should be instant.
        void setDuration
            (
            int aMsec
            );

        //! @return the duration of one run, in milliseconds.
        int duration() const
        {
            return mDuration;
        }

        //! Sets how many times the animation runs. 1 by default; -1 runs until stopped.
        //!
        //! Zero and other negative values are refused and reported, because a loop count of zero
        //! is far more often a calculation that went wrong than a request for an animation that
        //! does nothing.
        void setLoopCount
            (
            int aCount
            );

        //! @return how many times the animation runs, or -1 for indefinitely.
        int loopCount() const
        {
            return mLoopCount;
        }

        //! Sets the curve the progress is shaped by. Easing::Linear by default.
        //!
        //! Replaces any function given to setEasingFunction().
        void setEasing
            (
            Easing aCurve
            );

        //! Sets a curve of the caller's own, for a shape Easing does not name.
        //!
        //! Takes linear progress in [0, 1] and returns shaped progress, which may leave that range
        //! -- an overshoot is a curve, not a fault. An empty function puts the enum back in
        //! charge.
        void setEasingFunction
            (
            std::function<double( double )> aCurve
            );

        void start();

        void stop();

        void pause();

        void resume();

        //! @return whether the animation is running, paused or stopped.
        AnimationState state() const
        {
            return mState;
        }

        //! @return how far into the current run the animation is, in milliseconds.
        int currentTime() const
        {
            return mCurrentTime;
        }

        //! @return which run the animation is on, counting from zero.
        int currentLoop() const
        {
            return mCurrentLoop;
        }

        //! Emitted when the animation finishes of its own accord, on its own thread.
        //!
        //! **Not emitted by stop().** A caller that stopped an animation knows it stopped; the
        //! signal exists to report the thing the caller cannot see, which is the end arriving. Qt
        //! draws the same line, and it is what makes chaining two animations from finished()
        //! behave when the first is cancelled.
        //!
        //! An animation with a loop count of -1 never emits it.
        SignalView<>& getFinished() const
        {
            return mFinished.view();
        }

        //! Stops the internal clock, so the caller advances every animation itself.
        //!
        //! Process-wide and off by default. See the class documentation for why a program with a
        //! paced window wants it, and what it must then call.
        //!
        //! Switching it on does not stop a running animation; it stops the timer that was
        //! advancing it, so nothing moves again until advanceAll() is called.
        static void tickExternally
            (
            bool aExternal
            );

        //! @return true if the internal clock has been switched off.
        static bool isTickingExternally();

        //! Advances every running animation on the calling thread by the time since the last tick.
        //!
        //! For a caller that has switched the internal clock off. Safe to call at any rate: the
        //! step is measured from the clock rather than assumed, so calling it twice in a row
        //! advances almost nothing rather than double.
        static void advanceAll();

        //! Advances every running animation on the calling thread by exactly @p aMsec.
        //!
        //! For a test, which needs a step it chose rather than one the wall clock happened to
        //! give. Also for a program replaying at a fixed step. A negative step is ignored.
        static void advanceAll
            (
            int aMsec
            );

    protected:
        //! Called on every tick with the shaped progress of the current run.
        //!
        //! @p aProgress is what the easing curve produced: 0.0 at the start and 1.0 at the end,
        //! and possibly outside that in between if the curve overshoots. A subclass turns it into
        //! whatever it is animating.
        virtual void updateCurrentValue
            (
            double aProgress
            ) = 0;

        //! Called when the state changes, for a subclass that has to react.
        //!
        //! The default does nothing. Overriding it is how a subclass takes a starting value at the
        //! moment the animation begins rather than at the moment it was configured.
        virtual void updateState
            (
            AnimationState aNewState,  //!< The state now in force.
            AnimationState aOldState   //!< The state it was in before.
            );

    private:
        //! Advances this animation by @p aMsec and reports whether it is still running.
        //!
        //! The whole of the timing rule, in one place: accumulate, wrap at the duration for each
        //! completed loop, and finish when the last loop is done.
        bool advance
            (
            int aMsec
            );

        //! Ends one loop: starts the next, or finishes and reports it.
        //!
        //! @return true if the animation is still running afterwards.
        bool finishOrLoop();

        //! Shapes @p aProgress with whichever curve is in force.
        double shaped
            (
            double aProgress
            ) const;

        //! Moves to @p aState, reporting it to the subclass if it is a change.
        void changeState
            (
            AnimationState aState
            );

        //! Emitted when the animation finishes of its own accord. See getFinished().
        mutable Signal<> mFinished;

        //! Length of one run, in milliseconds. Never negative.
        int mDuration { 0 };

        //! How many runs, or -1 for indefinitely. Never zero.
        int mLoopCount { 1 };

        //! How far into the current run, in milliseconds.
        int mCurrentTime { 0 };

        //! Which run this is, counting from zero.
        int mCurrentLoop { 0 };

        //! Running, paused or stopped.
        AnimationState mState { AnimationState::Stopped };

        //! The named curve, used when mEasingFunction is empty.
        Easing mEasing { Easing::Linear };

        //! A curve of the caller's own. Empty unless setEasingFunction() was given one.
        std::function<double( double )> mEasingFunction;

        //! Grants the per-thread clock the right to advance an animation.
        //!
        //! The alternative was a public advance(), which would let any caller move one animation
        //! out of step with the rest on its thread -- the one thing a single shared clock exists
        //! to prevent.
        friend class AnimationClock;
    };
}

#endif // QT_LIKE_SIGNAL_ABSTRACTANIMATION_HPP
