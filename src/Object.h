#ifndef OBJECT_H
#define OBJECT_H

#include "Event.h"
#include "Global.h"
#include "ThreadData.hpp"

#include <array>
#include <atomic>
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
    class Thread;
    template <typename ... Args> class Signal;
    class AbstractEventDispatcher;
    class EventDispatcherDefault;
    class CoreApplication;

    //! Base class for all objects participating in the signal-slot and event system.
    class Object
    {
    public:
        Object();

        virtual ~Object();

        //! Object is neither copyable nor movable.
        //!
        //! These are already deleted implicitly, because the class holds std::mutex members -- but
        //! only by accident. Stating it makes the guarantee survive refactoring: mLife is a
        //! shared_ptr, so a copy would raise its use count and ~Object()'s mLife.reset() would no
        //! longer expire the token. Every connect()/callLater() wrapper's weakLife.lock() would keep
        //! succeeding and invoke slots on a destroyed object -- a use-after-free reintroduced silently
        //! by an unrelated change.
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

        virtual void timerEvent
            (
            TimerEvent* aEvent
            );

        int startTimer
            (
            int aInterval
            );

        void killTimer
            (
            int aId
            );

        void addCleanupCallback
            (
            std::function<void()> aCallback
            );

        //! Gets the weak pointer tracking the lifetime of this object. Thread-safe.
        //!
        //! Callers testing whether the object is still alive should use `expired()`, **not**
        //! `lock()`. The two are equally safe here and `expired()` is around 68x cheaper: measured
        //! 0.25 ns against 17.1 ns, because `lock()` is an atomic read-modify-write on the control
        //! block where `expired()` is a plain load. On the emit path that was about 22% of a direct
        //! emit, spent on nothing.
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

        //! Connect Overload 1: connects a signal to a non-overloaded member function slot.
        template <typename Signal, typename Receiver, typename Slot>
        static std::enable_if_t<MemberFunctionTraits<Slot>::is_member_function,
            ConnectionHandle>
        connect
            (
            Signal& aSignal,
            Receiver* aReceiver,
            Slot aSlot,
            ConnectionType aType = ConnectionType::Auto
            );

        //! Connect Overload 2: connects an overloaded void member function slot inherited from a
        //! base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 3: connects an overloaded const void member function slot inherited
        //! from a base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( SlotClass::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 4: connects an overloaded non-void returning member function slot
        //! inherited from a base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<
            std::is_base_of<Object, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 5: connects an overloaded non-void returning const member function
        //! slot inherited from a base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<
            std::is_base_of<Object, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 6: connects an overloaded void member function slot defined directly
        //! on the receiver.
        template <typename ... SignalArgs, typename Receiver>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value, ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 7: connects an overloaded const void member function slot defined
        //! directly on the receiver.
        template <typename ... SignalArgs, typename Receiver>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value, ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 8: connects an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        template <typename ... SignalArgs, typename Receiver, typename Ret>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ),
            ConnectionType aType = ConnectionType::Auto );

        //! Connect Overload 9: connects an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        template <typename ... SignalArgs, typename Receiver, typename Ret>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            ConnectionHandle>
        connect( Signal<SignalArgs...>& aSignal,
            Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ) const,
            ConnectionType aType = ConnectionType::Auto );
        //! Connect Overload 10: connects a signal to a free function, lambda, or general functor
        //! slot.
        template <typename Signal, typename Functor>
        static std::enable_if_t<!MemberFunctionTraits<Functor>::is_member_function,
            ConnectionHandle>
        connect
            (
            Signal& aSignal,
            Object* aContext,
            Functor aSlot,
            ConnectionType aType = ConnectionType::Auto
            );

        static void disconnect
            (
            const ConnectionHandle& aHandle
            );

        //! CallLater Overload 1: schedules a non-overloaded member function slot to run deferred.
        template <typename Receiver, typename Slot, typename ... Args>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            MemberFunctionTraits<Slot>::is_member_function,
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
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            void>
        callLater( Receiver* aReceiver, void ( SlotClass::*aSlot )( NonDeduced<Args>... ), Args&&
            ...
            aArgs );

        //! CallLater Overload 3: schedules an overloaded const void member function slot inherited
        //! from a base class.
        template <typename Receiver, typename SlotClass, typename ... Args>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            void>
        callLater( Receiver* aReceiver,
            void ( SlotClass::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 4: schedules an overloaded non-void returning member function slot
        //! inherited from a base class.
        template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
        static std::enable_if_t<
            std::is_base_of<Object, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* aReceiver, Ret ( SlotClass::*aSlot )( NonDeduced<Args>... ), Args&&
            ...
            aArgs );

        //! CallLater Overload 5: schedules an overloaded non-void returning const member function
        //! slot inherited from a base class.
        template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
        static std::enable_if_t<
            std::is_base_of<Object, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* aReceiver,
            Ret ( SlotClass::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 6: schedules an overloaded void member function slot defined
        //! directly on the receiver.
        template <typename Receiver, typename ... Args>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value, void>
        callLater( Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ),
            Args&&... aArgs );

        //! CallLater Overload 7: schedules an overloaded const void member function slot defined
        //! directly on the receiver.
        template <typename Receiver, typename ... Args>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value, void>
        callLater( Receiver* aReceiver,
            void ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ) const,
            Args&&... aArgs );

        //! CallLater Overload 8: schedules an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        template <typename Receiver, typename Ret, typename ... Args>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* aReceiver,
            Ret ( NonDeduced<Receiver>::*aSlot )( NonDeduced<Args>... ),
            Args&&... aArgs );

        //! CallLater Overload 9: schedules an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        template <typename Receiver, typename Ret, typename ... Args>
        static std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            void>
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

        std::shared_ptr<ThreadData> threadData() const;

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

        //! Prunes one connection from its receiver's mIncoming when that connection ends.
        //!
        //! Held by shared_ptr inside the connection's own slot closure, so it is destroyed exactly
        //! when boost destroys the slot -- whether that is a manual disconnect(), the sender Signal
        //! being destroyed, or ~Object() below. Without it mIncoming would only ever grow: an
        //! object that outlives a connection it received would keep a handle to a connection that
        //! no longer exists, and eventually disconnect() a recycled one.
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

            Object* mOwner;                //!< Receiver owning the mIncoming entry.
            std::weak_ptr<int> mLife;      //!< Receiver's life token; expired means it is gone.
            ConnectionHandle mHandle;      //!< The entry to prune; set once the handle exists.
        };

        //! The one body shared by all ten connect() overloads.
        //!
        //! The overloads above differ only in what the compiler needs in order to *name* the slot:
        //! whether it is overloaded, inherited, const, or returns a value. None of them differs in
        //! what the resulting connection does. So each one binds the receiver and the slot into a
        //! small adapter and hands it here, exactly as QtMimic's overloads hand theirs to its
        //! connectImpl(); everything that is actually a connection -- the life token, the affinity
        //! box, the cleanup token, the emit-time wrapper and the incoming-connection bookkeeping --
        //! is written once, here.
        //!
        //! @p aSlot is a template parameter rather than a std::function on purpose. The adapter
        //! captures only a receiver pointer and a member-function pointer, and keeping its concrete
        //! type all the way into the wrapper below is what lets the direct and same-thread branches
        //! call the slot without type erasure and without a heap allocation. Type-erasing it here
        //! would put back the per-emit allocation removed on 2026-08-09.
        //!
        //! Thread-safe. Returns a default-constructed handle if @p aContext is null.
        template <typename SignalType, typename Callable>
        static ConnectionHandle connectImpl
            (
            SignalType& aSignal,     //!< Signal to connect to.
            Object* aContext,        //!< Receiver/context supplying thread affinity and lifetime.
            Callable aSlot,          //!< Adapter that performs the call, given the emitted arguments.
            ConnectionType aType     //!< Requested connection type.
            )
        {
            if( !aContext )
            {
                return {};
            }

            std::weak_ptr<int> weakLife = aContext->objectLife();
            std::shared_ptr<Affinity> ctxAffinity = aContext->mAffinity;
            std::shared_ptr<Cleanup> cleanup = std::make_shared<Cleanup>( aContext, weakLife );

            // Generic in its arguments so one wrapper serves every signal signature. Taking them by
            // forwarding reference rather than by the signal's declared value types also stops a
            // by-value signal argument being reconstructed at the wrapper boundary before anything
            // has even decided whether the call is inline.
            auto wrapper = [weakLife, aContext, aSlot, aType, ctxAffinity, cleanup]
                ( auto&&... aArgs )
                {
                    if( aType == ConnectionType::Direct )
                    {
                        // Always synchronous in the emitting thread, whatever the affinity is --
                        // Qt::DirectConnection ignores thread affinity too.
                        aSlot( aArgs ... );
                        return;
                    }

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
                        aSlot( aArgs ... );
                        return;
                    }

                    // Queued: the arguments have to outlive this call, so copy them once into a
                    // tuple the closure owns. Re-check the life token when it finally runs, since
                    // it was only checked at emit time and the receiver may be destroyed before the
                    // loop reaches it.
                    //
                    // Held in the closure itself, not boxed behind a make_shared tuple the way
                    // QtMimic does it. The shared_ptr costs a second heap allocation on each queued
                    // emit and buys nothing here: dispatchMetaCallTo() takes the std::function by
                    // value and moves it into the MetaCallEvent, so the tuple is built once and
                    // never copied. Measured at -O2: 3.90 -> 2.86 allocations and ~1030 -> ~850 ns
                    // per queued emit.
                    dispatchMetaCallTo( ctxData, aContext,
                        [weakLife, aSlot,
                        argTuple = std::make_tuple( std::forward<decltype( aArgs )>( aArgs )... )]()
                        {
                            if( !weakLife.expired() )
                            {
                                std::apply( aSlot, argTuple );
                            }
                        } );
                };

            ConnectionHandle handle = aSignal.connect( wrapper );

            if( !cleanup || !cleanup->mOwner )
            {
                return handle;
            }

            // Publish the handle into the Cleanup before registering it, so that if the connection is
            // torn down concurrently the destructor below has something to match on rather than the
            // default-constructed handle.
            cleanup->mHandle = handle;

            Object* receiver = cleanup->mOwner;
            std::lock_guard<std::mutex> lock( receiver->mIncomingMutex );
            receiver->mIncoming.push_back( handle );
            return handle;
        }

        //! Grants the event queue access to event(), which it alone invokes.
        friend class EventDispatcherDefault;

        //! Grants the callLater pending-call registry (defined in Object.cpp) the ability to
        //! name the private CallLaterKey/CallLaterKeyHash types its map is keyed on.
        friend struct CallLaterRegistry;

        //! Grants Thread access to dispatchMetaCall(), which Thread::post() uses to queue an
        //! arbitrary task onto itself.
        friend class Thread;

        std::shared_ptr<int> mLife;                          //!< Lifetime token; reset in ~Object() so weak references expire.
        const std::shared_ptr<Affinity> mAffinity;           //!< Thread affinity box; the box itself is never reassigned, only its contents (see moveToThread()).
        std::atomic<bool> mDeleteLaterPosted { false };       //!< True once deleteLater() has posted a DeferredDeleteEvent; de-bounces repeat calls, matching QObject::deleteLaterCalled.
        std::string mObjectName;                              //!< This object's descriptive name.
        mutable std::mutex mNameMutex;                       //!< Guards mObjectName.
        std::vector<std::function<void()> > mCleanupCallbacks;  //!< Callbacks to run on destruction.
        std::mutex mCleanupMutex;                            //!< Guards mCleanupCallbacks.

        //! Connections where this object is the receiver, disconnected by ~Object().
        //!
        //! Without this a destroyed receiver's slot stays in the sender's slot list forever. The
        //! wrapper's life-token check makes it inert, but inert is not gone: it retains its
        //! captured state and is still walked on every emit, so one long-lived signal feeding
        //! many short-lived receivers grows without bound in both memory and emit cost. Qt does
        //! the equivalent by walking cd->senders in ~QObject().
        std::vector<ConnectionHandle> mIncoming;
        mutable std::mutex mIncomingMutex;                   //!< Guards mIncoming.

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
        //! object's own thread, but ~Object() may run elsewhere (it warns, but it still runs), so
        //! the list is not single-threaded in practice.
        mutable std::mutex mRunningTimerIdsMutex;
    };

    //! Connect Overload 1 definition. This is the primary overload for standard member functions.
    //! Because the target slot is not overloaded, the compiler can directly deduce the Slot type
    //! without needing explicit template resolution.
    template <typename Signal, typename Receiver, typename Slot>
    std::enable_if_t<MemberFunctionTraits<Slot>::is_member_function, ConnectionHandle>Object::
    connect
        (
        Signal& aSignal,          //!< The signal to connect.
        Receiver* aReceiver,      //!< The object receiving the signal (must derive from Object).
        Slot aSlot,                //!< The member function to call when the signal is emitted.
        ConnectionType aType   //!< The type of connection.
        )
    {
        using SlotClass = typename MemberFunctionTraits<Slot>::class_type;

        static_assert(
            std::is_base_of<Object, Receiver>::value, "Receiver must be an instance of Object." );
        static_assert( MemberFunctionTraits<Slot>::is_member_function,
            "Slot must be a member function pointer." );
        static_assert( std::is_base_of<SlotClass, Receiver>::value,
            "Slot must be a member function of Receiver or one of its base classes." );

        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 2 definition. If the target slot is overloaded, the compiler cannot deduce
    //! Slot in Overload 1. When the overloaded slot is defined in a base class of the receiver,
    //! type deduction fails. This overload explicitly resolves the base class pointer so you can
    //! connect inherited overloaded methods seamlessly. SignalArgs are the signal's parameter
    //! types, used to select the slot overload; Receiver must derive from Object; SlotClass is
    //! the base class owning the member function slot.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,           //!< The signal to connect.
        Receiver* aReceiver,                        //!< The object receiving the signal.
        void ( SlotClass::*aSlot )
        (
        SignalArgs...
        ),                                          //!< The member function pointer matching SignalArgs.
        ConnectionType aType                     //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 3 definition. Same as Overload 2, but specifically for const member
    //! functions; C++ requires separate template matching for const qualifiers on member
    //! function pointers.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        void ( SlotClass::*aSlot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 4 definition. If an overloaded inherited slot returns a value (e.g. bool),
    //! it won't match the void-returning Overloads 2 and 3. This overload explicitly catches
    //! non-void slots from base classes; the return value is safely discarded during emission.
    //! Ret is that discarded return type.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        Ret ( SlotClass::*aSlot )
        (
        SignalArgs...
        ),                                    //!< The member function pointer matching SignalArgs and returning Ret.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 5 definition. Same as Overload 4, but specifically for const member
    //! functions.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        Ret ( SlotClass::*aSlot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs and returning Ret.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 6 definition. If the target slot is overloaded (e.g. onEvent() and
    //! onEvent(int)), the compiler cannot deduce Slot in Overload 1. Using NonDeduced<Receiver>
    //! forces the compiler to use SignalArgs from the signal to perfectly select the right
    //! overload pointer. SignalArgs is used both to deduce and to select the slot overload.
    template <typename ... SignalArgs, typename Receiver>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value, ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        void ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ),  //!< The member function pointer matching SignalArgs.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 7 definition. Same as Overload 6, but specifically matches const member
    //! functions.
    template <typename ... SignalArgs, typename Receiver>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value, ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        void ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 8 definition. If an overloaded slot returns a value (e.g. bool), it won't
    //! match the void-returning Overload 6. This overload ensures connecting an overloaded method
    //! that returns Ret compiles successfully.
    template <typename ... SignalArgs, typename Receiver, typename Ret>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value && !std::is_same<Ret, void>::value,
        ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ),  //!< The member function pointer matching SignalArgs and returning Ret.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 9 definition. Same as Overload 8, but specifically for const member
    //! functions.
    template <typename ... SignalArgs, typename Receiver, typename Ret>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value && !std::is_same<Ret, void>::value,
        ConnectionHandle>Object::connect
        (
        Signal<SignalArgs...>& aSignal,     //!< The signal to connect.
        Receiver* aReceiver,                  //!< The object receiving the signal.
        Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs and returning Ret.
        ConnectionType aType               //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aReceiver,
            [aReceiver, aSlot]( auto&&... aCallArgs )
            {
                ( aReceiver->*aSlot )( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Connect Overload 10 definition. Captures anything that is not a member function: free
    //! functions, lambdas, or general functors. Binds the functor's lifetime and thread affinity
    //! to the provided context object.
    template <typename Signal, typename Functor>
    std::enable_if_t<!MemberFunctionTraits<Functor>::is_member_function, ConnectionHandle>Object::
    connect
        (
        Signal& aSignal,           //!< The signal to connect.
        Object* aContext,         //!< The Object context defining thread affinity and lifetime.
        Functor aSlot,             //!< The slot functor (lambda, std::function, etc.).
        ConnectionType aType    //!< The type of connection.
        )
    {
        return connectImpl( aSignal, aContext,
            [aSlot]( auto&&... aCallArgs )
            {
                aSlot( std::forward<decltype( aCallArgs )>( aCallArgs )... );
            },
            aType );
    }

    //! Disconnects a signal connection using a connection handle. Thread-safe.
    inline void Object::disconnect
        (
        const ConnectionHandle& aHandle  //!< The handle to disconnect.
        )
    {
        aHandle.disconnect();
    }

    //! CallLater Overload 1 definition. This is the primary overload for standard member
    //! functions. Because the target slot is not overloaded, the compiler can directly deduce the
    //! Slot type.
    template <typename Receiver, typename Slot, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        MemberFunctionTraits<Slot>::is_member_function,
        void>Object::callLater
        (
        Receiver* aReceiver,  //!< Target object receiving the call.
        Slot aSlot,            //!< Member function pointer.
        Args&&... aArgs        //!< Arguments passed to slot.
        )
    {
        using SlotClass = typename MemberFunctionTraits<Slot>::class_type;

        static_assert(
            std::is_base_of<Object, Receiver>::value, "Receiver must be an instance of Object." );
        static_assert( MemberFunctionTraits<Slot>::is_member_function,
            "Slot must be a member function pointer." );
        static_assert( std::is_base_of<SlotClass, Receiver>::value,
            "Slot must be a member function of Receiver or one of its base classes." );
        static_assert( std::is_invocable_v<Slot, Receiver*, Args...>,
            "Arguments do not match the parameters of the member function." );

        if( !aReceiver )
        {
            return;
        }

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( Slot ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 2 definition. If the target slot is overloaded and inherited from a base
    //! class, type deduction fails. This overload explicitly resolves the base class pointer so
    //! you can defer execution of inherited overloaded methods.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( void ( SlotClass::* )( Args... ) ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 3 definition. Same as Overload 2, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( void ( SlotClass::* )( Args... ) const ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 4 definition. If an overloaded inherited slot returns a value, it won't
    //! match the void-returning overloads. This overload explicitly catches non-void slots from
    //! base classes; the return value is safely discarded upon invocation.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( Ret ( SlotClass::* )( Args... ) ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 5 definition. Same as Overload 4, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( Ret ( SlotClass::* )( Args... ) const ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 6 definition. If the target slot is overloaded, the compiler cannot
    //! deduce Slot in Overload 1. Using NonDeduced<Receiver>, this overload forces the compiler
    //! to use the passed args types to select the right overload.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value, void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( void ( Receiver::* )( Args... ) ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 7 definition. Same as Overload 6, but specifically for const member
    //! functions.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value, void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( void ( Receiver::* )( Args... ) const ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 8 definition. If an overloaded slot returns a value, it won't match the
    //! void-returning Overload 6. This ensures deferring overloaded methods that return Ret
    //! compiles successfully.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value &&
        !std::is_same<Ret, void>::value,
        void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( Ret ( Receiver::* )( Args... ) ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
    }

    //! CallLater Overload 9 definition. Same as Overload 8, but specifically for const member
    //! functions.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<Object, Receiver>::value && !std::is_same<Ret, void>::value,
        void>Object::callLater
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

        CallLaterKey key;
        key.mContext = aReceiver;
        key.mTypeHash = typeid( Ret ( Receiver::* )( Args... ) const ).hash_code();
        key.mTargetSize = sizeof( aSlot );
        static_assert( sizeof( aSlot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aSlot, sizeof( aSlot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aReceiver, aSlot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aReceiver, aSlot]( auto&&... a )
                    {
                        ( aReceiver->*aSlot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aReceiver, key, invoker );
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

        CallLaterKey key;
        key.mContext = aContext;
        key.mTypeHash = typeid( Func ).hash_code();
        key.mTargetSize = sizeof( aFunc );
        static_assert( sizeof( aFunc ) <= 32, "Function pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &aFunc, sizeof( aFunc ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [aFunc, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [aFunc]( auto&&... a )
                    {
                        (*aFunc )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aContext, key, invoker );
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

        CallLaterKey key;
        key.mContext = aContext;
        key.mTypeHash = typeid( Signal<SignalArgs...> ).hash_code();
        key.mTargetSize = sizeof( sigPtr );
        static_assert( sizeof( sigPtr ) <= 32, "Signal pointer exceeds key size limit." );
        std::memcpy( key.mTargetBytes.data(), &sigPtr, sizeof( sigPtr ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( aArgs )... );
        auto invoker = [sigPtr, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [sigPtr]( auto&&... a )
                    {
                        sigPtr->emit( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( aContext, key, invoker );
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
}

#endif // OBJECT_H
