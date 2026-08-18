// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Qt-like object model: signal/slot connections with thread affinity, built on Signal and the
//! C++17 threading library.
//!
//! Two building blocks are provided:
//!   * QtLikeSignal::Thread - an event-loop thread. Every object "lives" in one
//!                        Thread; queued slot invocations are executed in
//!                        that thread's event loop.
//!   * QtLikeSignal::Object - a base class that carries thread affinity and offers
//!                        static connect() helpers that mirror Qt's
//!                        connect(sender-signal, receiver, &Receiver::slot).
//!
//! Like Qt, the delivery is decided at emit time (ConnectionType::Auto):
//!   * If the signal is emitted on the receiver's own thread, the slot runs
//!     synchronously (direct connection).
//!   * If the signal is emitted on a different thread, the slot invocation is
//!     queued into the receiver thread's event loop and executed there
//!     (queued connection). The arguments are copied, exactly like Qt.

#ifndef QT_LIKE_SIGNAL_OBJECT_HPP
#define QT_LIKE_SIGNAL_OBJECT_HPP

#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Global.hpp"
#include "QtLikeSignal/ThreadData.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace QtLikeSignal
{
    class Object;
    class Thread;
    template <typename ... Args> class Signal;
    class AbstractEventDispatcher;
    class EventDispatcherDefault;
    class CoreApplication;

    //! Shorthand for the constraints the connect()/callLater() overload sets are selected on.
    //!
    //! These appear in around thirty enable_if_t chains. Spelled out, each is three or four
    //! std::is_base_of/std::is_same terms, the chain no longer fits on a line, and two overloads
    //! that must agree can drift apart without it being visible. Named once, they cannot.
    template <typename Child> constexpr bool is_obj = std::is_base_of<Object, Child>::value;

    //! Child is an Object and derives from Parent, which is also an Object. Same type qualifies.
    template <typename Child, typename Parent>
    constexpr bool obj_is_base_of = std::is_base_of<Parent, Child>::value && is_obj<Parent>;

    //! Child is an Object and derives from a *different* Object type Parent. This is what
    //! separates "the slot is declared in the receiver itself" from "the slot is inherited".
    template <typename Child, typename Parent>
    constexpr bool obj_is_child_of = std::is_base_of<Parent, Child>::value
        && !std::is_same<Parent, Child>::value && is_obj<Parent>;

    //! True when T is void.
    template <typename T> constexpr bool is_void = std::is_same<void, T>::value;

    //! A token that outlives the Object it came from, and says whether that Object still exists.
    //!
    //! What Object::objectLife() used to hand back as a `std::weak_ptr<int>`. The token is now the
    //! Object's Affinity box, which already outlives it by design, rather than a second heap block
    //! allocated for the purpose -- see Affinity::isObjectAlive(). Wrapped in a type of its own so
    //! that the box stays an implementation detail and callers keep the one operation they ever
    //! used, `expired()`.
    //!
    //! Copyable and default-constructible, like the `weak_ptr` it replaces. A default-constructed
    //! token reports expired, since it names no object.
    //!
    //! Holding one keeps a small box alive; it does nothing whatsoever to stop the Object being
    //! destroyed, which was equally true before. It answers exactly one question -- "had
    //! destruction begun at the instant of the check" -- and does not close the check-then-use race
    //! that follows it.
    class ObjectLife
    {
    public:
        ObjectLife() = default;

        //! @return true once the Object this came from has begun destruction, or if this names no
        //! object at all.
        bool expired() const
        {
            return !mAffinity || !mAffinity->isObjectAlive();
        }

    private:
        //! Wraps @p aAffinity. Only Object may mint a token.
        explicit ObjectLife
            (
            std::shared_ptr<Affinity> aAffinity   //!< The object's affinity box.
            )
            : mAffinity( std::move( aAffinity ) )
        {
        }

        std::shared_ptr<Affinity> mAffinity;   //!< The box carrying the flag; null when empty.

        friend class Object;
    };

    //! Base class for all objects participating in the signal-slot and event system.
    //!
    //! Derive from it and declare signals as public Signal<Args...> members, then connect them to
    //! member-function slots of other Objects with Object::connect(). An Object carries thread
    //! affinity, which is what lets a queued connection know where to deliver.
    class Object
    {
    public:
        //! Constructs an object living in @p aThread.
        //!
        //! Null -- the default -- means the thread that is constructing it, which is what a
        //! parent-less QObject gets. Passing the thread explicitly is equivalent to constructing
        //! and then calling moveToThread(), but is available to an object being built *on* another
        //! thread, where moveToThread() would be refused as a pull.
        explicit Object
            (
            Thread* aThread = nullptr
            );

        virtual ~Object();

        //! Object is neither copyable nor movable.
        //!
        //! These are already deleted implicitly, because the class holds std::mutex members -- but
        //! only by accident. Stating it makes the guarantee survive refactoring: a copy would share
        //! one Affinity box between two Objects, so the first of them to be destroyed would mark
        //! the box dead and silence every connection belonging to the other, while the second would
        //! leave a box already reporting dead. Both halves of that are wrong, and neither announces
        //! itself.
        Object
            (
            const Object&
            ) = delete;

        Object& operator=
            (
            const Object&
            ) = delete;

        Object
            (
            Object&&
            ) = delete;

        Object& operator=
            (
            Object&&
            ) = delete;

        Thread* thread() const;

        bool moveToThread
            (
            Thread* aThread
            );

        //! **Not thread-safe: both must be called from this object's own thread.** Stated here as
        //! well as on the definitions, because the member these two touch is documented as
        //! unguarded further down this file and the two comments have to agree.
        std::string objectName() const;

        void setObjectName
            (
            const std::string& aName
            );

        void deleteLater();

        //! Called when one of this object's timers comes due. Override to react to it; the default
        //! does nothing. Delivered by the event loop of the thread the object lives in, so an
        //! override runs there and needs no locking of its own.
        //!
        //! An object may run several timers, so an override that cares which one fired must check
        //! aEvent->timerId() -- see Timer::timerEvent().
        virtual void timerEvent
            (
            TimerEvent* aEvent
            );

        int startTimer
            (
            int aIntervalMs
            );

        void killTimer
            (
            int aTimerId
            );

        //! Number of live connections where this object is the receiver. Thread-safe.
        //!
        //! A diagnostic, for asserting that a disconnect really pruned the entry rather than
        //! leaving an inert slot behind.
        // See ObjectTest.IncomingPrunedOnDisconnect.
        std::size_t incomingConnectionCount() const
        {
            std::lock_guard<std::mutex> lock( mIncomingMutex );
            return mIncomingCount;
        }

        //! Gets a token tracking the lifetime of this object. Thread-safe.
        //!
        //! The token outlives the Object and reports `expired()` once destruction has begun, which
        //! is the whole of what it is for. See ObjectLife, and Affinity::isObjectAlive() for why
        //! the flag lives where it does.
        //!
        //! Returned a `std::weak_ptr<int>` until 2026-08-18, when the separate life-token
        //! allocation was folded into the affinity box. The operation callers used, `expired()`, is
        //! unchanged; only the type's name is.
        ObjectLife objectLife() const
        {
            return ObjectLife( mAffinity );
        }

        //! Connect Overload 1: Connects a signal to a non-overloaded member function slot.
        //!
        //! This is the primary overload for standard member functions. Because the target
        //! slot is not overloaded, the compiler can directly deduce the `Slot` type without needing
        //! explicit template resolution.
        template <typename Signal, typename Receiver, typename Slot>
        static std::enable_if_t<MemberFunctionTraits<Slot>::is_member_function,
            Connection> connect
            (
            Signal& aSignal,
            Receiver* aReceiver,
            Slot aSlot,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            using SlotClass = typename MemberFunctionTraits<Slot>::class_type;

            static_assert( is_obj<Receiver>, "Receiver must be an instance of Object." );
            static_assert( MemberFunctionTraits<Slot>::is_member_function,
                "Slot must be a member function pointer." );
            static_assert( obj_is_base_of<Receiver, SlotClass>,
                "Slot must be a member function of Receiver or one of its base classes." );

            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 2: Connects an overloaded void member function slot inherited from
        //! a base class.
        //!
        //! If the target slot is overloaded, the compiler cannot deduce `Slot` in
        //! Overload 1. When the overloaded slot is defined in a base class of the receiver, type
        //! deduction fails. This overload explicitly resolves the base class pointer so you can connect
        //! inherited overloaded methods seamlessly.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, Connection> connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::*aSlot )
            (
            SignalArgs...
            ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 3: Connects an overloaded const void member function slot inherited
        //! from a base class.
        //!
        //! Similar to Overload 2, but specifically for const member functions. C++
        //! requires separate template matching for const qualifiers on member function pointers.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, Connection> connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 4: Connects an overloaded non-void returning member function slot
        //! inherited from a base class.
        //!
        //! If an overloaded inherited slot returns a value (e.g. `bool`), it won't match
        //! the void-returning Overloads 2 and 3. This overload explicitly catches non-void slots from
        //! base classes (the return value is safely discarded during emission).
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, Connection>
        connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )
            (
            SignalArgs...
            ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 5: Connects an overloaded non-void returning const member function
        //! slot inherited from a base class.
        //!
        //! Similar to Overload 4, but specifically for const member functions.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, Connection>
        connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 6: Connects an overloaded void member function slot defined
        //! directly on the receiver.
        //!
        //! If the target slot is overloaded (e.g. `onEvent()` and `onEvent(int)`), the
        //! compiler cannot deduce `Slot` in Overload 1. By using `NonDeduced<Receiver>`, this
        //! overload forces the compiler to use `SignalArgs` from the signal to perfectly select the
        //! right overload pointer.
        //! signature.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver>
        static std::enable_if_t<is_obj<Receiver>, Connection> connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 7: Connects an overloaded const void member function slot defined
        //! directly on the receiver.
        //!
        //! Similar to Overload 6, but specifically matches const member functions.
        //! signature.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver>
        static std::enable_if_t<is_obj<Receiver>, Connection> connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 8: Connects an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        //!
        //! If an overloaded slot returns a value (e.g. `bool`), it won't match the
        //! void-returning Overload 6. This overload ensures connecting an overloaded method that returns
        //! `Ret` compiles successfully.
        //! signature.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename Ret>
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, Connection> connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 9: Connects an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        //!
        //! Similar to Overload 8, but specifically for const member functions.
        //! signature.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename Ret>
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, Connection> connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };

            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! Connect Overload 10: Connects a signal to an arbitrary callable (e.g. lambda, functor,
        //! std::function) with a context object for thread affinity and lifetime management
        //! (like Qt's context-object connect).
        //!        lifetime management.
        //! @note The callable must be invocable with the signal's arguments.
        template <template <typename ...> class SignalSource, typename Func, typename ... Args,
            typename = std::enable_if_t<!std::is_member_function_pointer<std::decay_t<Func> >::
            value> >
        static Connection connect
            (
            SignalSource<Args...>& aSignal,
            Object* aContext,
            Func&& aSlot,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            #if __cplusplus >= 201703L
                static_assert( std::is_invocable_v<Func, Args...>,
                "The provided lambda or callable does not match the Signal's arguments." );
            #endif

            return connectImpl( aSignal, aContext, std::forward<Func>( aSlot ), aType );
        }

        //! Disconnects a signal connection using a connection handle. Thread-safe.
        //!
        //! A named spelling of handle.disconnect(), so a call site reads as the counterpart of
        //! Object::connect() rather than reaching into the Signal directly.
        static void disconnect
            (
            const Connection& aHandle
            );

        //! CallLater Overload 1: schedules a non-overloaded member function slot to run deferred.
        template <typename Receiver, typename Slot, typename ... Args>
        static std::enable_if_t<is_obj<Receiver> && MemberFunctionTraits<Slot>::is_member_function,
            void>
        callLater
            (
            Receiver* aReceiver,
            Slot aSlot,
            Args&&... aArgs
            );

        //! CallLater Overload 2: schedules an overloaded void member function slot inherited from
        //! a base class.
        template <typename Receiver, typename SlotClass, typename ... Args>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, void>
        callLater( Receiver* aReceiver,
            void ( SlotClass::*aSlot )( NonDeduced<Args>... ),
            Args&&... aArgs );

        //! CallLater Overload 3: schedules an overloaded const void member function slot inherited
        //! from a base class.
        template <typename Receiver, typename SlotClass, typename ... Args>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, void>
        callLater( Receiver* aReceiver,
            void ( SlotClass::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 4: schedules an overloaded non-void returning member function slot
        //! inherited from a base class.
        template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, void>
        callLater( Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( NonDeduced<Args>... ),
            Args&&... aArgs );

        //! CallLater Overload 5: schedules an overloaded non-void returning const member function
        //! slot inherited from a base class.
        template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, void>
        callLater( Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 6: schedules an overloaded void member function slot defined
        //! directly on the receiver.
        template <typename Receiver, typename ... Args>
        static std::enable_if_t<is_obj<Receiver>, void>
        callLater( Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ),
            Args&&... aArgs );

        //! CallLater Overload 7: schedules an overloaded const void member function slot defined
        //! directly on the receiver.
        template <typename Receiver, typename ... Args>
        static std::enable_if_t<is_obj<Receiver>, void>
        callLater( Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 8: schedules an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        template <typename Receiver, typename Ret, typename ... Args>
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, void>
        callLater( Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ),
            Args&&... aArgs );

        //! CallLater Overload 9: schedules an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        template <typename Receiver, typename Ret, typename ... Args>
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, void>
        callLater( Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 10: schedules a static or free function to run deferred.
        template <typename Func, typename ... Args>
        static std::enable_if_t<std::is_pointer<Func>::value &&
            std::is_function<std::remove_pointer_t<Func> >::value,
            void>
        callLater
            (
            Object* aContext,
            Func aFunc,
            Args&&... aArgs
            );

        //! CallLater Overload 11: schedules a Signal emission to run deferred.
        //!
        //! Takes a Signal and not a SignalView, unlike the connect() overloads: this one emits,
        //! which is exactly what a view exists to withhold.
        template <typename ... SignalArgs, typename ... Args>
        static void callLater
            (
            Object* aContext,
            Signal<SignalArgs...>& aSignal,
            Args&&... aArgs
            );

        //! CallLater Overload 12: fallback overload producing a compile-time error for unsupported
        //! targets (e.g. lambdas).
        template <typename Target, typename ... Args>
        static std::enable_if_t<!MemberFunctionTraits<Target>::is_member_function &&
            !( std::is_pointer<Target>::value &&
            std::is_function<std::remove_pointer_t<Target> >::value ) &&
            !IsSignal<std::decay_t<Target> >::value,
            void>
        callLater
            (
            Object* aContext,
            Target&& aTarget,
            Args&&... aArgs
            );

    protected:
        //! Constructs an Object directly on stable thread data.
        //!
        //! For internal helpers that must stay safe if the public Thread object is destroyed
        //! concurrently: the data outlives its Thread, the Thread pointer does not.
        explicit Object
            (
            std::shared_ptr<ThreadData> aThreadData
            );

    private:
        //! Key identifying a deduplicated deferred call.
        //!
        //! Implementation detail of callLater()'s per-cycle deduplication; not part of the API.
        struct CallLaterKey
        {
            Object* mContext { nullptr };            //!< Target context Object.
            size_t mTypeHash { 0 };                    //!< Type hash code of the callable target.
            size_t mTargetSize { 0 };                  //!< Size of the callable target representation, in bytes.
            std::array<uint8_t, 32> mTargetBytes {};   //!< Binary payload representing the callable target.

            //! Compares two keys for equality.
            bool operator==
                (
                const CallLaterKey& aOther  //!< Key to compare.
                ) const
            {
                if( mContext != aOther.mContext || mTypeHash != aOther.mTypeHash ||
                    mTargetSize != aOther.mTargetSize )
                {
                    return false;
                }
                return std::memcmp( mTargetBytes.data(), aOther.mTargetBytes.data(), mTargetSize )
                       == 0;
            }

        };

        //! Hash functor for CallLaterKey.
        struct CallLaterKeyHash
        {
            //! Computes the hash value for a key.
            size_t operator()
                (
                const CallLaterKey& aKey  //!< Key to hash.
                ) const
            {
                size_t h = std::hash<Object*>()( aKey.mContext ) ^ ( aKey.mTypeHash << 1 );
                for( size_t i = 0; i < aKey.mTargetSize; ++i )
                {
                    h = h * 31 + aKey.mTargetBytes[i];
                }
                return h;
            }

        };
        //! Builds the key and the invoker for one deferred call, then schedules it.
        //!
        //! The eleven callLater() overloads differ only in how the compiler has to be told to name
        //! the target -- overloaded, inherited, const, non-void returning, free function, signal.
        //! What each one then *does* is identical, and this is that: hash the target into a
        //! deduplication key, pack the arguments into a tuple the invoker owns, and hand both to
        //! scheduleCallLater().
        //!
        //! @tparam KeyType The type hashed into the key. Deliberately separate from Target: the
        //!         inherited-slot overloads hash the *declared* member-pointer signature rather
        //!         than a deduced type, so that naming one slot through a base class and through
        //!         the receiver yields the same key and therefore deduplicates against itself.
        template <typename KeyType, typename Target, typename Caller, typename ... Args>
        static void dispatchCallLater
            (
            Object* aContext,      //!< Context owning the call; also the key's identity.
            const Target& aTarget,  //!< The callable being deferred, hashed by value into the key.
            Caller aCaller,         //!< Performs the call, given the unpacked arguments.
            Args&&... aArgs         //!< Arguments to copy and replay when the call runs.
            )
        {
            static_assert( sizeof( Target ) <= 32, "callLater target exceeds the key size limit." );

            CallLaterKey key;
            key.mContext = aContext;
            key.mTypeHash = typeid( KeyType ).hash_code();
            key.mTargetSize = sizeof( Target );
            std::memcpy( key.mTargetBytes.data(), &aTarget, sizeof( Target ) );

            auto invoker =
                [aCaller, tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... )]() mutable
                {
                    std::apply( aCaller, std::move( tupleArgs ) );
                };

            scheduleCallLater( aContext, key, invoker );
        }

        static void
        scheduleCallLater
            (
            Object* aContext,
            const CallLaterKey& aKey,
            std::function<void()> aInvoker
            );

        static bool isCurrentThread
            (
            const std::shared_ptr<ThreadData>& aData
            );

        bool forgetTimerId
            (
            int aTimerId
            );

        std::shared_ptr<ThreadData> threadData() const;

        //! Carries this object's already-posted events across in moveToThread(). See the definition.
        void migratePostedEvents
            (
            const std::shared_ptr<ThreadData>& aOldData,
            const std::shared_ptr<ThreadData>& aNewData
            );

        bool event
            (
            Event* aEvent
            );

        static bool
        dispatchMetaCall
            (
            Object* aTarget,
            std::function<void()> aSlot,
            ConnectionType aType
            );

        //! Dispatches a metacall to an explicitly named thread, ignoring the receiver's affinity.
        //!
        //! The shared core of the two overloads above, and the entry point for a caller that knows
        //! which thread it means rather than inferring it from an Object. Thread::post() needs
        //! exactly that: it targets the thread's *own* queue, which is not the same as the queue the
        //! Thread object happens to live in -- a Thread is constructed on one thread and then runs on
        //! another, so routing post() through its Object affinity would deliver to whoever created it
        //! until its loop started and re-pointed the affinity at itself.
        static bool
        dispatchMetaCallTo
            (
            const std::shared_ptr<ThreadData>& aData,
            Object* aReceiver,
            std::function<void()> aSlot
            );

        //! The one body shared by all ten connect() overloads.
        //!
        //! The overloads above differ only in what the compiler needs in order to *name* the slot:
        //! whether it is overloaded, inherited, const, or returns a value. None of them differs in
        //! what the resulting connection does. So each one binds the receiver and the slot into a
        //! small adapter and hands it here. Everything that is actually a connection -- the life
        //! token, the affinity box, the emit-time wrapper and the incoming-connection bookkeeping
        //! -- is written once, here.
        //!
        //! @p aSlot is a template parameter rather than a std::function on purpose. The adapter
        //! captures only a receiver pointer and a member-function pointer, and keeping its concrete
        //! type all the way into the wrapper below is what lets the direct and same-thread branches
        //! call the slot without type erasure and without a heap allocation. Type-erasing it here
        //! would put back the per-emit allocation removed on 2026-08-09.
        //!
        //! Thread-safe. Returns a default-constructed handle if @p aContext is null.
        template <typename SignalType, typename Callable>
        static Connection connectImpl
            (
            SignalType& aSignal,     //!< Signal to connect to.
            Object* aContext,        //!< Receiver/context supplying thread affinity and lifetime.
            Callable&& aSlot,        //!< Performs the call, given the emitted arguments.
            ConnectionType aType     //!< Requested connection type.
            )
        {
            // No context, no connection. Everything that makes a connection safe hangs off the
            // context: the life token that lets a queued invocation be dropped when the receiver
            // dies, the affinity that decides which thread it runs on, and the receiver whose
            // incoming list is pruned on disconnect. A connection without one has none of that --
            // it would fire forever, on whichever thread emitted, with nothing able to stop it. Qt
            // refuses the same call for the same reason, returning an invalid
            // QMetaObject::Connection.
            if( !aContext )
            {
                return {};
            }

            // The receiver's Affinity box, not a Thread* and not a snapshot of its ThreadData. The
            // box is resolved at emit time, so moveToThread() redirects even a connection made
            // before it, and it stays readable after the Object is destroyed.
            //
            // It is also the life token: the box carries the flag ~Object() clears, so a queued
            // invocation can be dropped if the receiver is destroyed before it runs. That used to
            // be a separate weak_ptr<int> captured alongside this one, which made every closure
            // here sixteen bytes larger to carry a bit this box already had room for.
            std::shared_ptr<Affinity> ctxAffinity = aContext->mAffinity;

            // Generic in its arguments so one wrapper serves every signal signature. Taking them by
            // forwarding reference rather than by the signal's declared value types also stops a
            // by-value signal argument being reconstructed at the wrapper boundary before anything
            // has even decided whether the call is inline.
            //
            // aContext is captured as a raw pointer, but never dereferenced here: it is handed to
            // dispatchMetaCallTo() purely as the queue key that removeEventsForReceiver() later
            // matches on. ~Object() strips every event still queued for it before it goes away, so
            // the dispatcher never delivers to a dead receiver.
            auto wrapper = [aContext, slot = std::forward<Callable>( aSlot ), aType,
                    ctxAffinity]( auto&&... aArgs )
                {
                    if( aType == ConnectionType::Direct )
                    {
                        // Always synchronous in the emitting thread, whatever the affinity is --
                        // Qt::DirectConnection ignores thread affinity too.
                        slot( aArgs ... );
                        return;
                    }

                    // Resolve the receiver's CURRENT affinity on every emit, like Qt reading
                    // QObjectPrivate::threadData at activate time. This is what makes
                    // moveToThread() affect connections made before it.
                    const auto ctxData = ctxAffinity ? ctxAffinity->data() : std::shared_ptr<
                            ThreadData>();

                    // No live thread to deliver on: either the receiver was detached with
                    // moveToThread(nullptr), or the Thread it lived in has been destroyed. Qt parks
                    // such an object on an orphan QThreadData whose event loop never runs, so the
                    // invocation is silently dropped -- "if targetThread is nullptr, all event
                    // processing for this object stops". Deliberately NOT a fallback direct call:
                    // that would run the slot on the emitting thread, which is precisely the thread
                    // confinement the caller gave up. thread() is read only as a yes/no test, never
                    // followed, so it cannot dangle.
                    if( ctxData == nullptr || ctxData->thread() == nullptr )
                    {
                        return;
                    }

                    if( aType == ConnectionType::Auto && isCurrentThread( ctxData ) )
                    {
                        // Already on the receiver's thread: deliver inline, like Qt::AutoConnection.
                        slot( aArgs ... );
                        return;
                    }

                    // Queued: the arguments have to outlive this call, so copy them once into a
                    // tuple the closure owns. Re-check the life token when it finally runs, since it
                    // was only checked at emit time and the receiver may be destroyed before the
                    // loop reaches it. Dispatched through the ThreadData, never a raw Thread*, so a
                    // concurrent ~Thread() cannot turn this into a use-after-free; if the target
                    // has no dispatcher the invocation is dropped, as Qt leaves events undelivered
                    // once the thread is gone.
                    //
                    // The tuple lives in the closure itself rather than behind a make_shared box:
                    // dispatchMetaCallTo() takes the std::function by value and moves it into the
                    // MetaCallEvent, so the tuple is built once and never copied, and the second
                    // heap allocation the box cost is gone.
                    dispatchMetaCallTo( ctxData, aContext,
                        [ctxAffinity, slot,
                        argTuple = std::make_tuple( std::forward<decltype( aArgs )>( aArgs )... )]()
                        {
                            if( ctxAffinity->isObjectAlive() )
                            {
                                std::apply( slot, argTuple );
                            }
                        } );
                };

            // Moved, not copied: connect() takes the slot by value, so passing the named local
            // built a second closure -- two shared_ptrs, a weak_ptr and the slot itself -- and
            // threw the first away.
            // The receiver and its life token go into the connection node, so ending the connection
            // prunes the receiver's incoming list in the same step, whichever route ends it.
            Connection handle = aSignal.connect( std::move( wrapper ), aContext, ctxAffinity );

            // Links the node into aContext's incoming list, and does nothing if a concurrent
            // disconnectAll() unlinked the connection while we were between the two lines. Both this
            // and the prune take aContext->mIncomingMutex, so one of the two orders always holds and
            // nothing is left linked for an unlink that already ran.
            // The Cleanup token this replaced got the same result from its own lifetime, and needed
            // a paragraph to say why; see R29 in history/OPEN-RISKS-20260813.md for the lock that was
            // added for a race a TSan probe then failed to reproduce, and reverted.
            handle.registerWithReceiver();
            return handle;
        }

        //! Grants the event queue access to event(), which it alone invokes.
        friend class EventDispatcherDefault;

        //! Grants a connection node the two members that are its half of the bookkeeping: it
        //! links itself into the incoming list when the connection is made, and unlinks itself
        //! when the connection ends.
        friend struct Private::ConnectionNode;

        //! Grants the callLater pending-call registry (defined in Object.cpp) the ability to
        //! name the private CallLaterKey/CallLaterKeyHash types its map is keyed on.
        friend struct CallLaterRegistry;

        //! Grants Thread access to dispatchMetaCall(), which Thread::post() uses to queue an
        //! arbitrary task onto itself.
        friend class Thread;

        //! Grants Timer access to the affinity plumbing its single-shot helper needs: it builds the
        //! helper directly on the context's thread data rather than moving it there afterwards.
        friend class Timer;

        const std::shared_ptr<Affinity> mAffinity;           //!< Thread affinity box, which also carries the life flag ~Object() clears; the box itself is never reassigned, only its contents (see moveToThread()).
        std::atomic<bool> mDeleteLaterPosted { false };       //!< True once deleteLater() has posted a DeferredDeleteEvent; de-bounces repeat calls, matching QObject::deleteLaterCalled.

        //! True once this object has been the context of a callLater(), so ~Object() knows whether
        //! the process-wide pending registry can possibly hold anything of ours.
        // Atomic because callLater() is callable from any thread while the destructor reads it, and
        // set with release / read with acquire so that seeing it true also means seeing the registry
        // entry it stands for.
        std::atomic<bool> mUsedCallLater { false };

        //! True once an event has been posted for this object, so ~Object() knows whether the
        //! dispatcher's queue can possibly hold anything of ours.
        //!
        //! Both flags are set-once. They exist because both scans are O(backlog) and were run on
        //! every destruction, including for the objects -- most of them -- that never used either
        //! feature. Qt guards the same call the same way: `if (d->postedEvents)` in ~QObject().
        // Set before the post, never cleared. An object that has received one queued call keeps paying
        // the scan; Qt keeps an exact count instead, which needs the dispatch side to decrement and is
        // more machinery than the difference is worth here.
        std::atomic<bool> mMayHaveQueuedWork { false };

        //! True once this object has started a timer, so ~Object() knows whether mRunningTimerIds
        //! can possibly hold anything.
        //!
        //! The third flag of the same set-once family, and there for the same reason as the other
        //! two: the destructor took mRunningTimerIdsMutex unconditionally, so every object that
        //! never owned a timer -- nearly all of them -- paid an uncontended lock and unlock to swap
        //! an empty vector. Cheaper than the scans P1 removed, but paid on a path whose whole cost
        //! is a few hundred nanoseconds. See PERFORMANCE-20260817.md (P11).
        // Set with release before the id is pushed, read with acquire, so seeing it false means no
        // push has been made visible to us. Never cleared: an object that has owned a timer keeps
        // taking the lock, which is the same honest trade the other two flags make.
        std::atomic<bool> mUsedTimers { false };
        //! This object's descriptive name.
        //!
        //! Deliberately unguarded, matching QObject, whose objectName() has no locking either. A
        //! mutex here would be paid for by every Object in the program to make one accessor safe
        //! against a use the thread-affinity rules already forbid: an Object belongs to one thread,
        //! and naming it from another is the same misuse as calling any of its other setters from
        //! there. Use the object from the thread it lives in.
        std::string mObjectName;

        //! Head of the list of connections where this object is the receiver, disconnected by
        //! ~Object().
        //!
        //! Without this a destroyed receiver's slot stays in the sender's slot list forever. The
        //! wrapper's life-token check makes it inert, but inert is not gone: it retains its
        //! captured state and is still walked on every emit, so one long-lived signal feeding
        //! many short-lived receivers grows without bound in both memory and emit cost. Qt does
        //! the equivalent by walking cd->senders in ~QObject().
        //!
        //! Intrusive: the list is threaded through the connection nodes themselves, so an incoming
        //! connection costs no allocation here and both linking and unlinking are O(1). It was a
        //! std::vector<Connection>, which cost a block per receiver and made unlinking a linear
        //! scan -- see PERFORMANCE-20260813.md (P10) for the block, and (P7) for the scan.
        Private::ConnectionNode* mIncomingHead { nullptr };

        //! How many nodes mIncomingHead's list holds, so the count stays O(1) rather than a walk.
        std::size_t mIncomingCount { 0 };

        //! Guards mIncomingHead, mIncomingCount, and every node's incoming links.
        mutable std::mutex mIncomingMutex;

        //! Timer ids started on this object and not yet killed.
        //!
        //! Exists so ~Object() can return them to the shared pool. Without it a destroyed object
        //! with a running timer would strand its id forever, and the pool would climb exactly as
        //! the old monotonic counter did. Qt keeps the same list in
        //! QObjectPrivate::extraData->runningTimers for the same reason.
        std::vector<int> mRunningTimerIds;

        //! Guards mRunningTimerIds.
        //!
        //! startTimer()/killTimer() are thread-confined so they only ever touch it from this
        //! object's own thread, but ~Object() and moveToThread() need it too and neither is bound
        //! quite that tightly, so the list is not single-threaded in practice.
        mutable std::mutex mRunningTimerIdsMutex;
    };

    //! Disconnects a signal connection using a connection handle. Thread-safe.
    inline void Object::disconnect
        (
        const Connection& aHandle  //!< The handle to disconnect.
        )
    {
        aHandle.disconnect();
    }

    //! CallLater Overload 1 definition. This is the primary overload for standard member
    //! functions. Because the target slot is not overloaded, the compiler can directly deduce the
    //! Slot type.
    template <typename Receiver, typename Slot, typename ... Args>
    std::enable_if_t<is_obj<Receiver> && MemberFunctionTraits<Slot>::is_member_function,
        void> Object::callLater
        (
        Receiver* aReceiver,  //!< Target object receiving the call.
        Slot aSlot,            //!< Member function pointer.
        Args&&... aArgs        //!< Arguments passed to slot.
        )
    {
        using SlotClass = typename MemberFunctionTraits<Slot>::class_type;

        static_assert( is_obj<Receiver>, "Receiver must be an instance of Object." );
        static_assert( MemberFunctionTraits<Slot>::is_member_function,
            "Slot must be a member function pointer." );
        static_assert( obj_is_base_of<Receiver, SlotClass>,
            "Slot must be a member function of Receiver or one of its base classes." );
        static_assert( std::is_invocable_v<Slot, Receiver*, Args...>,
            "Arguments do not match the parameters of the member function." );

        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<Slot>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 2 definition. If the target slot is overloaded and inherited from a base
    //! class, type deduction fails. This overload explicitly resolves the base class pointer so
    //! you can defer execution of inherited overloaded methods.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, void> Object::callLater
        (
        Receiver* aReceiver,  //!< Target object receiving the call.
        void ( SlotClass::*aSlot )
        (
        NonDeduced<Args>...
        ),                    //!< Member function pointer.
        Args&&... aArgs        //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<void ( SlotClass::* )
            (
            Args...
            )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 3 definition. Same as Overload 2, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, void> Object::callLater
        (
        Receiver* aReceiver,                                             //!< Target object receiving the call.
        void ( SlotClass::*aSlot )( NonDeduced<Args>... ) const,       //!< Const member function pointer.
        Args&&... aArgs                                                   //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<void ( SlotClass::* )
            (
            Args...
            ) const>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 4 definition. If an overloaded inherited slot returns a value, it won't
    //! match the void-returning overloads. This overload explicitly catches non-void slots from
    //! base classes; the return value is safely discarded upon invocation.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, void> Object::callLater
        (
        Receiver* aReceiver,  //!< Target object receiving the call.
        Ret ( SlotClass::*aSlot )
        (
        NonDeduced<Args>...
        ),                    //!< Member function pointer.
        Args&&... aArgs        //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<Ret ( SlotClass::* )
            (
            Args...
            )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 5 definition. Same as Overload 4, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, void> Object::callLater
        (
        Receiver* aReceiver,                                        //!< Target object receiving the call.
        Ret ( SlotClass::*aSlot )( NonDeduced<Args>... ) const,   //!< Const member function pointer.
        Args&&... aArgs                                              //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<Ret ( SlotClass::* )
            (
            Args...
            ) const>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 6 definition. If the target slot is overloaded, the compiler cannot
    //! deduce Slot in Overload 1. Using NonDeduced<Receiver>, this overload forces the compiler
    //! to use the passed args types to select the right overload.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<is_obj<Receiver>, void> Object::callLater
        (
        Receiver* aReceiver,                                                  //!< Target object receiving the call.
        void ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ),   //!< Member function pointer.
        Args&&... aArgs                                                       //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<void ( Receiver::* )
            (
            Args...
            )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 7 definition. Same as Overload 6, but specifically for const member
    //! functions.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<is_obj<Receiver>, void> Object::callLater
        (
        Receiver* aReceiver,                                                        //!< Target object receiving the call.
        void ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ) const,   //!< Const member function pointer.
        Args&&... aArgs                                                             //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<void ( Receiver::* )
            (
            Args...
            ) const>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 8 definition. If an overloaded slot returns a value, it won't match the
    //! void-returning Overload 6. This ensures deferring overloaded methods that return Ret
    //! compiles successfully.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, void> Object::callLater
        (
        Receiver* aReceiver,                                                 //!< Target object receiving the call.
        Ret ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ),   //!< Member function pointer.
        Args&&... aArgs                                                     //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<Ret ( Receiver::* )
            (
            Args...
            )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 9 definition. Same as Overload 8, but specifically for const member
    //! functions.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, void> Object::callLater
        (
        Receiver* aReceiver,                                                       //!< Target object receiving the call.
        Ret ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ) const,   //!< Const member function pointer.
        Args&&... aArgs                                                           //!< Arguments passed to slot.
        )
    {
        if( !aReceiver )
        {
            return;
        }

        dispatchCallLater<Ret ( Receiver::* )
            (
            Args...
            ) const>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 10 definition. Captures static and free functions, binding their
    //! execution to the provided context object's thread loop.
    template <typename Func, typename ... Args>
    std::enable_if_t<std::is_pointer<Func>::value &&
        std::is_function<std::remove_pointer_t<Func> >::value,
        void> Object::callLater
        (
        Object* aContext,  //!< Target Object defining thread affinity and lifetime.
        Func aFunc,          //!< Function pointer.
        Args&&... aArgs      //!< Arguments passed to function.
        )
    {
        static_assert( std::is_invocable_v<Func, Args...>,
            "Arguments do not match the parameters of the function." );

        if( !aContext || !aFunc )
        {
            return;
        }

        dispatchCallLater<Func>( aContext, aFunc,
            [aFunc]( auto&&... a )
            {
                (*aFunc )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 11 definition. Allows callLater to queue a signal emission
    //! (signal.emit(args...)) on a target thread instead of executing a function. SignalArgs are
    //! the signal's parameter types.
    template <typename ... SignalArgs, typename ... Args>
    void Object::callLater
        (
        Object* aContext,               //!< Target Object defining thread affinity and lifetime.
        Signal<SignalArgs...>& aSignal,  //!< Signal instance to emit.
        Args&&... aArgs                  //!< Arguments passed to signal.
        )
    {
        static_assert( std::is_invocable_v<Signal<SignalArgs...>, Args...>,
            "Arguments do not match the parameters of the signal." );

        if( !aContext )
        {
            return;
        }

        Signal<SignalArgs...>* sigPtr = &aSignal;

        dispatchCallLater<Signal<SignalArgs...> >( aContext, sigPtr,
            [sigPtr]( auto&&... a )
            {
                sigPtr->emit( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 12 definition. callLater relies on hashing the target address for
    //! deduplication. Lambdas cannot be reliably hashed, so this overload intentionally catches
    //! lambdas and general functors (Target) and triggers a static_assert.
    template <typename Target, typename ... Args>
    std::enable_if_t<!MemberFunctionTraits<Target>::is_member_function &&
        !( std::is_pointer<Target>::value &&
        std::is_function<std::remove_pointer_t<Target> >::value ) &&
        !IsSignal<std::decay_t<Target> >::value,
        void> Object::callLater
        (
        Object* aContext,  //!< Target Object context.
        Target&& aTarget,   //!< Unsupported callable object (e.g. lambda).
        Args&&... aArgs      //!< Arguments.
        )
    {
        ( void )aContext;
        ( void )aTarget;
        static_assert(
            sizeof( Target ) == 0, "Lambdas and general functors are not allowed in callLater." );
    }
}

#endif // QT_LIKE_SIGNAL_OBJECT_HPP
