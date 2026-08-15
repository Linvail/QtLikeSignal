// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

#ifndef QT_LIKE_SIGNAL_CONNECTION_HPP
#define QT_LIKE_SIGNAL_CONNECTION_HPP

#include <atomic>
#include <memory>

namespace QtLikeSignal
{
    namespace Private
    {
        //! The shared "is this connection still live" flag.
        //!
        //! Separate from the slot itself, and untemplated, so a Connection can answer connected()
        //! and compare equal without knowing the signal's argument types. Its address is also the
        //! connection's identity: two Connection copies refer to the same connection exactly when
        //! they point at the same state.
        struct ConnectionState
        {
            std::atomic<bool> mConnected { true };

            //! The slot this state belongs to, type-erased, so disconnect() can reach it.
            //!
            //! Weak, because the slot owns this state and not the other way round. Type-erased,
            //! because this struct cannot name Signal<Args...>::Slot; the Signal that created it
            //! static_pointer_casts it back, which is well-defined because only that Signal ever
            //! writes it.
            //!
            //! It exists so removing one connection is O(1). Without it the only way to find the
            //! slot was to scan the signal's whole list, which made tearing down N receivers of one
            //! signal O(N^2) -- see PERFORMANCE-20260813.md (P7).
            //!
            //! Written once, by connect(), before the handle reaches any caller. Never written
            //! again, so concurrent readers need no lock.
            std::weak_ptr<void> mSlot;
        };

        //! The part of a Signal a Connection can reach without knowing its argument types.
        //!
        //! A Connection outlives its Signal routinely -- Object::mIncoming holds handles to signals
        //! that have already been destroyed -- so it holds a weak reference to this, not to the
        //! Signal. Once the Signal is gone the weak reference expires and disconnect() has nothing
        //! to do, which is correct: every connection died with the signal.
        class SignalImplBase
        {
        public:
            virtual ~SignalImplBase() = default;

            //! Drops the one slot @p aState belongs to.
            //!
            //! Called after a Connection clears its own flag. Removing the slot is what destroys
            //! it, which is what the rest of the library depends on: the slot owns the Cleanup
            //! token that prunes Object::mIncoming, so a disconnect has to be visible there
            //! immediately rather than at some later sweep.
            //!
            //! Takes the state rather than sweeping for dead slots, so the cost is O(1) in the
            //! number of connections rather than O(all of them).
            virtual void removeConnection
                (
                const std::shared_ptr<ConnectionState>& aState
                ) = 0;
        };
    }

    //! A handle to a single signal-slot connection.
    //!
    //! Copyable, and copies are equal: Object::mIncoming stores one copy while the slot's Cleanup
    //! token holds another, and ~Cleanup finds its entry by comparing them.
    //!
    //! Thread-safe. Default-constructed handles are valid and simply report themselves
    //! disconnected, which is what connect() returns when it is given no context to attach to.
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
        //! Does **not** wait for an invocation already in progress on another thread. That is
        //! deliberate and load-bearing: ~Object() disconnects its incoming connections and must not
        //! block on a slot that is mid-call, which is precisely why a queued connection captures an
        //! Affinity box and a weak life token rather than reaching through the receiver. See the
        //! Affinity comment in ThreadData.hpp.
        //!
        //! The slot itself is destroyed once nothing is still calling it -- immediately when idle,
        //! or when the last in-flight call returns.
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

#endif // QT_LIKE_SIGNAL_CONNECTION_HPP
