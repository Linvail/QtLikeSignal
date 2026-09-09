// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::Property -- a value that announces when it changes.

#ifndef QT_LIKE_SIGNAL_PROPERTY_HPP
#define QT_LIKE_SIGNAL_PROPERTY_HPP

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/PropertyNotifyMode.hpp"
#include "QtLikeSignal/Signal.hpp"

#include <memory>
#include <type_traits>
#include <utility>

namespace QtLikeSignal
{
    //----------------------------------------------------------------
    //! @class Property
    //!
    //! A value that announces when it changes.
    //!
    //! @code
    //!   Property<int> score;
    //!   Object::connect( score.getChanged(), &label, &Label::onScoreChanged );
    //!   score.set( 10 );        // the label is told
    //!   score.set( 10 );        // ...and not told again: an equal write is not a change
    //! @endcode
    //!
    //! **It is not a binding.** Qt 6's QProperty re-evaluates a lambda when the properties that
    //! lambda read change, working the dependency out by watching which ones were read. That is a
    //! large machine -- lazy evaluation, dependency capture, loop detection -- and this is
    //! deliberately not it. A program that wants `c = a + b` connects to `a` and `b` and writes one
    //! line, which is easier to reason about and cannot loop.
    //!
    //! **Nothing is allocated until somebody subscribes.** The signal lives behind a pointer and is
    //! created by the first getChanged(), because a screen has hundreds of properties and watches
    //! few of them. Object does the same with its name and its timer list, and for the same reason.
    //!
    //! **Not thread-safe: a Property belongs to one thread**, like the parent-child tree, and every
    //! get() and set() must come from it. The Signal inside it keeps its own thread-safety, so a
    //! cross-thread connect() still works and still delivers on the receiver's own loop; it is the
    //! value that is confined. Qt's QProperty is not thread-safe either.
    //!
    //! @tparam T The value type. It must be equality-comparable, because set() reports a change
    //!         only when there is one -- see set(). It must also be copy-constructible and
    //!         assignable, which every type a property is worth having is.
    //----------------------------------------------------------------
    template <typename T>
    class Property
    {
    public:
        //! The type this property holds, for a caller writing generic code over properties.
        using value_type = T;

        //! Constructs a property holding a value-initialised T, with no owner.
        //!
        //! No owner means PropertyNotifyMode::Coalesced is unavailable; see setNotifyMode().
        Property() = default;

        //! Constructs a property holding @p aValue, with no owner.
        explicit Property
            (
            T aValue  //!< Initial value. No notification is sent for it: nobody can be connected yet.
            )
            : mValue( std::move( aValue ) )
        {
        }

        //! Constructs a property owned by @p aOwner, holding @p aValue.
        //!
        //! The owner is the context PropertyNotifyMode::Coalesced defers through, and it is not
        //! kept alive by this: a property is normally a member of its owner and outlived by
        //! nothing. **A property must not outlive the owner it was given.**
        explicit Property
            (
            Object* aOwner,     //!< Context for coalescing. May be null, and then Coalesced is
                                //!< refused.
            T aValue = T()      //!< Initial value.
            )
            : mValue( std::move( aValue ) )
            , mOwner( aOwner )
        {
        }

        //! A Property is neither copyable nor movable.
        //!
        //! The Signal inside it is neither, because every Connection handed out refers back to it,
        //! and a property that could be copied would leave two values with one subscriber list.
        Property
            (
            const Property&
            ) = delete;

        Property& operator=
            (
            const Property&
            ) = delete;

        Property
            (
            Property&&
            ) = delete;

        Property& operator=
            (
            Property&&
            ) = delete;

        //! @return the current value. Cheap, and never triggers an evaluation of anything.
        const T& get() const
        {
            return mValue;
        }

        //! Sets the value, and reports the change if there is one.
        //!
        //! **An equal write is not a change** and produces no notification, which is what makes a
        //! property safe to write from a loop that recomputes the same answer. Qt's QProperty
        //! compares in the same place and for the same reason.
        //!
        //! The value is stored before anything is emitted, so a slot that reads this property
        //! during the notification sees the new value rather than the one it is being told about.
        //! Every other reporting seam in this library keeps that order.
        void set
            (
            T aValue  //!< The value to store.
            )
        {
            static_assert( isEqualityComparable(),
                "Property<T> needs T to support ==, because set() reports a change only when there "
                "is one. A type without it needs a property that notifies on every write, which "
                "would be a different class rather than a silent difference in this one." );

            if( mValue == aValue )
            {
                return;
            }

            mValue = std::move( aValue );

            // Nothing to tell. The signal is created by the first getChanged(), so its absence is
            // exactly the statement that nobody has ever asked to be told.
            if( !mChanged )
            {
                return;
            }

            if( mMode == PropertyNotifyMode::Coalesced && mOwner != nullptr )
            {
                // Deduplicated on the signal's own address for the rest of this pass, with the
                // arguments replaced each time -- so a thousand writes become one emission
                // carrying the last of them. That is callLater()'s existing behaviour rather than
                // a mechanism invented here.
                Object::callLater( mOwner, *mChanged, mValue );
                return;
            }

            mChanged->emit( mValue );
        }

        //! Gets the subscribe-only view of the signal emitted when the value changes.
        //!
        //! **This is what creates the signal**, so a property nobody subscribes to never allocates.
        //! Const, and the pointer behind it is mutable, because subscribing does not change the
        //! value -- the same reasoning that makes Signal::view() const.
        SignalView<T>& getChanged() const
        {
            if( !mChanged )
            {
                mChanged.reset( new Signal<T>() );
            }
            return mChanged->view();
        }

        //! Chooses when a change is reported. See PropertyNotifyMode.
        //!
        //! Coalesced needs an owner to defer through. Asking for it without one reports a critical
        //! record and leaves the property Immediate, rather than accepting a mode it cannot honour
        //! and silently notifying immediately anyway.
        void setNotifyMode
            (
            PropertyNotifyMode aMode  //!< The mode to use from now on.
            )
        {
            if( aMode == PropertyNotifyMode::Coalesced && mOwner == nullptr )
            {
                qCCritical( gLogObject )
                    << "Property: coalesced notification needs an owner to defer through, and this"
                    << "property was constructed without one; it stays immediate";
                return;
            }

            mMode = aMode;
        }

        //! @return the mode in force. Immediate unless setNotifyMode() accepted a change.
        PropertyNotifyMode notifyMode() const
        {
            return mMode;
        }

        //! @return the owner given at construction, or null.
        Object* owner() const
        {
            return mOwner;
        }

        //! @return true if anything has ever asked for getChanged().
        //!
        //! A diagnostic, and the observable form of "this property has not allocated". A test
        //! asserts on it; ordinary code has no reason to.
        bool hasSubscribers() const
        {
            return static_cast<bool>( mChanged ) && !mChanged->empty();
        }

    private:
        //! @return true if T can be compared with ==, which set() requires.
        //!
        //! A constexpr function rather than a trait alias so that the static_assert inside set()
        //! reads as a sentence. std::equality_comparable is C++20 and this library is C++17, so
        //! the detection is done by hand.
        static constexpr bool isEqualityComparable()
        {
            return EqualityDetector<T>::value;
        }

        //! Detects `a == b` for U. The primary template is the "no" answer.
        template <typename U, typename = void>
        struct EqualityDetector : std::false_type
        {
        };

        //! The specialisation that matches when `a == b` compiles and converts to bool.
        template <typename U>
        struct EqualityDetector<U, std::void_t<decltype( bool( std::declval<const U&>() ==
            std::declval<const U&>() ) )> > : std::true_type
        {
        };

        //! The value. Written by set() and read by get(), both on the owning thread only.
        T mValue {};

        //! When a change is reported. See PropertyNotifyMode.
        //!
        //! Declared here, between the value and the two pointers, so that it lands in the padding
        //! a small T leaves rather than adding eight bytes of its own.
        PropertyNotifyMode mMode { PropertyNotifyMode::Immediate };

        //! The change signal, created by the first getChanged() and not before.
        //!
        //! Mutable because getChanged() is const: asking to be told about a value does not change
        //! it. Qt makes its own binding data mutable for the same reason.
        mutable std::unique_ptr<Signal<T> > mChanged;

        //! The context Coalesced defers through, or null. Not owned, and must outlive this.
        Object* mOwner { nullptr };
    };
}

#endif // QT_LIKE_SIGNAL_PROPERTY_HPP
