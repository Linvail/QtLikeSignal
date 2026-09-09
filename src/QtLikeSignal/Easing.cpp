// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Evaluation of the easing curves declared in Easing.hpp.

#include "QtLikeSignal/Easing.hpp"

#include <cmath>

namespace QtLikeSignal
{
    namespace
    {
        //! How far Back_In and Back_Out pull back or overshoot.
        //!
        //! Robert Penner's constant, which every implementation of these curves uses, so that a
        //! designer moving between toolkits sees the same motion. It is 10 % of the travel,
        //! derived rather than chosen: the curve is defined to overshoot by that much.
        const double kBackOvershoot = 1.70158;

        //! The period of Elastic_Out's oscillation, as a fraction of the duration.
        const double kElasticPeriod = 0.3;

        //! Where each of Bounce_Out's landings happens, and how steep the parabola between them is.
        //!
        //! The same four segments every implementation of this curve uses. The value is a parabola
        //! whose peak shrinks at each landing, which is what a dropped object does.
        const double kBounceScale = 7.5625;
        const double kBounceStep  = 2.75;
    }

    //! Evaluates one easing curve. See the declaration for what the result means.
    double easingValue
        (
        Easing aCurve,    //!< The curve to apply.
        double aProgress  //!< Linear progress, clamped into [0, 1] before use.
        )
    {
        // Clamped here rather than at every call site. An animation that overruns its duration by
        // part of a frame is ordinary, and the answer wanted for it is the end value rather than
        // an extrapolation past it.
        double t = aProgress;
        if( t <= 0.0 )
        {
            return 0.0;
        }
        if( t >= 1.0 )
        {
            return 1.0;
        }

        switch( aCurve )
        {
        case Easing::Linear:
            return t;

        case Easing::Quadratic_In:
            return t * t;

        case Easing::Quadratic_Out:
            return 1.0 - ( 1.0 - t ) * ( 1.0 - t );

        case Easing::Quadratic_InOut:
            return t < 0.5
                   ? 2.0 * t * t
                   : 1.0 - 2.0 * ( 1.0 - t ) * ( 1.0 - t );

        case Easing::Cubic_In:
            return t * t * t;

        case Easing::Cubic_Out:
        {
            const double u = 1.0 - t;
            return 1.0 - u * u * u;
        }

        case Easing::Cubic_InOut:
        {
            if( t < 0.5 )
            {
                return 4.0 * t * t * t;
            }
            const double u = 1.0 - t;
            return 1.0 - 4.0 * u * u * u;
        }

        case Easing::Back_In:
            return t * t * ( ( kBackOvershoot + 1.0 ) * t - kBackOvershoot );

        case Easing::Back_Out:
        {
            const double u = t - 1.0;
            return 1.0 + u * u * ( ( kBackOvershoot + 1.0 ) * u + kBackOvershoot );
        }

        case Easing::Elastic_Out:
        {
            const double quarter = kElasticPeriod / 4.0;
            return std::pow( 2.0, -10.0 * t ) *
                   std::sin( ( t - quarter ) * ( 2.0 * 3.14159265358979323846 ) /
                kElasticPeriod ) + 1.0;
        }

        case Easing::Bounce_Out:
        {
            // Four parabolic segments, each starting where the previous one landed. Written as the
            // chain of thresholds every implementation uses, because the alternative -- deriving
            // the segment arithmetically -- is harder to check against a reference curve.
            if( t < 1.0 / kBounceStep )
            {
                return kBounceScale * t * t;
            }
            if( t < 2.0 / kBounceStep )
            {
                const double u = t - 1.5 / kBounceStep;
                return kBounceScale * u * u + 0.75;
            }
            if( t < 2.5 / kBounceStep )
            {
                const double u = t - 2.25 / kBounceStep;
                return kBounceScale * u * u + 0.9375;
            }
            const double u = t - 2.625 / kBounceStep;
            return kBounceScale * u * u + 0.984375;
        }
        }

        // Unreachable for any declared curve. Linear is the answer that does the least harm if a
        // value from a newer header ever reaches an older build of this file.
        return t;
    }
}
