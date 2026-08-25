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
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#if defined( _MSC_VER )
    // For Object::Lock. See its declaration for why only this toolchain wants it.
    #include <shared_mutex>
#endif
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
        //! Constructs an object, optionally giving it a parent that will own it.
        //!
        //! QObject's signature, and the same meaning: the parent destroys this object, so a subtree
        //! can be built as it is declared.
        //!
        //! The object lives in the thread that constructs it. A parent in *another* thread is
        //! refused -- with a warning, leaving this object parentless -- because a child lives in
        //! its parent's thread and nothing here may silently move an object somewhere the caller
        //! did not ask for. Qt refuses it identically, in check_parent_thread().
        //!
        //! This took a `Thread*` until 2026-08-21, which let an object be built directly on another
        //! thread's affinity. Nothing needed it: building on the target thread already gives the
        //! right affinity, and building elsewhere is `Object o; o.moveToThread( &t );`, which is a
        //! push and therefore allowed. The parent form is worth more, and the two cannot coexist --
        //! `Object obj( nullptr )` would be ambiguous between them.
        //!
        //! **On the stack, prefer this to setParent().** Not a style point: this form requires the
        //! parent to already exist, so in a block the parent is declared first and therefore
        //! destroyed last, and the child unlinks itself on the way out. setParent() lets the
        //! declarations go the other way round, and then the parent calls `delete` on automatic
        //! storage. See the README's parent-child section.
        //!
        //! For a heap child, createChild<T>() remains the recommended form: it allocates, so the
        //! storage duration a parent requires is right by construction rather than by care.
        explicit Object
            (
            Object* aParent = nullptr
            );

        //! Passing a Thread* is a compile error, not a parent.
        //!
        //! Thread derives from Object, so `Object( &worker )` -- which meant "live in that thread"
        //! until this constructor changed -- would otherwise still compile and quietly mean
        //! "be a child of that Thread object" instead. It is refused as cross-thread at runtime, so
        //! the object ends up parentless in whatever thread built it, and the only clue is a line
        //! on stderr. That is precisely the migration this deletion exists to catch: eleven call
        //! sites in this repo compiled unchanged and failed at runtime before it was added.
        //!
        //! Say what you mean instead:
        //!
        //! ```
        //! Object object;
        //! object.moveToThread( &worker );                  // live in that thread
        //! Object child( static_cast<Object*>( &worker ) ); // really parent it to the Thread
        //! ```
        explicit Object
            (
            Thread* aThread
            ) = delete;

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

        //! Gets this object's parent, or nullptr when it has none.
        //!
        //! **Not thread-safe: must be called from this object's own thread.** The tree is
        //! thread-confined rather than locked -- a child lives in its parent's thread, and only
        //! that thread may read or change either link. Qt guards its own parent/children the same
        //! way, with an invariant instead of a mutex; see mParent.
        Object* parent() const;

        //! Makes @p aParent this object's parent, or detaches it when @p aParent is nullptr.
        //!
        //! **Not thread-safe: must be called from this object's own thread.**
        //!
        //! @return true when the object now has the requested parent (including when it already
        //! did); false when the change was refused, in which case the existing parent is
        //! **unchanged**.
        //!
        //! Refusing without changing anything is a deliberate departure from Qt, which on a
        //! refused re-parent leaves the object with *no* parent at all (`qobject.cpp:2341`) --
        //! neither keeping the old one nor failing loudly, so a caller not watching stderr silently
        //! leaks. Two cases are refused here:
        //!
        //!   - @p aParent is this object or one of its own descendants, which would make a cycle.
        //!     Qt detects this only in debug builds, only past depth 4096, and only as a warning
        //!     (`CheckForParentChildLoopsWarnDepth`); the walk is O(depth) on a cold path, so we
        //!     do it in every build.
        //!   - @p aParent is already being destroyed. Its extras box is freed at the end of its
        //!     destructor, so attaching to it would leave this object linked into storage that is
        //!     about to go away.
        //!
        //! See ForAI/mission-parent-child-relationship.md.
        bool setParent
            (
            Object* aParent
            );

        //! Gets the first of this object's children, or nullptr when it has none. Iterate with
        //! nextSibling():
        //!
        //! ```
        //! for( Object* child = parent->firstChild(); child; child = child->nextSibling() )
        //! ```
        //!
        //! **Not thread-safe**, as parent().
        //!
        //! Two accessors rather than a `children()` container, which is what QObject returns
        //! (`const QObjectList&`). The list is intrusive -- threaded through the children
        //! themselves -- so there is no container here to hand back, and building a std::vector per
        //! call would allocate on every traversal and give back exactly the cost the intrusive form
        //! exists to avoid. See mFirstChild.
        Object* firstChild() const;

        //! Gets the next child of this object's parent, or nullptr when this is the last one.
        //!
        //! **Not thread-safe**, as parent(). Order is unspecified and callers must not rely on it;
        //! it is currently most-recently-attached first, because attaching at the head is what
        //! makes it O(1).
        Object* nextSibling() const;

        //! Number of direct children of this object.
        //!
        //! **Not thread-safe**, as parent(). O(children): the count is walked rather than cached,
        //! because nothing but diagnostics asks for it, and a cached count would be a fourth thing
        //! for every attach and detach to keep right. Contrast incomingConnectionCount(), which is
        //! O(1) because emit walks that list.
        std::size_t childCount() const;

        //! Creates a @p Child on the heap and gives it to @p aParent, which owns it from here.
        //!
        //! The recommended way to build a subtree. `aParent` destroys what this returns, so the
        //! caller neither has to nor may delete it:
        //!
        //! ```
        //! auto* button = Object::createChild<Button>( window, "OK" );
        //! ```
        //!
        //! @return the new child, or nullptr when @p aParent is null or refused it. The refusal
        //! cases are setParent()'s, and none of them is reachable for a freshly built object except
        //! a parent that is already being destroyed -- a brand new object cannot close a cycle. The
        //! result is therefore **not** `[[nodiscard]]`: discarding it is the normal fire-and-forget
        //! idiom and loses nothing, because the parent is what owns the child. That is the opposite
        //! of Thread::post(), where a discarded false is work that silently never runs.
        //!
        //! Object *does* also take a parent at construction now -- `Object( Object* aParent )`, as
        //! QObject spells it -- so this is no longer the only way to attach as you build. It
        //! remains the recommended one for a heap child, because of the next paragraph.
        //!
        //! It is the only leverage available on the storage-duration problem
        //! Object::deleteChildren() describes: a parent deletes its children, so a child has to be
        //! on the heap, and standard C++ cannot check that. This allocates, so a subtree built
        //! through it is right by construction. `setParent()` remains for re-homing an object that
        //! already exists, and cannot make the same guarantee.
        //!
        //! **Exception safety.** If @p Child's constructor throws, its Object base has already been
        //! constructed, so ~Object() runs and destroys anything the constructor had already
        //! attached to it. The exception propagates and nothing leaks. Attach-as-you-build is
        //! therefore safe here, exactly as it is in Qt.
        template <typename Child, typename ... Args>
        static Child* createChild
            (
            Object* aParent,   //!< Parent that will own the new child; must not be null.
            Args&&... aArgs    //!< Forwarded to Child's constructor.
            );

        //! Finds the first descendant of type @p T, optionally matching @p aName.
        //!
        //! Searches the whole subtree, not just the immediate children, depth-first. An empty name
        //! -- the default -- matches any name, exactly as QObject::findChild() treats a null
        //! QString.
        //!
        //! **Not thread-safe**, as parent(): the tree is thread-confined.
        //!
        //! @return the first match, or nullptr when there is none.
        //!
        //! O(subtree) per call, with a dynamic_cast per node, and no index anywhere. Qt's
        //! qt_qFindChildren_helper is the same plain recursive scan. That is fine for a dialog and
        //! wrong for a large model, so treat this as a diagnostic and wiring convenience rather
        //! than something to call in a loop.
        //!
        //! Uses dynamic_cast because there is no moc here and therefore no qobject_cast. RTTI has to
        //! stay enabled; neither toolchain disables it, and this is the first thing that would break
        //! if one did.
        template <typename T>
        T* findChild
            (
            const std::string& aName = std::string()   //!< Name to match, or empty for any.
            ) const;

        //! Finds every descendant of type @p T, optionally matching @p aName.
        //!
        //! As findChild(), but collects all matches rather than stopping at the first.
        //!
        //! Returns a vector, which allocates -- and that is deliberately inconsistent with
        //! firstChild()/nextSibling(), which exist precisely to avoid building a container per
        //! call. The inconsistency is accepted rather than hidden: iteration is the answer for the
        //! traversal a program does constantly, and a reflection query that already costs a
        //! dynamic_cast per node is not that.
        template <typename T>
        std::vector<T*> findChildren
            (
            const std::string& aName = std::string()   //!< Name to match, or empty for any.
            ) const;

        //! Writes this object's subtree to stderr, one line per node, indented by depth.
        //!
        //! A debugging aid, like QObject::dumpObjectTree(). Names come from objectName(); an
        //! unnamed object prints as its address alone.
        void dumpObjectTree() const;

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
            std::lock_guard<Lock> lock( mIncomingMutex );
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
            SignalArgs ...
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
            SignalArgs ...
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
            void ( NonDeduced<Receiver>::*aSlot )( SignalArgs ... ),
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
            void ( NonDeduced<Receiver>::*aSlot )( SignalArgs ... ) const,
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
            Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs ... ),
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
            Ret ( NonDeduced<Receiver>::*aSlot )( SignalArgs ... ) const,
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
            ThreadData* aThreadData
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
            ThreadData* aData
            );

        bool forgetTimerId
            (
            int aTimerId
            );

        ThreadData* threadData() const;

        //! Carries this object's already-posted events across in moveToThread(). See the definition.
        void migratePostedEvents
            (
            ThreadData* aOldData,
            ThreadData* aNewData
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
            ThreadData* aData,
            Object* aReceiver,
            std::function<void()> aSlot
            );

        //! The one body shared by all ten connect() overloads.
        //!
        //! The overloads above differ only in what the compiler needs in order to *name* the slot:
        //! whether it is overloaded, inherited, const, or returns a value. None of them differs in
        //! what the resulting connection does. So each one binds the receiver and the slot into a
        //! small adapter and hands it here, exactly as QtLikeSignal's overloads hand theirs to its
        //! connectImpl(); everything that is actually a connection -- the life token, the affinity
        //! box, the emit-time wrapper and the incoming-connection bookkeeping -- is written once,
        //! here.
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
                    ThreadData* const ctxData = ctxAffinity ? ctxAffinity->data() : nullptr;

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
        //! The four per-object flags, packed into one byte.
        //!
        //! Each was a std::atomic<bool> of its own. Four bytes of payload, but they sat between
        //! two 8-aligned members and cost eight; packed, and with mIncomingCount narrowed to fill
        //! the hole they leave, sizeof(Object) drops from 96 to 88. Qt packs twelve flags into one
        //! 32-bit word in QObjectData for the same reason.
        //!
        //! All four are set-once and never cleared, which is what makes the packing safe: two
        //! threads setting different bits cannot lose each other's write, because fetch_or is a
        //! read-modify-write rather than a store.
        enum Flag : std::uint8_t
        {
            //! Set once deleteLater() has posted a DeferredDeleteEvent; de-bounces repeat calls,
            //! matching QObject::deleteLaterCalled.
            kDeleteLaterPosted = 1u << 0,

            //! Set once this object has been the context of a callLater(), so ~Object() knows
            //! whether the process-wide pending registry can possibly hold anything of ours.
            kUsedCallLater = 1u << 1,

            //! Set once an event has been posted for this object, so ~Object() knows whether the
            //! dispatcher's queue can possibly hold anything of ours.
            //!
            //! These flags exist because the scans they guard are O(backlog) and were run on every
            //! destruction, including for the objects -- most of them -- that never used the
            //! feature. Qt guards the same call the same way: `if (d->postedEvents)` in ~QObject().
            kMayHaveQueuedWork = 1u << 2,

            //! Set once this object has started a timer, so ~Object() knows whether the extras box
            //! can possibly hold a timer id. See Extras.
            kUsedTimers = 1u << 3,
        };

        std::atomic<std::uint8_t> mFlags { 0 };

        //! How many nodes mIncomingHead's list holds, so the count stays O(1) rather than a walk.
        //!
        //! Declared here, right after mFlags, on purpose: the flag byte leaves three bytes of
        //! padding before the next 8-aligned member, and a 32-bit count fits in it for free. As a
        //! std::size_t further down it cost a whole word. Four billion incoming connections on one
        //! object is not a limit anybody will meet.
        std::uint32_t mIncomingCount { 0 };

        //! @return true when @p aBit is set. Thread-safe.
        bool hasFlag
            (
            std::uint8_t aBit   //!< The Flag to test.
            ) const
        {
            return ( mFlags.load( std::memory_order_acquire ) & aBit ) != 0;
        }

        //! Sets @p aBit, if it is not set already. Thread-safe.
        //!
        //! The load before the fetch_or is not an optimisation for its own sake. kMayHaveQueuedWork
        //! is set on *every* queued post, where a release store compiles to a plain move on x86 and
        //! an unconditional fetch_or would compile to a locked read-modify-write -- a regression on
        //! the hot queued path in exchange for the eight bytes this packing saves. Reading first
        //! keeps that path a plain load once the bit is set, which it is after the first post.
        //!
        //! Relaxed on that first load, and that is safe rather than merely cheap: reading a stale
        //! zero only costs a redundant fetch_or, which is itself release-ordered, and reading a one
        //! means some thread already published the bit with release. Either way a later
        //! hasFlag() acquire-load sees a correctly ordered value.
        void setFlag
            (
            std::uint8_t aBit   //!< The Flag to set.
            )
        {
            if( ( mFlags.load( std::memory_order_relaxed ) & aBit ) == 0 )
            {
                mFlags.fetch_or( aBit, std::memory_order_release );
            }
        }

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

        //! The lock guarding this object's incoming-connection list.
        //!
        //! Two standard types, picked per toolchain on size alone. MSVC's std::mutex is **80
        //! bytes** and its std::shared_mutex is **8**, because the latter is a bare SRWLOCK while
        //! the former carries an ABI-frozen structure supporting timed and recursive locking that
        //! nothing here asks for. libstdc++ is the other way round -- std::mutex 40,
        //! std::shared_mutex 56, a pthread_rwlock_t -- so there it stays std::mutex. Measured, not
        //! assumed; see src/OBJECT-SIZE-REPORT.md.
        //!
        //! Only the exclusive half of the interface is ever used -- lock(), try_lock(), unlock() --
        //! which both types provide with identical semantics under std::lock_guard. Nothing takes a
        //! shared lock, and nothing should start taking one without measuring first: a reader-writer
        //! lock is slower than a plain mutex when every user is a writer.
        //!
        //! **An alias rather than a bare type, deliberately.** Qt's QBasicMutex is 8 bytes
        //! everywhere -- one tagged QBasicAtomicPointer -- and a hand-written equivalent would take
        //! Linux from 40 to 8 as well. It is not written because a custom lock synchronising with
        //! raw futex(2) is **invisible to ThreadSanitizer**, which is precisely why
        //! src/perf/tsan-suppressions.txt exists for Qt: nineteen false positives, none of
        //! them a real defect. Doing it here would mean writing the lock *and* its
        //! __tsan_mutex_pre_lock annotations, and an annotation that is subtly wrong hides real
        //! races rather than merely reporting fake ones. This alias keeps that a one-line change if
        //! the bytes ever justify the risk. See ForAI/mission-object-size.md.
        #if defined( _MSC_VER )
            using Lock = std::shared_mutex;
        #else
            using Lock = std::mutex;
        #endif

        //! Guards mIncomingHead, mIncomingCount, and every node's incoming links.
        mutable Lock mIncomingMutex;

        //! Per-object state that almost no object actually carries, behind one pointer.
        //!
        //! Most objects are never named and never own a timer. Inline, these members cost every
        //! Object 96 bytes -- a std::string (32), a std::vector (24) and a std::mutex (40) -- used
        //! or not, which was 52% of sizeof(Object). Behind a pointer they cost 8, and nothing on
        //! the heap at all until one of them is first needed.
        //!
        //! The box has since shrunk twice more, and for a reason that applies to anything added
        //! here: **every byte in it is paid by every tree node**, because the parent-child links
        //! live here too. The timer mutex was deleted outright (its touchers are all
        //! thread-confined), and the name and the timer list went behind pointers of their own, so
        //! the box is 48 bytes rather than 128. Hold a new member by pointer unless a bare tree
        //! node genuinely needs it.
        //!
        //! Qt does exactly this with QObjectPrivate::extraData, which holds objectName,
        //! runningTimers, the dynamic properties and the event-filter list for the same reason.
        struct Extras
        {
            //! This object's descriptive name, or null while it has none.
            //!
            //! Deliberately unguarded, matching QObject, whose objectName() has no locking either.
            //! A mutex here would be paid for by every named Object in the program to make one
            //! accessor safe against a use the thread-affinity rules already forbid: an Object
            //! belongs to one thread, and naming it from another is the same misuse as calling any
            //! of its other setters from there. Use the object from the thread it lives in.
            //!
            //! Held behind a pointer rather than by value because this box is what every *tree*
            //! node allocates, and a tree node is usually not named. A std::string is 32 bytes
            //! here (40 on MSVC) whether or not anything is in it; a pointer is 8, and the string
            //! itself is allocated only by the objects that actually take a name. QString is one
            //! pointer for the same reason, which is why Qt's ExtraData can afford to hold one by
            //! value.
            //!
            //! The trade, stated: a named object now pays one more allocation than it did. That is
            //! the right way round -- naming is a diagnostic convenience, and being in a tree is
            //! not.
            std::unique_ptr<std::string> mObjectName;

            //! Timer ids started on this object and not yet killed.
            //!
            //! Exists so ~Object() can return them to the shared pool. Without it a destroyed
            //! object with a running timer would strand its id forever, and the pool would climb
            //! exactly as the old monotonic counter did. Qt keeps the same list in
            //! QObjectPrivate::extraData->runningTimers for the same reason.
            //!
            //! Unguarded, on the same rule as the four tree pointers below rather than by
            //! oversight. It has exactly three touchers -- startTimer(), killTimer() and
            //! ~Object() -- and all three are confined to the thread this object lives in.
            //! Destroying an object from another thread is what the mutex here used to defend
            //! against, and that is *already* diagnosed as misuse by ~Object()'s own
            //! namesOtherRunningThread() warning; 40 bytes on every boxed object (80 on MSVC) to
            //! make one already-reported bug marginally less bad was the wrong trade. Qt guards
            //! its equivalent list no more than this. See ForAI/mission-object-size.md.
            //!
            //! Behind a pointer for the same reason as mObjectName: 24 bytes of empty vector in
            //! every tree node, to serve the objects that run a timer, is the wrong way round.
            //! Null means no timer was ever started.
            std::unique_ptr<std::vector<int> > mRunningTimerIds;

            //! This object's parent, or null. See Object::parent().
            //!
            //! Unguarded, and that is the design rather than an omission: the tree is
            //! thread-confined, so the one thread allowed to touch these four pointers is the
            //! thread the parent and all its children live in. QObjectData::parent and
            //! QObjectData::children are unguarded for exactly the same reason -- Qt carries no
            //! per-object mutex at all. A std::mutex here would be 40 bytes to lock something no
            //! two threads may legally reach at once. See ForAI/mission-object-size.md.
            Object* mParent { nullptr };

            //! Head of this object's child list, or null when it has none.
            //!
            //! Intrusive, exactly like Object::mIncomingHead: the links live on the children (the
            //! two sibling pointers below) rather than in a container here, so attaching a child
            //! costs no allocation and *both* attaching and detaching are O(1).
            //!
            //! Qt keeps a QList<QObject*> instead, which is contiguous, so removing one child is
            //! an indexOf scan plus a removeAt shift -- O(siblings) each (`qobject.cpp:2315-2321`).
            //! Destroying N children of one parent individually is therefore quadratic in Qt. This
            //! is the one place the design here is structurally better rather than merely equal,
            //! and it is the same trick P10 already applied to the incoming list.
            Object* mFirstChild { nullptr };

            //! This object's place in its parent's child list; both null when it has no parent.
            //!
            //! Raw pointers, and owning in the opposite direction from mIncomingHead's: a parent
            //! owns its children, so it deletes them rather than merely unlinking them. A child
            //! never owns its parent.
            Object* mPrevSibling { nullptr };
            Object* mNextSibling { nullptr };
        };

        //! The box above, or null while this object has needed neither half of it. Owned; deleted
        //! by ~Object().
        //!
        //! Atomic and read with acquire for the same reason the set-once flags above are: the
        //! destructor reads it, and it is written by whichever call first needed it.
        std::atomic<Extras*> mExtras { nullptr };

        //! @return the extras box, creating it on first use. Never null.
        Extras& ensureExtras();

        //! @return the extras box, or null when this object has never needed one.
        //!
        //! The read half of ensureExtras(), for the callers that must not create a box just to
        //! discover it is not there. Every object that has a parent or a child has one by
        //! construction, so the tree code below can and does treat a null here as "not in a tree"
        //! rather than as a case to handle.
        Extras* extrasOrNull() const
        {
            return mExtras.load( std::memory_order_acquire );
        }

        //! Links this object into @p aParent's child list. Callers have already refused the cases
        //! setParent() refuses, and have already detached from any previous parent.
        void attachToParent
            (
            Object* aParent
            );

        //! Unlinks this object from its parent's child list, and does nothing when it has no
        //! parent. Mirrors Private::ConnectionNode::pruneReceiver().
        void detachFromParent();

        //! Destroys every child of this object. Called by ~Object().
        //!
        //! A child must therefore be heap-allocated; see the definition.
        void deleteChildren();

        //! Moves this object and everything under it to @p aThread. See moveToThread().
        void moveSubtreeToThread
            (
            Thread* aThread
            );

        //! Moves this one object to @p aThread, leaving its children alone. See moveToThread().
        void moveSelfToThread
            (
            Thread* aThread
            );

    };

    //! Creates a child and attaches it to its parent. See the declaration.
    template <typename Child, typename ... Args>
    Child* Object::createChild
        (
        Object* aParent,   //!< Parent that will own the new child; must not be null.
        Args&&... aArgs    //!< Forwarded to Child's constructor.
        )
    {
        static_assert( is_obj<Child>, "Child must be an instance of Object." );

        if( aParent == nullptr )
        {
            std::fprintf( stderr,
                "Object::createChild: no parent to attach to; nothing was created\n" );
            return nullptr;
        }

        // Held by a unique_ptr for the length of the attach, so a refusal frees the object instead
        // of leaking it. The attach is the only thing between the allocation and the caller taking
        // ownership of the result, and it is the only thing here that can fail.
        std::unique_ptr<Child> child( new Child( std::forward<Args>( aArgs )... ) );
        if( !child->setParent( aParent ) )
        {
            // setParent() has already said why on stderr.
            return nullptr;
        }

        // The parent owns it now, so this must stop owning it. Not a leak: ~Object() on the parent
        // is what frees it, which is the whole contract of the tree.
        return child.release();
    }

    //! Finds the first descendant of type T. See the declaration.
    template <typename T>
    T* Object::findChild
        (
        const std::string& aName   //!< Name to match, or empty for any.
        ) const
    {
        static_assert( is_obj<T>, "T must be an instance of Object." );

        for( Object* child = firstChild(); child != nullptr; child = child->nextSibling() )
        {
            // The cast first, then the name: dynamic_cast is the more selective of the two in
            // every use this is meant for, and objectName() copies a std::string.
            if( T* typed = dynamic_cast<T*>( child ) )
            {
                if( aName.empty() || child->objectName() == aName )
                {
                    return typed;
                }
            }

            // Depth-first, so the whole of one branch is searched before the next sibling. Qt
            // orders qt_qFindChildren_helper the same way; nothing should rely on it beyond that
            // an existing match is found.
            if( T* found = child->findChild<T>( aName ) )
            {
                return found;
            }
        }
        return nullptr;
    }

    //! Finds every descendant of type T. See the declaration.
    template <typename T>
    std::vector<T*> Object::findChildren
        (
        const std::string& aName   //!< Name to match, or empty for any.
        ) const
    {
        static_assert( is_obj<T>, "T must be an instance of Object." );

        std::vector<T*> found;
        for( Object* child = firstChild(); child != nullptr; child = child->nextSibling() )
        {
            if( T* typed = dynamic_cast<T*>( child ) )
            {
                if( aName.empty() || child->objectName() == aName )
                {
                    found.push_back( typed );
                }
            }

            const std::vector<T*> deeper = child->findChildren<T>( aName );
            found.insert( found.end(), deeper.begin(), deeper.end() );
        }
        return found;
    }

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
