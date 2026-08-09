#ifndef SIGNAL_H
#define SIGNAL_H

#include "Global.h"
#include <boost/signals2.hpp>
#include <functional>

namespace QtLikeSignal
{
    template<typename ... Args> class Signal;

    //! Subscription-only view of a Signal.
    //!
    //! A view can be handed to Object::connect() but cannot emit the signal it refers to, nor
    //! disconnect its other subscribers. That is what makes it the right thing for a class to
    //! expose: emitting Timer::timeout or Thread::finished is the owner's job, and a caller able
    //! to emit them directly would be announcing something that never happened. Qt gets the same
    //! protection from moc, which makes signals callable only by the declaring class; without moc
    //! the view is how a plain C++ signal member says the same thing.
    //!
    //! Holds a reference, not a copy: the view is a window onto the owner's Signal and is only
    //! valid while that owner lives, exactly like the reference an accessor returns.
    template<typename ... Args>
    class SignalView
    {
    public:
        SignalView
            (
            const SignalView&
            ) = default;

    private:
        //! Constructs a view onto @p aSignal. Private: only the Signal itself hands one out.
        explicit SignalView
            (
            Signal<Args...>& aSignal  //!< The signal this view refers to.
            )
            : mSignal( aSignal )
        {
        }

        //! Subscribes a slot to the viewed signal. Thread-safe.
        //!
        //! Private, with Object a friend, so subscribing goes through Object::connect() and picks
        //! up the thread affinity and lifetime tracking that raw boost connections have no idea
        //! about.
        ConnectionHandle connect
            (
            std::function<void( Args... )> aSlot  //!< The callable slot function.
            );

        Signal<Args...>& mSignal;

        friend class Object;
        friend class Signal<Args...>;
    };

    //! Simple thread-safe signal class wrapping boost::signals2::signal. Args are the argument
    //! types passed when emitting the signal.
    template<typename ... Args>
    class Signal
    {
    public:
        //! Constructs a new Signal.
        Signal()
            : mView( *this )
        {
        }

        //! Gets a subscription-only view of this signal, for a class that wants to let callers
        //! connect without letting them emit. Thread-safe.
        //!
        //! Const because obtaining a view only permits subscribing -- like Qt's connect() on a
        //! const sender -- so it does not modify the signal.
        SignalView<Args...>& view() const
        {
            return mView;
        }

        //! Connects a callable slot to this signal. Thread-safe.
        ConnectionHandle connect
            (
            std::function<void( Args... )> aSlot  //!< The callable slot function.
            )
        {
            return mSignal.connect( aSlot );
        }

        //! Disconnects a connection by handle. Thread-safe.
        void disconnect
            (
            const ConnectionHandle& aConnection  //!< The connection handle to disconnect.
            )
        {
            aConnection.disconnect();
        }

        //! Emits the signal with the specified arguments. Thread-safe.
        void emit
            (
            Args... aArgs  //!< Arguments to pass to all connected slots.
            )
        {
            mSignal( aArgs ... );
        }

        //! Function call operator to emit the signal. Thread-safe.
        void operator()
            (
            Args... aArgs  //!< Arguments to pass to all connected slots.
            )
        {
            mSignal( aArgs ... );
        }

        //! Gets the number of slots currently connected to this signal. Thread-safe.
        //!
        //! Read-only diagnostic, mirroring Qt's QObject::receivers(). Mainly useful for asserting
        //! that a destroyed receiver really was disconnected rather than left as an inert slot --
        //! see ObjectDefectTest.DestroyedReceiverIsDisconnectedFromItsSender.
        std::size_t receivers() const
        {
            return mSignal.num_slots();
        }

    private:
        boost::signals2::signal<void( Args... )> mSignal;

        //! The view handed out by view(). Mutable because handing one out does not modify the
        //! signal, but the view it returns has to be usable for subscribing.
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

    //! Subscribes a slot to the viewed signal. Defined out of line because it needs Signal to be
    //! complete. Thread-safe.
    template<typename ... Args>
    ConnectionHandle SignalView<Args...>::connect
        (
        std::function<void( Args... )> aSlot  //!< The callable slot function.
        )
    {
        return mSignal.connect( std::move( aSlot ) );
    }
}

#endif // SIGNAL_H
