// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The easing curves an animation can follow, and the function that evaluates one.

#ifndef QT_LIKE_SIGNAL_EASING_HPP
#define QT_LIKE_SIGNAL_EASING_HPP

namespace QtLikeSignal
{
    //! How an animation's progress is shaped between its start and its end.
    //!
    //! Every curve maps a progress of 0 to 0 and a progress of 1 to 1. What differs is the path
    //! between them, and that is the whole difference between a value that slides and one that
    //! moves like an object.
    //!
    //! **Eleven, where Qt has forty-one.** The rest are variations nobody names twice in a real
    //! program, and an application that wants one can pass its own function to
    //! AbstractAnimation::setEasingFunction() rather than wait for this list to grow.
    //!
    //! "In" means the curve is slow at the start, "Out" that it is slow at the end, and "InOut"
    //! both. Out is the one to reach for by default: a thing that arrives gently reads as
    //! deliberate, and a thing that leaves gently reads as reluctant.
    enum class Easing
    {
        //! No shaping. Progress is the value. Right for a rotation that must not vary in speed.
        Linear,

        Quadratic_In,     //!< Starts still and accelerates. t^2.
        Quadratic_Out,    //!< Starts fast and settles. The default choice for a UI move.
        Quadratic_InOut,  //!< Still at both ends.

        Cubic_In,         //!< As Quadratic_In, more pronounced. t^3.
        Cubic_Out,        //!< As Quadratic_Out, more pronounced.
        Cubic_InOut,      //!< Still at both ends, more pronounced.

        //! Pulls back before moving forward, as a thing with weight does.
        Back_In,

        //! Overshoots the target and returns to it.
        Back_Out,

        //! Overshoots and oscillates to a stop, as something on a spring does.
        Elastic_Out,

        //! Lands, bounces, lands again, smaller each time.
        Bounce_Out
    };

    //! Evaluates @p aCurve at progress @p aProgress.
    //!
    //! @p aProgress is clamped into [0, 1] before the curve is applied, so an animation that
    //! overruns its duration by a frame reports its end value rather than an extrapolation.
    //!
    //! The result is **not** clamped. Back_Out and Elastic_Out deliberately leave [0, 1] partway
    //! through -- that overshoot is what makes them look like motion -- and clamping the result
    //! would flatten exactly the part worth having.
    //!
    //! @return the shaped progress: 0.0 at aProgress 0, 1.0 at aProgress 1, and something between
    //! or slightly beyond in between.
    double easingValue
        (
        Easing aCurve,    //!< The curve to apply.
        double aProgress  //!< Linear progress through the animation, clamped into [0, 1].
        );

}

#endif // QT_LIKE_SIGNAL_EASING_HPP
