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
#include <functional>
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
        Connection connect
            (
            std::function<void( Args... )> aSlot  //!< The callable slot function.
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
        Connection connect
            (
            std::function<void( Args... )> aSlot  //!< The callable slot function.
            )
        {
            return mImpl->connect( std::move( aSlot ), mImpl );
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
        struct Slot;

        //! The writers' list. Holds null entries where connections have been removed; see mLinked.
        using SlotList = std::vector<std::shared_ptr<Slot> >;

        //! One connection: the slot itself, the flag its handles share, and its place in the list.
        struct Slot
        {
            //! Constructs a slot, taking ownership of both members. Unlinked until connect() has
            //! put it in the list and recorded where.
            Slot
                (
                std::function<void( Args... )> aSlot,               //!< The callable.
                std::shared_ptr<Private::ConnectionState> aState    //!< Its shared live flag.
                )
                : mSlot( std::move( aSlot ) )
                , mState( std::move( aState ) )
            {
            }

            std::function<void( Args... )> mSlot;
            std::shared_ptr<Private::ConnectionState> mState;

            //! Where this slot sits in the owning Impl's mWorking. Guarded by that Impl's mMutex.
            //!
            //! An index rather than an iterator, so the writers' side stays a vector: removal at a
            //! known index is O(1) when the element is nulled rather than erased, and the snapshot
            //! rebuild copies a contiguous block rather than chasing pointers.
            // A std::list would also give O(1) removal, and was tried: it costs 66% more on a
            // connect/emit churn loop, because every snapshot rebuild then chases pointers.
            std::size_t mIndex { 0 };

            //! True while mIndex names a live element. Guarded by the owning Impl's mMutex.
            //!
            //! Needed because a slot can be removed by more than one route -- its own handle,
            //! disconnectAll(), or the Signal being destroyed -- and whichever gets there second
            //! must not null an element the first one has already given to somebody else.
            bool mLinked { false };
        };

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
            using PublishedList = std::vector<std::shared_ptr<Slot> >;
            using PublishedListPtr = std::shared_ptr<const PublishedList>;

            //! Constructs an empty list. Never null, so readers need no null check.
            Impl()
                : mPublished( std::make_shared<const PublishedList>() )
            {
            }

            //! Marks every remaining connection dead, so handles that outlive this signal report
            //! themselves disconnected rather than pointing at a list that no longer exists.
            virtual ~Impl() override
            {
                std::lock_guard<std::mutex> lock( mMutex );
                for( const auto& slot : mWorking )
                {
                    if( !slot )
                    {
                        continue;
                    }
                    slot->mState->mConnected.store( false, std::memory_order_release );

                    // Unlinked as well as marked dead: a handle disconnected after this point still
                    // reaches removeConnection(), which must not touch a list about to be destroyed
                    // with us.
                    slot->mLinked = false;
                }
            }

            //! Adds a slot and returns a handle to it.
            Connection connect
                (
                std::function<void( Args... )> aSlot,
                const std::shared_ptr<Impl>& aSelf
                )
            {
                auto state = std::make_shared<Private::ConnectionState>();
                auto slot = std::make_shared<Slot>( std::move( aSlot ), state );

                // The back-pointer disconnect() follows to find this slot in O(1). Set before the
                // handle below can reach any caller, and never written again.
                state->mSlot = slot;

                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    slot->mIndex  = mWorking.size();
                    slot->mLinked = true;
                    mWorking.push_back( std::move( slot ) );
                    mDirty = true;
                }
                return Connection( aSelf, std::move( state ) );
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
                    if( slot->mState->mConnected.load( std::memory_order_acquire ) )
                    {
                        slot->mSlot( aArgs ... );
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
                        slot->mState->mConnected.store( false, std::memory_order_release );

                        // Unlinked before the list is emptied, so a handle disconnected after this
                        // point finds nothing to remove instead of nulling somebody else's element.
                        slot->mLinked = false;
                    }
                    dropped.swap( mWorking );
                    mTombstones = 0;
                    discardSnapshot();
                }
                // `dropped` dies here, with the lock released. A slot's destructor runs the Cleanup
                // token, which takes Object::mIncomingMutex, and holding ours across that would
                // nest the two locks in the opposite order to connect().
            }

            //! Number of connections still live. A diagnostic, so it counts rather than caches.
            std::size_t connectionCount() const
            {
                std::lock_guard<std::mutex> lock( mMutex );
                std::size_t count = 0;
                for( const auto& slot : mWorking )
                {
                    if( slot && slot->mState->mConnected.load( std::memory_order_acquire ) )
                    {
                        ++count;
                    }
                }
                return count;
            }

            //! Drops the one slot @p aState belongs to. See SignalImplBase.
            //!
            //! O(1): the slot is reached through the back-pointer in its own state and nulled at the
            //! index it carries, so the cost does not depend on how many other connections exist.
            // It used to scan the whole list looking for anything marked dead, which made destroying N
            // receivers of one signal O(N^2) -- 671 ms for 16 000 of them. See PERFORMANCE-20260813.md
            // (P7).
            virtual void removeConnection
                (
                const std::shared_ptr<Private::ConnectionState>& aState
                ) override
            {
                if( !aState )
                {
                    return;
                }

                // Declared before the lock, so that when it turns out to hold the last reference
                // the slot is destroyed *after* the unlock below. A slot's destructor runs the
                // Cleanup token, which takes Object::mIncomingMutex, and holding ours across that
                // would nest the two locks in the opposite order to connect().
                const std::shared_ptr<Slot> slot
                    = std::static_pointer_cast<Slot>( aState->mSlot.lock() );
                if( !slot )
                {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    if( !slot->mLinked )
                    {
                        return;   // already removed, by another handle or by disconnectAll()
                    }

                    // Nulled where it stands rather than erased, so every other slot's index stays
                    // valid and this costs nothing regardless of how many there are. The reference
                    // dropped here is the list's; ours above is what keeps the slot alive until the
                    // unlock.
                    mWorking[slot->mIndex].reset();
                    slot->mLinked = false;
                    ++mTombstones;
                    discardSnapshot();
                    compactIfMostlyDead();
                }
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
                        slot->mIndex = live.size();
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
            //! prunes Object::mIncoming -- alive until the next emit. An emit already in flight is
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
    Connection SignalView<Args...>::connect
        (
        std::function<void( Args... )> aSlot  //!< The callable slot function.
        )
    {
        return mSignal.connect( std::move( aSlot ) );
    }
}

#endif // QT_LIKE_SIGNAL_SIGNAL_HPP
