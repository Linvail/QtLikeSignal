// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::PropertyAnimation -- moves a Property from one value to another over time.

#ifndef QT_LIKE_SIGNAL_PROPERTYANIMATION_HPP
#define QT_LIKE_SIGNAL_PROPERTYANIMATION_HPP

#include "QtLikeSignal/AbstractAnimation.hpp"
#include "QtLikeSignal/Property.hpp"

#include <limits>
#include <type_traits>

namespace QtLikeSignal
{
    //! Blends @p aFrom and @p aTo by @p aProgress, for a type arithmetic enough to be blended.
    //!
    //! **This is the customisation point for animating a type this library has never heard of.**
    //! A program with its own colour, vector or angle type declares its own `propertyLerp` in that
    //! type's namespace, and argument-dependent lookup finds it in preference to this one:
    //!
    //! @code
    //!   namespace MyGame
    //!   {
    //!       Colour propertyLerp( const Colour& aFrom, const Colour& aTo, double aProgress )
    //!       {
    //!           return Colour::blend( aFrom, aTo, aProgress );
    //!       }
    //!   }
    //! @endcode
    //!
    //! @p aProgress is the *shaped* progress from the easing curve, so it may be slightly outside
    //! [0, 1] where the curve overshoots. An implementation that cannot represent a value past the
    //! end should clamp; one that can -- a position, a rotation -- should not, because the
    //! overshoot is the effect.
    //!
    //! Computed in double and converted back, so that an integer property animated over a long
    //! duration advances smoothly rather than in the steps integer arithmetic would give it.
    //!
    //! **An integer result is held inside its own type before the conversion**, which is a
    //! correctness matter rather than a nicety: converting a floating-point value that the target
    //! integer type cannot represent is undefined behaviour, and an overshooting curve produces
    //! exactly that. Back_In on an unsigned property goes below zero one tick in, and the
    //! conversion turned a bar easing in from the left edge into a bar four billion pixels wide.
    //! A floating-point T is left alone: the overshoot is representable, and it is the effect the
    //! curve was chosen for.
    template <typename T>
    T propertyLerp
        (
        const T& aFrom,    //!< Value at a progress of 0.
        const T& aTo,      //!< Value at a progress of 1.
        double aProgress   //!< Shaped progress, usually in [0, 1].
        )
    {
        static_assert( std::is_arithmetic<T>::value,
            "PropertyAnimation<T> does not know how to blend this type. Declare a propertyLerp( "
            "const T&, const T&, double ) in T's own namespace and it will be found." );

        const double from    = static_cast<double>( aFrom );
        const double to      = static_cast<double>( aTo );
        const double blended = from + ( to - from ) * aProgress;

        if constexpr( std::is_integral<T>::value )
        {
            // Compared in double, where both ends are exactly the value being tested against or
            // safely past it, rather than converting first and checking afterwards -- by then the
            // damage is done.
            if( blended <= static_cast<double>( std::numeric_limits<T>::lowest() ) )
            {
                return std::numeric_limits<T>::lowest();
            }
            if( blended >= static_cast<double>( std::numeric_limits<T>::max() ) )
            {
                return std::numeric_limits<T>::max();
            }
        }

        return static_cast<T>( blended );
    }

    //----------------------------------------------------------------
    //! @class PropertyAnimation
    //!
    //! Moves a Property from one value to another over time.
    //!
    //! @code
    //!   Property<double> opacity( &widget, 0.0 );
    //!   PropertyAnimation<double> fade( opacity );
    //!   fade.setRange( 0.0, 1.0 );
    //!   fade.setDuration( 300 );
    //!   fade.setEasing( Easing::Cubic_Out );
    //!   fade.start();
    //! @endcode
    //!
    //! The property announces every step, so anything connected to it redraws without knowing an
    //! animation exists. That is the point of building this on Property rather than on a callback:
    //! the thing being animated and the thing being watched are the same thing.
    //!
    //! **The animation does not own the property and must not outlive it.** A property is normally
    //! a member of the object being animated and the animation a member beside it, which makes the
    //! rule hold by construction.
    //!
    //! **A start value of "wherever it is now"** is what setEndValue() alone gives: leave the start
    //! unset and the animation reads the property when it starts, which is what an interruptible
    //! animation needs. Setting both is for a move that must always begin in the same place.
    //!
    //! @tparam T The property's type. It must be blendable -- see propertyLerp().
    //----------------------------------------------------------------
    template <typename T>
    class PropertyAnimation : public AbstractAnimation
    {
    public:
        //! Constructs an animation that will drive @p aProperty.
        explicit PropertyAnimation
            (
            Property<T>& aProperty,    //!< The property to move. Must outlive this animation.
            Object* aParent = nullptr  //!< Owner that will delete this animation; none by default.
            )
            : AbstractAnimation( aParent )
            , mProperty( &aProperty )
        {
        }

        //! Sets both ends of the move.
        void setRange
            (
            T aFrom,  //!< Value at the start of each run.
            T aTo     //!< Value at the end of each run.
            )
        {
            mFrom = std::move( aFrom );
            mTo = std::move( aTo );
            mHasFrom = true;
        }

        //! Sets where the move starts. Leave it unset to start from wherever the property is.
        void setStartValue
            (
            T aFrom  //!< Value at the start of each run.
            )
        {
            mFrom = std::move( aFrom );
            mHasFrom = true;
        }

        //! Sets where the move ends.
        void setEndValue
            (
            T aTo  //!< Value at the end of each run.
            )
        {
            mTo = std::move( aTo );
        }

        //! @return the value each run starts from, which is meaningless until one has.
        const T& startValue() const
        {
            return mFrom;
        }

        //! @return the value each run ends at.
        const T& endValue() const
        {
            return mTo;
        }

        //! @return the property being moved.
        Property<T>& property() const
        {
            return *mProperty;
        }

    protected:
        //! Writes the blended value into the property.
        void updateCurrentValue
            (
            double aProgress  //!< Shaped progress through the current run.
            ) override
        {
            mProperty->set( propertyLerp( mFrom, mTo, aProgress ) );
        }

        //! Reads the starting value from the property, if the caller did not supply one.
        //!
        //! Done here rather than in start() so that it happens on every run of a looping
        //! animation... which is exactly what it must **not** do, and why the flag is checked:
        //! re-reading on each loop would make the second run start where the first ended, and a
        //! loop would walk away from its origin instead of repeating.
        void updateState
            (
            AnimationState aNewState,  //!< The state now in force.
            AnimationState aOldState   //!< The state it was in before.
            ) override
        {
            if( aNewState == AnimationState::Running &&
                aOldState == AnimationState::Stopped && !mHasFrom )
            {
                mFrom = mProperty->get();
            }
        }

    private:
        //! The property being moved. Not owned, and must outlive this animation.
        Property<T>* mProperty;

        //! Value each run starts from. Read from the property at start() unless mHasFrom.
        T mFrom {};

        //! Value each run ends at.
        T mTo {};

        //! True once the caller has supplied a start value, so it is not read from the property.
        bool mHasFrom { false };
    };
}

#endif // QT_LIKE_SIGNAL_PROPERTYANIMATION_HPP
