// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! SettingsValue implementation: the constructors and the conversions between the six kinds.

#include "QtLikeSignal/SettingsValue.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>

// The floating-point overloads of std::to_chars and std::from_chars came later than the integer
// ones: libstdc++ has them from GCC 11, and the MinGW cross toolchain in this build matrix is
// GCC 10. A library defines __cpp_lib_to_chars only when it has them, so its absence selects the
// fallback below.
#if !defined( __cpp_lib_to_chars )
    #include <cerrno>
    #include <clocale>
    #include <cstdio>
    #include <cstdlib>
#endif

namespace QtLikeSignal
{
    namespace
    {
        //! Sets @p aOk to @p aValue if @p aOk is not null. Every conversion ends with this.
        void setOk
            (
            bool* aOk,   //!< Where the caller wants the result. Can be null.
            bool aValue  //!< The result.
            )
        {
            if( aOk != nullptr )
            {
                *aOk = aValue;
            }
        }

        //! Returns true if @p aChar is a space, a tab, or a line break.
        bool isSpace
            (
            char aChar  //!< The character to test.
            )
        {
            return aChar == ' ' || aChar == '\t' || aChar == '\n' || aChar == '\r' ||
                   aChar == '\f' || aChar == '\v';
        }

        //! Returns @p aText without the spaces at its start and end.
        //!
        //! A number read from a hand-edited file often has a space after it. QString::toInt()
        //! ignores such spaces, so the conversions here do too.
        std::string trimmed
            (
            const std::string& aText  //!< The text to trim.
            )
        {
            std::size_t first = 0;
            std::size_t last = aText.size();
            while( first < last && isSpace( aText[first] ) )
            {
                ++first;
            }
            while( last > first && isSpace( aText[last - 1] ) )
            {
                --last;
            }
            return aText.substr( first, last - first );
        }

        //! Removes a '+' at the start of @p aText, if a digit or a '.' follows it.
        //!
        //! std::from_chars accepts a '-' but not a '+'. A file written by hand can have "+5", and
        //! QString::toInt() accepts it, so it is removed here before from_chars sees the text.
        void skipPlusSign
            (
            std::string& aText  //!< The trimmed text. Changed in place.
            )
        {
            if( aText.size() > 1 && aText[0] == '+' && aText[1] != '-' && aText[1] != '+' )
            {
                aText.erase( 0, 1 );
            }
        }

        //! Reads all of @p aText as a decimal integer. Returns false if any character is not part
        //! of the number, or if the number does not fit in a long long.
        bool parseLongLong
            (
            const std::string& aText,  //!< The text to read.
            long long& aValue          //!< Set to the number on success.
            )
        {
            std::string text = trimmed( aText );
            skipPlusSign( text );
            if( text.empty() )
            {
                return false;
            }
            const char* const end = text.data() + text.size();
            const std::from_chars_result result = std::from_chars( text.data(), end, aValue );
            return result.ec == std::errc() && result.ptr == end;
        }

        #if !defined( __cpp_lib_to_chars )

        //! Returns the decimal point of the C locale that is in force, which snprintf() writes
        //! and strtod() reads. "." if the locale gives none.
        std::string localeDecimalPoint()
        {
            const std::lconv* const conventions = std::localeconv();
            if( conventions == nullptr || conventions->decimal_point == nullptr ||
                conventions->decimal_point[0] == '\0' )
            {
                return ".";
            }
            return conventions->decimal_point;
        }

        //! Returns @p aText with each @p aFrom replaced by @p aTo.
        std::string replaced
            (
            const std::string& aText,      //!< The text.
            const std::string& aFrom,      //!< What to replace. Not empty.
            const std::string& aTo         //!< What to put in its place.
            )
        {
            std::string result;
            std::size_t position = 0;
            for( ;; )
            {
                const std::size_t found = aText.find( aFrom, position );
                if( found == std::string::npos )
                {
                    result.append( aText, position, std::string::npos );
                    return result;
                }
                result.append( aText, position, found - position );
                result += aTo;
                position = found + aFrom.size();
            }
        }

        //! Reads @p aText as std::from_chars() would, with strtod().
        //!
        //! strtod() uses the locale, so the text is checked first to have only the characters
        //! of a plain number -- a ',' is refused even where it is the decimal point -- and its
        //! '.' is then changed to the locale's decimal point. "inf", "infinity" and "nan" are
        //! read here directly, with any case. A number too large or too small for a double
        //! fails, as it does in from_chars.
        bool parseDoubleWithStrtod
            (
            const std::string& aText,      //!< The trimmed text, not empty.
            double& aValue                 //!< Set to the number on success.
            )
        {
            std::string lower;
            for( const char ch : aText )
            {
                lower += ( ch >= 'A' && ch <= 'Z' ) ? static_cast<char>( ch - 'A' + 'a' ) : ch;
            }
            const bool negative = lower[0] == '-';
            const std::string word = negative ? lower.substr( 1 ) : lower;
            if( word == "inf" || word == "infinity" )
            {
                aValue = negative ? -HUGE_VAL : HUGE_VAL;
                return true;
            }
            if( word == "nan" )
            {
                aValue = std::nan( "" );
                return true;
            }

            for( const char ch : aText )
            {
                if( !( ( ch >= '0' && ch <= '9' ) || ch == '.' || ch == 'e' || ch == 'E' ||
                    ch == '+' || ch == '-' ) )
                {
                    return false;
                }
            }

            const std::string text = replaced( aText, ".", localeDecimalPoint() );
            char* end = nullptr;
            errno = 0;
            const double value = std::strtod( text.c_str(), &end );
            if( end != text.c_str() + text.size() )
            {
                return false;
            }
            // strtod() also sets ERANGE for a subnormal result, which is exact and which
            // from_chars accepts. Only an overflow, or an underflow to zero, fails.
            if( errno == ERANGE && ( std::isinf( value ) || value == 0.0 ) )
            {
                return false;
            }
            aValue = value;
            return true;
        }

        //! Writes @p aValue as std::to_chars( first, last, value ) would, with snprintf().
        //!
        //! It finds the fewest significant digits that read back as the same double, then
        //! writes those digits in fixed or in scientific notation, whichever is shorter, and
        //! fixed when both have the same length. That is the rule of to_chars. snprintf()
        //! writes the locale's decimal point, which is changed back to '.'.
        std::string formatDoubleWithSnprintf
            (
            double aValue      //!< The number to write.
            )
        {
            if( std::isnan( aValue ) )
            {
                return std::signbit( aValue ) ? "-nan" : "nan";
            }
            if( std::isinf( aValue ) )
            {
                return aValue < 0 ? "-inf" : "inf";
            }

            const std::string point = localeDecimalPoint();
            char buffer[512];
            std::string scientific;
            int digits = 1;
            for( ; digits <= 17; ++digits )
            {
                std::snprintf( buffer, sizeof( buffer ), "%.*e", digits - 1, aValue );
                scientific = replaced( buffer, point, "." );
                double readBack = 0.0;
                if( parseDoubleWithStrtod( scientific, readBack ) && readBack == aValue )
                {
                    break;
                }
            }
            if( digits > 17 )
            {
                digits = 17;
            }

            // The exponent that %e wrote tells how many decimals the fixed form needs to show
            // the same digits. %e writes it with a sign and at least two digits, as to_chars
            // does, so the scientific text needs no change.
            const std::size_t e = scientific.find( 'e' );
            const int exponent = e == std::string::npos ? 0 :
                std::atoi( scientific.c_str() + e + 1 );
            const int decimals = digits - 1 - exponent > 0 ? digits - 1 - exponent : 0;
            std::snprintf( buffer, sizeof( buffer ), "%.*f", decimals, aValue );
            const std::string fixed = replaced( buffer, point, "." );

            return fixed.size() <= scientific.size() ? fixed : scientific;
        }

        #endif

        //! Reads all of @p aText as a floating-point number. Returns false if any character is not
        //! part of the number.
        //!
        //! std::from_chars and not strtod, because strtod uses the C locale of the program. With
        //! a German locale, strtod reads "1.5" as 1 and stops at the '.'. from_chars always uses
        //! '.', which is what the file contains.
        bool parseDouble
            (
            const std::string& aText,  //!< The text to read.
            double& aValue             //!< Set to the number on success.
            )
        {
            std::string text = trimmed( aText );
            skipPlusSign( text );
            if( text.empty() )
            {
                return false;
            }
            #if defined( __cpp_lib_to_chars )
                const char* const end = text.data() + text.size();
                const std::from_chars_result result = std::from_chars( text.data(), end, aValue );
                return result.ec == std::errc() && result.ptr == end;
            #else
                return parseDoubleWithStrtod( text, aValue );
            #endif
        }

        //! Returns true if @p aText equals @p aWord when case is ignored. @p aWord is lower case.
        bool equalsIgnoringCase
            (
            const std::string& aText,  //!< The text to compare.
            const char* aWord          //!< A lower-case ASCII word.
            )
        {
            std::size_t i = 0;
            for( ; aWord[i] != '\0'; ++i )
            {
                if( i >= aText.size() )
                {
                    return false;
                }
                char ch = aText[i];
                if( ch >= 'A' && ch <= 'Z' )
                {
                    ch = static_cast<char>( ch - 'A' + 'a' );
                }
                if( ch != aWord[i] )
                {
                    return false;
                }
            }
            return i == aText.size();
        }

        //! Reads @p aText as a bool. Accepts "true", "false", "1" and "0", with any case and with
        //! spaces around them.
        //!
        //! This is stricter than QVariant, on purpose. QVariant reads every string as true except
        //! an empty one, "0" and "false", so a typing error such as "flase" reads as true. Here it
        //! fails, and Settings::value<bool>() gives the caller's default.
        bool parseBool
            (
            const std::string& aText,  //!< The text to read.
            bool& aValue               //!< Set to the result on success.
            )
        {
            const std::string text = trimmed( aText );
            if( text == "1" || equalsIgnoringCase( text, "true" ) )
            {
                aValue = true;
                return true;
            }
            if( text == "0" || equalsIgnoringCase( text, "false" ) )
            {
                aValue = false;
                return true;
            }
            return false;
        }

        //! Writes @p aValue in the shortest form that reads back as the same double.
        //!
        //! This is what Qt writes too (QString::number( d, 'g', QLocale::FloatingPointShortest )),
        //! so 0.1 is written as "0.1" and not as "0.10000000000000001". std::to_chars does not use
        //! the locale, so the text always has a '.', never a ','. A ',' would be dangerous: in an
        //! INI value, "1,5" is a list of two strings.
        std::string formatDouble
            (
            double aValue  //!< The number to write.
            )
        {
            #if defined( __cpp_lib_to_chars )
                char buffer[64];
                const std::to_chars_result result = std::to_chars( buffer,
                    buffer + sizeof( buffer ), aValue );
                return std::string( buffer, result.ptr );
            #else
                return formatDoubleWithSnprintf( aValue );
            #endif
        }

        //! Writes @p aValue as a decimal integer.
        std::string formatLongLong
            (
            long long aValue  //!< The number to write.
            )
        {
            char buffer[32];
            const std::to_chars_result result = std::to_chars( buffer, buffer + sizeof( buffer ),
                aValue );
            return std::string( buffer, result.ptr );
        }
    }

    //! Constructs an Invalid value, which holds no data.
    SettingsValue::SettingsValue()
    {
    }

    //! Constructs a Bool that holds @p aValue.
    SettingsValue::SettingsValue
        (
        bool aValue  //!< The bool to hold.
        )
        : mType( SettingsType::Bool )
        , mBool( aValue )
    {
    }

    //! Constructs a String that holds @p aValue, or an Invalid value if @p aValue is null.
    //!
    //! Without this constructor, a string literal would convert to bool rather than to
    //! std::string, because a pointer-to-bool conversion is a standard conversion and wins over
    //! a user-defined one. `setValue( "name", "Evan" )` would then store true.
    SettingsValue::SettingsValue
        (
        const char* aValue  //!< UTF-8 text to hold. Can be null.
        )
    {
        if( aValue != nullptr )
        {
            mType   = SettingsType::String;
            mString = aValue;
        }
    }

    //! Constructs a String that holds @p aValue.
    SettingsValue::SettingsValue
        (
        std::string aValue  //!< UTF-8 text to hold.
        )
        : mType( SettingsType::String )
        , mString( std::move( aValue ) )
    {
    }

    //! Constructs a StringList that holds @p aValue.
    SettingsValue::SettingsValue
        (
        std::vector<std::string> aValue  //!< UTF-8 strings to hold.
        )
        : mType( SettingsType::StringList )
        , mStringList( std::move( aValue ) )
    {
    }

    //! Converts the value to a bool.
    //!
    //! - Bool: the value.
    //! - Int and Double: true if not zero.
    //! - String: "true" and "1" are true, "false" and "0" are false, with any case. Other text
    //!   fails. This is stricter than QVariant; see parseBool() above.
    //! - StringList with one element: that element, read as a String.
    //! - Invalid, and other lists: fails.
    //!
    //! @return the bool, or false if the conversion fails.
    bool SettingsValue::toBool
        (
        bool* aOk  //!< Set to true if the conversion succeeded, false if not. Can be null.
        ) const
    {
        bool result = false;
        bool ok = true;
        switch( mType )
        {
        case SettingsType::Bool:
            result = mBool;
            break;
        case SettingsType::Int:
            result = mInt != 0;
            break;
        case SettingsType::Double:
            result = mDouble != 0.0;
            break;
        case SettingsType::String:
            ok = parseBool( mString, result );
            break;
        case SettingsType::StringList:
            ok = mStringList.size() == 1 && parseBool( mStringList[0], result );
            break;
        case SettingsType::Invalid:
        default:
            ok = false;
            break;
        }
        setOk( aOk, ok );
        return ok ? result : false;
    }

    //! Converts the value to an int. This is toLongLong() followed by a range check.
    //!
    //! @return the int, or 0 if the conversion fails or the value does not fit in an int.
    int SettingsValue::toInt
        (
        bool* aOk  //!< Set to true if the conversion succeeded, false if not. Can be null.
        ) const
    {
        return to<int>( aOk );
    }

    //! Converts the value to a long long.
    //!
    //! - Int: the value.
    //! - Bool: 1 or 0.
    //! - Double: rounded to the nearest integer, as QVariant does. It fails if the double is not
    //!   finite or does not fit in a long long.
    //! - String: read as a decimal integer, with spaces allowed around it. "12.5" and "0x10" fail.
    //! - StringList with one element: that element, read as a String.
    //! - Invalid, and other lists: fails.
    //!
    //! @return the integer, or 0 if the conversion fails.
    long long SettingsValue::toLongLong
        (
        bool* aOk  //!< Set to true if the conversion succeeded, false if not. Can be null.
        ) const
    {
        long long result = 0;
        bool ok = true;
        switch( mType )
        {
        case SettingsType::Int:
            result = mInt;
            break;
        case SettingsType::Bool:
            result = mBool ? 1 : 0;
            break;
        case SettingsType::Double:
        {
            // 2^63 is exactly representable as a double, and LLONG_MAX is not: it rounds up
            // to 2^63. So the upper test is "less than 2^63", not "less or equal LLONG_MAX".
            const double rounded = std::round( mDouble );
            ok = std::isfinite( rounded ) && rounded >= -9223372036854775808.0 &&
                rounded < 9223372036854775808.0;
            if( ok )
            {
                result = static_cast<long long>( rounded );
            }
            break;
        }
        case SettingsType::String:
            ok = parseLongLong( mString, result );
            break;
        case SettingsType::StringList:
            ok = mStringList.size() == 1 && parseLongLong( mStringList[0], result );
            break;
        case SettingsType::Invalid:
        default:
            ok = false;
            break;
        }
        setOk( aOk, ok );
        return ok ? result : 0;
    }

    //! Converts the value to a double.
    //!
    //! - Double: the value.
    //! - Int: the value, which can lose precision above 2^53.
    //! - Bool: 1.0 or 0.0.
    //! - String: read as a number with '.' as the decimal point, whatever the locale is. "inf"
    //!   and "nan" are accepted, because that is how a Double that holds them is written.
    //! - StringList with one element: that element, read as a String.
    //! - Invalid, and other lists: fails.
    //!
    //! @return the double, or 0.0 if the conversion fails.
    double SettingsValue::toDouble
        (
        bool* aOk  //!< Set to true if the conversion succeeded, false if not. Can be null.
        ) const
    {
        double result = 0.0;
        bool ok = true;
        switch( mType )
        {
        case SettingsType::Double:
            result = mDouble;
            break;
        case SettingsType::Int:
            result = static_cast<double>( mInt );
            break;
        case SettingsType::Bool:
            result = mBool ? 1.0 : 0.0;
            break;
        case SettingsType::String:
            ok = parseDouble( mString, result );
            break;
        case SettingsType::StringList:
            ok = mStringList.size() == 1 && parseDouble( mStringList[0], result );
            break;
        case SettingsType::Invalid:
        default:
            ok = false;
            break;
        }
        setOk( aOk, ok );
        return ok ? result : 0.0;
    }

    //! Converts the value to a string. This is also the text that the INI file stores.
    //!
    //! - String: the value.
    //! - Bool: "true" or "false".
    //! - Int: the decimal number.
    //! - Double: the shortest text that reads back as the same double, for example "0.1".
    //! - StringList with one element: that element.
    //! - Invalid, and other lists: fails.
    //!
    //! @return the text, or an empty string if the conversion fails.
    std::string SettingsValue::toString
        (
        bool* aOk  //!< Set to true if the conversion succeeded, false if not. Can be null.
        ) const
    {
        switch( mType )
        {
        case SettingsType::String:
            setOk( aOk, true );
            return mString;
        case SettingsType::Bool:
            setOk( aOk, true );
            return mBool ? "true" : "false";
        case SettingsType::Int:
            setOk( aOk, true );
            return formatLongLong( mInt );
        case SettingsType::Double:
            setOk( aOk, true );
            return formatDouble( mDouble );
        case SettingsType::StringList:
            if( mStringList.size() == 1 )
            {
                setOk( aOk, true );
                return mStringList[0];
            }
            break;
        case SettingsType::Invalid:
        default:
            break;
        }
        setOk( aOk, false );
        return std::string();
    }

    //! Converts the value to a list of strings.
    //!
    //! - StringList: the value.
    //! - Invalid: an empty list, and the conversion succeeds. This is not an accident. An INI
    //!   file writes an empty list as `@Invalid()`, because "a list with one empty string" must
    //!   look different in the file. So Invalid is how an empty list comes back, and it must read
    //!   as one.
    //! - String, Bool, Int, Double: a list of one element, which is toString().
    //!
    //! @return the list. The conversion does not fail.
    std::vector<std::string> SettingsValue::toStringList
        (
        bool* aOk  //!< Always set to true. Can be null. Here for the same shape as the others.
        ) const
    {
        setOk( aOk, true );
        switch( mType )
        {
        case SettingsType::StringList:
            return mStringList;
        case SettingsType::Invalid:
            return std::vector<std::string>();
        case SettingsType::String:
        case SettingsType::Bool:
        case SettingsType::Int:
        case SettingsType::Double:
        default:
            return std::vector<std::string>( 1, toString() );
        }
    }

    //! Returns true if both values have the same type and the same data.
    //!
    //! No conversion is made: the Int 5 and the String "5" are not equal. A Double compares with
    //! ==, so NaN is not equal to itself.
    bool SettingsValue::operator==
        (
        const SettingsValue& aOther  //!< The value to compare with.
        ) const
    {
        if( mType != aOther.mType )
        {
            return false;
        }
        switch( mType )
        {
        case SettingsType::Bool:
            return mBool == aOther.mBool;
        case SettingsType::Int:
            return mInt == aOther.mInt;
        case SettingsType::Double:
            return mDouble == aOther.mDouble;
        case SettingsType::String:
            return mString == aOther.mString;
        case SettingsType::StringList:
            return mStringList == aOther.mStringList;
        case SettingsType::Invalid:
        default:
            return true;
        }
    }

    //! Returns true if the values are not equal. See operator==().
    bool SettingsValue::operator!=
        (
        const SettingsValue& aOther  //!< The value to compare with.
        ) const
    {
        return !( *this == aOther );
    }

    //! Reports that the integer constructor got an unsigned value that a long long cannot hold.
    //!
    //! Out of line, so that the header does not include the logging headers for one warning.
    void SettingsValue::reportUnsignedOutOfRange
        (
        unsigned long long aValue  //!< The value that was refused.
        )
    {
        qCWarning( gLogSettings ) << "SettingsValue: the unsigned value" << aValue
                                  << "is larger than a long long can hold; the value is Invalid";
    }

}
