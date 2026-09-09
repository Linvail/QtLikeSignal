// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogRecord - one log line under construction, the stream operators that fill it,
//! and the macros that start one.

#ifndef QT_LIKE_SIGNAL_LOG_RECORD_HPP
#define QT_LIKE_SIGNAL_LOG_RECORD_HPP

#include "QtLikeSignal/LogCategory.hpp"
#include "QtLikeSignal/LogLevel.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace QtLikeSignal
{
    //! One log line under construction. Formats into a fixed buffer and emits when destroyed.
    //!
    //! A record is a temporary created by one of the logging macros at the foot of this
    //! header, filled by a chain of `<<`, and emitted at the semicolon that ends the full
    //! expression:
    //!
    //! @code
    //!   qCWarning( gLogTimer ) << "startTimer: interval cannot be negative" << aMsec;
    //! @endcode
    //!
    //! A space is inserted before every value but the first, so that reads as
    //! `startTimer: interval cannot be negative -5` with nothing to say about it at the call site.
    //!
    //! **It never allocates.** The text goes into mBuffer, which is part of the object and so is on
    //! the caller's stack; integers are converted by hand rather than through std::to_string or an
    //! ostream. That is not micro-optimisation, it is the property that lets a log line sit on a
    //! thread holding a frame budget, and there is a guard in src/perf/ asserting a thousand
    //! records cost zero allocations. Only the floating-point overloads step outside it, into
    //! std::snprintf; see appendDouble().
    //!
    //! **It never throws**, because the destructor emits and can run while an exception is already
    //! propagating.
    //!
    //! Text past kTextCapacity is dropped and the record is marked truncated, its text ending in
    //! an ellipsis. Truncating is the only option that keeps the no-allocation promise, and 500-odd
    //! bytes is past the point where a log line was going to be read anyway.
    class LogRecord
    {
    public:
        //! Size of the text buffer, in bytes, including everything below.
        static constexpr std::size_t kCapacity = 512;

        //! Bytes of text a record will actually hold.
        //!
        //! Four less than the buffer: three for the "..." a truncated record ends with, and one
        //! for the terminator. Reserving them up front is what lets appendRaw() truncate without
        //! having to make room afterwards.
        static constexpr std::size_t kTextCapacity = kCapacity - 4;

        LogRecord
            (
            const LogCategory& aCategory,
            LogLevel aLevel,
            const char* aFile,
            int aLine
            );

        ~LogRecord();

        //! A record is a temporary that emits exactly once when it dies. Copying one would emit
        //! the same line twice.
        LogRecord
            (
            const LogRecord&
            ) = delete;

        LogRecord& operator=
            (
            const LogRecord&
            ) = delete;

        //! Appends text exactly as given, with no separating space.
        //!
        //! For an operator<< that is continuing a value rather than starting one. Everything that
        //! starts a value goes through appendValue() instead.
        LogRecord& appendRaw
            (
            const char* aText,    //!< Text to append; null is ignored.
            std::size_t aLength   //!< Length of @p aText in bytes.
            );

        //! Appends one streamed value: a separating space first, unless this is the first value.
        LogRecord& appendValue
            (
            const char* aText,    //!< Text to append; null is ignored.
            std::size_t aLength   //!< Length of @p aText in bytes.
            );

        //! Appends a signed integer in base ten.
        LogRecord& appendSigned
            (
            long long aValue
            );

        //! Appends an unsigned integer in base ten.
        LogRecord& appendUnsigned
            (
            unsigned long long aValue
            );

        //! Appends a floating-point value.
        LogRecord& appendDouble
            (
            double aValue
            );

        //! Appends an integer as lower-case hexadecimal with an 0x prefix.
        //!
        //! Reached through QtLikeSignal::logHex() rather than by an operator of its own, because
        //! "write this one in hex" is a property of the value and not of the record. See LogHex.
        LogRecord& appendHex
            (
            unsigned long long aValue,
            int aMinDigits
            );

        //! Appends a pointer as lower-case hexadecimal with an 0x prefix, or "nullptr".
        LogRecord& appendPointer
            (
            const void* aValue
            );

        //! @return the text built so far, NUL-terminated. Never null.
        const char* text() const
        {
            return mBuffer;
        }

        //! @return the length of text() in bytes, not counting the terminator.
        std::size_t length() const
        {
            return mUsed;
        }

        //! @return true when the text did not fit and was cut short.
        bool isTruncated() const
        {
            return mTruncated;
        }

    private:
        const LogCategory* mCategory;  //!< Category this record belongs to. Never null.
        LogLevel mLevel;               //!< Severity.
        const char* mFile;             //!< Source file, from __FILE__.
        int mLine;                     //!< Source line, from __LINE__.

        //! When the record was created.
        //!
        //! Taken in the constructor, not the destructor: the interesting instant is when the thing
        //! being reported happened, and a record whose `<<` chain calls something slow would
        //! otherwise be stamped with the time that call finished.
        std::chrono::steady_clock::time_point mTime;

        std::size_t mUsed;  //!< Bytes of mBuffer in use, not counting the terminator.
        bool mTruncated;    //!< Set once something did not fit; nothing is appended after.

        //! The text. Part of the object, and therefore on the stack of whoever logged.
        char mBuffer[ kCapacity ];
    };

    //! @name Stream operators
    //!
    //! One overload per built-in type rather than a single template over the widest one. An `int`
    //! converts to both `long long` and `unsigned long long` with the same rank, so a pair of
    //! catch-all overloads would make every `<< someInt` ambiguous. They are one line each, and
    //! being explicit here is what keeps the call sites free of casts.
    //! @{

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        bool aValue
        )
    {
        return aValue ? aRecord.appendValue( "true", 4 ) : aRecord.appendValue( "false", 5 );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        char aValue
        )
    {
        return aRecord.appendValue( &aValue, 1 );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        signed char aValue
        )
    {
        return aRecord.appendSigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        unsigned char aValue
        )
    {
        return aRecord.appendUnsigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        short aValue
        )
    {
        return aRecord.appendSigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        unsigned short aValue
        )
    {
        return aRecord.appendUnsigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        int aValue
        )
    {
        return aRecord.appendSigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        unsigned int aValue
        )
    {
        return aRecord.appendUnsigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        long aValue
        )
    {
        return aRecord.appendSigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        unsigned long aValue
        )
    {
        return aRecord.appendUnsigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        long long aValue
        )
    {
        return aRecord.appendSigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        unsigned long long aValue
        )
    {
        return aRecord.appendUnsigned( aValue );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        float aValue
        )
    {
        return aRecord.appendDouble( static_cast<double>( aValue ) );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        double aValue
        )
    {
        return aRecord.appendDouble( aValue );
    }

    //! Appends a C string, or "(null)" for a null pointer.
    //!
    //! A null here is far more often a bug being reported than a deliberate empty string, so it
    //! prints as something a reader will notice rather than as nothing at all.
    LogRecord& operator<<
        (
        LogRecord& aRecord,
        const char* aValue
        );

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        const std::string& aValue
        )
    {
        return aRecord.appendValue( aValue.c_str(), aValue.size() );
    }

    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        std::string_view aValue
        )
    {
        return aRecord.appendValue( aValue.data(), aValue.size() );
    }

    //! Appends a pointer as hexadecimal.
    //!
    //! Anything that is not one of the types above arrives here, which is what makes an arbitrary
    //! object pointer printable without an overload of its own.
    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        const void* aValue
        )
    {
        return aRecord.appendPointer( aValue );
    }

    //! Appends "nullptr".
    //!
    //! Without this, a literal `nullptr` is ambiguous between the const char* and const void*
    //! overloads and will not compile.
    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        std::nullptr_t
        )
    {
        return aRecord.appendValue( "nullptr", 7 );
    }

    //! Bridges the first `<<` in a chain, whose left operand is the macro's temporary.
    //!
    //! A prvalue does not bind to the `LogRecord&` the overloads above take, so without this every
    //! log statement would fail to compile at its first value. It forwards to those overloads by
    //! naming the temporary, which is an lvalue inside this function.
    //!
    //! The template is over the *value*, not over the record, so it adds no candidate that could
    //! compete with them: for `record << x` where record is an lvalue, `LogRecord&&` does not bind
    //! and this is not a candidate at all.
    template <typename T> LogRecord& operator<<
        (
        LogRecord&& aRecord,   //!< The macro's temporary.
        const T& aValue        //!< Value to append; resolved against the overloads above.
        )
    {
        return aRecord << aValue;
    }

    //! @}
}

//! The category the uncategorized macros report on. Named "default", as Qt names its own.
//!
//! Deliberately not under "qtlikesignal.", because it belongs to whichever application logs through
//! qWarning() rather than to this library. A rule of `qtlikesignal.*=warning` is aimed at the
//! library, and must not silence the caller's own records as a side effect. Every category this
//! library reports on is in LogCategories.hpp, and none of them is this one.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogDefault )

//! Starts a log record on @p aCategory at @p aLevel, if that category has @p aLevel switched on.
//!
//! A macro rather than a function, and the whole point of the facility is in that choice. A
//! function `logWarning( category, a, b, c )` must evaluate a, b and c before it can look at the
//! category, so a switched-off `<< dumpObjectTree()` still walks the tree. Here the arguments live
//! inside the else branch and are never evaluated when the category is off, which is what makes a
//! log line affordable in code that runs every frame.
//!
//! `if( !x ) {} else` rather than `if( x )`, so that a caller's own `else` after a log statement
//! binds to the caller's `if` -- the inner one already has an else and cannot take it.
#define QTLIKESIGNAL_LOG( aCategory, aLevel )                                             \
        if( !( aCategory )( ).isEnabled( aLevel ) ) {} else                               \
        QtLikeSignal::LogRecord( ( aCategory )( ), aLevel, __FILE__, __LINE__ )

//! @name Categorized macros
//!
//! Each starts a record on the category it is given. These are what the library itself uses, and
//! what an application with categories of its own should use, because a record on a named category
//! can be switched on and off without touching every other record in the process.
//!
//! Qt spells the same four the same way, so a caller moving between the two toolkits does not have
//! to relearn them. Its fifth, qCFatal(), has no counterpart here: this library has no Fatal level,
//! because a library is not the right place to decide that a process should stop.
//! @{

//! Starts a Debug record. Off unless a filter rule switches the category on.
#define qCDebug( aCategory ) QTLIKESIGNAL_LOG( aCategory, QtLikeSignal::LogLevel::Debug )

//! Starts an Info record.
#define qCInfo( aCategory ) QTLIKESIGNAL_LOG( aCategory, QtLikeSignal::LogLevel::Info )

//! Starts a Warning record. This is what the library's misuse reports are.
#define qCWarning( aCategory ) QTLIKESIGNAL_LOG( aCategory, QtLikeSignal::LogLevel::Warning )

//! Starts a Critical record.
#define qCCritical( aCategory ) QTLIKESIGNAL_LOG( aCategory, QtLikeSignal::LogLevel::Critical )

//! @}

//! @name Uncategorized macros
//!
//! Each starts a record on gLogDefault, for a caller that has not defined a category of its own:
//!
//! @code
//!   qWarning() << "config: no display was found; falling back to the first one";
//! @endcode
//!
//! **The empty parentheses are required**, because these are function-like macros and Qt's are
//! not -- Qt's qWarning() is a call to a function that takes a printf format, and ours is the
//! start of a `<<` chain. `qWarning << "..."` will not compile, which is the failure a caller
//! wants: it is loud rather than silent.
//!
//! Prefer the categorized four above for anything that will be read more than once. A record with
//! no category can only be filtered by level, so a program that logs everything through these has
//! no way to turn up one part of itself without turning up all of it.
//! @{

//! Starts a Debug record on gLogDefault. Off unless a filter rule switches "default" on.
#define qDebug() qCDebug( gLogDefault )

//! Starts an Info record on gLogDefault.
#define qInfo() qCInfo( gLogDefault )

//! Starts a Warning record on gLogDefault.
#define qWarning() qCWarning( gLogDefault )

//! Starts a Critical record on gLogDefault.
#define qCritical() qCCritical( gLogDefault )

//! @}

#endif // QT_LIKE_SIGNAL_LOG_RECORD_HPP
