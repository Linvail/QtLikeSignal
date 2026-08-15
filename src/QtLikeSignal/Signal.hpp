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
    //! Emission takes a snapshot of the connection list under the lock, releases the lock, and
    //! then invokes. Three properties the rest of the library depends on fall out of that, and
    //! none of them is optional:
    //!
    //!   - **No lock is held while a slot runs.** A slot may connect, disconnect, emit this same
    //!     signal again, post to another thread or destroy an object, and every one of those
    //!     reaches back into this signal or into Object's own mutexes. Holding the lock across the
    //!     call would deadlock on the first of them.
    //!   - **A slot stays alive for the whole of its invocation**, even if it disconnects itself
    //!     mid-call. The snapshot holds a shared_ptr to each slot, so disconnecting merely drops
    //!     the list's reference; the slot is destroyed when the last call into it returns. The
    //!     library leans on this twice over -- the slot owns the Cleanup token that prunes
    //!     Object::mIncoming, and the captured Affinity the queued path reads on every emit.
    //!   - **A connection made during an emission does not run in that emission**, because it is
    //!     not in the snapshot; and one *disconnected* during an emission does not run either,
    //!     because each entry is re-checked immediately before it is called.
    //!
    //! What emission deliberately does not do is wait. disconnect() returns immediately even if
    //! the slot is mid-call on another thread; see Connection::disconnect().
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

        //! A Signal is neither copyable nor movable.
        //!
        //! It is a member of the classes that own it, and every Connection handed out refers back
        //! to this instance. Copying would give two signals one connection list; moving would
        //! leave the view below pointing at the wrong object.
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
        //! Forwards rather than taking Args... by value. By value cost one copy of every argument
        //! per emit before the slots had even been reached -- invisible for an int, a whole
        //! payload for anything that owns memory. Caught by
        //! ObjectTest.DeepArgumentCopying_QueuedEventsMinimizeCopies, which counts copies.
        //!
        //! The arguments are passed to each slot as lvalues, never forwarded into one: with more
        //! than one receiver, moving into the first would leave the rest with a moved-from value.
        //! Moving into the *last* slot would be sound and is deliberately not done -- it would make
        //! what happens to the caller's object depend on how many receivers happen to be connected,
        //! which is a worse thing to owe a caller than one copy. Qt makes the same choice.
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
        //! Blunt by design, and correspondingly rare: a receiver that wants to stop listening
        //! should disconnect its own handle. This exists for the sender tearing itself down, and
        //! for tests that need a signal emptied without tracking every handle.
        //!
        //! Called from inside a slot, the slots this emission has not yet reached are skipped.
        void disconnectAll()
        {
            mImpl->disconnectAll();
        }

        //! True if no slots are connected to this signal. Thread-safe, and stale on return: a
        //! connect() on another thread may land before you act on the answer. See Global.hpp.
        bool empty() const
        {
            return mImpl->connectionCount() == 0;
        }

        //! Gets the number of slots currently connected to this signal. Thread-safe.
        //!
        //! Read-only diagnostic, mirroring Qt's QObject::receivers(). Mainly useful for asserting
        //! that a destroyed receiver really was disconnected rather than left as an inert slot --
        //! see ObjectDefectTest.DestroyedReceiverIsDisconnectedFromItsSender.
        //!
        //! Stale on return, like every count taken from another thread. See Global.hpp.
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
            //! An index rather than an iterator, so the writers' side can stay a *vector*. Removal
            //! at a known index is O(1) if the element is merely nulled rather than erased, and the
            //! nulls are compacted away in bulk later -- which keeps both of the things that matter
            //! cheap. A std::list would also give O(1) removal, and was tried: it costs 66% more on
            //! a connect/emit churn loop, because every snapshot rebuild then chases pointers
            //! instead of copying a contiguous block.
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
        //! Two lists, not one. Writers own a list no reader ever touches; readers get an immutable
        //! vector snapshot of it, rebuilt only when it has actually changed. So a run of emits with
        //! no connects in it costs no allocation at all, and a run of connects with no emits in it
        //! costs no copying at all -- each pays only when the other has been busy.
        //!
        //! Removal is O(1) and does not disturb the order: the element is nulled where it stands,
        //! and the nulls are compacted away in bulk once they outnumber the live entries. Each slot
        //! knows its own index, so nothing has to be searched for. This is what stops tearing down
        //! N receivers of one signal costing O(N^2) -- see PERFORMANCE-20260813.md (P7).
        //!
        //! The obvious alternative -- one list, copied on write, mutated in place when
        //! shared_ptr::use_count() says nobody is reading -- is **wrong**, and was written and
        //! caught by ThreadSanitizer before this replaced it. use_count() is a relaxed load, so
        //! reading 1 establishes no ordering against the reader that just released its reference;
        //! the writer is then free, in the memory model, to reorder its writes before the reader's
        //! last read of the vector. It happens to work most of the time, which is the worst
        //! property a concurrency bug can have.
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
            //! O(1). The slot is reached through the back-pointer in its own state and erased at
            //! the iterator it carries, so no part of this depends on how many other connections
            //! exist. It used to scan the whole list looking for anything marked dead, which made
            //! destroying N receivers of one signal O(N^2) -- 671 ms for 16 000 of them, against
            //! boost::signals2's 2.7 ms. See PERFORMANCE-20260813.md (P7).
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
            //! list, so the passes are geometrically rare. Callers already hold mMutex.
            //!
            //! Deferred rather than immediate because compacting reassigns indices, and doing that
            //! on every removal would put back exactly the O(all connections) this design exists to
            //! avoid.
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

            //! Returns the immutable snapshot readers walk, rebuilding it if the working list has
            //! changed since the last one was taken.
            //!
            //! The rebuild is the only copy in the whole design, and it happens once per *change*
            //! rather than once per emit. A steady emit loop rebuilds nothing; a burst of connects
            //! with no emit between them rebuilds nothing either, and pays one copy on the emit
            //! that follows.
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

            //! Drops our reference to the snapshot, because it still names slots that have just
            //! been removed. Callers already hold mMutex.
            //!
            //! Needed for promptness, not correctness of dispatch: a removed slot is skipped by its
            //! own flag either way. But the snapshot holding the last reference would keep the slot
            //! -- and therefore the Cleanup token that prunes Object::mIncoming, and whatever the
            //! closure captured -- alive until the next emit rebuilt it. A disconnect has to be
            //! visible immediately, so the reference goes now.
            //!
            //! An emit already in flight is unaffected: it holds its own reference to the old
            //! snapshot and keeps walking it.
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
