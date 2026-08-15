//! @file
//!
//! Handle to a signal-slot connection, and the node that is the connection.
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
#include <cstddef>
#include <memory>

namespace QtMimic
{
    class Connection;
    class Object;

    namespace Private
    {
        //! Everything about one connection except the callable itself.
        //!
        //! Untemplated so a Connection can answer connected() and compare equal without knowing the
        //! signal's argument types. Two Connection copies refer to the same connection exactly when
        //! they point at the same node.
        //!
        //! The node and the callable are two allocations because they are two lifetimes: the
        //! callable is released the moment the connection ends, while the node lives on for as long
        //! as any handle can still be asked whether the connection is live. Qt splits the same two
        //! the same way, into QObjectPrivate::Connection and QSlotObjectBase.
        struct ConnectionNode
        {
            //! Constructs a node for a connection whose receiver is @p aOwner.
            //!
            //! @p aOwner is null for a slot connected through Signal::connect() directly, which has
            //! no receiver and therefore no incoming list to keep.
            ConnectionNode
                (
                Object* aOwner,          //!< Receiver owning the mIncoming entry, or null.
                std::weak_ptr<int> aLife //!< Receiver's life token; expired means it is gone.
                )
                : mOwner( aOwner )
                , mLife( std::move( aLife ) )
            {
            }

            ConnectionNode
                (
                const ConnectionNode&
                ) = delete;

            ConnectionNode& operator=
                (
                const ConnectionNode&
                ) = delete;

            //! Records @p aHandle in the receiver's incoming list. Called once, by connect().
            //!
            //! Does nothing if the connection ended before the handle got here, which a concurrent
            //! disconnectAll() can do: the entry would otherwise never be pruned, since the pruning
            //! already happened. Both sides take the receiver's mIncomingMutex, so one of the two
            //! orders always holds.
            void registerWithReceiver
                (
                const Connection& aHandle
                );

            //! Removes this connection's handle from the receiver's incoming list.
            //!
            //! Called by the Signal on every route out of its list -- a manual disconnect(),
            //! disconnectAll(), or the Signal being destroyed -- so a disconnect is visible in the
            //! receiver immediately rather than at some later sweep. Without it mIncoming would only
            //! ever grow for an object that outlives connections made to it, and would eventually
            //! hold stale handles.
            //!
            //! Must be called with the Signal's own mutex released: this takes the receiver's
            //! mIncomingMutex, and ~Object() takes the two in the opposite order.
            void pruneReceiver();

            std::atomic<bool> mConnected { true };

            //! Where this slot sits in the owning Signal's working list. Guarded by that Signal's
            //! mutex.
            //!
            //! An index rather than an iterator, so the writers' side stays a vector: removal at a
            //! known index is O(1) when the element is nulled rather than erased, and the snapshot
            //! rebuild copies a contiguous block rather than chasing pointers.
            std::size_t mIndex { 0 };

            //! True while mIndex names a live element. Guarded by the owning Signal's mutex.
            //!
            //! Needed because a connection can be removed by more than one route -- its own handle,
            //! disconnectAll(), or the Signal being destroyed -- and whichever gets there second
            //! must not null an element the first one has already given to somebody else.
            bool mLinked { false };

            //! True once pruneReceiver() has run. Guarded by the receiver's mIncomingMutex.
            bool mPruned { false };

            Object* mOwner;             //!< Receiver owning the mIncoming entry, or null.
            std::weak_ptr<int> mLife;   //!< Receiver's life token; expired means it is gone.
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

            //! Drops the connection @p aNode describes. O(1) in the number of connections.
            //!
            //! Prunes the receiver's incoming list in the same step, so a disconnect is visible on
            //! both sides at once.
            virtual void removeConnection
                (
                const std::shared_ptr<ConnectionNode>& aNode
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
            std::weak_ptr<Private::SignalImplBase> aImpl,       //!< The signal that owns it.
            std::shared_ptr<Private::ConnectionNode> aNode      //!< That connection's node.
            )
            : mImpl( std::move( aImpl ) )
            , mNode( std::move( aNode ) )
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
            if( !mNode )
            {
                return;
            }

            // Released, so a thread that reads the flag as false is guaranteed to see everything
            // this thread did before clearing it.
            mNode->mConnected.store( false, std::memory_order_release );

            if( auto impl = mImpl.lock() )
            {
                impl->removeConnection( mNode );
            }
        }

        //! True while the connection is live. Thread-safe.
        bool connected() const
        {
            return mNode && mNode->mConnected.load( std::memory_order_acquire );
        }

        //! True if both handles refer to the same connection, including two default-constructed
        //! handles, which refer to none.
        bool operator==
            (
            const Connection& aOther
            ) const
        {
            return mNode == aOther.mNode;
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
        //! Records this handle in its receiver's incoming list. Called once, by Object::connect().
        void registerWithReceiver() const
        {
            if( mNode )
            {
                mNode->registerWithReceiver( *this );
            }
        }

        std::weak_ptr<Private::SignalImplBase> mImpl;     //!< Expires when the signal dies.
        std::shared_ptr<Private::ConnectionNode> mNode;   //!< The connection itself; the identity.

        //! Grants Object the private registration above, which only connect() may perform, and the
        //! node the pointer comparison pruneReceiver() finds its own entry by.
        friend class Object;
        friend struct Private::ConnectionNode;
    };
}

#endif // QT_MIMIC_CONNECTION_HPP
