#ifndef GLOBAL_H
#define GLOBAL_H

#include <memory>
#include <type_traits>
#include <boost/signals2.hpp>

namespace QtLikeSignal
{
    //! Specifies the type of a signal-slot connection.
    //!
    //! The enumerators are not suffixed with "Connection" the way Qt's are: this is a scoped enum,
    //! so every use already reads ConnectionType::Direct and the suffix only repeated the type
    //! name. Qt::DirectConnection predates scoped enums and had no such luxury.
    enum class ConnectionType
    {
        //! Automatically determines the connection type based on thread affinity.
        //!
        //! If the receiver lives in the thread that emits the signal, Direct is used. Otherwise,
        //! Queued is used.
        Auto,

        //! The slot is invoked immediately when the signal is emitted.
        //!
        //! The slot is executed in the signaling thread.
        Direct,

        //! The slot is invoked when control returns to the event loop of the receiver's thread.
        //!
        //! The slot is executed in the receiver's thread.
        Queued
    };

    //! A handle representing a signal-slot connection.
    using Connection = boost::signals2::connection;

    //! Selects a non-const overload of a member function by its argument types.
    //!
    //! `&Receiver::onEvent` is ambiguous when onEvent is overloaded. `nonConstOverload<int>( ... )`
    //! names which one is meant. Mirrors Qt's qNonConstOverload.
    template <typename ... Args>
    struct NonConstOverload
    {
        //! Returns @p aPtr, having forced it to resolve to the Args... overload.
        template <typename R, typename T>
        constexpr auto operator()
            (
            R ( T::* aPtr )( Args... )
            ) const noexcept -> decltype( aPtr )
        {
            return aPtr;
        }
    };

    //! Selects a const overload of a member function by its argument types. Mirrors qConstOverload.
    template <typename ... Args>
    struct ConstOverload
    {
        //! Returns @p aPtr, having forced it to resolve to the const Args... overload.
        template <typename R, typename T>
        constexpr auto operator()
            (
            R ( T::* aPtr )( Args... ) const
            ) const noexcept -> decltype( aPtr )
        {
            return aPtr;
        }
    };

    //! Selects either overload, const or not, plus free functions. Mirrors Qt's qOverload.
    template <typename ... Args>
    struct Overload : ConstOverload<Args...>, NonConstOverload<Args...>
    {
        using ConstOverload<Args...>::operator();
        using NonConstOverload<Args...>::operator();

        //! Returns @p aPtr, having forced it to resolve to the Args... free-function overload.
        template <typename R>
        constexpr auto operator()
            (
            R ( * aPtr )( Args... )
            ) const noexcept -> decltype( aPtr )
        {
            return aPtr;
        }
    };

    template <typename ... Args> constexpr Overload<Args...> overload = {};
    template <typename ... Args> constexpr ConstOverload<Args...> constOverload = {};
    template <typename ... Args> constexpr NonConstOverload<Args...> nonConstOverload = {};

    //! Helper identity struct establishing a non-deduced context for the wrapped type T.
    template<typename T>
    struct Identity
    {
        //! Wrapped type alias.
        using type = T;
    };

    //! Type alias establishing a non-deduced context in template argument deduction for T.
    template<typename T>
    using NonDeduced = typename Identity<T>::type;

    //! Type trait detecting Signal instances. False for every T except a Signal<Args...>
    //! specialization, which the Signal.h header specializes to true.
    template<typename T>
    struct IsSignal : std::false_type
    {
    };

    //! Type traits for inspecting member function pointers. This primary template is the
    //! fallback for anything that is not a pointer to member function; the specializations
    //! below cover every const/volatile/noexcept combination such a pointer can have.
    template<typename T>
    struct MemberFunctionTraits
    {
        //! True if T is a member function pointer.
        static constexpr bool is_member_function = false;
        //! The class type of the member function pointer (void for non-member functions).
        using class_type = void;
    };

    //! Specialization of MemberFunctionTraits for plain (non-const, non-volatile) member
    //! function pointers.
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        )>
    {
        //! True; this specialization matches a member function pointer.
        static constexpr bool is_member_function = true;
        //! The class type containing the member function.
        using class_type = C;
        //! The return type of the member function.
        using return_type = R;
    };

    //! Specialization of MemberFunctionTraits for const member function pointers.
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        ) const>
    {
        //! True; this specialization matches a member function pointer.
        static constexpr bool is_member_function = true;
        //! The class type containing the member function.
        using class_type = C;
        //! The return type of the member function.
        using return_type = R;
    };

    //! Specialization of MemberFunctionTraits for volatile member function pointers.
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        ) volatile>
    {
        //! True; this specialization matches a member function pointer.
        static constexpr bool is_member_function = true;
        //! The class type containing the member function.
        using class_type = C;
        //! The return type of the member function.
        using return_type = R;
    };

    //! Specialization of MemberFunctionTraits for const volatile member function pointers.
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        ) const volatile>
    {
        //! True; this specialization matches a member function pointer.
        static constexpr bool is_member_function = true;
        //! The class type containing the member function.
        using class_type = C;
        //! The return type of the member function.
        using return_type = R;
    };

    #if __cplusplus >= 201703L || ( defined( _MSVC_LANG ) && _MSVC_LANG >= 201703L )
        //! Specialization of MemberFunctionTraits for noexcept member function pointers.
        //! Only available from C++17 onward, when noexcept became part of a function's type.
        template<typename C, typename R, typename ... Args>
        struct MemberFunctionTraits<R ( C::* )
            (
            Args...
            ) noexcept>
        {
            //! True; this specialization matches a member function pointer.
            static constexpr bool is_member_function = true;
            //! The class type containing the member function.
            using class_type = C;
            //! The return type of the member function.
            using return_type = R;
        };

        //! Specialization of MemberFunctionTraits for const noexcept member function pointers.
        //! Only available from C++17 onward, when noexcept became part of a function's type.
        template<typename C, typename R, typename ... Args>
        struct MemberFunctionTraits<R ( C::* )
            (
            Args...
            ) const noexcept>
        {
            //! True; this specialization matches a member function pointer.
            static constexpr bool is_member_function = true;
            //! The class type containing the member function.
            using class_type = C;
            //! The return type of the member function.
            using return_type = R;
        };
    #endif
}

#endif // GLOBAL_H
