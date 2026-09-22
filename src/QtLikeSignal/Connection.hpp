// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

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

#ifndef QT_LIKE_SIGNAL_CONNECTION_HPP
#define QT_LIKE_SIGNAL_CONNECTION_HPP

#include <atomic>
#include <cstddef>
#include <memory>

namespace QtLikeSignal
{
    class Connection;
    class Object;

    //! An Object's thread-affinity box, which also carries the flag saying whether that Object is
    //! still alive. Declared here because a connection node holds one; defined in ThreadData.hpp.
    class Affinity;

    namespace Private
    {
        class SignalImplBase;

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
        struct ConnectionNode : std::enable_shared_from_this<ConnectionNode>
        {
            //! Constructs a node for a connection whose receiver is @p aOwner.
            //!
            //! @p aOwner is null for a slot connected through Signal::connect() directly, which has
            //! no receiver and therefore no incoming list to keep.
            ConnectionNode
                (
                std::weak_ptr<SignalImplBase> aImpl,  //!< The signal that owns the connection.
                Object* aOwner,                       //!< Receiver keeping it incoming, or null.
                std::shared_ptr<Affinity> aOwnerLife  //!< Receiver's affinity box, which carries its life flag.
                )
                : mImpl( std::move( aImpl ) )
                , mOwner( aOwner )
                , mOwnerLife( std::move( aOwnerLife ) )
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

            //! Links this node into the receiver's incoming list. Called once, by connect().
            //!
            //! Does nothing if the connection ended before we got here, which a concurrent
            //! disconnectAll() can do: the node would otherwise stay linked forever, since the
            //! unlinking already happened. Both sides take the receiver's mIncomingMutex, so one of
            //! the two orders always holds.
            void registerWithReceiver();

            //! Unlinks this node from the receiver's incoming list. O(1).
            //!
            //! Called by the Signal on every route out of its list -- a manual disconnect(),
            //! disconnectAll(), or the Signal being destroyed -- so a disconnect is visible in the
            //! receiver immediately rather than at some later sweep. Without it the list would only
            //! ever grow for an object that outlives connections made to it, and would eventually
            //! hold stale entries.
            //!
            //! Must be called with the Signal's own mutex released: this takes the receiver's
            //! mIncomingMutex, and ~Object() takes the two in the opposite order.
            void pruneReceiver();

            std::atomic<bool> mConnected { true };

            //! The Signal that owns this connection, so ~Object() can end it. Weak: a Connection
            //! may outlive its Signal, and once the Signal is gone every connection died with it.
            const std::weak_ptr<SignalImplBase> mImpl;

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

            //! True while this node is in mOwner's incoming list. Guarded by mOwner's
            //! mIncomingMutex, as are the two sibling pointers below.
            bool mInIncoming { false };

            //! True once the receiver is finished with this node, so registerWithReceiver() must
            //! not put it back. Set by pruneReceiver() and by ~Object(). Guarded the same way.
            bool mIncomingDone { false };

            Object* mOwner;   //!< Receiver keeping this node incoming, or null.

            //! The receiver's affinity box, which carries the flag ~Object() clears. Null when
            //! there is no receiver.
            //!
            //! Strong, where the life token it replaced was weak, and that costs nothing: the slot
            //! this node belongs to already holds the same box, and both are owned by the same
            //! Signal. The box is a few dozen bytes and is designed to outlive its Object.
            std::shared_ptr<Affinity> mOwnerLife;

            //! This node's place in the receiver's list, which is what makes both linking and
            //! unlinking O(1) and costs no allocation of its own.
            //!
            //! Raw, because mIncomingSelf below is what owns the node while it is listed. The two
            //! are set and cleared together, under mIncomingMutex.
            ConnectionNode* mPrevIncoming { nullptr };
            ConnectionNode* mNextIncoming { nullptr };

            //! Keeps this node alive for as long as it is in mOwner's incoming list. Guarded by
            //! mOwner's mIncomingMutex, as are the two pointers above.
            //!
            //! **The receiver's list owns what it points at.** It used to point at nodes it did not
            //! own, on the reasoning that a listed node is also in its Signal's list, which keeps it
            //! alive. That is not true for the whole of a disconnect: disconnectAll() drops the node
            //! from the Signal under the Signal's mutex, then calls pruneReceiver() with that mutex
            //! released, and a pruneReceiver() that finds the receiver already dying returns without
            //! unlinking. Between those two steps the node is listed and owned by nobody, so the
            //! last reference falling at the end of the disconnect freed a node ~Object() was
            //! walking -- one thread disconnecting while the receiver's own thread destroys it.
            //!
            //! A self-reference rather than a shared_ptr in the Object, because the list is
            //! intrusive: there is nowhere else to put it that does not cost an allocation for each
            //! connection. It is not a leak -- every route out of the list clears it, and each of
            //! those routes holds another reference while it does, so nothing is destroyed halfway
            //! through its own unlinking.
            std::shared_ptr<ConnectionNode> mIncomingSelf;
        };

        //! The part of a Signal a Connection can reach without knowing its argument types.
        //!
        //! A Connection may outlive its Signal, so the node holds a weak reference to this rather
        //! than to the Signal. Once the Signal is gone the reference expires and disconnect() has
        //! nothing to do, which is correct: every connection died with the signal.
        class SignalImplBase
        {
        public:
            virtual ~SignalImplBase() = default;

            //! Drops the connection @p aNode describes. O(1) in the number of connections.
            //!
            //! Unlinks the node from the receiver's incoming list in the same step, so a disconnect
            //! is visible on both sides at once.
            //!
            //! @p aNode is raw: the caller must hold a reference to it for the whole call.
            virtual void removeConnection
                (
                ConnectionNode* aNode
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
        explicit Connection
            (
            std::shared_ptr<Private::ConnectionNode> aNode  //!< That connection's node.
            )
            : mNode( std::move( aNode ) )
        {
        }

        //! Ends this connection, so its slot is no longer called. Thread-safe.
        //!
        //! **Does not wait** for an invocation already in progress on another thread; ~Object()
        //! relies on that, which is why a queued connection captures an Affinity box and a weak
        //! life token rather than reaching through the receiver.
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

            // mNode keeps the node alive for the whole call, which is what lets
            // removeConnection() take it raw.
            if( auto impl = mNode->mImpl.lock() )
            {
                impl->removeConnection( mNode.get() );
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
        //! Links this connection into its receiver's incoming list. Called once, by
        //! Object::connect().
        void registerWithReceiver() const
        {
            if( mNode )
            {
                mNode->registerWithReceiver();
            }
        }

        std::shared_ptr<Private::ConnectionNode> mNode;   //!< The connection itself; the identity.

        //! Grants Object the private registration above, which only connect() may perform.
        friend class Object;
    };
}

#endif // QT_LIKE_SIGNAL_CONNECTION_HPP
