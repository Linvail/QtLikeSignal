// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsValue - one value that a Settings object stores: a bool, an integer, a
//! double, a string, a list of strings, or nothing.

#ifndef QT_LIKE_SIGNAL_SETTINGS_VALUE_HPP
#define QT_LIKE_SIGNAL_SETTINGS_VALUE_HPP

#include "QtLikeSignal/SettingsType.hpp"

#include <climits>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace QtLikeSignal
{
    //! A value in a settings file. It takes the place of QVariant in QSettings.
    //!
    //! @code
    //!   QtLikeSignal::SettingsValue width( 800 );
    //!   bool ok = false;
    //!   const int w = width.toInt( &ok );          // 800, ok is true
    //!
    //!   QtLikeSignal::SettingsValue text( "800" );      // what a value read from a file looks
    //!   like const int t = text.to<int>( &ok );         // 800, ok is true
    //! @endcode
    //!
    //! **Six kinds of data, and no others.** They are the kinds that an INI file can give back; see
    //! SettingsType. There is no fallback for other types, because this library has no
    //! serialisation framework. Store a size as two integers, not as one object.
    //!
    //! **A value from a file is text.** The file does not record whether "800" was an integer or a
    //! string when it was written, so a value read back is a String. The conversions below read a
    //! String as well as they read the type that was set. This is also what QVariant does.
    //!
    //! **A conversion that fails says so.** Each conversion takes an optional `bool* aOk`, which is
    //! set to false when the value cannot be converted, and the result is then zero, false or
    //! empty. Settings::value<T>() uses this to return the caller's default instead.
    //!
    //! **Not a std::variant.** std::get throws, and this library must compile with exceptions
    //! disabled. The members are plain members, not a union, so the type costs about 90 bytes.
    //! That is acceptable for a type that a program makes a few times at startup.
    class SettingsValue
    {
    public:
        SettingsValue();

        SettingsValue
            (
            bool aValue
            );

        //! Constructs an Int from any integer type except bool. See the definition below.
        template <typename T, std::enable_if_t<std::is_integral<T>::value &&
            !std::is_same<T, bool>::value, int> = 0>
        SettingsValue
            (
            T aValue
            );

        //! Constructs a Double from any floating-point type. See the definition below.
        template <typename T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
        SettingsValue
            (
            T aValue
            );

        SettingsValue
            (
            const char* aValue
            );

        SettingsValue
            (
            std::string aValue
            );

        SettingsValue
            (
            std::vector<std::string> aValue
            );

        //! Returns the kind of data that the value holds.
        SettingsType type() const
        {
            return mType;
        }

        //! Returns true if the value holds data, that is, if type() is not SettingsType::Invalid.
        bool isValid() const
        {
            return mType != SettingsType::Invalid;
        }

        bool toBool
            (
            bool* aOk = nullptr
            ) const;

        int toInt
            (
            bool* aOk = nullptr
            ) const;

        long long toLongLong
            (
            bool* aOk = nullptr
            ) const;

        double toDouble
            (
            bool* aOk = nullptr
            ) const;

        std::string toString
            (
            bool* aOk = nullptr
            ) const;

        std::vector<std::string> toStringList
            (
            bool* aOk = nullptr
            ) const;

        //! Converts the value to T. See the definition below.
        template <typename T>
        T to
            (
            bool* aOk = nullptr
            ) const;

        bool operator==
            (
            const SettingsValue& aOther
            ) const;

        bool operator!=
            (
            const SettingsValue& aOther
            ) const;

    private:
        //! Always false. It lets the static_assert in to<T>() fire only when to<T>() is used with
        //! a type that it does not support, not every time the header is compiled.
        template <typename T>
        struct UnsupportedType : std::false_type
        {
        };

        static void reportUnsignedOutOfRange
            (
            unsigned long long aValue
            );

        //! The kind of data that the value holds. It tells which member below is in use.
        SettingsType mType { SettingsType::Invalid };

        //! The data when mType is Bool.
        bool mBool { false };

        //! The data when mType is Int.
        long long mInt { 0 };

        //! The data when mType is Double.
        double mDouble { 0.0 };

        //! The data when mType is String.
        std::string mString;

        //! The data when mType is StringList.
        std::vector<std::string> mStringList;
    };

    //! Constructs an Int that holds @p aValue.
    //!
    //! A template, so that every integer type is accepted without an ambiguity. Without it, an
    //! `unsigned int` or a `short` could convert to bool, to long long and to double equally well,
    //! and the call would not compile.
    //!
    //! @tparam T An integer type other than bool. bool has a constructor of its own. A `char` is
    //!         also an integer type, so `SettingsValue( 'x' )` holds the integer 120, not a string.
    //!
    //! An unsigned value above LLONG_MAX does not fit in the long long that holds the data. It is
    //! not wrapped to a negative number, because that would store a wrong value without a sign of
    //! it. The value is Invalid instead, and a warning goes to the "qtlikesignal.settings"
    //! category.
    template <typename T, std::enable_if_t<std::is_integral<T>::value &&
        !std::is_same<T, bool>::value, int> >
    SettingsValue::SettingsValue
        (
        T aValue  //!< The integer to hold.
        )
    {
        if constexpr( std::is_unsigned<T>::value && sizeof( T ) >= sizeof( long long ) )
        {
            if( aValue > static_cast<T>( LLONG_MAX ) )
            {
                reportUnsignedOutOfRange( aValue );
                return;
            }
        }
        mType = SettingsType::Int;
        mInt  = static_cast<long long>( aValue );
    }

    //! Constructs a Double that holds @p aValue.
    //!
    //! @tparam T float, double or long double. A long double is narrowed to double, which is what
    //!         the file can hold.
    template <typename T, std::enable_if_t<std::is_floating_point<T>::value, int> >
    SettingsValue::SettingsValue
        (
        T aValue  //!< The number to hold.
        )
        : mType( SettingsType::Double )
        , mDouble( static_cast<double>( aValue ) )
    {
    }

    //! Converts the value to T, with the conversions that the named functions above use.
    //!
    //! This is the function that Settings::value<T>() calls. It gives one name to the conversions,
    //! so that a template can use it without a switch on the type.
    //!
    //! @tparam T One of:
    //!         - bool: toBool().
    //!         - any other integer type: toLongLong(), then a range check. A value that does not
    //!           fit in T fails, and the result is 0. It is not truncated.
    //!         - float, double, long double: toDouble(), then a cast.
    //!         - std::string: toString().
    //!         - std::vector<std::string>: toStringList().
    //!         - SettingsValue: the value itself, and the conversion always succeeds.
    //!         Any other T does not compile, and the message names the rule.
    //!
    //! @return the converted value, or zero, false or empty if the conversion fails.
    template <typename T>
    T SettingsValue::to
        (
        bool* aOk  //!< Set to true if the conversion succeeded, false if not. Can be null.
        ) const
    {
        if constexpr( std::is_same<T, bool>::value )
        {
            return toBool( aOk );
        }
        else if constexpr( std::is_integral<T>::value )
        {
            bool ok = false;
            const long long value = toLongLong( &ok );
            if( ok )
            {
                if constexpr( std::is_signed<T>::value )
                {
                    ok = value >= static_cast<long long>( std::numeric_limits<T>::min() ) &&
                        value <= static_cast<long long>( std::numeric_limits<T>::max() );
                }
                else
                {
                    ok = value >= 0 && static_cast<unsigned long long>( value ) <=
                        static_cast<unsigned long long>( std::numeric_limits<T>::max() );
                }
            }
            if( aOk != nullptr )
            {
                *aOk = ok;
            }
            return ok ? static_cast<T>( value ) : T( 0 );
        }
        else if constexpr( std::is_floating_point<T>::value )
        {
            return static_cast<T>( toDouble( aOk ) );
        }
        else if constexpr( std::is_same<T, std::string>::value )
        {
            return toString( aOk );
        }
        else if constexpr( std::is_same<T, std::vector<std::string> >::value )
        {
            return toStringList( aOk );
        }
        else if constexpr( std::is_same<T, SettingsValue>::value )
        {
            if( aOk != nullptr )
            {
                *aOk = true;
            }
            return *this;
        }
        else
        {
            static_assert( UnsupportedType<T>::value,
                "SettingsValue::to<T>: T must be bool, an integer type, a floating-point type, "
                "std::string, std::vector<std::string> or SettingsValue." );
            return T();
        }
    }

}

#endif // QT_LIKE_SIGNAL_SETTINGS_VALUE_HPP
