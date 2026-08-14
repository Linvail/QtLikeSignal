//! @file
//!
//! Handle to a signal-slot connection, and the shared state behind it.
//!
//! Usage: keep the Connection returned by Object::connect() to end that one connection later, or
//! discard it -- the connection is also ended when either the sender Signal or the receiver Object
//! is destroyed.
//!
//! Limitations: disconnect() does not wait for a slot already running on another thread, and a
//! handle carries no type information, so it cannot tell you what it was connected to.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_CONNECTION_HPP
#define QT_MIMIC_CONNECTION_HPP

#include <atomic>
#include <memory>

namespace QtMimic
{
    namespace Private
    {
        //! Shared "is this connection still live" flag, and the connection's identity.
        //!
        //! Untemplated so a Connection can answer connected() and compare equal without knowing the
        //! signal's argument types. Two Connection copies refer to the same connection exactly when
        //! they point at the same state.
        struct ConnectionState
        {
            std::atomic<bool> mConnected { true };

            //! The slot this state belongs to, type-erased, so disconnect() can find it in O(1).
            //!
            //! Weak, because the slot owns this state and not the other way round. Type-erased
            //! because this struct cannot name Signal<Args...>::Slot; the owning Signal casts it
            //! back, which is well defined because only that Signal ever writes it.
            //!
            //! Written once by connect(), before the handle reaches any caller, so readers need no
            //! lock.
            std::weak_ptr<void> mSlot;
        };

        //! The part of a Signal a Connection can reach without knowing its argument types.
        //!
        //! A Connection may outlive its Signal, so it holds a weak reference to this rather than to
        //! the Signal. Once the Signal is gone the reference expires and disconnect() has nothing to
        //! do, which is correct: every connection died with the signal.
        class SignalImplBase
        {
        public:
            virtual ~SignalImplBase() = default;

            //! Drops the one slot @p aState belongs to. O(1) in the number of connections.
            //!
            //! Removing the slot destroys it, and the slot owns the Cleanup token that prunes
            //! Object::mIncoming -- so a disconnect is visible there immediately rather than at
            //! some later sweep.
            virtual void removeConnection
                (
                const std::shared_ptr<ConnectionState>& aState
                ) = 0;

        };
    }

    //! Handle to a single signal-slot connection.
    //!
    //! Copyable, and copies compare equal. Thread-safe. A default-constructed handle is valid and
    //! reports itself disconnected, which is what connect() returns when given no context.
    class Connection
    {
    public:
        //! Constructs a handle that refers to no connection.
        Connection() = default;

        //! Constructs a handle to a live connection. Called only by Signal.
        Connection
            (
            std::weak_ptr<Private::SignalImplBase> aImpl,        //!< The signal that owns it.
            std::shared_ptr<Private::ConnectionState> aState     //!< That connection's flag.
            )
            : mImpl( std::move( aImpl ) )
            , mState( std::move( aState ) )
        {
        }

        //! Ends this connection, so its slot is no longer called. Thread-safe.
        //!
        //! **Does not wait** for an invocation already in progress on another thread; ~Object()
        //! relies on that, which is why a queued connection captures an Affinity box and a weak life
        //! token rather than reaching through the receiver.
        //!
        //! The slot is destroyed once nothing is still calling it.
        void disconnect() const
        {
            if( !mState )
            {
                return;
            }

            // Released, so a thread that reads the flag as false is guaranteed to see everything
            // this thread did before clearing it.
            mState->mConnected.store( false, std::memory_order_release );

            if( auto impl = mImpl.lock() )
            {
                impl->removeConnection( mState );
            }
        }

        //! True while the connection is live. Thread-safe.
        bool connected() const
        {
            return mState && mState->mConnected.load( std::memory_order_acquire );
        }

        //! True if both handles refer to the same connection, including two default-constructed
        //! handles, which refer to none.
        bool operator==
            (
            const Connection& aOther
            ) const
        {
            return mState == aOther.mState;
        }

        //! True if the handles refer to different connections.
        bool operator!=
            (
            const Connection& aOther
            ) const
        {
            return !( *this == aOther );
        }

    private:
        std::weak_ptr<Private::SignalImplBase> mImpl;      //!< Expires when the signal dies.
        std::shared_ptr<Private::ConnectionState> mState;  //!< Shared with the slot; the identity.
    };
}

#endif // QT_MIMIC_CONNECTION_HPP
