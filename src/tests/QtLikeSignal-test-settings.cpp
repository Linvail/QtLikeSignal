// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for Settings -- the value type, the INI codec rule by rule, the store with its
//! groups and arrays, the write to disk, the write lock, and the deferred write through an event
//! loop.
//!
//! **No test here touches the real user settings folder.** Every test either names a file in its
//! own temporary directory, or calls Settings::setPath() to point the organization constructors
//! there. The fixture removes the directory afterwards, also when the test fails, so a failed run
//! leaves nothing that the next run reads.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Settings.hpp"
#include "QtLikeSignal/SettingsFileIo.hpp"
#include "QtLikeSignal/SettingsIniCodec.hpp"
#include "QtLikeSignal/SettingsLockFile.hpp"
#include "QtLikeSignal/SettingsValue.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <clocale>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined( _WIN32 )
    #include <sys/stat.h>
#endif

using namespace QtLikeSignal;

namespace
{
    //! The line end that the writer uses on this platform. See SettingsIniCodec.cpp.
    #if defined( _WIN32 )
        const std::string kEol = "\r\n";
    #else
        const std::string kEol = "\n";
    #endif

    //! Returns the escaped form of @p aText, from SettingsIniCodec::escapeString().
    std::string escaped
        (
        const std::string& aText  //!< The text.
        )
    {
        std::string result;
        SettingsIniCodec::escapeString( aText, result );
        return result;
    }

    //! Returns the escaped form of the key @p aKey, from SettingsIniCodec::escapeKey().
    std::string escapedKey
        (
        const std::string& aKey  //!< The key.
        )
    {
        std::string result;
        SettingsIniCodec::escapeKey( aKey, result );
        return result;
    }

    //! Reads the INI value text @p aText as a single string. Fails the test if it reads as a list.
    std::string unescaped
        (
        const std::string& aText  //!< The value text.
        )
    {
        std::string text;
        std::vector<std::string> list;
        EXPECT_FALSE( SettingsIniCodec::unescapeStringList( aText, text, list ) ) << aText;
        return text;
    }

    //! Reads the INI value text @p aText as a list. Fails the test if it reads as a single string.
    std::vector<std::string> unescapedList
        (
        const std::string& aText  //!< The value text.
        )
    {
        std::string text;
        std::vector<std::string> list;
        EXPECT_TRUE( SettingsIniCodec::unescapeStringList( aText, text, list ) ) << aText;
        return list;
    }

    //! A fixture that gives each test an empty temporary directory, and points the organization
    //! constructors of Settings at it.
    //!
    //! TearDown() runs also after a failed assertion, so the directory never outlives the test.
    //! setPath() is put back first, so that a test that follows cannot write into a directory that
    //! no longer exists -- or, worse, into the real user folder.
    //!
    //! std::filesystem makes and removes the directory, and nothing else. The files in it are read
    //! and written through SettingsFileIo, which takes UTF-8 paths on both platforms; a
    //! std::filesystem::path made from a std::string would take the Windows code page instead.
    class SettingsTest : public ::testing::Test
    {
    protected:
        //! Makes the directory and calls Settings::setPath().
        void SetUp() override
        {
            static std::atomic<int> sCounter { 0 };
            std::ostringstream name;
            name << "QtLikeSignal-settings-" <<
                std::chrono::steady_clock::now().time_since_epoch().count() << "-" <<
                sCounter.fetch_add( 1 );
            mDirectory = std::filesystem::temp_directory_path() / name.str();
            std::error_code error;
            std::filesystem::remove_all( mDirectory, error );
            ASSERT_TRUE( std::filesystem::create_directories( mDirectory, error ) )
                << error.message();
            Settings::setPath( SettingsFormat::Ini, directory() );
        }

        //! Puts setPath() back, and removes the directory and everything in it.
        void TearDown() override
        {
            Settings::setPath( SettingsFormat::Ini, std::string() );
            #if !defined( _WIN32 )
                // A test that made a directory read-only must not stop the removal.
                std::error_code error;
                for( const auto& entry : std::filesystem::recursive_directory_iterator(
                    mDirectory, error ) )
                {
                    std::filesystem::permissions( entry.path(),
                        std::filesystem::perms::owner_all, std::filesystem::perm_options::add,
                        error );
                }
            #endif
            std::error_code error2;
            std::filesystem::remove_all( mDirectory, error2 );
        }

        //! Returns the directory, in UTF-8.
        std::string directory() const
        {
            return mDirectory.u8string();
        }

        //! Returns the path of @p aName in the directory, in UTF-8, with the platform's separator.
        std::string pathOf
            (
            const std::string& aName  //!< A file name, which can contain '/'.
            ) const
        {
            std::string name = aName;
            std::replace( name.begin(), name.end(), '/', SettingsFileIo::separator() );
            return directory() + SettingsFileIo::separator() + name;
        }

        //! Replaces the file at @p aPath with @p aText, byte for byte, as another program would.
        static void writeFile
            (
            const std::string& aPath,  //!< The file, in UTF-8.
            const std::string& aText   //!< The bytes.
            )
        {
            ASSERT_TRUE( SettingsFileIo::writeAtomically( aPath, aText ) ) << aPath;
        }

        //! Returns the bytes of the file at @p aPath, or an empty string if it cannot be read.
        static std::string readFile
            (
            const std::string& aPath  //!< The file, in UTF-8.
            )
        {
            std::string data;
            static_cast<void>( SettingsFileIo::readAll( aPath, data ) );
            return data;
        }

        //! Returns true if the file at @p aPath exists.
        static bool exists
            (
            const std::string& aPath  //!< The file, in UTF-8.
            )
        {
            return SettingsFileIo::stamp( aPath ).mExists;
        }

        //! The directory of this test.
        std::filesystem::path mDirectory;
    };
}

// ------------------------------------------------------------------------------------------------
// SettingsValue
// ------------------------------------------------------------------------------------------------

//! Verifies that each constructor makes the type it should, and that a string literal is a string,
//! not a bool.
TEST( SettingsValueTest, EachConstructorMakesItsType )
{
    EXPECT_EQ( SettingsValue().type(), SettingsType::Invalid );
    EXPECT_FALSE( SettingsValue().isValid() );
    EXPECT_EQ( SettingsValue( true ).type(), SettingsType::Bool );
    EXPECT_EQ( SettingsValue( 5 ).type(), SettingsType::Int );
    EXPECT_EQ( SettingsValue( 5u ).type(), SettingsType::Int );
    EXPECT_EQ( SettingsValue( static_cast<short>( 5 ) ).type(), SettingsType::Int );
    EXPECT_EQ( SettingsValue( 5LL ).type(), SettingsType::Int );
    EXPECT_EQ( SettingsValue( 1.5 ).type(), SettingsType::Double );
    EXPECT_EQ( SettingsValue( 1.5f ).type(), SettingsType::Double );
    EXPECT_EQ( SettingsValue( "text" ).type(), SettingsType::String );
    EXPECT_EQ( SettingsValue( std::string( "text" ) ).type(), SettingsType::String );
    EXPECT_EQ( SettingsValue( std::vector<std::string> { "a" } ).type(), SettingsType::StringList );
    EXPECT_EQ( SettingsValue( static_cast<const char*>( nullptr ) ).type(), SettingsType::Invalid );
}

//! Verifies that an unsigned value too large for a long long is Invalid, not a wrapped negative
//! number.
TEST( SettingsValueTest, AnUnsignedValueAboveLongLongIsInvalid )
{
    EXPECT_EQ( SettingsValue( static_cast<unsigned long long>( LLONG_MAX ) ).toLongLong(),
        LLONG_MAX );
    EXPECT_FALSE( SettingsValue( static_cast<unsigned long long>( LLONG_MAX ) + 1ULL ).isValid() );
}

//! Verifies the conversions from a String, which is what every value read from a file is.
TEST( SettingsValueTest, ATextValueConvertsToEachType )
{
    bool ok = false;
    EXPECT_EQ( SettingsValue( "42" ).toInt( &ok ), 42 );
    EXPECT_TRUE( ok );
    EXPECT_EQ( SettingsValue( " +42 " ).toInt( &ok ), 42 );
    EXPECT_TRUE( ok );
    EXPECT_EQ( SettingsValue( "-7" ).toLongLong( &ok ), -7 );
    EXPECT_TRUE( ok );
    EXPECT_DOUBLE_EQ( SettingsValue( "2.5" ).toDouble( &ok ), 2.5 );
    EXPECT_TRUE( ok );
    EXPECT_TRUE( SettingsValue( "TRUE" ).toBool( &ok ) );
    EXPECT_TRUE( ok );
    EXPECT_FALSE( SettingsValue( "0" ).toBool( &ok ) );
    EXPECT_TRUE( ok );
    EXPECT_EQ( SettingsValue( "a" ).toStringList(), std::vector<std::string> { "a" } );

    EXPECT_EQ( SettingsValue( "12.5" ).toInt( &ok ), 0 );
    EXPECT_FALSE( ok );
    EXPECT_EQ( SettingsValue( "0x10" ).toInt( &ok ), 0 );
    EXPECT_FALSE( ok );
    EXPECT_EQ( SettingsValue( "3000000000" ).toInt( &ok ), 0 );
    EXPECT_FALSE( ok ) << "out of range for int must fail, not truncate";
    EXPECT_FALSE( SettingsValue( "flase" ).toBool( &ok ) );
    EXPECT_FALSE( ok ) << "a misspelt bool must fail, not read as true";
    EXPECT_DOUBLE_EQ( SettingsValue( "abc" ).toDouble( &ok ), 0.0 );
    EXPECT_FALSE( ok );
}

//! Verifies the conversions to a string, including the shortest exact form of a double.
TEST( SettingsValueTest, EachTypeConvertsToText )
{
    EXPECT_EQ( SettingsValue( true ).toString(), "true" );
    EXPECT_EQ( SettingsValue( false ).toString(), "false" );
    EXPECT_EQ( SettingsValue( -123 ).toString(), "-123" );
    EXPECT_EQ( SettingsValue( 0.1 ).toString(), "0.1" );
    EXPECT_EQ( SettingsValue( 100.0 ).toString(), "100" );
    EXPECT_EQ( SettingsValue( std::vector<std::string> { "only" } ).toString(), "only" );

    bool ok = true;
    EXPECT_EQ( SettingsValue( std::vector<std::string> { "a", "b" } ).toString( &ok ), "" );
    EXPECT_FALSE( ok );
    EXPECT_EQ( SettingsValue().toString( &ok ), "" );
    EXPECT_FALSE( ok );
}

//! Verifies that Invalid reads as an empty list, because that is how the file keeps one.
TEST( SettingsValueTest, InvalidIsAnEmptyList )
{
    bool ok = false;
    EXPECT_TRUE( SettingsValue().toStringList( &ok ).empty() );
    EXPECT_TRUE( ok );
}

//! Verifies to<T>() on the types it supports, including its range check.
TEST( SettingsValueTest, ToConvertsAndChecksTheRange )
{
    bool ok = false;
    EXPECT_EQ( SettingsValue( 200 ).to<unsigned char>( &ok ), 200 );
    EXPECT_TRUE( ok );
    EXPECT_EQ( SettingsValue( 300 ).to<unsigned char>( &ok ), 0 );
    EXPECT_FALSE( ok );
    EXPECT_EQ( SettingsValue( -1 ).to<unsigned int>( &ok ), 0U );
    EXPECT_FALSE( ok );
    EXPECT_FLOAT_EQ( SettingsValue( "1.25" ).to<float>( &ok ), 1.25f );
    EXPECT_TRUE( ok );
    EXPECT_EQ( SettingsValue( 7 ).to<std::string>( &ok ), "7" );
    EXPECT_EQ( SettingsValue( 7 ).to<SettingsValue>( &ok ), SettingsValue( 7 ) );
    EXPECT_EQ( SettingsValue( 2.6 ).to<long long>( &ok ), 3 );
}

//! Verifies that equality compares type and data, with no conversion.
TEST( SettingsValueTest, EqualityHasNoConversion )
{
    EXPECT_EQ( SettingsValue( 5 ), SettingsValue( 5LL ) );
    EXPECT_NE( SettingsValue( 5 ), SettingsValue( "5" ) );
    EXPECT_NE( SettingsValue( 5 ), SettingsValue( 5.0 ) );
    EXPECT_EQ( SettingsValue(), SettingsValue() );
}

//! Verifies that a double is read and written with '.', whatever the C locale is.
//!
//! A locale with ',' as the decimal point must not turn "1.5" into 1, and must not write 1.5 as
//! "1,5" -- which in an INI value is a list of two strings.
TEST( SettingsValueTest, NumbersIgnoreTheLocale )
{
    const char* const previous = std::setlocale( LC_NUMERIC, nullptr );
    const std::string saved = previous != nullptr ? previous : "C";
    if( std::setlocale( LC_NUMERIC, "de_DE.UTF-8" ) == nullptr &&
        std::setlocale( LC_NUMERIC, "German_Germany.1252" ) == nullptr )
    {
        GTEST_SKIP() << "no locale with a decimal comma is installed";
    }

    const std::string written = SettingsValue( 1.5 ).toString();
    bool ok = false;
    const double read = SettingsValue( "1.5" ).toDouble( &ok );
    std::setlocale( LC_NUMERIC, saved.c_str() );

    EXPECT_EQ( written, "1.5" );
    EXPECT_TRUE( ok );
    EXPECT_DOUBLE_EQ( read, 1.5 );
}

// ------------------------------------------------------------------------------------------------
// SettingsIniCodec, rule by rule
// ------------------------------------------------------------------------------------------------

//! Verifies the key escapes: '/' to '\', and %XX, %UXXXX for everything but letters, digits, '_',
//! '-' and '.'.
TEST( SettingsIniCodecTest, KeysAreEscapedAsQtEscapesThem )
{
    EXPECT_EQ( escapedKey( "abc_XYZ-0.9" ), "abc_XYZ-0.9" );
    EXPECT_EQ( escapedKey( "a/b" ), "a\\b" );
    EXPECT_EQ( escapedKey( "a b" ), "a%20b" );
    EXPECT_EQ( escapedKey( "a=b" ), "a%3Db" );
    EXPECT_EQ( escapedKey( "\xC3\xA4" ), "%E4" );                    // U+00E4
    EXPECT_EQ( escapedKey( "\xE2\x82\xAC" ), "%U20AC" );             // U+20AC
    EXPECT_EQ( escapedKey( "\xF0\x9F\x98\x80" ), "%UD83D%UDE00" );   // U+1F600, two halves

    for( const std::string key : { "a/b", "a b=c", "\xC3\xA4", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
                                   "100%" } )
    {
        EXPECT_EQ( SettingsIniCodec::unescapeKey( escapedKey( key ) ), key ) << key;
    }
}

//! Verifies that a key that a person wrote in the file without escapes also reads, and that a '%'
//! without hex digits stays a '%'.
TEST( SettingsIniCodecTest, AnUnescapedKeyStillReads )
{
    EXPECT_EQ( SettingsIniCodec::unescapeKey( "\xC3\xA4" ), "\xC3\xA4" );
    EXPECT_EQ( SettingsIniCodec::unescapeKey( "50%" ), "50%" );
    EXPECT_EQ( SettingsIniCodec::unescapeKey( "%zz" ), "%zz" );
    EXPECT_EQ( SettingsIniCodec::unescapeKey( "%U12" ), "%U12" );
}

//! Verifies when a value is put in quotes.
TEST( SettingsIniCodecTest, AValueIsQuotedWhenItMustBe )
{
    EXPECT_EQ( escaped( "plain" ), "plain" );
    EXPECT_EQ( escaped( "a;b" ), "\"a;b\"" );
    EXPECT_EQ( escaped( "a,b" ), "\"a,b\"" );
    EXPECT_EQ( escaped( "a=b" ), "\"a=b\"" );
    EXPECT_EQ( escaped( " lead" ), "\" lead\"" );
    EXPECT_EQ( escaped( "trail " ), "\"trail \"" );
    EXPECT_EQ( escaped( "in side" ), "in side" );
}

//! Verifies the backslash escapes, and the rule that a hex digit after `\0` or `\xHH` is escaped
//! too.
TEST( SettingsIniCodecTest, ControlCharactersAreEscaped )
{
    EXPECT_EQ( escaped( "a\"b\\c" ), "a\\\"b\\\\c" );
    EXPECT_EQ( escaped( "\n\r\t\a\b\f\v" ), "\\n\\r\\t\\a\\b\\f\\v" );
    EXPECT_EQ( escaped( std::string( "x\0y", 3 ) ), "x\\0y" );
    EXPECT_EQ( escaped( "\x01" "z" ), "\\x1z" );
    EXPECT_EQ( escaped( "\x1f" ), "\\x1f" );

    // Without the rule, "\x1" and then "2" would read back as the one character 0x12.
    EXPECT_EQ( escaped( "\x01" "2" ), "\\x1\\x32" );
    EXPECT_EQ( escaped( std::string( "\0" "12", 3 ) ), "\\0\\x31\\x32" );
    EXPECT_EQ( unescaped( escaped( "\x01" "2" ) ), "\x01" "2" );
}

//! Verifies that non-ASCII text goes out as UTF-8, unchanged.
TEST( SettingsIniCodecTest, NonAsciiTextIsKept )
{
    const std::string text = "Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC \xF0\x9F\x98\x80";
    EXPECT_EQ( escaped( text ), text );
    EXPECT_EQ( unescaped( escaped( text ) ), text );
}

//! Verifies how a list is written: joined with ", ", and an empty list as @Invalid().
TEST( SettingsIniCodecTest, AListIsWrittenJoinedAndAnEmptyOneAsInvalid )
{
    std::string text;
    SettingsIniCodec::escapeStringList( { "a", "b,c", "" }, text );
    EXPECT_EQ( text, "a, \"b,c\", " );
    EXPECT_EQ( unescapedList( text ), ( std::vector<std::string> { "a", "b,c", "" } ) );

    text.clear();
    SettingsIniCodec::escapeStringList( {}, text );
    EXPECT_EQ( text, "@Invalid()" );
}

//! Verifies the reader's extra forms: trimmed ends outside quotes, kept inside, `\?`, `\'`, octal
//! and hex escapes, a dropped unknown escape, and a line continuation.
TEST( SettingsIniCodecTest, TheReaderAcceptsMoreThanTheWriter )
{
    EXPECT_EQ( unescaped( "  value   " ), "value" );
    EXPECT_EQ( unescaped( "\"  value  \"" ), "  value  " );
    EXPECT_EQ( unescaped( "\\?\\'" ), "?'" );
    EXPECT_EQ( unescaped( "\\101\\x42" ), "AB" );
    EXPECT_EQ( unescaped( "\\xe9" ), "\xC3\xA9" ) << "a \\x escape names a code unit, not a byte";
    EXPECT_EQ( unescaped( "a\\qb" ), "ab" );
    EXPECT_EQ( unescaped( "one\\\ntwo" ), "onetwo" );
    EXPECT_EQ( unescapedList( "a , b ,c" ), ( std::vector<std::string> { "a", "b", "c" } ) );
}

//! Verifies the markers for a single value: '@' doubled, @Invalid(), @String() and the Qt markers
//! that this library reads as text.
TEST( SettingsIniCodecTest, TheAtMarkersRoundTrip )
{
    EXPECT_EQ( SettingsIniCodec::valueToString( SettingsValue( "@home" ) ), "@@home" );
    EXPECT_EQ( SettingsIniCodec::stringToValue( "@@home" ), SettingsValue( "@home" ) );
    EXPECT_EQ( SettingsIniCodec::valueToString( SettingsValue() ), "@Invalid()" );
    EXPECT_EQ( SettingsIniCodec::stringToValue( "@Invalid()" ), SettingsValue() );

    const std::string withNul( "a\0b", 3 );
    EXPECT_EQ( SettingsIniCodec::valueToString( SettingsValue( withNul ) ),
        "@String(" + withNul + ")" );
    EXPECT_EQ( SettingsIniCodec::stringToValue( "@String(" + withNul + ")" ),
        SettingsValue( withNul ) );

    EXPECT_EQ( SettingsIniCodec::stringToValue( "@ByteArray(raw)" ), SettingsValue( "raw" ) );
    EXPECT_EQ( SettingsIniCodec::stringToValue( "@Rect(1 2 3 4)" ),
        SettingsValue( "@Rect(1 2 3 4)" ) );
    EXPECT_EQ( SettingsIniCodec::stringToValue( "@Variant(\\0\\0\\0\\x7f)" ),
        SettingsValue( "@Variant(\\0\\0\\0\\x7f)" ) );
}

//! Verifies that every kind of value, and every awkward string, survives a write and a read.
TEST( SettingsIniCodecTest, EveryValueRoundTripsThroughAFile )
{
    const std::vector<std::string> awkward = {
        "", " ", "a;b", "a,b", "a=b", " both ", "\"quoted\"", "back\\slash", "@at", "@@two",
        std::string( "nul\0mid", 7 ), "\x01" "2", "tab\there", "line\nbreak", "Gr\xC3\xBC\xC3\x9F"
        "e",
        "[section]", "; not a comment"
    };

    SettingsCodec::KeyMap keys;
    std::size_t position = 0;
    for( std::size_t i = 0; i < awkward.size(); ++i )
    {
        keys["strings/s" + std::to_string( i )] = { SettingsValue( awkward[i] ), position++ };
    }
    keys["lists/empty"] = { SettingsValue( std::vector<std::string>() ), position++ };
    keys["lists/one"] = { SettingsValue( std::vector<std::string> { "x" } ), position++ };
    keys["lists/many"] = { SettingsValue( awkward ), position++ };
    keys["numbers/int"] = { SettingsValue( -12345678901LL ), position++ };
    keys["numbers/double"] = { SettingsValue( 0.1 ), position++ };
    keys["numbers/bool"] = { SettingsValue( true ), position++ };
    keys["top"] = { SettingsValue( "level" ), position++ };
    keys["deep/a/b/c"] = { SettingsValue( "nested" ), position++ };

    const SettingsCodec& codec = SettingsCodec::forFormat( SettingsFormat::Ini );
    std::string file;
    codec.write( keys, file );
    SettingsCodec::KeyMap read;
    ASSERT_TRUE( codec.read( file, read ) ) << file;

    for( std::size_t i = 0; i < awkward.size(); ++i )
    {
        const std::string key = "strings/s" + std::to_string( i );
        ASSERT_EQ( read.count( key ), 1U ) << key;
        EXPECT_EQ( read[key].mValue.toString(), awkward[i] ) << key << " in\n" << file;
    }
    EXPECT_TRUE( read["lists/empty"].mValue.toStringList().empty() );
    EXPECT_EQ( read["lists/one"].mValue.toStringList(), std::vector<std::string> { "x" } );
    EXPECT_EQ( read["lists/many"].mValue.toStringList(), awkward );
    EXPECT_EQ( read["numbers/int"].mValue.toLongLong(), -12345678901LL );
    EXPECT_EQ( read["numbers/double"].mValue.toDouble(), 0.1 );
    EXPECT_TRUE( read["numbers/bool"].mValue.toBool() );
    EXPECT_EQ( read["top"].mValue.toString(), "level" );
    EXPECT_EQ( read["deep/a/b/c"].mValue.toString(), "nested" );
    EXPECT_EQ( read.size(), keys.size() );
}

//! Verifies [General] for the top level, and [%general] for a group called "general", with its
//! case kept.
TEST( SettingsIniCodecTest, TheGeneralSectionsAreKeptApart )
{
    SettingsCodec::KeyMap keys;
    keys["top"] = { SettingsValue( "1" ), 0 };
    keys["general/inner"] = { SettingsValue( "2" ), 1 };
    keys["General/other"] = { SettingsValue( "3" ), 2 };

    const SettingsCodec& codec = SettingsCodec::forFormat( SettingsFormat::Ini );
    std::string file;
    codec.write( keys, file );
    EXPECT_EQ( file, "[General]" + kEol + "top=1" + kEol + kEol + "[%general]" + kEol + "inner=2" +
        kEol + kEol + "[%General]" + kEol + "other=3" + kEol );

    SettingsCodec::KeyMap read;
    ASSERT_TRUE( codec.read( file, read ) );
    EXPECT_EQ( read["top"].mValue, SettingsValue( "1" ) );
    EXPECT_EQ( read["general/inner"].mValue, SettingsValue( "2" ) );
    EXPECT_EQ( read["General/other"].mValue, SettingsValue( "3" ) );
}

//! Verifies that a file written by hand keeps its order of sections and keys when written again,
//! and that a new key goes after the old ones.
TEST( SettingsIniCodecTest, TheOrderOfAFileIsKept )
{
    const std::string original = "[zeta]" + kEol + "b=1" + kEol + "a=2" + kEol + kEol + "[alpha]" +
        kEol + "y=3" + kEol + "x=4" + kEol;
    const SettingsCodec& codec = SettingsCodec::forFormat( SettingsFormat::Ini );
    SettingsCodec::KeyMap keys;
    ASSERT_TRUE( codec.read( original, keys ) );

    std::string again;
    codec.write( keys, again );
    EXPECT_EQ( again, original );

    keys["zeta/c"] = { SettingsValue( "5" ), 1000 };
    std::string withNew;
    codec.write( keys, withNew );
    EXPECT_EQ( withNew, "[zeta]" + kEol + "b=1" + kEol + "a=2" + kEol + "c=5" + kEol + kEol +
        "[alpha]" + kEol + "y=3" + kEol + "x=4" + kEol );
}

//! Verifies that comments, blank lines, a BOM, CRLF line ends and spaces around '=' are all read.
TEST( SettingsIniCodecTest, AHandWrittenFileReads )
{
    const std::string file =
        "\xEF\xBB\xBF; a comment\r\n"
        "\r\n"
        "[General]\r\n"
        "  name = Chart plotter   ; trailing comment\r\n"
        "[window]\r\n"
        "geometry\\x=10\r\n"
        "list=a, b\r\n"
        "quoted=\"a;b\"\r\n";
    SettingsCodec::KeyMap keys;
    ASSERT_TRUE( SettingsCodec::forFormat( SettingsFormat::Ini ).read( file, keys ) );
    EXPECT_EQ( keys["name"].mValue, SettingsValue( "Chart plotter" ) );
    EXPECT_EQ( keys["window/geometry/x"].mValue, SettingsValue( "10" ) );
    EXPECT_EQ( keys["window/list"].mValue, SettingsValue( std::vector<std::string> { "a", "b" } ) );
    EXPECT_EQ( keys["window/quoted"].mValue, SettingsValue( "a;b" ) );
    EXPECT_EQ( keys.size(), 4U );
}

//! Verifies that a malformed file is a format error, and that the keys it has are still read.
TEST( SettingsIniCodecTest, AMalformedFileStillGivesItsKeys )
{
    const std::string file = "[good]" + kEol + "a=1" + kEol + "not a key line" + kEol +
        "[broken" + kEol + "b=2" + kEol;
    SettingsCodec::KeyMap keys;
    EXPECT_FALSE( SettingsCodec::forFormat( SettingsFormat::Ini ).read( file, keys ) );
    EXPECT_EQ( keys["good/a"].mValue, SettingsValue( "1" ) );
    EXPECT_EQ( keys.count( "broken/b" ), 1U );
}

// ------------------------------------------------------------------------------------------------
// The write lock
// ------------------------------------------------------------------------------------------------

//! Verifies that a second holder waits until the first unlocks, and that unlocking removes the
//! lock file.
TEST_F( SettingsTest, TheLockIsExclusiveAndLeavesNothingBehind )
{
    const std::string path = pathOf( "file.ini.lock" );
    SettingsLockFile first( path );
    SettingsLockFile second( path );

    ASSERT_TRUE( first.tryLock( 0 ) );
    EXPECT_TRUE( first.isLocked() );
    EXPECT_FALSE( second.tryLock( 0 ) );
    EXPECT_FALSE( second.tryLock( 50 ) );

    first.unlock();
    EXPECT_FALSE( exists( path ) ) << "unlock must remove the lock file";
    EXPECT_TRUE( second.tryLock( 0 ) );
    second.unlock();
    EXPECT_FALSE( exists( path ) );
}

// ------------------------------------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------------------------------------

//! Verifies the basic cycle: set, read back, keep across two objects, and the file that results.
TEST_F( SettingsTest, AValueIsWrittenAndReadBack )
{
    const std::string file = pathOf( "basic.ini" );
    {
        Settings settings( file, SettingsFormat::Ini );
        EXPECT_EQ( settings.status(), SettingsStatus::NoError );
        EXPECT_EQ( settings.fileName(), file );
        settings.setValue( "width", 800 );
        settings.setValue( "title", "Chart plotter" );
        settings.setValue( "window/maximized", true );
        EXPECT_EQ( settings.value( "width", 0 ), 800 );
        EXPECT_FALSE( exists( file ) ) << "nothing is written before sync or destruction";
    }

    EXPECT_EQ( readFile( file ), "[General]" + kEol + "width=800" + kEol + "title=Chart plotter" +
        kEol + kEol + "[window]" + kEol + "maximized=true" + kEol );

    Settings again( file, SettingsFormat::Ini );
    EXPECT_EQ( again.value( "width", 0 ), 800 );
    EXPECT_EQ( again.value( "title", "" ), "Chart plotter" );
    EXPECT_TRUE( again.value( "window/maximized", false ) );
    EXPECT_EQ( again.value( "width" ).type(), SettingsType::String )
        << "a value read from the file is text";
}

//! Verifies value() with a default: a missing key and a value that does not convert both give it.
TEST_F( SettingsTest, TheDefaultIsGivenForAMissingOrWrongValue )
{
    Settings settings( pathOf( "defaults.ini" ), SettingsFormat::Ini );
    settings.setValue( "name", "not a number" );

    EXPECT_EQ( settings.value( "missing", 5 ), 5 );
    EXPECT_EQ( settings.value( "name", 5 ), 5 );
    EXPECT_EQ( settings.value( "name" ).toInt(), 0 ) << "the QSettings form gives 0";
    EXPECT_FALSE( settings.value( "missing" ).isValid() );
    EXPECT_EQ( settings.value( "missing", SettingsValue( "d" ) ), SettingsValue( "d" ) );
    EXPECT_EQ( settings.value( "missing", "text" ), "text" );
    EXPECT_EQ( settings.value( "missing", std::vector<std::string> { "x" } ),
        std::vector<std::string> { "x" } );
}

//! Verifies that keys are case-sensitive, on every platform.
TEST_F( SettingsTest, KeysAreCaseSensitive )
{
    Settings settings( pathOf( "case.ini" ), SettingsFormat::Ini );
    settings.setValue( "Key", 1 );
    settings.setValue( "key", 2 );
    settings.sync();

    Settings again( pathOf( "case.ini" ), SettingsFormat::Ini );
    EXPECT_EQ( again.value( "Key", 0 ), 1 );
    EXPECT_EQ( again.value( "key", 0 ), 2 );
    EXPECT_FALSE( again.contains( "KEY" ) );
}

//! Verifies key normalizing: repeated, leading and trailing slashes do not matter, and an empty
//! key is refused.
TEST_F( SettingsTest, SlashesInAKeyAreNormalized )
{
    Settings settings( pathOf( "slashes.ini" ), SettingsFormat::Ini );
    settings.setValue( "/a//b///", 1 );
    EXPECT_TRUE( settings.contains( "a/b" ) );
    EXPECT_EQ( settings.value( "//a/b", 0 ), 1 );
    EXPECT_EQ( settings.allKeys(), std::vector<std::string> { "a/b" } );

    settings.setValue( "///", 2 );
    EXPECT_EQ( settings.allKeys().size(), 1U ) << "an empty key must be ignored";
    EXPECT_FALSE( settings.contains( "" ) );
}

//! Verifies nested groups, and allKeys(), childKeys() and childGroups() relative to the group.
TEST_F( SettingsTest, GroupsNestAndListTheirChildren )
{
    Settings settings( pathOf( "groups.ini" ), SettingsFormat::Ini );
    settings.setValue( "top", 0 );
    settings.beginGroup( "window" );
    EXPECT_EQ( settings.group(), "window" );
    settings.setValue( "width", 800 );
    settings.beginGroup( "geometry" );
    EXPECT_EQ( settings.group(), "window/geometry" );
    settings.setValue( "x", 10 );
    settings.setValue( "y", 20 );
    settings.endGroup();
    settings.setValue( "panel/left", true );

    EXPECT_EQ( settings.allKeys(),
        ( std::vector<std::string> { "geometry/x", "geometry/y", "panel/left", "width" } ) );
    EXPECT_EQ( settings.childKeys(), std::vector<std::string> { "width" } );
    EXPECT_EQ( settings.childGroups(), ( std::vector<std::string> { "geometry", "panel" } ) );
    settings.endGroup();

    EXPECT_EQ( settings.group(), "" );
    EXPECT_EQ( settings.childKeys(), std::vector<std::string> { "top" } );
    EXPECT_EQ( settings.childGroups(), std::vector<std::string> { "window" } );
    EXPECT_EQ( settings.value( "window/geometry/y", 0 ), 20 );

    settings.endGroup();  // No group open: reported, and nothing happens.
    EXPECT_EQ( settings.group(), "" );
}

//! Verifies that a key several groups deep is written as "a\b" in its section, and reads back.
TEST_F( SettingsTest, ADeepKeyUsesBackslashesInItsSection )
{
    const std::string file = pathOf( "deep.ini" );
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.setValue( "window/geometry/x", 10 );
    }
    EXPECT_EQ( readFile( file ), "[window]" + kEol + "geometry\\x=10" + kEol );
    Settings again( file, SettingsFormat::Ini );
    EXPECT_EQ( again.value( "window/geometry/x", 0 ), 10 );
}

//! Verifies remove(): a key and everything under it, and remove( "" ) for the current group.
TEST_F( SettingsTest, RemoveTakesAKeyAndEverythingUnderIt )
{
    const std::string file = pathOf( "remove.ini" );
    writeFile( file, "[General]" + kEol + "keep=1" + kEol + kEol + "[a]" + kEol + "x=1" + kEol +
        "b\\y=2" + kEol + kEol + "[c]" + kEol + "z=3" + kEol );
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.setValue( "a/new", 4 );
        settings.remove( "a" );
        EXPECT_FALSE( settings.contains( "a/x" ) );
        EXPECT_FALSE( settings.contains( "a/b/y" ) );
        EXPECT_FALSE( settings.contains( "a/new" ) );

        settings.beginGroup( "c" );
        settings.remove( "" );
        settings.endGroup();
        EXPECT_FALSE( settings.contains( "c/z" ) );
        EXPECT_EQ( settings.allKeys(), std::vector<std::string> { "keep" } );
    }
    EXPECT_EQ( readFile( file ), "[General]" + kEol + "keep=1" + kEol );
}

//! Verifies clear(), and that remove( "" ) at the top level does the same.
TEST_F( SettingsTest, ClearRemovesEverything )
{
    const std::string file = pathOf( "clear.ini" );
    writeFile( file, "[General]" + kEol + "a=1" + kEol + kEol + "[g]" + kEol + "b=2" + kEol );
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.beginGroup( "g" );
        settings.clear();
        settings.endGroup();
        EXPECT_TRUE( settings.allKeys().empty() );
    }
    EXPECT_EQ( readFile( file ), "" );

    writeFile( file, "[General]" + kEol + "a=1" + kEol );
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.remove( "" );
        EXPECT_TRUE( settings.allKeys().empty() );
    }
    EXPECT_EQ( readFile( file ), "" );
}

//! Verifies a written array: one-based elements, and a size counted from the indices used.
TEST_F( SettingsTest, AnArrayIsWrittenAndReadBack )
{
    const std::string file = pathOf( "array.ini" );
    const std::vector<std::string> paths = { "/charts/a.kap", "/charts/b.kap", "/charts/c.kap" };
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.beginWriteArray( "recent" );
        for( std::size_t i = 0; i < paths.size(); ++i )
        {
            settings.setArrayIndex( static_cast<int>( i ) );
            settings.setValue( "path", paths[i] );
        }
        settings.endArray();
        EXPECT_EQ( settings.group(), "" );
        EXPECT_EQ( settings.value( "recent/size", 0 ), 3 );
        EXPECT_EQ( settings.value( "recent/1/path", "" ), paths[0] );
    }

    Settings settings( file, SettingsFormat::Ini );
    std::vector<std::string> read;
    const int size = settings.beginReadArray( "recent" );
    for( int i = 0; i < size; ++i )
    {
        settings.setArrayIndex( i );
        read.push_back( settings.value( "path", std::string() ) );
    }
    settings.endArray();
    EXPECT_EQ( read, paths );
    EXPECT_EQ( settings.value( "recent/size", 0 ), 3 ) << "reading must not change the size";
}

//! Verifies an array written with a fixed size, and the warnings for misuse.
TEST_F( SettingsTest, AnArrayWithAGivenSizeKeepsIt )
{
    Settings settings( pathOf( "sized.ini" ), SettingsFormat::Ini );
    settings.beginWriteArray( "items", 5 );
    settings.setArrayIndex( 0 );
    settings.setValue( "name", "first" );
    settings.endArray();
    EXPECT_EQ( settings.value( "items/size", 0 ), 5 );

    settings.setArrayIndex( 3 );  // No array open: reported, and nothing happens.
    EXPECT_EQ( settings.group(), "" );
    EXPECT_EQ( settings.beginReadArray( "none" ), 0 );
    settings.endArray();
}

//! Verifies that two Settings objects on one file see each other's changes before any write.
TEST_F( SettingsTest, TwoObjectsOnOneFileShareTheirChanges )
{
    const std::string file = pathOf( "shared.ini" );
    Settings first( file, SettingsFormat::Ini );
    Settings second( file, SettingsFormat::Ini );
    first.setValue( "key", "from first" );
    EXPECT_EQ( second.value( "key", "" ), "from first" );
    second.remove( "key" );
    EXPECT_FALSE( first.contains( "key" ) );
}

//! Verifies that a write merges with the file on disk: a key that another program wrote in the
//! meantime is kept, and the change made here is added.
TEST_F( SettingsTest, AWriteMergesWithTheFileOnDisk )
{
    const std::string file = pathOf( "merge.ini" );
    writeFile( file, "[General]" + kEol + "old=1" + kEol );
    Settings settings( file, SettingsFormat::Ini );
    EXPECT_EQ( settings.value( "old", 0 ), 1 );

    settings.setValue( "mine", 2 );
    // Another program replaces the file. Its size is different, so the store sees the change.
    writeFile( file, "[General]" + kEol + "old=1" + kEol + "theirs=3" + kEol );
    settings.sync();

    EXPECT_EQ( settings.status(), SettingsStatus::NoError );
    EXPECT_EQ( settings.value( "theirs", 0 ), 3 );
    EXPECT_EQ( settings.value( "mine", 0 ), 2 );
    EXPECT_EQ( readFile( file ), "[General]" + kEol + "old=1" + kEol + "theirs=3" + kEol +
        "mine=2" +
        kEol );
}

//! Verifies that sync() with no changes reads a file that another program changed.
TEST_F( SettingsTest, SyncReadsAChangedFile )
{
    const std::string file = pathOf( "reread.ini" );
    writeFile( file, "[General]" + kEol + "v=1" + kEol );
    Settings settings( file, SettingsFormat::Ini );
    EXPECT_EQ( settings.value( "v", 0 ), 1 );

    writeFile( file, "[General]" + kEol + "v=22" + kEol );
    settings.sync();
    EXPECT_EQ( settings.value( "v", 0 ), 22 );
}

//! Verifies that a malformed file gives FormatError, and still gives the keys it has.
TEST_F( SettingsTest, AMalformedFileIsAFormatError )
{
    const std::string file = pathOf( "bad.ini" );
    writeFile( file, "[General]" + kEol + "good=1" + kEol + "garbage line" + kEol );
    Settings settings( file, SettingsFormat::Ini );
    EXPECT_EQ( settings.status(), SettingsStatus::FormatError );
    EXPECT_EQ( settings.value( "good", 0 ), 1 );
}

//! Verifies AccessError for a file that cannot be written, and that the change stays in memory.
//!
//! The directory of the file is a regular file, so it cannot be made. That fails for every user
//! on every platform, unlike a read-only directory, which an administrator can still write.
TEST_F( SettingsTest, AnUnwritablePathIsAnAccessError )
{
    const std::string blocker = pathOf( "not-a-directory" );
    writeFile( blocker, "x" );
    const std::string file = blocker + SettingsFileIo::separator() + "settings.ini";

    Settings settings( file, SettingsFormat::Ini );
    EXPECT_EQ( settings.status(), SettingsStatus::NoError ) << "a missing file reads as empty";
    EXPECT_FALSE( settings.isWritable() );
    settings.setValue( "key", 1 );
    settings.sync();
    EXPECT_EQ( settings.status(), SettingsStatus::AccessError );
    EXPECT_EQ( settings.value( "key", 0 ), 1 ) << "the change must stay in memory";

    settings.remove( "key" );  // Nothing left to write, so the destructor writes nothing.
    settings.sync();
    EXPECT_EQ( settings.status(), SettingsStatus::AccessError ) << "the first error is kept";
}

//! Verifies that the directories above a new file are made at the first write.
TEST_F( SettingsTest, MissingDirectoriesAreMade )
{
    const std::string file = pathOf( "one/two/three.ini" );
    {
        Settings settings( file, SettingsFormat::Ini );
        EXPECT_TRUE( settings.isWritable() );
        settings.setValue( "k", 1 );
    }
    EXPECT_EQ( readFile( file ), "[General]" + kEol + "k=1" + kEol );
}

//! Verifies that a write keeps the permission bits of the file.
TEST_F( SettingsTest, AWriteKeepsThePermissionsOfTheFile )
{
    #if defined( _WIN32 )
        GTEST_SKIP() << "Windows has no mode bits to keep";
    #else
        const std::string file = pathOf( "private.ini" );
        writeFile( file, "[General]" + kEol + "a=1" + kEol );
        ASSERT_EQ( ::chmod( file.c_str(), 0600 ), 0 );
        {
            Settings settings( file, SettingsFormat::Ini );
            settings.setValue( "b", 2 );
        }
        struct stat info;
        ASSERT_EQ( ::stat( file.c_str(), &info ), 0 );
        EXPECT_EQ( info.st_mode & 0777, 0600U );
    #endif
}

//! Verifies that a write leaves no temporary file and no lock file behind.
TEST_F( SettingsTest, AWriteLeavesNothingBehind )
{
    const std::string file = pathOf( "tidy.ini" );
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.setValue( "k", 1 );
    }
    std::vector<std::string> names;
    for( const auto& entry : std::filesystem::directory_iterator( mDirectory ) )
    {
        names.push_back( entry.path().filename().u8string() );
    }
    EXPECT_EQ( names, std::vector<std::string> { "tidy.ini" } );
}

//! Verifies where the organization constructor puts the file, with and without an application.
TEST_F( SettingsTest, TheOrganizationConstructorNamesTheFile )
{
    const char separator = SettingsFileIo::separator();
    {
        Settings settings( "Example", "Chartplotter" );
        EXPECT_EQ( settings.fileName(),
            directory() + separator + "Example" + separator + "Chartplotter.ini" );
        EXPECT_EQ( settings.organizationName(), "Example" );
        EXPECT_EQ( settings.applicationName(), "Chartplotter" );
        settings.setValue( "k", 1 );
    }
    EXPECT_TRUE( exists( pathOf( "Example/Chartplotter.ini" ) ) );

    Settings organizationOnly( "Example" );
    EXPECT_EQ( organizationOnly.fileName(), directory() + separator + "Example.ini" );
    EXPECT_EQ( organizationOnly.status(), SettingsStatus::NoError );
}

//! Verifies that an empty organization is an AccessError, and uses "Unknown Organization".
TEST_F( SettingsTest, AnEmptyOrganizationIsAnAccessError )
{
    Settings settings( "", "App" );
    EXPECT_EQ( settings.status(), SettingsStatus::AccessError );
    const char separator = SettingsFileIo::separator();
    EXPECT_EQ( settings.fileName(),
        directory() + separator + "Unknown Organization" + separator + "App.ini" );
}

//! Verifies that the default constructor uses the names from CoreApplication.
TEST_F( SettingsTest, TheDefaultConstructorUsesTheApplicationNames )
{
    const std::string savedOrganization = CoreApplication::organizationName();
    const std::string savedApplication = CoreApplication::applicationName();
    CoreApplication::setOrganizationName( "Org" );
    CoreApplication::setApplicationName( "App" );

    Settings settings;
    const char separator = SettingsFileIo::separator();
    EXPECT_EQ( settings.fileName(), directory() + separator + "Org" + separator + "App.ini" );
    EXPECT_EQ( settings.organizationName(), "Org" );

    CoreApplication::setOrganizationName( savedOrganization );
    CoreApplication::setApplicationName( savedApplication );
}

//! Verifies that a non-ASCII file name works, which on Windows needs the wide API.
TEST_F( SettingsTest, ANonAsciiFileNameWorks )
{
    const std::string file = pathOf( "Einstellungen-\xC3\xBC\xE2\x82\xAC.ini" );
    {
        Settings settings( file, SettingsFormat::Ini );
        settings.setValue( "k", "v" );
    }
    ASSERT_TRUE( exists( file ) );
    Settings again( file, SettingsFormat::Ini );
    EXPECT_EQ( again.value( "k", "" ), "v" );
}

//! Verifies the deferred write: with an event loop, a change reaches the file without sync() and
//! without destroying the object, once control returns to the loop.
TEST_F( SettingsTest, AChangeIsWrittenWhenTheLoopRuns )
{
    const std::string file = pathOf( "deferred.ini" );
    Thread worker;
    worker.start();
    ASSERT_TRUE( waitUntilRunning( worker ) );

    Settings* settings = nullptr;
    runOnThread( worker, [&settings, &file]()
        {
            settings = new Settings( file, SettingsFormat::Ini );
            settings->setValue( "first", 1 );
            settings->setValue( "second", 2 );
        } );

    // The two changes are written by one deferred call, after the task above returned.
    EXPECT_TRUE( waitFor( [&file]()
        {
            return readFile( file ) == "[General]" + kEol + "first=1" + kEol + "second=2" + kEol;
        } ) ) << readFile( file );

    runOnThread( worker, [&settings]()
        {
            delete settings;
        } );
    worker.quit();
    worker.wait();
}

//! Verifies that Settings objects in several threads that write one file lose no key.
//!
//! Each thread has its own object, as the class comment asks. The objects share one store, and
//! each sync() takes the lock, so the file at the end has every key from every thread.
TEST_F( SettingsTest, ThreadsWritingOneFileLoseNothing )
{
    const std::string file = pathOf( "threads.ini" );
    constexpr int kThreads = 4;
    constexpr int kKeysPerThread = 25;

    std::vector<std::thread> threads;
    for( int t = 0; t < kThreads; ++t )
    {
        threads.emplace_back( [t, &file]()
            {
                for( int k = 0; k < kKeysPerThread; ++k )
                {
                    Settings settings( file, SettingsFormat::Ini );
                    settings.setValue( "t" + std::to_string( t ) + "/k" + std::to_string( k ), k );
                    settings.sync();
                }
            } );
    }
    for( std::thread& thread : threads )
    {
        thread.join();
    }

    Settings settings( file, SettingsFormat::Ini );
    EXPECT_EQ( settings.status(), SettingsStatus::NoError );
    EXPECT_EQ( settings.allKeys().size(), static_cast<std::size_t>( kThreads * kKeysPerThread ) );
    for( int t = 0; t < kThreads; ++t )
    {
        for( int k = 0; k < kKeysPerThread; ++k )
        {
            EXPECT_EQ( settings.value( "t" + std::to_string( t ) + "/k" + std::to_string( k ), -1 ),
                k );
        }
    }
}

//! Verifies that two stores for one file -- here two names for it, as two processes would have --
//! do not overwrite each other's keys, because each write merges under the lock.
TEST_F( SettingsTest, TwoStoresForOneFileKeepBothChanges )
{
    const std::string file = pathOf( "two-stores.ini" );
    // "dir/./file" is the same file under a different name, so it gets a store of its own.
    const std::string otherName = directory() + SettingsFileIo::separator() + "." +
        SettingsFileIo::separator() + "two-stores.ini";

    Settings first( file, SettingsFormat::Ini );
    Settings second( otherName, SettingsFormat::Ini );
    #if !defined( _WIN32 )
        ASSERT_NE( first.fileName(), second.fileName() ) << "the test needs two stores";
    #endif

    first.setValue( "a", 1 );
    second.setValue( "b", 2 );
    first.sync();
    second.sync();
    first.sync();

    EXPECT_EQ( first.value( "b", 0 ), 2 );
    EXPECT_EQ( second.value( "a", 0 ), 1 );
}
