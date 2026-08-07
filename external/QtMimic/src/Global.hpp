//! @file
//!
//! Type trait helpers and template utilities for QtMimic.
//!
//! This file provides:
//!   * Overload<>, ConstOverload<>, NonConstOverload<> — helpers for resolving
//!     overloaded member function pointers.
//!   * NonDeduced<> — identity wrapper to prevent template argument deduction,
//!     used to force the compiler to use explicit template parameters.
//!   * MemberFunctionTraits specializations — extract const/noexcept qualifiers
//!     from member function pointers.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_GLOBAL_HPP
#define QT_MIMIC_GLOBAL_HPP

namespace QtMimic
{
    template <typename ... Args>
    struct NonConstOverload
    {
        template <typename R, typename T>
        constexpr auto operator()
            (
            R ( T::* ptr )( Args... )
            ) const noexcept -> decltype( ptr )
        {
            return ptr;
        }

    };

    template <typename ... Args>
    struct ConstOverload
    {
        template <typename R, typename T>
        constexpr auto operator()
            (
            R ( T::* ptr )( Args... ) const
            ) const noexcept -> decltype( ptr )
        {
            return ptr;
        }

    };

    template <typename ... Args>
    struct Overload : ConstOverload<Args...>, NonConstOverload<Args...>
    {
        using ConstOverload<Args...>::operator();
        using NonConstOverload<Args...>::operator();

        template <typename R>
        constexpr auto operator()
            (
            R ( * ptr )( Args... )
            ) const noexcept -> decltype( ptr )
        {
            return ptr;
        }

        template <typename R>
        static constexpr auto of
            (
            R ( * ptr )( Args... )
            ) noexcept -> decltype( ptr )
        {
            return ptr;
        }

    };

    template <typename ... Args> constexpr Overload<Args...> overload = {};
    template <typename ... Args> constexpr ConstOverload<Args...> constOverload = {};
    template <typename ... Args> constexpr NonConstOverload<Args...> nonConstOverload = {};

    /**
     * @brief Helper identity struct for establishing a non-deduced context.
     * @tparam T The type to wrap.
     */
    template<typename T>
    struct Identity
    {
        /** @brief Wrapped type alias. */
        using type = T;
    };

    /**
     * @brief Type alias for establishing a non-deduced context in template argument deduction.
     * @tparam T Target type.
     */
    template<typename T>
    using NonDeduced = typename Identity<T>::type;

    /**
     * @brief Type traits for inspecting member function pointers.
     * @tparam T The type to inspect.
     */
    template<typename T>
    struct MemberFunctionTraits
    {
        /** @brief Flag indicating if T is a member function pointer. */
        static constexpr bool is_member = false;
        /** @brief The class type of the member function pointer (void for non-member functions). */
        using class_type = void;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for standard member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        )>
    {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for const member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        ) const>
    {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for volatile member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        ) volatile>
    {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for const volatile member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template<typename C, typename R, typename ... Args>
    struct MemberFunctionTraits<R ( C::* )
        (
        Args...
        ) const volatile>
    {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    #if __cplusplus >= 201703L || ( defined( _MSVC_LANG ) && _MSVC_LANG >= 201703L )
        /**
         * @brief Specialization of MemberFunctionTraits for noexcept member function pointers.
         * @tparam C The class type.
         * @tparam R The return type.
         * @tparam Args The argument types.
         */
        template<typename C, typename R, typename ... Args>
        struct MemberFunctionTraits<R ( C::* )
            (
            Args...
            ) noexcept>
        {
            /** @brief Flag indicating T is a member function pointer. */
            static constexpr bool is_member = true;
            /** @brief The class type containing the member function. */
            using class_type = C;
            /** @brief The return type of the member function. */
            using return_type = R;
        };

        /**
         * @brief Specialization of MemberFunctionTraits for const noexcept member function pointers.
         * @tparam C The class type.
         * @tparam R The return type.
         * @tparam Args The argument types.
         */
        template<typename C, typename R, typename ... Args>
        struct MemberFunctionTraits<R ( C::* )
            (
            Args...
            ) const noexcept>
        {
            /** @brief Flag indicating T is a member function pointer. */
            static constexpr bool is_member = true;
            /** @brief The class type containing the member function. */
            using class_type = C;
            /** @brief The return type of the member function. */
            using return_type = R;
        };
    #endif

} // namespace QtMimic

#endif // QT_MIMIC_GLOBAL_HPP
