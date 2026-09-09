// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::LogSinkFile.

#include "QtLikeSignal/LogSinkFile.hpp"

#include "QtLikeSignal/LogCategory.hpp"

#include <cstring>

namespace QtLikeSignal
{
    namespace
    {
        //! Returns the file name part of @p aPath, without any directories.
        //!
        //! __FILE__ expands to whatever path the compiler was given, which for this build system
        //! is a long relative path that would take up half the line. The record keeps the full
        //! literal -- trimming it there would cost a scan on every emit -- and the trimming
        //! happens here, on the path that is already writing to a stream.
        //!
        //! Both separators are checked because a Windows build compiles with backslashes and a
        //! cross build from Linux to Windows does not.
        const char* fileNameOnly
            (
            const char* aPath  //!< Path to trim; null yields "?".
            )
        {
            if( aPath == nullptr )
            {
                return "?";
            }

            const char* name = aPath;
            for( const char* cursor = aPath; *cursor != '\0'; ++cursor )
            {
                if( *cursor == '/' || *cursor == '\\' )
                {
                    name = cursor + 1;
                }
            }
            return name;
        }

        //! Returns the single letter this sink prints for @p aLevel.
        char levelInitial
            (
            LogLevel aLevel  //!< The level.
            )
        {
            switch( aLevel )
            {
            case LogLevel::Debug:
                return 'D';
            case LogLevel::Info:
                return 'I';
            case LogLevel::Warning:
                return 'W';
            case LogLevel::Critical:
                return 'C';
            }
            return '?';
        }
    }

    //! Constructs a sink writing to @p aFile.
    LogSinkFile::LogSinkFile
        (
        std::FILE* aFile,        //!< Stream to write to. Not owned. Records are dropped if null.
        bool aFlushEachRecord    //!< Whether to fflush() after every record. See mFlushEachRecord.
        )
        : mFile( aFile )
        , mFlushEachRecord( aFlushEachRecord )
        , mOrigin( std::chrono::steady_clock::now() )
    {
    }

    LogSinkFile::~LogSinkFile()
    {
    }

    //! Formats @p aMessage as one line and writes it.
    //!
    //! The elapsed time is measured from when this sink was constructed rather than from process
    //! start, because a sink installed by an application partway through a run has no access to
    //! the latter and a column of six-digit seconds helps nobody. Taking the origin in the
    //! constructor rather than on the first record is also what keeps this function free of
    //! mutable shared state: two threads writing at once touch nothing in common.
    //!
    //! std::snprintf, not the stream-formatting this facility exists to offer callers: this is the
    //! one place in the library that has to turn a fixed, known set of fields into a fixed layout,
    //! which is the job format strings are actually good at. It is also measurably the cheaper of
    //! the two, and it is on the enabled path of every log call.
    void LogSinkFile::write
        (
        const LogMessage& aMessage
        )
    {
        if( mFile == nullptr )
        {
            return;
        }

        const std::chrono::microseconds elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>( aMessage.mTime - mOrigin );
        const long long seconds = static_cast<long long>( elapsed.count() / 1000000 );
        const long milliseconds = static_cast<long>( ( elapsed.count() % 1000000 ) / 1000 );

        const char* const category =
            aMessage.mCategory != nullptr ? aMessage.mCategory->name() : "?";

        char line[ kLineCapacity ];
        const int written = std::snprintf( line, sizeof( line ),
            "%6lld.%03ld %c %s %llu %s:%d %s\n",
            seconds,
            milliseconds,
            levelInitial( aMessage.mLevel ),
            category,
            aMessage.mThreadId,
            fileNameOnly( aMessage.mFile ),
            aMessage.mLine,
            aMessage.mText != nullptr ? aMessage.mText : "" );

        if( written <= 0 )
        {
            return;
        }

        // snprintf reports what it would have written, not what it did, so a line at the cap comes
        // back with a count past the end of the buffer. Clamp to what is actually there, and put
        // the newline back: truncation would otherwise have eaten it and glued two records
        // together, which is worse than losing the tail of one.
        std::size_t length = static_cast<std::size_t>( written );
        if( length >= sizeof( line ) )
        {
            length = sizeof( line ) - 1;
            line[ length - 1 ] = '\n';
        }

        std::fwrite( line, 1, length, mFile );

        if( mFlushEachRecord )
        {
            std::fflush( mFile );
        }
    }

    //! Flushes the underlying stream.
    void LogSinkFile::flush()
    {
        if( mFile != nullptr )
        {
            std::fflush( mFile );
        }
    }
}
