// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for the animation half of the Property work: the easing curves,
//! AbstractAnimation's timing and looping, and PropertyAnimation driving a Property.
//!
//! **Every test drives the clock itself**, with AbstractAnimation::advanceAll( int ). A test that
//! waited for the internal 16 ms timer would be measuring the machine rather than the arithmetic,
//! and would be the kind of test that passes on a quiet desk and fails under a sanitizer.

#include "QtLikeSignal/AbstractAnimation.hpp"
#include "QtLikeSignal/PropertyAnimation.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/AbstractEventDispatcher.hpp"
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Easing.hpp"
#include "QtLikeSignal/Property.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <chrono>
#include <thread>
#include <vector>

using namespace QtLikeSignal;

namespace
{
    //! An animation that records every progress value it was given.
    class RecordingAnimation : public AbstractAnimation
    {
    public:
        //! Records one step.
        virtual void updateCurrentValue
            (
            double aProgress  //!< The shaped progress.
            ) override
        {
            mProgress.push_back( aProgress );
        }

        std::vector<double> mProgress;   //!< Every progress value seen, in order.
    };

    //! Keeps the external-tick flag switched on for as long as it lives.
    //!
    //! The flag is process-wide, so a test that set it and returned would leave every later test
    //! running without an internal clock. This is what makes that impossible to forget.
    class ScopedExternalTick
    {
    public:
        //! Switches the internal clock off.
        ScopedExternalTick()
        {
            AbstractAnimation::tickExternally( true );
        }

        //! Switches it back on.
        ~ScopedExternalTick()
        {
            AbstractAnimation::tickExternally( false );
        }

        ScopedExternalTick
            (
            const ScopedExternalTick&
            ) = delete;

        ScopedExternalTick& operator=
            (
            const ScopedExternalTick&
            ) = delete;

    };
}

//! Verifies every curve starts at 0, ends at 1, and clamps outside its range.
//!
//! The one property all eleven share, and the one a caller relies on without thinking: an
//! animation that did not reach its end value would leave whatever it drives slightly wrong
//! forever.
TEST( EasingTest, EveryCurveStartsAtZeroAndEndsAtOne )
{
    const Easing curves[] = {
        Easing::Linear,
        Easing::Quadratic_In, Easing::Quadratic_Out, Easing::Quadratic_InOut,
        Easing::Cubic_In, Easing::Cubic_Out, Easing::Cubic_InOut,
        Easing::Back_In, Easing::Back_Out,
        Easing::Elastic_Out, Easing::Bounce_Out
    };

    for( const Easing curve : curves )
    {
        EXPECT_DOUBLE_EQ( easingValue( curve, 0.0 ), 0.0 );
        EXPECT_DOUBLE_EQ( easingValue( curve, 1.0 ), 1.0 );

        // Progress is clamped, so an animation that overran its duration reports the end value
        // rather than an extrapolation past it.
        EXPECT_DOUBLE_EQ( easingValue( curve, -0.5 ), 0.0 );
        EXPECT_DOUBLE_EQ( easingValue( curve, 1.5 ), 1.0 );
    }
}

//! Verifies the curves have the shape their names claim.
TEST( EasingTest, TheCurvesHaveTheShapeTheirNamesClaim )
{
    // Linear is the identity.
    EXPECT_DOUBLE_EQ( easingValue( Easing::Linear, 0.25 ), 0.25 );

    // "In" is slow at the start, so it is behind linear halfway through.
    EXPECT_LT( easingValue( Easing::Quadratic_In, 0.5 ), 0.5 );
    EXPECT_LT( easingValue( Easing::Cubic_In, 0.5 ), 0.5 );

    // "Out" is fast at the start, so it is ahead.
    EXPECT_GT( easingValue( Easing::Quadratic_Out, 0.5 ), 0.5 );
    EXPECT_GT( easingValue( Easing::Cubic_Out, 0.5 ), 0.5 );

    // "InOut" is symmetric and passes through the middle at the middle.
    EXPECT_DOUBLE_EQ( easingValue( Easing::Quadratic_InOut, 0.5 ), 0.5 );
    EXPECT_DOUBLE_EQ( easingValue( Easing::Cubic_InOut, 0.5 ), 0.5 );

    // Back pulls below zero on the way in and above one on the way out. That overshoot is the
    // effect, which is why easingValue() clamps its input and not its output.
    EXPECT_LT( easingValue( Easing::Back_In, 0.2 ), 0.0 );
    EXPECT_GT( easingValue( Easing::Back_Out, 0.8 ), 1.0 );
}

//! Verifies an animation reports its start value immediately, before the first tick.
//!
//! Otherwise a fade from transparent shows one frame at its old opacity -- the exact flash the
//! fade was added to avoid.
TEST( AnimationTest, TheStartValueIsShownBeforeTheFirstTick )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 100 );
    animation.start();

    ASSERT_EQ( animation.mProgress.size(), 1u );
    EXPECT_DOUBLE_EQ( animation.mProgress[0], 0.0 );
    EXPECT_EQ( animation.state(), AnimationState::Running );
}

//! Verifies an animation advances through its duration and finishes exactly once.
TEST( AnimationTest, AnAnimationRunsItsDurationAndFinishesOnce )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 100 );

    int finished = 0;
    Object::connect( animation.getFinished(), &animation, [&finished]()
        {
            ++finished;
        } );

    animation.start();
    AbstractAnimation::advanceAll( 50 );
    EXPECT_EQ( animation.state(), AnimationState::Running );
    EXPECT_EQ( finished, 0 );
    EXPECT_DOUBLE_EQ( animation.mProgress.back(), 0.5 );

    AbstractAnimation::advanceAll( 50 );
    EXPECT_EQ( animation.state(), AnimationState::Stopped );
    EXPECT_EQ( finished, 1 );
    EXPECT_DOUBLE_EQ( animation.mProgress.back(), 1.0 )
        << "an animation that ended did not report its end value.";

    // Nothing further happens to a finished animation, however long the clock runs.
    AbstractAnimation::advanceAll( 500 );
    EXPECT_EQ( finished, 1 );
}

//! Verifies stop() does not report a finish.
//!
//! The distinction that makes chaining from finished() safe: a caller that cancelled an animation
//! must not have the next one start as though the first had completed.
TEST( AnimationTest, StoppingDoesNotReportAFinish )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 100 );

    int finished = 0;
    Object::connect( animation.getFinished(), &animation, [&finished]()
        {
            ++finished;
        } );

    animation.start();
    AbstractAnimation::advanceAll( 50 );
    animation.stop();

    EXPECT_EQ( animation.state(), AnimationState::Stopped );
    EXPECT_EQ( finished, 0 ) << "a cancelled animation reported that it had finished.";

    AbstractAnimation::advanceAll( 100 );
    EXPECT_EQ( finished, 0 );
}

//! Verifies pause holds the position and resume continues from it.
TEST( AnimationTest, PauseHoldsAndResumeContinues )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 100 );
    animation.start();

    AbstractAnimation::advanceAll( 40 );
    animation.pause();
    EXPECT_EQ( animation.state(), AnimationState::Paused );

    const std::size_t stepsAtPause = animation.mProgress.size();
    AbstractAnimation::advanceAll( 1000 );
    EXPECT_EQ( animation.mProgress.size(), stepsAtPause )
        << "a paused animation advanced.";

    animation.resume();
    EXPECT_EQ( animation.state(), AnimationState::Running );
    AbstractAnimation::advanceAll( 10 );
    EXPECT_DOUBLE_EQ( animation.mProgress.back(), 0.5 )
        << "resuming did not continue from where the pause left it.";
}

//! Verifies a looping animation repeats and reports a finish only at the end of the last run.
TEST( AnimationTest, ALoopingAnimationRepeatsAndFinishesOnceAtTheEnd )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 100 );
    animation.setLoopCount( 3 );

    int finished = 0;
    Object::connect( animation.getFinished(), &animation, [&finished]()
        {
            ++finished;
        } );

    animation.start();

    AbstractAnimation::advanceAll( 250 );
    EXPECT_EQ( animation.currentLoop(), 2 ) << "two runs should have completed by 250 ms.";
    EXPECT_EQ( finished, 0 );

    AbstractAnimation::advanceAll( 50 );
    EXPECT_EQ( finished, 1 );
    EXPECT_EQ( animation.state(), AnimationState::Stopped );
}

//! Verifies a loop count of zero is refused rather than producing an animation that does nothing.
TEST( AnimationTest, ALoopCountOfZeroIsRefused )
{
    CoreApplication app;

    RecordingAnimation animation;
    EXPECT_EQ( animation.loopCount(), 1 );

    animation.setLoopCount( 0 );
    EXPECT_EQ( animation.loopCount(), 1 ) << "a loop count of zero was accepted.";

    animation.setLoopCount( -5 );
    EXPECT_EQ( animation.loopCount(), 1 ) << "a negative loop count other than -1 was accepted.";

    animation.setLoopCount( -1 );
    EXPECT_EQ( animation.loopCount(), -1 ) << "-1 means run until stopped and must be allowed.";
}

//! Verifies a duration of zero arrives at the end on the first tick rather than dividing by zero.
TEST( AnimationTest, AZeroDurationArrivesImmediately )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 0 );

    int finished = 0;
    Object::connect( animation.getFinished(), &animation, [&finished]()
        {
            ++finished;
        } );

    animation.start();
    AbstractAnimation::advanceAll( 1 );

    EXPECT_EQ( finished, 1 );
    EXPECT_DOUBLE_EQ( animation.mProgress.back(), 1.0 );

    // start() shows the beginning and the tick shows the end. A third entry would mean the end was
    // reported twice in one step, which a subclass writing to a device would send twice.
    EXPECT_EQ( animation.mProgress.size(), 2u )
        << "the end value was reported more than once for a single zero-duration run.";
}

//! Verifies a custom easing function is used in preference to the named curve.
TEST( AnimationTest, ACustomEasingFunctionReplacesTheNamedCurve )
{
    CoreApplication app;
    ScopedExternalTick external;

    RecordingAnimation animation;
    animation.setDuration( 100 );
    animation.setEasing( Easing::Cubic_In );
    animation.setEasingFunction( []( double aProgress )
        {
            return aProgress * 0.5;
        } );

    animation.start();
    AbstractAnimation::advanceAll( 40 );
    EXPECT_DOUBLE_EQ( animation.mProgress.back(), 0.2 );

    // An empty function puts the named curve back in charge.
    animation.setEasingFunction( nullptr );
    animation.setEasing( Easing::Linear );
    animation.start();
    AbstractAnimation::advanceAll( 40 );
    EXPECT_DOUBLE_EQ( animation.mProgress.back(), 0.4 );
}

//! Verifies a PropertyAnimation moves the property and the property reports every step.
//!
//! The whole point of building animation on Property: the thing being animated and the thing being
//! watched are the same thing, so a label connected to the property redraws without knowing an
//! animation exists.
TEST( PropertyAnimationTest, ItMovesThePropertyAndTheChangesAreReported )
{
    CoreApplication app;
    ScopedExternalTick external;

    Object owner;
    Property<double> opacity( &owner, 0.0 );

    std::vector<double> reported;
    Object::connect( opacity.getChanged(), &owner, [&reported]( double aValue )
        {
            reported.push_back( aValue );
        } );

    PropertyAnimation<double> fade( opacity );
    fade.setRange( 0.0, 1.0 );
    fade.setDuration( 100 );
    fade.start();

    AbstractAnimation::advanceAll( 50 );
    EXPECT_DOUBLE_EQ( opacity.get(), 0.5 );

    AbstractAnimation::advanceAll( 50 );
    EXPECT_DOUBLE_EQ( opacity.get(), 1.0 );

    ASSERT_FALSE( reported.empty() );
    EXPECT_DOUBLE_EQ( reported.back(), 1.0 );
    EXPECT_DOUBLE_EQ( reported.front(), 0.5 )
        << "the first reported value should be the first that differed from the start.";
}

//! Verifies an animation with no start value begins wherever the property already is.
//!
//! What an interruptible animation needs: a fade retargeted halfway through must continue from the
//! opacity on screen, not jump back to where the first fade began.
TEST( PropertyAnimationTest, WithNoStartValueItBeginsWhereThePropertyIs )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<double> value( 0.25 );

    PropertyAnimation<double> move( value );
    move.setEndValue( 1.25 );
    move.setDuration( 100 );
    move.start();

    EXPECT_DOUBLE_EQ( move.startValue(), 0.25 )
        << "the start value should have been read from the property at start().";

    AbstractAnimation::advanceAll( 50 );
    EXPECT_DOUBLE_EQ( value.get(), 0.75 );
}

//! Verifies a looping animation returns to its start rather than walking away from it.
//!
//! The defect this guards is re-reading the property at the start of every loop, which would make
//! each run begin where the last one ended.
TEST( PropertyAnimationTest, ALoopReturnsToItsStart )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<int> position( 0 );

    PropertyAnimation<int> sweep( position );
    sweep.setEndValue( 100 );
    sweep.setDuration( 100 );
    sweep.setLoopCount( 3 );
    sweep.start();

    AbstractAnimation::advanceAll( 100 );   // end of the first run
    AbstractAnimation::advanceAll( 25 );    // a quarter into the second

    EXPECT_EQ( position.get(), 25 )
        << "the second run began where the first ended instead of at the start.";
}

//! Verifies an animation destroyed while running takes itself off the clock.
//!
//! Otherwise the clock advances an object whose subclass has already been destroyed, which is a
//! use-after-free that AddressSanitizer would report and a release build would not.
TEST( PropertyAnimationTest, DestroyingARunningAnimationIsSafe )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<double> value( 0.0 );

    {
        PropertyAnimation<double> move( value );
        move.setRange( 0.0, 1.0 );
        move.setDuration( 100 );
        move.start();
        AbstractAnimation::advanceAll( 20 );
    }

    // The clock still exists and is asked to tick. Nothing should be left on it.
    AbstractAnimation::advanceAll( 100 );
    SUCCEED();
}

//! Verifies an animation started after a quiet period does not have that period charged to it.
//!
//! **The defect this guards made animation useless in exactly the case it was written for.** The
//! step is measured from the last tick, and the reset that restarts that measurement used to live
//! inside the function that starts the internal timer -- which returns immediately when the caller
//! is driving the clock itself. So a paced program that animated once in a while had the whole
//! idle gap charged to the first advance, and every animation shorter than the gap finished in one
//! step. The Wayland demo repainted once a second, which made a 900 ms sweep jump straight to its
//! end.
//!
//! Uses the wall clock deliberately, because that is what the defect was in: advanceAll( int )
//! takes the step it is given and could never have shown this. The margin is wide -- a 150 ms
//! pause against a 60 ms animation -- so a slow machine makes the pause longer rather than making
//! the test flaky.
TEST( AnimationTest, AQuietPeriodIsNotChargedToTheNextAnimation )
{
    CoreApplication app;
    ScopedExternalTick external;

    // Establishes a last-tick time, as a program repainting on a timer would.
    AbstractAnimation::advanceAll();

    std::this_thread::sleep_for( std::chrono::milliseconds( 150 ) );

    RecordingAnimation animation;
    animation.setDuration( 60 );
    animation.start();

    AbstractAnimation::advanceAll();

    EXPECT_EQ( animation.state(), AnimationState::Running )
        << "the pause before start() was charged to the animation, which finished at once.";
    ASSERT_FALSE( animation.mProgress.empty() );
    EXPECT_LT( animation.mProgress.back(), 0.5 )
        << "the first step should be a frame, not the whole quiet period.";
}

//! Verifies the external-tick flag is reported and restored.
TEST( AnimationTest, TheExternalTickFlagIsReported )
{
    EXPECT_FALSE( AbstractAnimation::isTickingExternally() );

    {
        ScopedExternalTick external;
        EXPECT_TRUE( AbstractAnimation::isTickingExternally() );
    }

    EXPECT_FALSE( AbstractAnimation::isTickingExternally() )
        << "the flag is process-wide, so a test that left it set would break every later one.";
}

//! Verifies a restart while running begins again from where the property is now.
//!
//! The interruptible case the class documentation promises and the demos rely on: a click that
//! retargets a bar halfway across must carry on from where the bar is, not jump back to the edge
//! it left. start() on a running animation is a restart, so the starting value has to be read
//! again -- reading it only on a Stopped-to-Running move would skip exactly this case.
TEST( PropertyAnimationTest, ARestartWhileRunningBeginsWhereThePropertyIs )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<double> value( 0.0 );

    PropertyAnimation<double> move( value );
    move.setEndValue( 100.0 );
    move.setDuration( 1000 );
    move.start();

    AbstractAnimation::advanceAll( 500 );
    ASSERT_DOUBLE_EQ( value.get(), 50.0 );

    move.setEndValue( 200.0 );
    move.start();

    EXPECT_DOUBLE_EQ( value.get(), 50.0 )
        << "the restart snapped the property back to where the first run began.";
    EXPECT_DOUBLE_EQ( move.startValue(), 50.0 );

    AbstractAnimation::advanceAll( 500 );
    EXPECT_DOUBLE_EQ( value.get(), 125.0 )
        << "half of the way from 50 to the new end of 200 is 125.";
}

//! Verifies a restart while paused reads the starting value again, as a restart while running does.
//!
//! Distinct from resume(), which must *not* re-read: it continues a run rather than beginning one.
TEST( PropertyAnimationTest, ARestartWhilePausedBeginsWhereThePropertyIs )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<double> value( 0.0 );

    PropertyAnimation<double> move( value );
    move.setEndValue( 100.0 );
    move.setDuration( 1000 );
    move.start();

    AbstractAnimation::advanceAll( 500 );
    move.pause();
    ASSERT_DOUBLE_EQ( value.get(), 50.0 );

    move.start();
    EXPECT_EQ( move.state(), AnimationState::Running );
    EXPECT_DOUBLE_EQ( move.startValue(), 50.0 );
    EXPECT_DOUBLE_EQ( value.get(), 50.0 );
}

//! Verifies resume() carries on rather than starting again.
//!
//! The other half of the test above. Re-reading the property here would restart the run from the
//! paused position with a fresh duration, which is a different animation from the one paused.
TEST( PropertyAnimationTest, AResumeContinuesTheRunItPaused )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<double> value( 0.0 );

    PropertyAnimation<double> move( value );
    move.setEndValue( 100.0 );
    move.setDuration( 1000 );
    move.start();

    AbstractAnimation::advanceAll( 500 );
    move.pause();
    move.resume();

    EXPECT_DOUBLE_EQ( move.startValue(), 0.0 )
        << "resume() re-read the property, so the paused run became a new one.";

    AbstractAnimation::advanceAll( 500 );
    EXPECT_DOUBLE_EQ( value.get(), 100.0 );
}

//! Verifies an overshooting curve on an unsigned property clamps instead of wrapping.
//!
//! Back_In goes below zero early in its run. Converting a negative double to an unsigned integer
//! is undefined behaviour, and in practice produces a value near the top of the range -- so a bar
//! easing in from zero would flash at four billion pixels before settling.
TEST( PropertyAnimationTest, AnOvershootOnAnUnsignedPropertyClamps )
{
    CoreApplication app;
    ScopedExternalTick external;

    Property<unsigned int> value( 0u );

    PropertyAnimation<unsigned int> move( value );
    move.setEndValue( 100u );
    move.setDuration( 100 );
    move.setEasing( Easing::Back_In );
    move.start();

    AbstractAnimation::advanceAll( 10 );

    EXPECT_EQ( value.get(), 0u )
        << "a blend below zero was converted to unsigned rather than clamped.";
}

//! Verifies pausing the last running animation lets the internal clock stop.
//!
//! A paused animation moves nothing, so a timer still firing sixty times a second for it wakes the
//! thread for no work at all. This is the only test here that leaves the internal clock switched
//! on, because the timer is the thing under test.
TEST( AnimationTest, PausingTheLastAnimationStopsTheInternalClock )
{
    CoreApplication app;

    Thread* const self = Thread::currentThread();
    ASSERT_NE( self, nullptr );

    auto dispatcher = self->eventDispatcher();
    ASSERT_NE( dispatcher, nullptr );
    ASSERT_EQ( dispatcher->remainingTimeMs(), -1 )
        << "this thread already had a timer, so the clock's would not be the earliest.";

    RecordingAnimation animation;
    animation.setDuration( 1000 );
    animation.start();

    EXPECT_GE( dispatcher->remainingTimeMs(), 0 )
        << "starting an animation did not arm the internal clock.";

    animation.pause();
    EXPECT_EQ( dispatcher->remainingTimeMs(), -1 )
        << "the internal clock kept ticking with every animation paused.";

    animation.resume();
    EXPECT_GE( dispatcher->remainingTimeMs(), 0 )
        << "resuming did not arm the internal clock again.";

    animation.stop();
    EXPECT_EQ( dispatcher->remainingTimeMs(), -1 );
}
