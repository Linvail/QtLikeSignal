//! @file
//!
//! Thin wrapper over boost::signals2 providing Qt-like signal/slot semantics.
//!
//! The Signal<Args...> class wraps boost::signals2::signal to present a
//! familiar API (emit() to fire, connect() to subscribe, connectReflective()
//! to receive connection metadata inside the slot). This keeps call sites
//! clean without spelling out boost details. Arguments are copied exactly
//! like Qt's queued connections when signals cross thread boundaries.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_SIGNAL_HPP
#define QT_MIMIC_SIGNAL_HPP

#include "Global.hpp"

#include <boost/signals2/connection.hpp>
#include <boost/signals2/signal.hpp>
#include <functional>

namespace QtMimic
{

    class Object;

    template <typename ... Args> class Signal;

    //! Handle to a single signal/slot connection.
    using Connection = boost::signals2::connection;

    //! Subscription-only view of a Signal. A view can be passed to Object::connect(),
    //! but cannot emit the signal or modify its connections directly.
    template <typename ... Args> class SignalView
    {
    public:
        SignalView
            (
            const SignalView&
            ) = default;

    private:
        explicit SignalView
            (
            Signal<Args...>& aSignal
            )
            : mSignal( aSignal )
        {
        }

        template <typename Func> Connection connectReflective
            (
            Func&& aSlot
            );

        Signal<Args...>& mSignal;

        friend class Object;
        friend class Signal<Args...>;
    };

    //! Thin wrapper over boost::signals2::signal that keeps the KDBindings-style
    //! API (emit() to fire, connectReflective() to receive the connection inside
    //! the slot) so call sites don't have to spell out boost.
    template <typename ... Args> class Signal
    {
    public:
        using Slot = std::function<void ( Args... )>;

        Signal()
            : mView( *this )
        {
        }

        //! Return a subscription-only view that cannot emit this signal.
        //! Const: obtaining a view only lets callers subscribe (like Qt's
        //! connect() on a const sender), so it does not modify the signal's value.
        SignalView<Args...>& view() const
        {
            return mView;
        }

        //! Connect a callable invoked with the signal's arguments.
        template <typename Func> Connection connect
            (
            Func&& aSlot
            )
        {
            return mSignal.connect( std::forward<Func>( aSlot ) );
        }

        //! Connect a callable that also receives its own Connection as the first
        //! argument (so it can disconnect/inspect itself), like KDBindings'
        //! connectReflective.
        template <typename Func> Connection connectReflective
            (
            Func&& aSlot
            )
        {
            return mSignal.connect_extended( std::forward<Func>( aSlot ) );
        }

        //! Fire the signal, invoking all connected slots.
        template <typename ... EmitArgs> void emit
            (
            EmitArgs&&... args
            ) const
        {
            mSignal( std::forward<EmitArgs>( args )... );
        }

        //! Convenience alias so signal(...) works.
        template <typename ... EmitArgs> void operator()
            (
            EmitArgs&&... args
            ) const
        {
            emit( std::forward<EmitArgs>( args )... );
        }

        //! Disconnect all slots from this signal.
        void disconnectAll()
        {
            mSignal.disconnect_all_slots();
        }

        //! Returns the number of slots connected to the signal.
        size_t receivers() const
        {
            return mSignal.num_slots();
        }

        //! Returns true if no slots are connected to the signal.
        bool empty() const
        {
            return mSignal.empty();
        }

    private:
        boost::signals2::signal<void( Args... )> mSignal;
        mutable SignalView<Args...> mView;
    };

    //! Specialization of IsSignal matching any Signal<Args...>, so IsSignal<T>::value is
    //! true precisely when T is a signal.
    template<typename ... Args>
    struct IsSignal<Signal<Args...> > : std::true_type
    {
    };

    //! A view is a signal for the purposes of the trait: both are things Object::connect() may be
    //! given as its source, and callLater() must reject both as a target for the same reason.
    template<typename ... Args>
    struct IsSignal<SignalView<Args...> > : std::true_type
    {
    };

    template <typename ... Args>
    template <typename Func> Connection SignalView<Args...>::connectReflective
        (
        Func&& aSlot
        )
    {
        return mSignal.connectReflective( std::forward<Func>( aSlot ) );
    }

} // namespace QtMimic

#endif // QT_MIMIC_SIGNAL_HPP
