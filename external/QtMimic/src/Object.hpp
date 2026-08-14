//! @file
//!
//! Qt-like object model: signal/slot connections with thread affinity, built on Signal and the
//! C++17 threading library.
//!
//! Two building blocks are provided:
//!   * QtMimic::Thread - an event-loop thread. Every object "lives" in one
//!                        Thread; queued slot invocations are executed in
//!                        that thread's event loop.
//!   * QtMimic::Object - a base class that carries thread affinity and offers
//!                        static connect() helpers that mirror Qt's
//!                        connect(sender-signal, receiver, &Receiver::slot).
//!
//! Like Qt, the delivery is decided at emit time (ConnectionType::Auto):
//!   * If the signal is emitted on the receiver's own thread, the slot runs
//!     synchronously (direct connection).
//!   * If the signal is emitted on a different thread, the slot invocation is
//!     queued into the receiver thread's event loop and executed there
//!     (queued connection). The arguments are copied, exactly like Qt.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_OBJECT_HPP
#define QT_MIMIC_OBJECT_HPP

#include "Event.hpp"
#include "Global.hpp"
#include "ThreadData.hpp"

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

namespace QtMimic
{
    class Object;
    // Forward-declared rather than included: Thread derives from Object, so Thread.hpp includes
    // this header and the reverse include would be a cycle. Object.cpp includes Thread.hpp for the
    // handful of places that need the definition, and isCurrentThread() below exists precisely so
    // the inline connect machinery in this header does not.
    class Thread;
    template <typename ... Args> class Signal;
    class AbstractEventDispatcher;
    class EventDispatcherDefault;
    class CoreApplication;

    //! Return true if child is the same as or derived from Object.
    template <typename Child> constexpr bool is_obj = std::is_base_of<Object, Child>::value;

    //! Return true if child object is the same as or derived from parent.
    template <typename Child, typename Parent>
    constexpr bool obj_is_base_of = std::is_base_of<Parent, Child>::value && is_obj<Parent>;

    //! Return true if child object is derived from parent.
    template <typename Child, typename Parent>
    constexpr bool obj_is_child_of = std::is_base_of<Parent, Child>::value
        && !std::is_same<Parent, Child>::value && is_obj<Parent>;

    //! Return true if T is void type.
    template <typename T> constexpr bool is_void = std::is_same<void, T>::value;

    //----------------------------------------------------------------
    //! @class Object
    //!
    //! Base class carrying thread affinity. Derive from it and declare signals as
    //! public QtMimic::Signal<Args...> members. Connect them to member-function
    //! slots of other Objects with Object::connect().
    //----------------------------------------------------------------
    class Object
    {
    public:
        explicit Object
            (
            Thread* aThread = nullptr
            );

        virtual ~Object();

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
        std::size_t incomingConnectionCount() const
        {
            std::lock_guard<std::mutex> lock( mIncomingMutex );
            return mIncoming.size();
        }

        //! Gets the weak pointer tracking the lifetime of this object. Thread-safe.
        //!
        //! Callers testing whether the object is still alive should use `expired()`, **not**
        //! `lock()`. The two are equally safe here and `expired()` is far cheaper: it is a plain
        //! load where `lock()` is an atomic read-modify-write on the control block.
        //!
        //! Equally safe because the token is an `int`, not the Object. Holding the `shared_ptr`
        //! that `lock()` returns keeps that `int` alive; it does nothing whatsoever to stop the
        //! Object being destroyed a moment later. Both forms answer exactly one question -- "had
        //! destruction begun at the instant of the check" -- and neither closes the check-then-use
        //! race that follows it. What actually stops a destroyed receiver being called is
        //! ~Object() disconnecting its incoming connections.
        std::weak_ptr<int> objectLife() const
        {
            return mLife;
        }


        //! [Connect Overload 1]: Connects a signal to a non-overloaded member function slot.
        //!
        //! Why it exists: This is the primary overload for standard member functions. Because the target
        //! slot is not overloaded, the compiler can directly deduce the `Slot` type without needing
        //! explicit template resolution.
        //! @tparam Signal The signal type.
        //! @tparam Receiver The receiver object type (must derive from Object).
        //! @tparam Slot The member function pointer type.
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The member function to call when the signal is emitted.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <typename Signal, typename Receiver, typename Slot>
        static std::enable_if_t<MemberFunctionTraits<Slot>::is_member_function,
            Connection>
        connect
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

        //! [Connect Overload 2]: Connects an overloaded void member function slot inherited from
        //! a base class.
        //!
        //! Why it exists: If the target slot is overloaded, the compiler cannot deduce `Slot` in
        //! Overload 1. When the overloaded slot is defined in a base class of the receiver, type
        //! deduction fails. This overload explicitly resolves the base class pointer so you can connect
        //! inherited overloaded methods seamlessly.
        //! @tparam SignalArgs Parameter types of the signal used to select the slot overload.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @tparam SlotClass Base class owning the member function slot.
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The member function pointer matching SignalArgs.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, Connection>
        connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };
            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! [Connect Overload 3]: Connects an overloaded const void member function slot inherited
        //! from a base class.
        //!
        //! Why it exists: Similar to Overload 2, but specifically for const member functions. C++
        //! requires separate template matching for const qualifiers on member function pointers.
        //! @tparam SignalArgs Parameter types of the signal used to select the slot overload.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @tparam SlotClass Base class owning the member function slot.
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The const member function pointer matching SignalArgs.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, Connection>
        connect
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

        //! [Connect Overload 4]: Connects an overloaded non-void returning member function slot
        //! inherited from a base class.
        //!
        //! Why it exists: If an overloaded inherited slot returns a value (e.g. `bool`), it won't match
        //! the void-returning Overloads 2 and 3. This overload explicitly catches non-void slots from
        //! base classes (the return value is safely discarded during emission).
        //! @tparam SignalArgs Parameter types of the signal used to select the slot overload.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @tparam SlotClass Base class owning the member function slot.
        //! @tparam Ret Return type of the slot (discarded upon invocation).
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The member function pointer matching SignalArgs and returning Ret.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, Connection>
        connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... aCallArgs )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
                };
            return connectImpl( aSignal, aReceiver, std::move( adapter ), aType );
        }

        //! [Connect Overload 5]: Connects an overloaded non-void returning const member function
        //! slot inherited from a base class.
        //!
        //! Why it exists: Similar to Overload 4, but specifically for const member functions.
        //! @tparam SignalArgs Parameter types of the signal used to select the slot overload.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @tparam SlotClass Base class owning the member function slot.
        //! @tparam Ret Return type of the slot (discarded upon invocation).
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The const member function pointer matching SignalArgs and returning Ret.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
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

        //! [Connect Overload 6]: Connects an overloaded void member function slot defined
        //! directly on the receiver.
        //!
        //! Why it exists: If the target slot is overloaded (e.g. `onEvent()` and `onEvent(int)`), the
        //! compiler cannot deduce `Slot` in Overload 1. By using `NonDeduced<Receiver>`, this
        //! overload forces the compiler to use `SignalArgs` from the signal to perfectly select the
        //! right overload pointer.
        //! @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
        //! signature.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The member function pointer matching SignalArgs.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver>
        static std::enable_if_t<is_obj<Receiver>, Connection>
        connect
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

        //! [Connect Overload 7]: Connects an overloaded const void member function slot defined
        //! directly on the receiver.
        //!
        //! Why it exists: Similar to Overload 6, but specifically matches const member functions.
        //! @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
        //! signature.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The const member function pointer matching SignalArgs.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver>
        static std::enable_if_t<is_obj<Receiver>, Connection>
        connect
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

        //! [Connect Overload 8]: Connects an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        //!
        //! Why it exists: If an overloaded slot returns a value (e.g. `bool`), it won't match the
        //! void-returning Overload 6. This overload ensures connecting an overloaded method that returns
        //! `Ret` compiles successfully.
        //! @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
        //! signature.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @tparam Ret Return type of the slot (discarded upon invocation).
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The member function pointer matching SignalArgs and returning Ret.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename Ret>
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, Connection>
        connect
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

        //! [Connect Overload 9]: Connects an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        //!
        //! Why it exists: Similar to Overload 8, but specifically for const member functions.
        //! @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
        //! signature.
        //! @tparam Receiver Receiver object type (must derive from Object).
        //! @tparam Ret Return type of the slot (discarded upon invocation).
        //! @param aSignal The signal to connect.
        //! @param aReceiver The object receiving the signal.
        //! @param aSlot The const member function pointer matching SignalArgs and returning Ret.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
        template <template <typename ...> class SignalSource, typename ... SignalArgs,
            typename Receiver, typename Ret>
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, Connection>
        connect
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

        //! [Connect Overload 10]: Connects a signal to an arbitrary callable (e.g. lambda, functor,
        //! std::function) with a context object for thread affinity and lifetime management
        //! (like Qt's context-object connect).
        //! @tparam Func The callable type (lambda, functor, std::function).
        //! @tparam Args Parameter types of the signal.
        //! @param aSignal The signal to connect.
        //! @param aContext The context object (must derive from Object) for thread affinity and
        //!        lifetime management.
        //! @param aSlot The callable to invoke when the signal is emitted.
        //! @param aType The type of connection.
        //! @return A handle representing the connection. Thread-safe.
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
            const Connection& aHandle  //!< The handle to disconnect.
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
        //! Construct an Object directly on stable thread data. Used by internal helpers that must
        //! remain safe if the public Thread object is destroyed concurrently.
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
        //! The entry point for a caller that knows which thread it means rather than inferring it
        //! from an Object. Thread::post() needs exactly that: it targets the thread's *own* queue,
        //! which is not the same as the queue the Thread object happens to live in -- a Thread is
        //! constructed on one thread and then runs on another, so routing post() through its Object
        //! affinity would deliver to whoever created it until its loop started and re-pointed the
        //! affinity at itself.
        static bool
        dispatchMetaCallTo
            (
            const std::shared_ptr<ThreadData>& aData,
            Object* aReceiver,
            std::function<void()> aSlot
            );

        //! Removes its connection from the receiver's mIncoming when destroyed,
        //! i.e. the moment the connection is disconnected. Lives only as long as the
        //! connection (captured by the slot).
        struct Cleanup
        {
            Cleanup
                (
                Object* aOwner,
                std::weak_ptr<int> aLife
                )
                : mOwner( aOwner )
                , mLife( std::move( aLife ) )
            {
            }

            ~Cleanup();

            Cleanup
                (
                const Cleanup&
                ) = delete;

            Cleanup& operator=
                (
                const Cleanup&
                ) = delete;

            Object* mOwner;
            std::weak_ptr<int> mLife;
            Connection mHandle;
        };

        //! Internal implementation of the connect() overloads. Handles thread affinity,
        //! queued/direct invocation, and lifetime management.
        //! @tparam SignalType The type of the signal being connected.
        //! @tparam ContextType The type of the context object (must derive from Object).
        //! @tparam Callable The type of the callable (lambda, functor, std::function).
        //! @param aSignal The signal to connect.
        //! @param aContext The context object for thread affinity and lifetime management.
        //! @param aSlot The callable to invoke when the signal is emitted.
        //! @param aType The type of connection (Auto, Direct, Queued).
        //! @return A handle representing the connection. Thread-safe.
        template <typename SignalType, typename Callable>
        static Connection connectImpl
            (
            SignalType& aSignal,
            Object* aContext,
            Callable&& aSlot,
            ConnectionType aType
            )
        {
            // No context, no connection. Everything that makes a connection safe hangs off the
            // context: the life token that lets a queued invocation be dropped when the receiver
            // dies, the affinity that decides which thread it runs on, and the cleanup token that
            // prunes it on disconnect. A connection without one has none of that -- it would fire
            // forever, on whichever thread emitted, with nothing able to stop it. Qt refuses the
            // same call for the same reason, returning an invalid QMetaObject::Connection.
            if( !aContext )
            {
                return {};
            }

            // Capture a weak reference to the context's life token so queued
            // invocations can be safely dropped if the receiver is destroyed before
            // they run. (Only used on the queued path, which requires a context.)
            std::weak_ptr<int> weakLife = aContext->objectLife();

            // Capture the receiver's Affinity box, not a Thread* and not a snapshot of its
            // ThreadData. The box is resolved at emit time, so moveToThread() redirects even a
            // connection made before it, and it stays readable after the Object is destroyed.
            std::shared_ptr<Affinity> ctxAffinity = aContext->mAffinity;

            // Cleanup token captured by the slot: when the connection ends, the Signal destroys
            // the slot, which prunes the handle from the receiver immediately. The weak life token
            // stops it touching a receiver that is already gone.
            std::shared_ptr<Cleanup> cleanup = std::make_shared<Cleanup>( aContext, weakLife );

            // aContext is captured as a raw pointer, but never dereferenced here: it is handed to
            // dispatchMetaCallTo() purely as the queue key that removeEventsForReceiver() later
            // matches on. ~Object() strips every event still queued for it before it goes away, so
            // the dispatcher never delivers to a dead receiver.
            auto wrapper = [weakLife, aContext, slot = std::forward<Callable>( aSlot ), aType,
                ctxAffinity, cleanup]( auto&&... aArgs )
                {
                    if( aType == ConnectionType::Direct )
                    {
                        // Always synchronous in the emitting thread, whatever the affinity is --
                        // Qt::DirectConnection ignores thread affinity too.
                        slot( aArgs ... );
                        return;
                    }

                    // Resolve the receiver's CURRENT affinity on every emit, like Qt reading
                    // QObjectPrivate::threadData at activate time. This is what makes moveToThread()
                    // affect connections made before it.
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

                    // Queued connection: copy the arguments and run later in the receiver's
                    // event loop. Dispatched through the ThreadData (kept alive by the captured
                    // ctxData shared_ptr), NEVER a raw Thread* -- so a concurrent ~Thread()
                    // cannot turn this into a use-after-free. If the target thread has no
                    // dispatcher, dispatchMetaCallTo() returns false and the invocation is safely
                    // dropped, exactly as Qt leaves events undelivered once the thread is gone.
                    // Skip too if the receiver itself is gone by the time the call runs (the life
                    // token), which is only known then and not at emit time.
                    //
                    // The argument tuple lives in the closure itself rather than behind a
                    // make_shared box: dispatchMetaCallTo() takes the std::function by value and
                    // moves it into the MetaCallEvent, so the tuple is built once and never
                    // copied, and the second heap allocation the box cost is gone.
                    dispatchMetaCallTo( ctxData, aContext,
                        [weakLife, slot,
                        argTuple = std::make_tuple( std::forward<decltype( aArgs )>( aArgs )... )]()
                        {
                            if( !weakLife.expired() )
                            {
                                std::apply( slot, argTuple );
                            }
                        } );
                };

            // Moved, not copied: connect() takes the slot by value, so passing the named local
            // built a second closure -- two shared_ptrs, a weak_ptr and the slot itself -- and
            // threw the first away.
            Connection handle = aSignal.connect( std::move( wrapper ) );

            cleanup->mHandle = handle;
            {
                std::lock_guard<std::mutex> lock( aContext->mIncomingMutex );
                aContext->mIncoming.push_back( handle );
            }
            return handle;
        }
        //! Grants the event queue access to event(), which it alone invokes.
        friend class EventDispatcherDefault;

        //! Grants the callLater pending-call registry (defined in Object.cpp) the ability to
        //! name the private CallLaterKey/CallLaterKeyHash types its map is keyed on.
        friend struct CallLaterRegistry;

        //! Grants Thread access to threadData() and mAffinity, which it needs to adopt a thread's
        //! affinity, and to dispatchMetaCallTo(), which Thread::post() queues through.
        friend class Thread;

        friend class Timer;

        std::shared_ptr<int> mLife;              //!< Liveness token; reset in ~Object() so weak references expire.
        const std::shared_ptr<Affinity> mAffinity;     //!< Affinity holder; never reassigned after ctor
        std::atomic<bool> mDeleteLaterPosted { false }; //!< true once deleteLater() has posted delete

        //! True once this object has been the context of a callLater(), so ~Object() knows whether
        //! the process-wide pending registry can possibly hold anything of ours.
        std::atomic<bool> mUsedCallLater { false };

        //! True once an event has been posted for this object, so ~Object() knows whether the
        //! dispatcher's queue can possibly hold anything of ours.
        //!
        //! Both flags are set-once. They exist because both scans are O(backlog) and were run on
        //! every destruction, including for the objects -- most of them -- that never used either
        //! feature. Qt guards the same call the same way: `if (d->postedEvents)` in ~QObject().
        std::atomic<bool> mMayHaveQueuedWork { false };
        //! This object's descriptive name.
        //!
        //! Deliberately unguarded, matching QObject, whose objectName() has no locking either. A
        //! mutex here would be paid for by every Object in the program to make one accessor safe
        //! against a use the thread-affinity rules already forbid: an Object belongs to one thread,
        //! and naming it from another is the same misuse as calling any of its other setters from
        //! there. Use the object from the thread it lives in.
        std::string mObjectName;

        std::vector<Connection> mIncoming;       //!< Connections where this is the receiver
        mutable std::mutex mIncomingMutex;       //!< Guards mIncoming.


        //! Ids of timers started on this object and not yet killed.
        //!
        //! Exists so ~Object() can hand them back to the shared pool: a destroyed object with a
        //! running timer would otherwise strand its id forever and the pool would climb without
        //! bound. Qt keeps the same list in QObjectPrivate::extraData->runningTimers for the same
        //! reason.
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
        const Connection& aHandle
        )
    {
        aHandle.disconnect();
    }

    //! CallLater Overload 1 definition. This is the primary overload for standard member
    //! functions. Because the target slot is not overloaded, the compiler can directly deduce the
    //! Slot type.
    template <typename Receiver, typename Slot, typename ... Args>
    std::enable_if_t<is_obj<Receiver> && MemberFunctionTraits<Slot>::is_member_function,
        void>Object::callLater
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
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, void>Object::callLater
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

        dispatchCallLater<void ( SlotClass::* )( Args... )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 3 definition. Same as Overload 2, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, void>Object::callLater
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

        dispatchCallLater<void ( SlotClass::* )( Args... ) const>( aReceiver, aSlot,
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
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, void>Object::callLater
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

        dispatchCallLater<Ret ( SlotClass::* )( Args... )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 5 definition. Same as Overload 4, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<obj_is_child_of<Receiver, SlotClass> && !is_void<Ret>, void>Object::callLater
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

        dispatchCallLater<Ret ( SlotClass::* )( Args... ) const>( aReceiver, aSlot,
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
    std::enable_if_t<is_obj<Receiver>, void>Object::callLater
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

        dispatchCallLater<void ( Receiver::* )( Args... )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 7 definition. Same as Overload 6, but specifically for const member
    //! functions.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<is_obj<Receiver>, void>Object::callLater
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

        dispatchCallLater<void ( Receiver::* )( Args... ) const>( aReceiver, aSlot,
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
    std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, void>Object::callLater
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

        dispatchCallLater<Ret ( Receiver::* )( Args... )>( aReceiver, aSlot,
            [aReceiver, aSlot]( auto&&... a )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
            },
            std::forward<Args>( aArgs )... );
    }

    //! CallLater Overload 9 definition. Same as Overload 8, but specifically for const member
    //! functions.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, void>Object::callLater
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

        dispatchCallLater<Ret ( Receiver::* )( Args... ) const>( aReceiver, aSlot,
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
        void>Object::callLater
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

        dispatchCallLater<Signal<SignalArgs...>>( aContext, sigPtr,
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
        void>Object::callLater
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

} // namespace QtMimic

#endif // QT_MIMIC_OBJECT_HPP
