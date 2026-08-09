#ifndef QT_LIKE_SIGNAL_GLOBAL_H
#define QT_LIKE_SIGNAL_GLOBAL_H

#include <memory>
#include <type_traits>
#include <boost/signals2.hpp>

namespace QtLikeSignal
{
    //! Specifies the type of a signal-slot connection.
    enum class ConnectionType
    {
        //! Automatically determines the connection type based on thread affinity.
        //!
        //! If the receiver lives in the thread that emits the signal, DirectConnection is used.
        //! Otherwise, QueuedConnection is used.
        AutoConnection,

        //! The slot is invoked immediately when the signal is emitted.
        //!
        //! The slot is executed in the signaling thread.
        DirectConnection,

        //! The slot is invoked when control returns to the event loop of the receiver's thread.
        //!
        //! The slot is executed in the receiver's thread.
        QueuedConnection
    };

    //! A handle representing a signal-slot connection.
    using ConnectionHandle = boost::signals2::connection;

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

#endif // QT_LIKE_SIGNAL_GLOBAL_H
