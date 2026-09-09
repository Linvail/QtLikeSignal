// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::LogRecord.

#include "QtLikeSignal/LogRecord.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogMessage.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

//! The category qDebug(), qInfo(), qWarning() and qCritical() report on.
//!
//! Defined here, beside the macros that name it, rather than in LogCategories.cpp with the six the
//! library reports on -- it is not one of them. "DFLT" is the four-character id a DLT sink
//! registers the context under, chosen like the other six to stay distinct at that length.
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogDefault, "default", "DFLT" )

namespace QtLikeSignal
{
    namespace
    {
        //! Longest text any of the numeric append helpers produces, plus room to spare.
        //!
        //! Twenty digits is the most an unsigned 64-bit value takes in base ten, and one more for
        //! a sign. The doubling is for the floating-point path, where the width depends on what
        //! the C library decides "%g" means.
        const std::size_t kNumberCapacity = 48;

        //! Lower-case hexadecimal digits, for appendPointer().
        const char kHexDigits[] = "0123456789abcdef";
    }

    //! Constructs a record and stamps it with the current time.
    //!
    //! Only ever reached through the logging macros, and only when the category has the
    //! level switched on -- so everything this constructor costs is on the enabled path, and the
    //! suppressed path costs one relaxed load and a branch.
    LogRecord::LogRecord
        (
        const LogCategory& aCategory,  //!< Category the record belongs to.
        LogLevel aLevel,               //!< Severity.
        const char* aFile,             //!< Source file, normally __FILE__.
        int aLine                      //!< Source line, normally __LINE__.
        )
        : mCategory( &aCategory )
        , mLevel( aLevel )
        , mFile( aFile != nullptr ? aFile : "?" )
        , mLine( aLine )
        , mTime( std::chrono::steady_clock::now() )
        , mUsed( 0 )
        , mTruncated( false )
    {
        // Terminated from the outset, so text() is valid on a record nothing was ever appended to
        // -- which is what a bare qCInfo( c ); with no values is.
        mBuffer[ 0 ] = '\0';
    }

    //! Emits the record.
    //!
    //! The ellipsis is added here rather than at the moment of truncation because appendRaw() has
    //! already stopped accepting text by then, and because a record can be truncated by any of
    //! several appends and should only ever grow one ellipsis. kTextCapacity leaves exactly the
    //! four bytes this needs, so there is no case in which it does not fit.
    //!
    //! Nothing here can throw: the appends are memcpy, and Log::emitRecord() is documented not to.
    //! That matters because this runs at the end of the full expression, which may be during stack
    //! unwinding.
    LogRecord::~LogRecord()
    {
        if( mTruncated )
        {
            std::memcpy( mBuffer + mUsed, "...", 3 );
            mUsed += 3;
            mBuffer[ mUsed ] = '\0';
        }

        LogMessage message;
        message.mLevel = mLevel;
        message.mCategory = mCategory;
        message.mTime = mTime;
        message.mThreadId = Log::currentThreadId();
        message.mFile = mFile;
        message.mLine = mLine;
        message.mText = mBuffer;
        message.mLength = mUsed;
        message.mTruncated = mTruncated;

        Log::emitRecord( message );
    }

    //! Appends text exactly as given.
    //!
    //! Once truncated, a record stays truncated and accepts nothing further. Continuing to append
    //! whatever happened to fit would produce a line whose middle is missing without saying so,
    //! which is worse than a line that visibly stops.
    LogRecord& LogRecord::appendRaw
        (
        const char* aText,   //!< Text to append; null is ignored.
        std::size_t aLength  //!< Length of @p aText in bytes.
        )
    {
        if( mTruncated || aText == nullptr || aLength == 0 )
        {
            return *this;
        }

        const std::size_t room = kTextCapacity - mUsed;
        std::size_t copied = aLength;
        if( copied > room )
        {
            copied = room;
            mTruncated = true;
        }

        std::memcpy( mBuffer + mUsed, aText, copied );
        mUsed += copied;
        mBuffer[ mUsed ] = '\0';
        return *this;
    }

    //! Appends one streamed value, with a separating space unless it is the first.
    //!
    //! The separator is what lets a call site read `<< "was" << aMsec` and get `was -5` without
    //! spelling the space out. QDebug does the same, and for the same reason: a stream of values
    //! with no separator is unreadable, and making every call site supply one is worse.
    LogRecord& LogRecord::appendValue
        (
        const char* aText,   //!< Text to append; null is ignored.
        std::size_t aLength  //!< Length of @p aText in bytes.
        )
    {
        if( mUsed > 0 )
        {
            appendRaw( " ", 1 );
        }
        return appendRaw( aText, aLength );
    }

    //! Appends a signed integer in base ten.
    //!
    //! The magnitude is taken in unsigned arithmetic so that the most negative value works. In
    //! signed arithmetic its negation overflows, which is undefined; `0 - static_cast<unsigned>(x)`
    //! is defined for every input and produces exactly the magnitude.
    LogRecord& LogRecord::appendSigned
        (
        long long aValue
        )
    {
        const bool negative = aValue < 0;
        unsigned long long magnitude = negative
            ? 0ULL - static_cast<unsigned long long>( aValue )
            : static_cast<unsigned long long>( aValue );

        char text[ kNumberCapacity ];
        std::size_t index = sizeof( text );
        do
        {
            --index;
            text[ index ] = static_cast<char>( '0' + ( magnitude % 10ULL ) );
            magnitude /= 10ULL;
        }
        while( magnitude != 0ULL );

        if( negative )
        {
            --index;
            text[ index ] = '-';
        }

        return appendValue( text + index, sizeof( text ) - index );
    }

    //! Appends an unsigned integer in base ten.
    //!
    //! Written out rather than delegated to std::to_string or std::snprintf, because both of those
    //! can allocate and this is the single most common thing a log line does. The digits come out
    //! backwards, so they are written from the end of a stack buffer forwards.
    LogRecord& LogRecord::appendUnsigned
        (
        unsigned long long aValue
        )
    {
        char text[ kNumberCapacity ];
        std::size_t index = sizeof( text );
        do
        {
            --index;
            text[ index ] = static_cast<char>( '0' + ( aValue % 10ULL ) );
            aValue /= 10ULL;
        }
        while( aValue != 0ULL );

        return appendValue( text + index, sizeof( text ) - index );
    }

    //! Appends a floating-point value.
    //!
    //! **The one place a record can allocate.** std::snprintf with "%g" is the only portable way to
    //! render a double without writing a float formatter, and the standard does not promise it is
    //! allocation-free -- in practice the implementations here are, for a conversion this simple,
    //! but that is an observation rather than a guarantee. The allocation guard in src/perf/
    //! therefore covers the integer and string paths, which is what a log line in hot code
    //! actually uses; a float on a frame-budget thread is worth a second thought anyway.
    LogRecord& LogRecord::appendDouble
        (
        double aValue
        )
    {
        char text[ kNumberCapacity ];
        const int written = std::snprintf( text, sizeof( text ), "%g", aValue );
        if( written <= 0 )
        {
            return *this;
        }

        std::size_t length = static_cast<std::size_t>( written );
        if( length >= sizeof( text ) )
        {
            length = sizeof( text ) - 1;
        }
        return appendValue( text, length );
    }

    //! Appends an integer as lower-case hexadecimal with an 0x prefix.
    //!
    //! Shares appendPointer()'s method and not its width: a pointer is always the machine's full
    //! address width so that a column of them lines up, whereas an error code or a bitmask is
    //! whatever the caller says it is, and padding a poll(2) revents out to sixteen digits would
    //! bury the three that carry the answer.
    LogRecord& LogRecord::appendHex
        (
        unsigned long long aValue,  //!< Value to write.
        int aMinDigits              //!< Least digits, zero-padded; 0 or less for natural width.
        )
    {
        char text[ 2 + 16 ];
        std::size_t index = sizeof( text );

        std::size_t written = 0;
        do
        {
            --index;
            text[ index ] = kHexDigits[ aValue & 0xFULL ];
            aValue >>= 4;
            ++written;
        }
        while( aValue != 0ULL );

        // The cap is the buffer, not the caller's request: sixteen digits is every bit there is in
        // the widest value, so a larger minimum cannot add information and would only run off the
        // front of the buffer.
        const std::size_t minimum = aMinDigits > 0
            ? ( static_cast<std::size_t>( aMinDigits ) < 16 ?
            static_cast<std::size_t>( aMinDigits ) : 16 )
            : 0;
        while( written < minimum )
        {
            --index;
            text[ index ] = '0';
            ++written;
        }

        index -= 2;
        text[ index ] = '0';
        text[ index + 1 ] = 'x';

        return appendValue( text + index, sizeof( text ) - index );
    }

    //! Appends a pointer as lower-case hexadecimal with an 0x prefix, or "nullptr".
    //!
    //! Hand-rolled for the same reason the integers are, and fixed-width rather than minimal: two
    //! pointers printed on different lines line up, which is most of what anyone reads a column of
    //! addresses for.
    LogRecord& LogRecord::appendPointer
        (
        const void* aValue
        )
    {
        if( aValue == nullptr )
        {
            return appendValue( "nullptr", 7 );
        }

        // Through uintptr-sized integer arithmetic rather than by reinterpreting the bytes, so the
        // result is the address the platform names rather than whatever order it stores it in.
        unsigned long long address =
            static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( aValue ) );

        const std::size_t digits = sizeof( void* ) * 2;
        char text[ 2 + ( sizeof( void* ) * 2 ) ];
        text[ 0 ] = '0';
        text[ 1 ] = 'x';
        for( std::size_t index = 0; index < digits; ++index )
        {
            text[ 2 + digits - 1 - index ] = kHexDigits[ address & 0xFULL ];
            address >>= 4;
        }

        return appendValue( text, sizeof( text ) );
    }

    //! Appends a C string, or "(null)" for a null pointer.
    LogRecord& operator<<
        (
        LogRecord& aRecord,
        const char* aValue
        )
    {
        if( aValue == nullptr )
        {
            return aRecord.appendValue( "(null)", 6 );
        }
        return aRecord.appendValue( aValue, std::strlen( aValue ) );
    }
}
