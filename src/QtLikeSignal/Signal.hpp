// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Thread-safe signal, and the subscription-only view a class exposes in its place.
//!
//! Usage: declare a Signal<Args...> as a member, return view() from an accessor so callers may
//! subscribe without being able to emit, and call emit() to fire. Connect through Object::connect()
//! rather than Signal::connect() -- that is what adds thread affinity, lifetime tracking, and the
//! queued delivery a cross-thread connection needs.
//!
//! Limitations: a Signal is neither copyable nor movable; a SignalView is valid only while the
//! Signal it refers to lives; and emission does not wait for slots already running on other
//! threads.

#ifndef QT_LIKE_SIGNAL_SIGNAL_HPP
#define QT_LIKE_SIGNAL_SIGNAL_HPP

#include "QtLikeSignal/Global.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace QtLikeSignal
{
    template<typename ... Args> class Signal;

    //! Subscription-only view of a Signal.
    //!
    //! Can be handed to Object::connect(), but cannot emit the signal or disconnect its other
    //! subscribers. Expose one of these so callers may subscribe to Timer::timeout or
    //! Thread::finished without being able to announce something that never happened; Qt gets the
    //! same protection from moc.
    // Without moc, the view is how a plain C++ signal member says the same thing.
    //!
    //! **Limitation:** holds a reference, not a copy, so it is valid only while the owning Signal
    //! lives.
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
        //! up the thread affinity and lifetime tracking that a bare slot has no idea about.
        template <typename Callable>
        Connection connect
            (
            Callable&& aSlot  //!< The callable slot function.
            );

        //! Subscribes a slot that belongs to a receiver Object. Thread-safe.
        template <typename Callable>
        Connection connect
            (
            Callable&& aSlot,          //!< The callable slot function.
            Object* aOwner,            //!< Receiver whose incoming list to keep.
            std::weak_ptr<int> aLife   //!< That receiver's life token.
            );

        Signal<Args...>& mSignal;

        friend class Object;
        friend class Signal<Args...>;
    };

    //! Thread-safe signal. Args are the argument types passed when emitting.
    //!
    //! Usage: declare one as a member, hand out view() for subscribers, and call emit() to fire.
    //! Object::connect() is what adds thread affinity and lifetime tracking on top.
    //!
    //! Emission snapshots the connection list under the lock, releases the lock, then invokes.
    //! Four guarantees follow, and callers may rely on all of them:
    //!
    //!   - **No lock is held while a slot runs**, so a slot may connect, disconnect, emit this
    //!     signal again, post to another thread, or destroy an object.
    //!   - **A slot stays alive for the whole of its invocation**, even if it disconnects itself
    //!     mid-call.
    //!   - **A connection made during an emission does not run in that emission**, and one
    //!     disconnected during an emission does not run either.
    //!   - **Slots run in connection order.**
    //!
    //! **Limitation:** emission does not wait. disconnect() returns immediately even if the slot is
    //! mid-call on another thread.
    template<typename ... Args>
    class Signal
    {
    public:
        //! Constructs a new Signal.
        Signal()
            : mImpl( std::make_shared<Impl>() )
            , mView( *this )
        {
        }

        //! A Signal is neither copyable nor movable: every Connection handed out refers back to
        //! this instance, and view() holds a reference to it.
        Signal
            (
            const Signal&
            ) = delete;

        Signal& operator=
            (
            const Signal&
            ) = delete;

        Signal
            (
            Signal&&
            ) = delete;

        Signal& operator=
            (
            Signal&&
            ) = delete;

        //! Gets a subscription-only view of this signal. Thread-safe.
        //!
        //! Const because obtaining a view only permits subscribing, like Qt's connect() on a const
        //! sender.
        SignalView<Args...>& view() const
        {
            return mView;
        }

        //! Connects a callable slot to this signal. Thread-safe.
        //!
        //! @p aSlot is taken as its own type rather than as a std::function<void(Args...)>, so it
        //! is stored inside the connection's own allocation instead of behind a second one. It must
        //! be callable with Args.
        template <typename Callable>
        Connection connect
            (
            Callable&& aSlot  //!< The callable slot function.
            )
        {
            return mImpl->connect( std::forward<Callable>( aSlot ), mImpl, nullptr,
                std::weak_ptr<int>() );
        }

        //! Disconnects a connection by handle. Thread-safe.
        void disconnect
            (
            const Connection& aConnection  //!< The connection handle to disconnect.
            )
        {
            aConnection.disconnect();
        }

        //! Emits the signal with the specified arguments. Thread-safe.
        //!
        //! Arguments are forwarded here and passed to each slot as lvalues, never moved into one:
        //! with several receivers, moving into the first would leave the rest with a moved-from
        //! value. A direct or same-thread slot therefore copies nothing; a queued one copies once.
        //! Qt makes the same choice.
        // Taking Args... by value cost one copy of every argument per emit before the slots had even
        // been reached. Caught by ObjectTest.DeepArgumentCopying_QueuedEventsMinimizeCopies.
        //
        // Moving into the *last* slot would be sound and is deliberately not done: it would make what
        // happens to the caller's object depend on how many receivers happen to be connected.
        template <typename ... EmitArgs>
        void emit
            (
            EmitArgs&&... aArgs  //!< Arguments to pass to all connected slots.
            )
        {
            mImpl->emit( aArgs ... );
        }

        //! Function call operator to emit the signal. Thread-safe.
        template <typename ... EmitArgs>
        void operator()
            (
            EmitArgs&&... aArgs  //!< Arguments to pass to all connected slots.
            )
        {
            emit( std::forward<EmitArgs>( aArgs )... );
        }

        //! Disconnects every slot from this signal. Thread-safe.
        //!
        //! Blunt by design: a receiver that wants to stop listening should disconnect its own
        //! handle. Called from inside a slot, the slots this emission has not yet reached are
        //! skipped.
        void disconnectAll()
        {
            mImpl->disconnectAll();
        }

        //! True if no slots are connected. Thread-safe, and stale on return.
        bool empty() const
        {
            return mImpl->connectionCount() == 0;
        }

        //! Gets the number of slots currently connected. Thread-safe, and stale on return.
        //!
        //! A diagnostic, mirroring Qt's QObject::receivers(). O(connections).
        std::size_t receivers() const
        {
            return mImpl->connectionCount();
        }

    private:
        //! Subscribes a slot that belongs to a receiver Object. Thread-safe.
        //!
        //! Private, with Object a friend: the receiver and its life token are bookkeeping
        //! Object::connect() adds, and the connection node carries them so that ending the
        //! connection prunes the receiver's incoming list in the same step.
        template <typename Callable>
        Connection connect
            (
            Callable&& aSlot,          //!< The callable slot function.
            Object* aOwner,            //!< Receiver whose incoming list to keep.
            std::weak_ptr<int> aLife   //!< That receiver's life token.
            )
        {
            return mImpl->connect( std::forward<Callable>( aSlot ), mImpl, aOwner,
                std::move( aLife ) );
        }

        //! The callable half of a connection, with its concrete type erased behind one virtual
        //! call.
        //!
        //! Separate from the node a Connection holds, and deliberately so: the callable must be
        //! released as soon as the connection ends, while the node has to outlive it for as long as
        //! any handle can still be asked whether the connection is live. Qt splits the same two
        //! lifetimes the same way, into QObjectPrivate::Connection and QSlotObjectBase.
        struct SlotBase
        {
            //! Constructs a slot for the connection @p aNode describes.
            explicit SlotBase
                (
                std::shared_ptr<Private::ConnectionNode> aNode  //!< Its connection node.
                )
                : mNode( std::move( aNode ) )
            {
            }

            virtual ~SlotBase() = default;

            SlotBase
                (
                const SlotBase&
                ) = delete;

            SlotBase& operator=
                (
                const SlotBase&
                ) = delete;

            //! Calls the slot with the emitted arguments.
            //!
            //! The parameters are the signal's own Args, exactly as a std::function<void(Args...)>
            //! declared them, so a value emitted as something merely convertible still converts
            //! here and a slot that asks for a mutable reference still gets one.
            // Args&... instead would save the conversion, and cannot be used: it rejects
            // sig.emit("literal") on a Signal<std::string>, which the type-erased call accepted.
            virtual void invoke
            (
                Args... aArgs
            ) = 0;

            //! The connection this slot belongs to. Strong, and the only strong reference from a
            //! slot to a node, so the node outlives the slot without either owning the other twice.
            const std::shared_ptr<Private::ConnectionNode> mNode;
        };

        //! One slot, holding its callable by value inside the node's own allocation.
        //!
        //! Keeping the callable's concrete type is what removes the block a
        //! std::function<void(Args...)> needed for it: the emit-time wrapper Object::connect()
        //! builds is far past any small-object buffer, so type-erasing it cost a second allocation
        //! per connection and an indirect call per emit. See PERFORMANCE-20260813.md (P10).
        template <typename Callable>
        struct SlotImpl : SlotBase
        {
            //! Constructs a slot, taking ownership of the callable.
            SlotImpl
                (
                Callable aSlot,                                 //!< The callable.
                std::shared_ptr<Private::ConnectionNode> aNode   //!< Its connection node.
                )
                : SlotBase( std::move( aNode ) )
                , mSlot( std::move( aSlot ) )
            {
            }

            virtual void invoke
                (
                Args... aArgs
                ) override
            {
                // Forwarded, not passed on as lvalues: these parameters are this slot's own copies,
                // made by the call above, so moving out of them costs the emitter nothing and no
                // other slot can see it. std::function<void(Args...)> forwarded them for the same
                // reason, and SignalTest.EmitCopiesOncePerByValueSlotAndNoMore counts the
                // difference.
                mSlot( std::forward<Args>( aArgs )... );
            }

            Callable mSlot;
        };

        //! The writers' list. Holds null entries where connections have been removed; see mLinked.
        using SlotList = std::vector<std::shared_ptr<SlotBase> >;

        //! The connection list, held behind a shared_ptr so a Connection can outlive the Signal.
        //!
        //! Two lists, not one. Writers own a list no reader ever touches; readers walk an immutable
        //! snapshot of it, rebuilt only when it has changed. A run of emits with no connects costs
        //! no allocation, and a run of connects with no emits costs no copying.
        //!
        //! Removal is O(1) and preserves order: the element is nulled where it stands, and the nulls
        //! are compacted in bulk once they outnumber the live entries. Each slot knows its own
        //! index, so nothing is searched for.
        // This is what stops tearing down N receivers of one signal costing O(N^2) -- see
        // PERFORMANCE-20260813.md (P7).
        //
        // The obvious alternative -- one list, copied on write, mutated in place when
        // shared_ptr::use_count() says nobody is reading -- is wrong, and was written and caught by
        // ThreadSanitizer before this replaced it. use_count() is a relaxed load, so reading 1
        // establishes no ordering against the reader that just released its reference; the writer is
        // then free, in the memory model, to reorder its writes before the reader's last read of the
        // vector. It happens to work most of the time, which is the worst property a concurrency bug
        // can have.
        class Impl : public Private::SignalImplBase
        {
        public:
            //! What readers walk: a snapshot of mWorking, contiguous and immutable.
            using PublishedList = std::vector<std::shared_ptr<SlotBase> >;
            using PublishedListPtr = std::shared_ptr<const PublishedList>;

            //! Constructs an empty list. Never null, so readers need no null check.
            Impl()
                : mPublished( std::make_shared<const PublishedList>() )
            {
            }

            //! Marks every remaining connection dead, so handles that outlive this signal report
            //! themselves disconnected rather than pointing at a list that no longer exists.
            //!
            //! disconnectAll() is exactly that, plus emptying a list about to be emptied anyway.
            virtual ~Impl() override
            {
                disconnectAll();
            }

            //! Adds a slot and returns a handle to it.
            template <typename Callable>
            Connection connect
                (
                Callable&& aSlot,
                const std::shared_ptr<Impl>& aSelf,
                Object* aOwner,
                std::weak_ptr<int> aLife
                )
            {
                auto node = std::make_shared<Private::ConnectionNode>( aSelf, aOwner,
                    std::move( aLife ) );
                auto slot = std::make_shared<SlotImpl<std::decay_t<Callable> > >(
                    std::forward<Callable>( aSlot ), node );

                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    node->mIndex  = mWorking.size();
                    node->mLinked = true;
                    mWorking.push_back( std::move( slot ) );
                    mDirty = true;
                }
                return Connection( std::move( node ) );
            }

            //! Calls every connected slot, with no lock held. See the class comment.
            template <typename ... EmitArgs>
            void emit
                (
                EmitArgs&... aArgs
                )
            {
                // The snapshot is immutable and kept alive by this pointer for the whole loop,
                // which is what lets the lock go before any slot runs, and what keeps a slot alive
                // through its own call even if it disconnects itself.
                const PublishedListPtr slots = publishedSlots();

                for( const auto& slot : *slots )
                {
                    // Re-checked here rather than only when the snapshot was taken: a slot earlier
                    // in this same loop may have disconnected this one, and it must not be called
                    // afterwards. Holding the snapshot is what keeps it alive to be asked.
                    if( slot->mNode->mConnected.load( std::memory_order_acquire ) )
                    {
                        slot->invoke( aArgs ... );
                    }
                }
            }

            //! Marks every connection dead and drops them all.
            void disconnectAll()
            {
                SlotList dropped;
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    for( const auto& slot : mWorking )
                    {
                        if( !slot )
                        {
                            continue;
                        }
                        slot->mNode->mConnected.store( false, std::memory_order_release );

                        // Unlinked before the list is emptied, so a handle disconnected after this
                        // point finds nothing to remove instead of nulling somebody else's element.
                        slot->mNode->mLinked = false;
                    }
                    dropped.swap( mWorking );
                    mTombstones = 0;
                    discardSnapshot();
                }

                // Pruned with the lock released, and `dropped` dies here for the same reason:
                // pruneReceiver() takes Object::mIncomingMutex, and holding ours across that would
                // nest the two locks in the opposite order to ~Object().
                for( const auto& slot : dropped )
                {
                    if( slot )
                    {
                        slot->mNode->pruneReceiver();
                    }
                }
            }

            //! Number of connections still live. A diagnostic, so it counts rather than caches.
            std::size_t connectionCount() const
            {
                std::lock_guard<std::mutex> lock( mMutex );
                std::size_t count = 0;
                for( const auto& slot : mWorking )
                {
                    if( slot && slot->mNode->mConnected.load( std::memory_order_acquire ) )
                    {
                        ++count;
                    }
                }
                return count;
            }

            //! Drops the connection @p aNode describes. See SignalImplBase.
            //!
            //! O(1): the node carries its own index, so the slot is nulled where it stands and the
            //! cost does not depend on how many other connections exist.
            // It used to scan the whole list looking for anything marked dead, which made destroying N
            // receivers of one signal O(N^2) -- 671 ms for 16 000 of them. See PERFORMANCE-20260813.md
            // (P7). The scan became a weak back-pointer from the state to the slot, and then nothing
            // at all once the index moved into the node the handle already holds (P10).
            virtual void removeConnection
                (
                Private::ConnectionNode* aNode
                ) override
            {
                if( !aNode )
                {
                    return;
                }

                // Declared before the lock, so that when it turns out to hold the last reference
                // the slot is destroyed *after* the unlock below. The callable is whatever the
                // caller handed to connect(), and its destructor must not run under our mutex.
                std::shared_ptr<SlotBase> dropped;
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    if( !aNode->mLinked )
                    {
                        return;   // already removed, by another handle or by disconnectAll()
                    }

                    // Taken where it stands rather than erased, so every other slot's index stays
                    // valid and this costs nothing regardless of how many there are.
                    dropped = std::move( mWorking[aNode->mIndex] );
                    aNode->mLinked = false;
                    ++mTombstones;
                    discardSnapshot();
                    compactIfMostlyDead();
                }
                aNode->pruneReceiver();
            }

        private:
            //! Squeezes the nulls out of mWorking once they outnumber the live entries.
            //!
            //! Amortised O(1) per removal: each compaction costs one pass but at least halves the
            //! list. Deferred rather than immediate because compacting reassigns indices, which
            //! would otherwise cost O(connections) on every removal. Callers already hold mMutex.
            // The passes are geometrically rare.
            void compactIfMostlyDead()
            {
                if( mTombstones * 2 <= mWorking.size() )
                {
                    return;
                }

                SlotList live;
                live.reserve( mWorking.size() - mTombstones );
                for( auto& slot : mWorking )
                {
                    if( slot )
                    {
                        slot->mNode->mIndex = live.size();
                        live.push_back( std::move( slot ) );
                    }
                }
                mWorking.swap( live );
                mTombstones = 0;
            }

            //! Returns the immutable snapshot readers walk, rebuilding it only if the working list
            //! has changed. The rebuild happens once per change, not once per emit.
            // The rebuild is the only copy in the whole design. A steady emit loop rebuilds nothing; a
            // burst of connects with no emit between them rebuilds nothing either, and pays one copy on
            // the emit that follows.
            PublishedListPtr publishedSlots() const
            {
                std::lock_guard<std::mutex> lock( mMutex );
                if( mDirty )
                {
                    // Built by hand rather than copy-constructed, because mWorking may hold nulls
                    // and readers should never have to test for them.
                    auto rebuilt = std::make_shared<PublishedList>();
                    rebuilt->reserve( mWorking.size() - mTombstones );
                    for( const auto& slot : mWorking )
                    {
                        if( slot )
                        {
                            rebuilt->push_back( slot );
                        }
                    }
                    mPublished = std::move( rebuilt );
                    mDirty     = false;
                }
                return mPublished;
            }

            //! Drops our reference to the snapshot, which still names slots just removed. Callers
            //! already hold mMutex.
            //!
            //! For promptness, not dispatch correctness: a removed slot is skipped by its own flag
            //! either way, but the snapshot would otherwise keep it -- and the Cleanup token that
            //! unlinks it from the receiver -- alive until the next emit. An emit already in flight is
            //! unaffected; it holds its own reference to the old snapshot.
            void discardSnapshot() const
            {
                mPublished.reset();
                mDirty = true;
            }

            //! Guards everything below. Held only to read or rebuild the snapshot, never across a
            //! slot call -- see the class comment for why that distinction is the whole design.
            mutable std::mutex mMutex;

            //! The writers' list. No reader ever sees it, so it can be mutated freely. Holds a null
            //! wherever a connection has been removed and not yet compacted away.
            SlotList mWorking;

            //! How many of mWorking's entries are null. Guarded by mMutex.
            std::size_t mTombstones { 0 };

            //! The readers' snapshot of mWorking. Null only while mDirty, which publishedSlots()
            //! resolves before handing anything out, so a reader never sees null.
            mutable PublishedListPtr mPublished;

            //! True when mPublished no longer reflects mWorking, and so must be rebuilt.
            mutable bool mDirty { false };
        };

        std::shared_ptr<Impl> mImpl;

        //! The view handed out by view(). Mutable because handing one out does not modify the
        //! signal, but the view it returns has to be usable for subscribing.
        mutable SignalView<Args...> mView;

        //! Grants Object the receiver-aware connect() above, and the view the right to forward it.
        friend class Object;
        friend class SignalView<Args...>;
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
    template<typename Callable>
    Connection SignalView<Args...>::connect
        (
        Callable&& aSlot  //!< The callable slot function.
        )
    {
        return mSignal.connect( std::forward<Callable>( aSlot ) );
    }

    //! Subscribes a slot that belongs to a receiver Object. Defined out of line because it needs
    //! Signal to be complete. Thread-safe.
    template<typename ... Args>
    template<typename Callable>
    Connection SignalView<Args...>::connect
        (
        Callable&& aSlot,          //!< The callable slot function.
        Object* aOwner,            //!< Receiver whose incoming list to keep.
        std::weak_ptr<int> aLife   //!< That receiver's life token.
        )
    {
        return mSignal.connect( std::forward<Callable>( aSlot ), aOwner, std::move( aLife ) );
    }
}

#endif // QT_LIKE_SIGNAL_SIGNAL_HPP
