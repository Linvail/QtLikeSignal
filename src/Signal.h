#ifndef QT_LIKE_SIGNAL_SIGNAL_H
#define QT_LIKE_SIGNAL_SIGNAL_H

#include "Global.h"
#include <boost/signals2.hpp>
#include <functional>

namespace QtLikeSignal
{
    //! Simple thread-safe signal class wrapping boost::signals2::signal. Args are the argument
    //! types passed when emitting the signal.
    template<typename ... Args>
    class Signal
    {
    public:
        //! Constructs a new Signal.
        Signal() = default;

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
    };

    //! Specialization of IsSignal matching any Signal<Args...>, so IsSignal<T>::value is
    //! true precisely when T is a signal.
    template<typename ... Args>
    struct IsSignal<Signal<Args...> > : std::true_type
    {
    };
}

#endif // QT_LIKE_SIGNAL_SIGNAL_H
