//! @file
//!
//! Qt-like object model providing signal/slot connections with thread
//! affinity on top of boost::signals2 (signals) and the C++17 standard threading
//! library (std::thread).
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

#include "Global.hpp"
#include "Signal.hpp"
#include "Thread.hpp"
#include "TimerEvent.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace QtMimic
{
    class Object;

    //! Return true if child is the same as or derived from Object.
    template <typename Child> constexpr bool is_obj = std::is_base_of<Object, Child>::value;

    //! Return true if child object is the same as or derived from parent.
    template <typename Child, typename Parent>
    constexpr bool obj_is_base_of = std::is_base_of<Parent, Child>::value && is_obj<Parent>;

    //! Return true if child object is derived from parent.
    template <typename Child, typename Parent>
    constexpr bool obj_is_child_of = std::is_base_of<Parent, Child>::value && !std::is_same<Parent,
            Child>::value && is_obj<Parent>;

    //! Return true if T is void type.
    template <typename T> constexpr bool is_void = std::is_same<void, T>::value;

    //! Controls how a slot is invoked relative to the emitting thread.
    enum class ConnectionType
    {
        //! Direct call if emitted on the receiver's thread, queued otherwise.
        //! The decision is made at emit time, like Qt::AutoConnection.
        Auto,
        //! Always call the slot synchronously in the emitting thread.
        Direct,
        //! Always queue the slot into the receiver thread's event loop.
        Queued,
    };

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

        std::size_t incomingConnectionCount() const;

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
        static std::enable_if_t<is_obj<Receiver> && MemberFunctionTraits<Slot>::is_member,
            Connection>connect
            (
            Signal& aSignal,
            Receiver* aReceiver,
            Slot aSlot,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            using SlotClass = typename MemberFunctionTraits<Slot>::class_type;

            static_assert( MemberFunctionTraits<Slot>::is_member,
                "Slot must be a member function pointer." );
            static_assert( obj_is_base_of<Receiver, SlotClass>,
                "Slot must be a member function of Receiver or one of its base classes." );

            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, Connection>connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::* aSlot )
            (
            SignalArgs...
            ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
        static std::enable_if_t<obj_is_child_of<Receiver, SlotClass>, Connection>connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::* aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
            Ret ( SlotClass::* aSlot )
            (
            SignalArgs...
            ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
            Ret ( SlotClass::* aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
        static std::enable_if_t<is_obj<Receiver>, Connection>connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void( NonDeduced<Receiver>::* aSlot )( SignalArgs ... ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
        static std::enable_if_t<is_obj<Receiver>, Connection>connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void( NonDeduced<Receiver>::* aSlot )( SignalArgs ... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, Connection>connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret( NonDeduced<Receiver>::* aSlot )( SignalArgs ... ),
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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
        static std::enable_if_t<is_obj<Receiver> && !is_void<Ret>, Connection>connect
            (
            SignalSource<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret( NonDeduced<Receiver>::* aSlot )( SignalArgs ... ) const,
            ConnectionType aType = ConnectionType::Auto
            )
        {
            auto adapter = [aReceiver, aSlot]( auto&&... args )
                {
                    ( aReceiver->*aSlot )( std::forward<decltype( args )>( args )... );
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

    protected:
        //! Construct an Object directly on stable thread data. Used by internal helpers that must
        //! remain safe if the public Thread object is destroyed concurrently.
        explicit Object
            (
            std::shared_ptr<ThreadData> aThreadData
            );

    private:
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
        template <typename SignalType, typename ContextType, typename Callable>
        static Connection connectImpl
            (
            SignalType& aSignal,
            ContextType* aContext,
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
            std::weak_ptr<int> life = aContext->mLife;

            // Capture the context's ThreadData, NOT a raw Thread*. Holding the data keeps it alive
            // for as long as this connection exists, and its thread() is resolved at emit time --
            // reporting nullptr, never a dangling pointer, if that Thread has since been destroyed.
            // Capturing the Thread* here instead was a real, ASan-confirmed use-after-free, for
            // both auto-adopted dummy Threads (destroyed at native thread exit) and explicit
            // user-owned Threads (destroyed whenever the user likes).
            // Capture the receiver's Affinity holder, NOT a snapshot of its ThreadData. The holder
            // is resolved at emit time (below), so moveToThread() redirects even this connection.
            // Held by shared_ptr so it stays readable after the Object is destroyed.
            std::shared_ptr<Affinity> ctxAffinity = aContext->mAffinity;

            // Cleanup token captured by the slot: when the connection ends (manual
            // disconnect or signal teardown), boost destroys the slot, running this
            // destructor and pruning the handle from the receiver immediately. The
            // weak life token avoids touching a receiver that is already gone.
            std::shared_ptr<Cleanup> cleanup = std::make_shared<Cleanup>( aContext, life );

            Connection handle = aSignal.connectReflective( [slot = std::forward<Callable>( aSlot ),
                life, ctxAffinity, aType, cleanup]( const Connection&, auto&&... args )
                {
                    if( aType == ConnectionType::Direct )
                    {
                        // Always synchronous in the emitting thread, whatever the affinity is --
                        // Qt::DirectConnection ignores thread affinity too.
                        slot( args ... );
                        return;
                    }

                    // Resolve the receiver's CURRENT affinity on every emit, like Qt reading
                    // QObjectPrivate::threadData at activate time. This is what makes moveToThread()
                    // affect connections made before it.
                    const std::shared_ptr<ThreadData> ctxData =
                    ctxAffinity ? ctxAffinity->data()
                    : std::shared_ptr<ThreadData>();

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

                    if( aType == ConnectionType::Auto && ctxData == Thread::currentData() )
                    {
                        // Already on the receiver's thread: deliver inline, like Qt::AutoConnection.
                        slot( args ... );
                        return;
                    }

                    // Queued connection: copy the arguments and run later in the receiver's
                    // event loop. Post through the ThreadData (kept alive by the captured
                    // ctxData shared_ptr), NEVER a raw Thread* -- so a concurrent ~Thread()
                    // cannot turn this into a use-after-free. If the target thread has already
                    // stopped, post() returns false and the invocation is safely dropped, exactly
                    // as Qt leaves events undelivered once the thread is gone. Skip too if the
                    // receiver itself is gone by the time the task runs (the life token).
                    auto sharedArgs =
                    std::make_shared<std::tuple<std::decay_t<decltype( args )>...> >(
                        std::forward<decltype( args )>( args )... );
                    ctxData->post( [slot, life, sharedArgs]()
                    {
                        if( !life.expired() )
                        {
                            std::apply( slot, *sharedArgs );
                        }
                    } );
                } );

            cleanup->mHandle = handle;
            {
                std::lock_guard<std::mutex> locker( aContext->mIncomingMutex );
                aContext->mIncoming.push_back( handle );
            }
            return handle;
        }

        //! Removes its connection from the receiver's mIncoming when destroyed,
        //! i.e. the moment the connection is disconnected. Lives only as long as the
        //! connection (captured by the slot).
        struct Cleanup
        {
            Cleanup
                (
                Object* aOwner,
                std::weak_ptr<int> aLife
                );

            ~Cleanup();

            Object* mOwner;
            std::weak_ptr<int> mLife;
            Connection mHandle;
        };
        bool forgetTimerId
            (
            int aTimerId
            );

        std::shared_ptr<ThreadData> threadDataRef() const;

        const std::shared_ptr<Affinity> mAffinity;     //!< Affinity holder; never reassigned after ctor
        std::shared_ptr<int> mLife;              //!< Liveness token for queued slots
        std::atomic<bool> mDeleteLaterPosted { false }; //!< true once deleteLater() has posted delete
        mutable std::mutex mIncomingMutex;       //!< Protects mIncoming
        std::vector<Connection> mIncoming;       //!< Connections where this is the receiver

        //! This object's descriptive name.
        //!
        //! Deliberately unguarded, matching QObject, whose objectName() has no locking either. A
        //! mutex here would be paid for by every Object in the program to make one accessor safe
        //! against a use the thread-affinity rules already forbid: an Object belongs to one thread,
        //! and naming it from another is the same misuse as calling any of its other setters from
        //! there. Use the object from the thread it lives in.
        std::string mObjectName;

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

        friend class Timer;
    };

} // namespace QtMimic

#endif // QT_MIMIC_OBJECT_HPP
