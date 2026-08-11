#ifndef SIGNAL_H
#define SIGNAL_H

#include "Global.h"

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

        //! True if no slots are connected to this signal. Thread-safe.
        bool empty() const
        {
            return mImpl->connectionCount() == 0;
        }

        //! Gets the number of slots currently connected to this signal. Thread-safe.
        //!
        //! Read-only diagnostic, mirroring Qt's QObject::receivers(). Mainly useful for asserting
        //! that a destroyed receiver really was disconnected rather than left as an inert slot --
        //! see ObjectDefectTest.DestroyedReceiverIsDisconnectedFromItsSender.
        std::size_t receivers() const
        {
            return mImpl->connectionCount();
        }

    private:
        //! One connection: the slot itself, and the flag its handles share.
        struct Slot
        {
            //! Constructs a slot, taking ownership of both members.
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
        };

        //! The connection list, held behind a shared_ptr so a Connection can outlive the Signal.
        //!
        //! Two lists, not one. Writers own a plain vector no reader ever touches; readers get an
        //! immutable snapshot of it, rebuilt only when it has actually changed. So a run of emits
        //! with no connects in it costs no allocation at all, and a run of connects with no emits
        //! in it costs no copying at all -- each pays only when the other has been busy.
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
            using SlotList = std::vector<std::shared_ptr<Slot> >;
            using SlotListPtr = std::shared_ptr<const SlotList>;

            //! Constructs an empty list. Never null, so readers need no null check.
            Impl()
                : mPublished( std::make_shared<const SlotList>() )
            {
            }

            //! Marks every remaining connection dead, so handles that outlive this signal report
            //! themselves disconnected rather than pointing at a list that no longer exists.
            virtual ~Impl() override
            {
                std::lock_guard<std::mutex> lock( mMutex );
                for( const auto& slot : mWorking )
                {
                    slot->mState->mConnected.store( false, std::memory_order_release );
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
                auto slot = std::make_shared<Slot>( std::move( aSlot ), std::move( state ) );
                auto handleState = slot->mState;

                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    mWorking.push_back( std::move( slot ) );
                    mDirty = true;
                }
                return Connection( aSelf, std::move( handleState ) );
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
                const SlotListPtr slots = publishedSlots();

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
                        slot->mState->mConnected.store( false, std::memory_order_release );
                    }
                    dropped.swap( mWorking );
                    discardSnapshot();
                }
                // `dropped` dies here, with the lock released. A slot's destructor runs the Cleanup
                // token, which takes Object::mIncomingMutex, and holding ours across that would
                // nest the two locks in the opposite order to connect().
            }

            //! Number of connections still live.
            std::size_t connectionCount() const
            {
                std::lock_guard<std::mutex> lock( mMutex );
                std::size_t count = 0;
                for( const auto& slot : mWorking )
                {
                    count += slot->mState->mConnected.load( std::memory_order_acquire ) ? 1 : 0;
                }
                return count;
            }

            //! Drops every slot whose handle has disconnected it. See SignalImplBase.
            virtual void removeDisconnected() override
            {
                SlotList dropped;
                {
                    std::lock_guard<std::mutex> lock( mMutex );

                    const auto isDead = []( const std::shared_ptr<Slot>& aSlot )
                        {
                            return !aSlot->mState->mConnected.load( std::memory_order_acquire );
                        };

                    auto firstDead = std::stable_partition( mWorking.begin(), mWorking.end(),
                        [&isDead]( const std::shared_ptr<Slot>& aSlot )
                        {
                            return !isDead( aSlot );
                        } );
                    if( firstDead == mWorking.end() )
                    {
                        return;   // nothing to drop, so nothing to rebuild either
                    }

                    // Moved out rather than erased in place, so they are destroyed after the
                    // unlock -- see disconnectAll().
                    dropped.assign( std::make_move_iterator( firstDead ),
                        std::make_move_iterator( mWorking.end() ) );
                    mWorking.erase( firstDead, mWorking.end() );
                    discardSnapshot();
                }
            }

        private:
            //! Returns the immutable snapshot readers walk, rebuilding it if the working list has
            //! changed since the last one was taken.
            //!
            //! The rebuild is the only copy in the whole design, and it happens once per *change*
            //! rather than once per emit. A steady emit loop rebuilds nothing; a burst of connects
            //! with no emit between them rebuilds nothing either, and pays one copy on the emit
            //! that follows.
            SlotListPtr publishedSlots() const
            {
                std::lock_guard<std::mutex> lock( mMutex );
                if( mDirty )
                {
                    mPublished = std::make_shared<const SlotList>( mWorking );
                    mDirty = false;
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

            //! The writers' list. No reader ever sees it, so it can be mutated freely.
            SlotList mWorking;

            //! The readers' snapshot of mWorking. Null only while mDirty, which publishedSlots()
            //! resolves before handing anything out, so a reader never sees null.
            mutable SlotListPtr mPublished;

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

#endif // SIGNAL_H
