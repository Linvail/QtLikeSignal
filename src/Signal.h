#ifndef SIGNAL_H
#define SIGNAL_H

#include "Global.h"

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
            mImpl->emit( aArgs ... );
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
            std::function<void( Args... )> mSlot;
            std::shared_ptr<Private::ConnectionState> mState;
        };

        //! The connection list, held behind a shared_ptr so a Connection can outlive the Signal.
        class Impl : public Private::SignalImplBase
        {
        public:
            //! Marks every remaining connection dead, so handles that outlive this signal report
            //! themselves disconnected rather than pointing at a list that no longer exists.
            virtual ~Impl() override
            {
                std::lock_guard<std::mutex> lock( mMutex );
                for( const auto& slot : mSlots )
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
                auto slot = std::make_shared<Slot>();
                slot->mSlot = std::move( aSlot );
                slot->mState = std::make_shared<Private::ConnectionState>();

                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    mSlots.push_back( slot );
                }
                return Connection( aSelf, slot->mState );
            }

            //! Calls every connected slot, with no lock held. See the class comment.
            template <typename ... EmitArgs>
            void emit
                (
                EmitArgs&... aArgs
                )
            {
                std::vector<std::shared_ptr<Slot> > snapshot;
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    snapshot = mSlots;
                }

                for( const auto& slot : snapshot )
                {
                    // Re-checked here rather than only when the snapshot was taken: a slot earlier
                    // in this same loop may have disconnected this one, and it must not be called
                    // afterwards. The snapshot's shared_ptr is what keeps it alive to be asked.
                    if( slot->mState->mConnected.load( std::memory_order_acquire ) )
                    {
                        slot->mSlot( aArgs ... );
                    }
                }
            }

            //! Marks every connection dead and drops them all.
            void disconnectAll()
            {
                std::vector<std::shared_ptr<Slot> > dropped;
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    for( const auto& slot : mSlots )
                    {
                        slot->mState->mConnected.store( false, std::memory_order_release );
                    }
                    dropped.swap( mSlots );
                }
                // Destroyed with the lock released: a slot's destructor runs the Cleanup token,
                // which takes Object::mIncomingMutex, and holding ours across that would nest the
                // two locks in the opposite order to connect().
            }

            //! Number of connections still live.
            std::size_t connectionCount() const
            {
                std::lock_guard<std::mutex> lock( mMutex );
                std::size_t count = 0;
                for( const auto& slot : mSlots )
                {
                    count += slot->mState->mConnected.load( std::memory_order_acquire ) ? 1 : 0;
                }
                return count;
            }

            //! Drops every slot whose handle has disconnected it. See SignalImplBase.
            virtual void removeDisconnected() override
            {
                std::vector<std::shared_ptr<Slot> > dropped;
                {
                    std::lock_guard<std::mutex> lock( mMutex );
                    for( auto it = mSlots.begin(); it != mSlots.end(); )
                    {
                        if( ( *it )->mState->mConnected.load( std::memory_order_acquire ) )
                        {
                            ++it;
                        }
                        else
                        {
                            dropped.push_back( std::move(*it ) );
                            it = mSlots.erase( it );
                        }
                    }
                }
                // Destroyed unlocked, for the reason given in disconnectAll().
            }

        private:
            mutable std::mutex mMutex;                   //!< Guards mSlots.
            std::vector<std::shared_ptr<Slot> > mSlots;  //!< Every connection, live or just-dead.
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
