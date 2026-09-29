// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! SettingsIniCodec implementation: key and value escaping, the line parser, and the writer.
//!
//! Ported from Qt 6's qsettings.cpp, from iniEscapedKey(), iniUnescapedKey(), iniEscapedString(),
//! iniEscapedStringList(), iniUnescapedStringList(), variantToString(), stringToVariant(),
//! readIniLine(), readIniFile(), readIniSection() and writeIniFile(). Qt works in UTF-16 QChars;
//! this works in UTF-8 bytes. Where the difference matters -- in key escaping, and in a `\x`
//! escape in a value -- the text is converted, so the file is the same as the one Qt writes.

#include "QtLikeSignal/SettingsIniCodec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace QtLikeSignal
{
    namespace
    {
        //! The character that replaces a byte sequence that is not valid UTF-8, as in Qt.
        const std::uint32_t kReplacementCharacter = 0xFFFD;

        //! The line end that the writer uses. Qt writes CRLF on Windows and LF elsewhere, so a
        //! file that a person opens in the platform's own editor looks correct.
        #if defined( _WIN32 )
            const char* const kEndOfLine = "\r\n";
        #else
            const char* const kEndOfLine = "\n";
        #endif

        //! Bit in kCharTraits: the character is white space at the start of a line.
        const unsigned char kSpace = 0x1;

        //! Bit in kCharTraits: the character ends the fast scan in readLine().
        const unsigned char kSpecial = 0x2;

        //! The class of each byte, for readLine(). The same table as Qt's charTraits.
        //!
        //! Space: '\t', '\n', '\r', ' '. Special: '\n', '\r', '"', ';', '=', '\\'. Every byte
        //! from 0x80 up is neither, so a UTF-8 sequence never stops the scan.
        struct CharTraits
        {
            //! One entry per byte value.
            unsigned char mTraits[256];

            //! Fills the table.
            CharTraits()
                : mTraits()
            {
                mTraits[static_cast<unsigned char>( '\t' )] = kSpace;
                mTraits[static_cast<unsigned char>( ' ' )]  = kSpace;
                mTraits[static_cast<unsigned char>( '\n' )] = kSpace | kSpecial;
                mTraits[static_cast<unsigned char>( '\r' )] = kSpace | kSpecial;
                mTraits[static_cast<unsigned char>( '"' )]  = kSpecial;
                mTraits[static_cast<unsigned char>( ';' )]  = kSpecial;
                mTraits[static_cast<unsigned char>( '=' )]  = kSpecial;
                mTraits[static_cast<unsigned char>( '\\' )] = kSpecial;
            }

        };

        //! The one table. Constant after its construction, so every thread can read it.
        const CharTraits kCharTraits;

        //! Returns the traits of @p aChar. See kCharTraits.
        unsigned char traitsOf
            (
            char aChar  //!< The byte.
            )
        {
            return kCharTraits.mTraits[static_cast<unsigned char>( aChar )];
        }

        //! Returns the value of the hexadecimal digit @p aChar, or -1 if it is not one.
        int fromHex
            (
            char aChar  //!< The character.
            )
        {
            if( aChar >= '0' && aChar <= '9' )
            {
                return aChar - '0';
            }
            if( aChar >= 'a' && aChar <= 'f' )
            {
                return aChar - 'a' + 10;
            }
            if( aChar >= 'A' && aChar <= 'F' )
            {
                return aChar - 'A' + 10;
            }
            return -1;
        }

        //! Returns the value of the octal digit @p aChar, or -1 if it is not one.
        int fromOct
            (
            char aChar  //!< The character.
            )
        {
            return ( aChar >= '0' && aChar <= '7' ) ? aChar - '0' : -1;
        }

        //! Appends @p aValue in lower-case hexadecimal, without leading zeros, as
        //! QByteArray::number( value, 16 ) does. That is the form of a `\x` escape that Qt writes.
        void appendHexLower
            (
            std::string& aResult,  //!< Where to append.
            unsigned int aValue    //!< The value.
            )
        {
            static const char kDigits[] = "0123456789abcdef";
            char buffer[8];
            std::size_t count = 0;
            do
            {
                buffer[count++] = kDigits[aValue % 16];
                aValue /= 16;
            }
            while( aValue != 0 && count < sizeof( buffer ) );
            while( count > 0 )
            {
                aResult += buffer[--count];
            }
        }

        //! Appends the code point @p aCodePoint as UTF-8.
        void appendUtf8
            (
            std::string& aResult,      //!< Where to append.
            std::uint32_t aCodePoint   //!< A code point. Not a surrogate.
            )
        {
            if( aCodePoint < 0x80 )
            {
                aResult += static_cast<char>( aCodePoint );
            }
            else if( aCodePoint < 0x800 )
            {
                aResult += static_cast<char>( 0xC0 | ( aCodePoint >> 6 ) );
                aResult += static_cast<char>( 0x80 | ( aCodePoint & 0x3F ) );
            }
            else if( aCodePoint < 0x10000 )
            {
                aResult += static_cast<char>( 0xE0 | ( aCodePoint >> 12 ) );
                aResult += static_cast<char>( 0x80 | ( ( aCodePoint >> 6 ) & 0x3F ) );
                aResult += static_cast<char>( 0x80 | ( aCodePoint & 0x3F ) );
            }
            else
            {
                aResult += static_cast<char>( 0xF0 | ( aCodePoint >> 18 ) );
                aResult += static_cast<char>( 0x80 | ( ( aCodePoint >> 12 ) & 0x3F ) );
                aResult += static_cast<char>( 0x80 | ( ( aCodePoint >> 6 ) & 0x3F ) );
                aResult += static_cast<char>( 0x80 | ( aCodePoint & 0x3F ) );
            }
        }

        //! Returns true if @p aUnit is half of a UTF-16 surrogate pair.
        bool isSurrogate
            (
            std::uint32_t aUnit  //!< The code unit or code point.
            )
        {
            return aUnit >= 0xD800 && aUnit <= 0xDFFF;
        }

        //! Appends one UTF-16 code unit as UTF-8. A surrogate cannot stand alone in UTF-8, so it
        //! becomes U+FFFD.
        //!
        //! This is for a `\x` or octal escape in a value, which Qt reads as one UTF-16 code unit.
        void appendCodeUnitAsUtf8
            (
            std::string& aResult,  //!< Where to append.
            std::uint16_t aUnit    //!< The code unit.
            )
        {
            appendUtf8( aResult, isSurrogate( aUnit ) ? kReplacementCharacter : aUnit );
        }

        //! Converts UTF-8 to UTF-16. A byte sequence that is not valid UTF-8 becomes U+FFFD, as it
        //! does in QString::fromUtf8().
        std::u16string utf8ToUtf16
            (
            const std::string& aText  //!< UTF-8 text.
            )
        {
            std::u16string result;
            result.reserve( aText.size() );
            std::size_t i = 0;
            while( i < aText.size() )
            {
                const unsigned char lead = static_cast<unsigned char>( aText[i] );
                std::uint32_t codePoint = kReplacementCharacter;
                std::size_t length = 1;
                std::size_t continuation = 0;
                std::uint32_t minimum = 0;

                if( lead < 0x80 )
                {
                    codePoint = lead;
                }
                else if( ( lead & 0xE0 ) == 0xC0 )
                {
                    continuation = 1;
                    codePoint = lead & 0x1F;
                    minimum = 0x80;
                }
                else if( ( lead & 0xF0 ) == 0xE0 )
                {
                    continuation = 2;
                    codePoint = lead & 0x0F;
                    minimum = 0x800;
                }
                else if( ( lead & 0xF8 ) == 0xF0 )
                {
                    continuation = 3;
                    codePoint = lead & 0x07;
                    minimum = 0x10000;
                }

                if( continuation > 0 )
                {
                    bool valid = i + continuation < aText.size();
                    for( std::size_t k = 1; valid && k <= continuation; ++k )
                    {
                        const unsigned char next = static_cast<unsigned char>( aText[i + k] );
                        if( ( next & 0xC0 ) != 0x80 )
                        {
                            valid = false;
                        }
                        else
                        {
                            codePoint = ( codePoint << 6 ) | ( next & 0x3F );
                        }
                    }
                    if( valid && codePoint >= minimum && codePoint <= 0x10FFFF &&
                        !isSurrogate( codePoint ) )
                    {
                        length = continuation + 1;
                    }
                    else
                    {
                        codePoint = kReplacementCharacter;
                    }
                }
                else if( lead >= 0x80 )
                {
                    codePoint = kReplacementCharacter;
                }

                if( codePoint >= 0x10000 )
                {
                    const std::uint32_t offset = codePoint - 0x10000;
                    result += static_cast<char16_t>( 0xD800 + ( offset >> 10 ) );
                    result += static_cast<char16_t>( 0xDC00 + ( offset & 0x3FF ) );
                }
                else
                {
                    result += static_cast<char16_t>( codePoint );
                }
                i += length;
            }
            return result;
        }

        //! Converts UTF-16 to UTF-8. A surrogate that is not part of a pair becomes U+FFFD.
        std::string utf16ToUtf8
            (
            const std::u16string& aText  //!< UTF-16 text.
            )
        {
            std::string result;
            result.reserve( aText.size() );
            for( std::size_t i = 0; i < aText.size(); ++i )
            {
                const std::uint32_t unit = aText[i];
                if( unit >= 0xD800 && unit <= 0xDBFF && i + 1 < aText.size() &&
                    aText[i + 1] >= 0xDC00 && aText[i + 1] <= 0xDFFF )
                {
                    const std::uint32_t low = aText[i + 1];
                    appendUtf8( result, 0x10000 + ( ( unit - 0xD800 ) << 10 ) + ( low - 0xDC00 ) );
                    ++i;
                }
                else
                {
                    appendUtf8( result, isSurrogate( unit ) ? kReplacementCharacter : unit );
                }
            }
            return result;
        }

        //! Returns true if @p aChar is white space, as QByteArray::trimmed() defines it.
        bool isTrimSpace
            (
            char aChar  //!< The byte.
            )
        {
            return aChar == ' ' || aChar == '\t' || aChar == '\n' || aChar == '\r' ||
                   aChar == '\v' || aChar == '\f';
        }

        //! Returns @p aText without white space at its start and end.
        std::string trimmed
            (
            const std::string& aText  //!< The text.
            )
        {
            std::size_t first = 0;
            std::size_t last = aText.size();
            while( first < last && isTrimSpace( aText[first] ) )
            {
                ++first;
            }
            while( last > first && isTrimSpace( aText[last - 1] ) )
            {
                --last;
            }
            return aText.substr( first, last - first );
        }

        //! Returns true if @p aText starts with @p aPrefix.
        bool startsWith
            (
            const std::string& aText,  //!< The text.
            const char* aPrefix        //!< The prefix.
            )
        {
            return aText.compare( 0, std::char_traits<char>::length( aPrefix ), aPrefix ) == 0;
        }

        //! Returns true if @p aText equals @p aWord when ASCII case is ignored. @p aWord is lower
        //! case.
        bool equalsIgnoringCase
            (
            const std::string& aText,  //!< The text.
            const char* aWord          //!< A lower-case ASCII word.
            )
        {
            const std::size_t length = std::char_traits<char>::length( aWord );
            if( aText.size() != length )
            {
                return false;
            }
            for( std::size_t i = 0; i < length; ++i )
            {
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
            return true;
        }

        //! Removes spaces and tabs from the end of @p aText, but not before index @p aLimit.
        //!
        //! The limit is where the current piece of the value started. An escape such as `\x20`
        //! moves it, so a space that the file escaped on purpose is kept.
        void chopTrailingSpaces
            (
            std::string& aText,  //!< The text. Changed in place.
            std::size_t aLimit   //!< The first index that can be removed.
            )
        {
            while( aText.size() > aLimit && ( aText.back() == ' ' || aText.back() == '\t' ) )
            {
                aText.pop_back();
            }
        }

        //! One section while the writer collects the keys.
        struct WriterSection
        {
            //! The lowest position of the keys in the section. The sections are written in this
            //! order, so a section comes out where its first key was.
            std::size_t mPosition { static_cast<std::size_t>( -1 ) };

            //! The keys of the section, without the section name, by (position, name).
            std::map<std::pair<std::size_t, std::string>, const SettingsValue*> mKeys;
        };
    }

    //! Writes @p aKey in the form an INI file uses for a key or a section name.
    //!
    //! '/' becomes '\'. An ASCII letter or digit, '_', '-' and '.' are kept. Every other UTF-16
    //! code unit is written as `%XX` if it is not above 0xFF, or as `%UXXXX`, with upper-case hex
    //! digits. A character above U+FFFF is two `%U` escapes, one for each half of its surrogate
    //! pair, because that is what Qt writes.
    void SettingsIniCodec::escapeKey
        (
        const std::string& aKey,  //!< The key, in UTF-8.
        std::string& aResult      //!< Where to append the escaped key.
        )
    {
        static const char kDigits[] = "0123456789ABCDEF";
        const std::u16string units = utf8ToUtf16( aKey );
        aResult.reserve( aResult.size() + units.size() * 3 / 2 );
        for( const char16_t unit : units )
        {
            if( unit == u'/' )
            {
                aResult += '\\';
            }
            else if( ( unit >= u'a' && unit <= u'z' ) || ( unit >= u'A' && unit <= u'Z' ) ||
                ( unit >= u'0' && unit <= u'9' ) || unit == u'_' || unit == u'-' || unit == u'.' )
            {
                aResult += static_cast<char>( unit );
            }
            else if( unit <= 0xFF )
            {
                aResult += '%';
                aResult += kDigits[unit / 16];
                aResult += kDigits[unit % 16];
            }
            else
            {
                aResult += "%U";
                aResult += kDigits[( unit >> 12 ) & 0xF];
                aResult += kDigits[( unit >> 8 ) & 0xF];
                aResult += kDigits[( unit >> 4 ) & 0xF];
                aResult += kDigits[unit & 0xF];
            }
        }
    }

    //! Reads a key or a section name as an INI file writes it. The reverse of escapeKey().
    //!
    //! '\' becomes '/', and `%XX` and `%UXXXX` become the code unit they name. A '%' that is not
    //! followed by enough hex digits is kept as a '%'. Other text is kept, so a key that a person
    //! wrote in UTF-8 without escapes also reads correctly.
    //!
    //! @return the key, in UTF-8.
    std::string SettingsIniCodec::unescapeKey
        (
        const std::string& aKey  //!< The key as it is in the file.
        )
    {
        const std::u16string decoded = utf8ToUtf16( aKey );
        const std::size_t size = decoded.size();
        std::u16string result;
        result.reserve( size );
        std::size_t i = 0;
        while( i < size )
        {
            const char16_t unit = decoded[i];
            if( unit == u'\\' )
            {
                result += u'/';
                ++i;
                continue;
            }
            if( unit != u'%' || i == size - 1 )
            {
                result += unit;
                ++i;
                continue;
            }

            std::size_t digitCount = 2;
            std::size_t firstDigit = i + 1;
            if( decoded[i + 1] == u'U' )
            {
                ++firstDigit;
                digitCount = 4;
            }
            if( firstDigit + digitCount > size )
            {
                result += u'%';
                ++i;
                continue;
            }

            unsigned int value = 0;
            bool ok = true;
            for( std::size_t k = 0; k < digitCount; ++k )
            {
                const char16_t digit = decoded[firstDigit + k];
                const int nibble = digit < 0x80 ? fromHex( static_cast<char>( digit ) ) : -1;
                if( nibble < 0 )
                {
                    ok = false;
                    break;
                }
                value = value * 16 + static_cast<unsigned int>( nibble );
            }
            if( !ok )
            {
                result += u'%';
                ++i;
                continue;
            }
            result += static_cast<char16_t>( value );
            i = firstDigit + digitCount;
        }
        return utf16ToUtf8( result );
    }

    //! Writes one string value in INI form, and appends it to @p aResult.
    //!
    //! - '\0', '\a', '\b', '\f', '\n', '\r', '\t' and '\v' become two-character escapes.
    //! - '"' and '\' get a backslash in front.
    //! - Any other byte below 0x20 becomes `\x` and its value in lower-case hex, as `\x1f`.
    //! - **After `\0` or `\xHH`, a hex digit is escaped too.** Without this, `\x1` followed by a
    //!   '2' would read back as the one character `\x12`. Qt does the same, and the rule stays in
    //!   force while the digits continue.
    //! - Other bytes, including UTF-8 sequences, are copied.
    //! - The result is put in quotes if the text contains ';', ',' or '=', or if the escaped text
    //!   starts or ends with a space. Otherwise the reader would cut it at the ';', split it at
    //!   the ',', or lose the space.
    //!
    //! Qt works on UTF-16 code units and this works on bytes. The output is the same for valid
    //! UTF-8, because every byte of a multi-byte sequence is 0x80 or above, and none of the rules
    //! above applies to such a byte.
    void SettingsIniCodec::escapeString
        (
        const std::string& aText,  //!< The text, in UTF-8.
        std::string& aResult       //!< Where to append the escaped text.
        )
    {
        bool needsQuotes = false;
        bool escapeNextIfDigit = false;
        const std::size_t startPos = aResult.size();
        aResult.reserve( startPos + aText.size() * 3 / 2 );

        for( const char ch : aText )
        {
            const unsigned char byte = static_cast<unsigned char>( ch );
            if( ch == ';' || ch == ',' || ch == '=' )
            {
                needsQuotes = true;
            }

            if( escapeNextIfDigit && fromHex( ch ) != -1 )
            {
                aResult += "\\x";
                appendHexLower( aResult, byte );
                continue;
            }
            escapeNextIfDigit = false;

            switch( ch )
            {
            case '\0':
                aResult += "\\0";
                escapeNextIfDigit = true;
                break;
            case '\a':
                aResult += "\\a";
                break;
            case '\b':
                aResult += "\\b";
                break;
            case '\f':
                aResult += "\\f";
                break;
            case '\n':
                aResult += "\\n";
                break;
            case '\r':
                aResult += "\\r";
                break;
            case '\t':
                aResult += "\\t";
                break;
            case '\v':
                aResult += "\\v";
                break;
            case '"':
            case '\\':
                aResult += '\\';
                aResult += ch;
                break;
            default:
                if( byte <= 0x1F )
                {
                    aResult += "\\x";
                    appendHexLower( aResult, byte );
                    escapeNextIfDigit = true;
                }
                else
                {
                    aResult += ch;
                }
                break;
            }
        }

        if( needsQuotes || ( startPos < aResult.size() &&
            ( aResult[startPos] == ' ' || aResult.back() == ' ' ) ) )
        {
            aResult.insert( startPos, 1, '"' );
            aResult += '"';
        }
    }

    //! Writes a list of strings in INI form, and appends it to @p aResult.
    //!
    //! The elements are escaped with escapeString() and joined with ", ". An empty list is
    //! written as `@Invalid()`. It cannot be written as nothing, because an empty value reads
    //! back as one empty string, and "no strings" and "one empty string" must stay different.
    void SettingsIniCodec::escapeStringList
        (
        const std::vector<std::string>& aList,  //!< The strings. Each is already passed through
                                                //!< valueToString(), so '@' is doubled.
        std::string& aResult                    //!< Where to append the escaped list.
        )
    {
        if( aList.empty() )
        {
            aResult += "@Invalid()";
            return;
        }
        for( std::size_t i = 0; i < aList.size(); ++i )
        {
            if( i != 0 )
            {
                aResult += ", ";
            }
            escapeString( aList[i], aResult );
        }
    }

    //! Reads a value as an INI file writes it. The reverse of escapeString() and
    //! escapeStringList().
    //!
    //! The reader accepts more than the writer produces, as Qt's does:
    //! - `\?` and `\'`, and octal escapes such as `\101`.
    //! - A backslash at the end of a line, which joins the next line to this one.
    //! - A backslash before any other character drops that character.
    //! - Spaces and tabs at the end of a part that is not in quotes are removed. In quotes, they
    //!   are kept.
    //! - A ',' outside quotes makes the value a list.
    //!
    //! A `\x` or octal escape names one UTF-16 code unit, and is added to the text as UTF-8.
    //!
    //! @return true if the value is a list. Then the elements are in @p aList. If not, the string
    //!         is in @p aString.
    bool SettingsIniCodec::unescapeStringList
        (
        const std::string& aText,        //!< The value as it is in the file, after the '='.
        std::string& aString,            //!< Receives the string. Must be empty on entry.
        std::vector<std::string>& aList  //!< Receives the list elements.
        )
    {
        //! The states of Qt's parser, which uses goto between labels of these names.
        enum class State
        {
            SkipSpaces,  //!< Skip spaces and tabs, then go to Normal.
            Normal,      //!< Read text, quotes, escapes and commas.
            HexEscape,   //!< Read the digits of a `\x` escape.
            OctEscape,   //!< Read the digits of an octal escape.
            End          //!< Finished.
        };

        static const char kEscapeCodes[][2] =
        {
            { 'a', '\a' },
            { 'b', '\b' },
            { 'f', '\f' },
            { 'n', '\n' },
            { 'r', '\r' },
            { 't', '\t' },
            { 'v', '\v' },
            { '"', '"' },
            { '?', '?' },
            { '\'', '\'' },
            { '\\', '\\' }
        };

        const std::size_t size = aText.size();
        bool isStringList = false;
        bool inQuotedString = false;
        bool currentValueIsQuoted = false;
        std::uint16_t escapeValue = 0;
        std::size_t i = 0;
        std::size_t chopLimit = 0;
        State state = State::SkipSpaces;

        while( state != State::End )
        {
            switch( state )
            {
            case State::SkipSpaces:
                while( i < size && ( aText[i] == ' ' || aText[i] == '\t' ) )
                {
                    ++i;
                }
                state = State::Normal;
                break;

            case State::Normal:
            {
                // Entering this state starts a new piece of text. Trailing spaces are removed
                // only back to here, so an escape that ended just before keeps its spaces.
                chopLimit = aString.size();
                State next = State::End;
                bool leaveLoop = false;
                while( i < size && !leaveLoop )
                {
                    const char ch = aText[i];
                    if( ch == '\\' )
                    {
                        ++i;
                        if( i >= size )
                        {
                            next = State::End;
                            leaveLoop = true;
                            continue;
                        }
                        const char code = aText[i++];
                        bool known = false;
                        for( const auto& escapeCode : kEscapeCodes )
                        {
                            if( code == escapeCode[0] )
                            {
                                aString += escapeCode[1];
                                known = true;
                                break;
                            }
                        }
                        if( known )
                        {
                            next = State::Normal;
                            leaveLoop = true;
                            continue;
                        }
                        if( code == 'x' )
                        {
                            escapeValue = 0;
                            if( i >= size )
                            {
                                next = State::End;
                                leaveLoop = true;
                                continue;
                            }
                            if( fromHex( aText[i] ) != -1 )
                            {
                                next = State::HexEscape;
                                leaveLoop = true;
                                continue;
                            }
                        }
                        else if( fromOct( code ) != -1 )
                        {
                            escapeValue = static_cast<std::uint16_t>( fromOct( code ) );
                            next = State::OctEscape;
                            leaveLoop = true;
                            continue;
                        }
                        else if( code == '\n' || code == '\r' )
                        {
                            // A line continuation. \n, \r, \r\n and \n\r all end a line.
                            if( i < size )
                            {
                                const char code2 = aText[i];
                                if( ( code2 == '\n' || code2 == '\r' ) && code2 != code )
                                {
                                    ++i;
                                }
                            }
                        }
                        // Any other character after a backslash is dropped.
                        chopLimit = aString.size();
                    }
                    else if( ch == '"' )
                    {
                        ++i;
                        currentValueIsQuoted = true;
                        inQuotedString = !inQuotedString;
                        if( !inQuotedString )
                        {
                            next = State::SkipSpaces;
                            leaveLoop = true;
                        }
                    }
                    else if( ch == ',' && !inQuotedString )
                    {
                        if( !currentValueIsQuoted )
                        {
                            chopTrailingSpaces( aString, chopLimit );
                        }
                        if( !isStringList )
                        {
                            isStringList = true;
                            aList.clear();
                        }
                        aList.push_back( aString );
                        aString.clear();
                        currentValueIsQuoted = false;
                        ++i;
                        next = State::SkipSpaces;
                        leaveLoop = true;
                    }
                    else
                    {
                        std::size_t j = i + 1;
                        while( j < size && aText[j] != '\\' && aText[j] != '"' &&
                            aText[j] != ',' )
                        {
                            ++j;
                        }
                        aString.append( aText, i, j - i );
                        i = j;
                    }
                }
                if( !leaveLoop )
                {
                    // The end of the text, not a transition: trim, then finish.
                    if( !currentValueIsQuoted )
                    {
                        chopTrailingSpaces( aString, chopLimit );
                    }
                    next = State::End;
                }
                state = next;
                break;
            }

            case State::HexEscape:
                if( i >= size )
                {
                    appendCodeUnitAsUtf8( aString, escapeValue );
                    state = State::End;
                }
                else if( fromHex( aText[i] ) != -1 )
                {
                    escapeValue = static_cast<std::uint16_t>( ( escapeValue << 4 ) +
                        fromHex( aText[i] ) );
                    ++i;
                }
                else
                {
                    appendCodeUnitAsUtf8( aString, escapeValue );
                    state = State::Normal;
                }
                break;

            case State::OctEscape:
                if( i >= size )
                {
                    appendCodeUnitAsUtf8( aString, escapeValue );
                    state = State::End;
                }
                else if( fromOct( aText[i] ) != -1 )
                {
                    escapeValue = static_cast<std::uint16_t>( ( escapeValue << 3 ) +
                        fromOct( aText[i] ) );
                    ++i;
                }
                else
                {
                    appendCodeUnitAsUtf8( aString, escapeValue );
                    state = State::Normal;
                }
                break;

            case State::End:
            default:
                state = State::End;
                break;
            }
        }

        if( isStringList )
        {
            aList.push_back( aString );
        }
        return isStringList;
    }

    //! Returns the text that the file stores for a single value, before escapeString().
    //!
    //! - Invalid: `@Invalid()`.
    //! - A value whose text contains a NUL: `@String(...)` around it, as Qt writes it.
    //! - A value whose text starts with '@': a second '@' in front, so that it cannot be taken
    //!   for a marker. stringToValue() removes it again.
    //! - Anything else: SettingsValue::toString().
    //!
    //! A StringList is not written through this function; see writeValue().
    std::string SettingsIniCodec::valueToString
        (
        const SettingsValue& aValue  //!< The value.
        )
    {
        if( !aValue.isValid() )
        {
            return "@Invalid()";
        }
        std::string result = aValue.toString();
        if( result.find( '\0' ) != std::string::npos )
        {
            return "@String(" + result + ")";
        }
        if( !result.empty() && result[0] == '@' )
        {
            result.insert( 0, 1, '@' );
        }
        return result;
    }

    //! Reads the text of a single value. The reverse of valueToString().
    //!
    //! `@Invalid()` is Invalid. `@String(...)` and `@ByteArray(...)` are the text inside. A
    //! leading "@@" loses one '@'. Every other text, including the Qt markers that this library
    //! does not support, is a String as it is.
    SettingsValue SettingsIniCodec::stringToValue
        (
        const std::string& aText  //!< The text, after unescapeStringList().
        )
    {
        if( !aText.empty() && aText[0] == '@' )
        {
            if( aText.back() == ')' )
            {
                if( startsWith( aText, "@ByteArray(" ) )
                {
                    return SettingsValue( aText.substr( 11, aText.size() - 12 ) );
                }
                if( startsWith( aText, "@String(" ) )
                {
                    return SettingsValue( aText.substr( 8, aText.size() - 9 ) );
                }
                if( aText == "@Invalid()" )
                {
                    return SettingsValue();
                }
            }
            if( startsWith( aText, "@@" ) )
            {
                return SettingsValue( aText.substr( 1 ) );
            }
        }
        return SettingsValue( aText );
    }

    //! Reads the elements of a list value. Each element goes through stringToValue(), so that
    //! "@@x" becomes "@x" and `@Invalid()` becomes an empty string.
    SettingsValue SettingsIniCodec::stringListToValue
        (
        const std::vector<std::string>& aList  //!< The elements, after unescapeStringList().
        )
    {
        std::vector<std::string> result;
        result.reserve( aList.size() );
        for( const std::string& element : aList )
        {
            result.push_back( stringToValue( element ).toString() );
        }
        return SettingsValue( std::move( result ) );
    }

    //! Finds the next logical line in @p aData, from @p aDataPos.
    //!
    //! A logical line can be longer than a physical one: a line break inside quotes, or after a
    //! backslash, does not end it. A line that starts with ';' is a comment and is skipped, and so
    //! are empty lines. A ';' later in the line, outside quotes, ends the line, so the rest of it
    //! comes back as the next line, which starts with ';' and is skipped then.
    //!
    //! @return true if a line was found. Then @p aLineStart and @p aLineLength give it, and
    //!         @p aEqualsPos is the index in @p aData of its first '=' outside quotes, or npos.
    bool SettingsIniCodec::readLine
        (
        const std::string& aData,   //!< The whole file.
        std::size_t& aDataPos,      //!< Where to start. Moved past the line.
        std::size_t& aLineStart,    //!< Receives the start of the line.
        std::size_t& aLineLength,   //!< Receives the length of the line.
        std::size_t& aEqualsPos     //!< Receives the position of the '=', or npos.
        )
    {
        const std::size_t dataLength = aData.size();
        bool inQuotes = false;
        aEqualsPos = std::string::npos;

        aLineStart = aDataPos;
        while( aLineStart < dataLength && ( traitsOf( aData[aLineStart] ) & kSpace ) != 0 )
        {
            ++aLineStart;
        }

        std::size_t i = aLineStart;
        bool lineEnded = false;
        while( i < dataLength && !lineEnded )
        {
            char ch = aData[i];
            while( ( traitsOf( ch ) & kSpecial ) == 0 )
            {
                if( ++i == dataLength )
                {
                    break;
                }
                ch = aData[i];
            }
            if( i == dataLength )
            {
                break;
            }

            ++i;
            if( ch == '=' )
            {
                if( !inQuotes && aEqualsPos == std::string::npos )
                {
                    aEqualsPos = i - 1;
                }
            }
            else if( ch == '\n' || ch == '\r' )
            {
                if( i == aLineStart + 1 )
                {
                    ++aLineStart;
                }
                else if( !inQuotes )
                {
                    --i;
                    lineEnded = true;
                }
            }
            else if( ch == '\\' )
            {
                if( i < dataLength )
                {
                    const char escaped = aData[i++];
                    if( i < dataLength )
                    {
                        const char next = aData[i];
                        // \n, \r, \r\n and \n\r are all line ends in an INI file.
                        if( ( escaped == '\n' && next == '\r' ) || ( escaped == '\r' && next ==
                            '\n' ) )
                        {
                            ++i;
                        }
                    }
                }
            }
            else if( ch == '"' )
            {
                inQuotes = !inQuotes;
            }
            else
            {
                // ch is ';'.
                if( i == aLineStart + 1 )
                {
                    while( i < dataLength && aData[i] != '\n' && aData[i] != '\r' )
                    {
                        ++i;
                    }
                    while( i < dataLength && ( traitsOf( aData[i] ) & kSpace ) != 0 )
                    {
                        ++i;
                    }
                    aLineStart = i;
                }
                else if( !inQuotes )
                {
                    --i;
                    lineEnded = true;
                }
            }
        }

        aDataPos = i;
        aLineLength = i - aLineStart;
        return aLineLength > 0;
    }

    //! Reads a whole INI file into @p aKeys.
    //!
    //! A UTF-8 byte order mark at the start is skipped. A section line is `[name]`; `[General]`,
    //! with any case, is the top level, and `[%general]` is a group called "general". Each key line
    //! is `key=value`. The keys get increasing positions in the order of the file.
    //!
    //! It is a format error, but not a fatal one, if a section line has no ']', if a section name
    //! contains a '/' (which the writer never produces), or if a line is not a section, a key or a
    //! comment. Every line that can be read is still read.
    //!
    //! @return false if there was a format error.
    bool SettingsIniCodec::read
        (
        const std::string& aData,  //!< The whole file.
        KeyMap& aKeys              //!< Receives the keys.
        ) const
    {
        std::size_t start = 0;
        if( aData.compare( 0, 3, "\xEF\xBB\xBF" ) == 0 )
        {
            start = 3;
        }
        const std::string data = aData.substr( start );

        std::string currentSection;
        std::size_t dataPos = 0;
        std::size_t lineStart = 0;
        std::size_t lineLength = 0;
        std::size_t equalsPos = std::string::npos;
        std::size_t position = 0;
        bool ok = true;

        while( readLine( data, dataPos, lineStart, lineLength, equalsPos ) )
        {
            const std::string line = data.substr( lineStart, lineLength );
            if( line[0] == '[' )
            {
                std::size_t close = line.find( ']' );
                if( close == std::string::npos )
                {
                    ok = false;
                    close = line.size();
                }
                const std::string section = trimmed( line.substr( 1, close - 1 ) );
                if( equalsIgnoringCase( section, "general" ) )
                {
                    currentSection.clear();
                }
                else
                {
                    if( equalsIgnoringCase( section, "%general" ) )
                    {
                        currentSection = section.substr( 1 );
                    }
                    else
                    {
                        currentSection = unescapeKey( section );
                    }
                    if( currentSection.find( '/' ) != std::string::npos )
                    {
                        ok = false;
                    }
                    currentSection += '/';
                }
                continue;
            }

            if( equalsPos == std::string::npos )
            {
                if( line[0] != ';' )
                {
                    ok = false;
                }
                continue;
            }

            const std::size_t equalsInLine = equalsPos - lineStart;
            const std::string key = unescapeKey( trimmed( line.substr( 0, equalsInLine ) ) );
            if( key.empty() )
            {
                // Settings cannot name an empty key, so the entry could never be read or removed.
                ok = false;
                continue;
            }

            std::string text;
            std::vector<std::string> list;
            const bool isList = unescapeStringList( line.substr( equalsInLine + 1 ), text, list );

            Entry& entry = aKeys[currentSection + key];
            entry.mValue = isList ? stringListToValue( list ) : stringToValue( text );
            entry.mPosition = position++;
        }

        return ok;
    }

    //! Writes the text for one value, escaped, to @p aResult.
    //!
    //! A StringList goes through escapeStringList(), and its elements through valueToString() first
    //! so that a leading '@' is doubled. A list of one element is therefore written as that element
    //! alone, and reads back as a String. SettingsValue::toStringList() turns that String back into
    //! a list of one, so a caller that reads a list still gets the list. This is also what Qt does.
    void SettingsIniCodec::writeValue
        (
        const SettingsValue& aValue,  //!< The value.
        std::string& aResult          //!< Where to append the escaped text.
        )
    {
        if( aValue.type() == SettingsType::StringList )
        {
            std::vector<std::string> elements;
            for( const std::string& element : aValue.toStringList() )
            {
                elements.push_back( valueToString( SettingsValue( element ) ) );
            }
            escapeStringList( elements, aResult );
        }
        else
        {
            escapeString( valueToString( aValue ), aResult );
        }
    }

    //! Writes @p aKeys as an INI file.
    //!
    //! A key's section is the part before its first '/', and the top level is `[General]`. The
    //! sections come out in the order of their lowest key position, and the keys in each section in
    //! the order of their positions. So a file that was read and written again keeps its order, and
    //! a new key comes after the keys that were there. There is an empty line between sections.
    void SettingsIniCodec::write
        (
        const KeyMap& aKeys,  //!< The keys.
        std::string& aData    //!< Receives the file.
        ) const
    {
        std::map<std::string, WriterSection> sections;
        for( const auto& item : aKeys )
        {
            const std::string& fullKey = item.first;
            const std::size_t slash = fullKey.find( '/' );
            const std::string section = slash == std::string::npos ? std::string() :
                fullKey.substr( 0, slash );
            const std::string key = slash == std::string::npos ? fullKey :
                fullKey.substr( slash + 1 );

            WriterSection& writerSection = sections[section];
            writerSection.mPosition = std::min( writerSection.mPosition, item.second.mPosition );
            writerSection.mKeys[std::make_pair( item.second.mPosition, key )] = &item.second.mValue;
        }

        std::vector<std::pair<std::size_t, std::string> > order;
        order.reserve( sections.size() );
        for( const auto& section : sections )
        {
            order.push_back( std::make_pair( section.second.mPosition, section.first ) );
        }
        std::sort( order.begin(), order.end() );

        for( std::size_t index = 0; index < order.size(); ++index )
        {
            const std::string& name = order[index].second;
            std::string escaped;
            escapeKey( name, escaped );

            if( index != 0 )
            {
                aData += kEndOfLine;
            }
            if( escaped.empty() )
            {
                aData += "[General]";
            }
            else if( equalsIgnoringCase( escaped, "general" ) )
            {
                aData += "[%" + escaped + "]";
            }
            else
            {
                aData += "[" + escaped + "]";
            }
            aData += kEndOfLine;

            for( const auto& key : sections[name].mKeys )
            {
                escapeKey( key.first.second, aData );
                aData += '=';
                writeValue( *key.second, aData );
                aData += kEndOfLine;
            }
        }
    }

}
