// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogSinkFile - a log sink that writes plain text to a C stdio stream.

#ifndef QT_LIKE_SIGNAL_LOG_SINK_FILE_HPP
#define QT_LIKE_SIGNAL_LOG_SINK_FILE_HPP

#include "QtLikeSignal/LogSink.hpp"

#include <cstdio>

namespace QtLikeSignal
{
    //! Writes records as one line of plain text each, to a stdio stream the caller supplies.
    //!
    //! One class rather than a StderrSink and a StdoutSink, because the two would have differed in
    //! a single pointer. Log::stderrSink() and Log::stdoutSink() are the two instances that ship;
    //! anything else -- a file opened by the application, a pipe -- is this class with a different
    //! stream.
    //!
    //! The line looks like this, and is meant to survive `grep`, `awk` and a human eye equally:
    //!
    //! ```
    //!    12.345 W qtlikesignal.timer 4711 Timer.cpp:120 startTimer: interval cannot be negative -5
    //! ```
    //!
    //! Fields in order: seconds since this sink's first record, the level's initial, the category
    //! name, the native thread id, the source location with the directories trimmed off, and the
    //! text. Elapsed rather than absolute time because LogMessage carries the monotonic clock and
    //! nothing else -- see LogMessage::mTime for why that is the right clock in a vehicle.
    //!
    //! **Thread-safe.** Each record is formatted into a stack buffer and written with a single
    //! fwrite, so two threads logging at once produce two whole lines rather than one interleaved
    //! one. Nothing weaker would do: stdio's own locking is per call, not per line.
    class LogSinkFile : public LogSink
    {
    public:
        //! Longest line this sink will produce, including the newline but not a terminator.
        //!
        //! LogRecord::kCapacity for the text, and the rest for the six fields in front of it --
        //! generous, because a source path can be long and truncating the location to fit would
        //! throw away the field most likely to be wanted.
        static constexpr std::size_t kLineCapacity = 1024;

        explicit LogSinkFile
            (
            std::FILE* aFile,
            bool aFlushEachRecord = true
            );

        ~LogSinkFile() override;

        //! Formats @p aMessage as one line and writes it.
        void write
            (
            const LogMessage& aMessage
            ) override;

        //! Flushes the underlying stream.
        void flush() override;

    private:
        //! The stream to write to. Not owned, and not closed by this class: the two instances that
        //! ship wrap stderr and stdout, which this library has no business closing.
        std::FILE* mFile;

        //! Whether to fflush() after every record.
        //!
        //! True by default, and worth what it costs. stderr is unbuffered anyway, so the flush is
        //! free there; on any other stream it is what makes the log useful after a crash, which is
        //! most of what a log is for. An application that logs hard enough for this to show up in
        //! a profile can turn it off and call Log::flush() on its own schedule.
        bool mFlushEachRecord;

        //! Instant the elapsed-time column counts from: when this sink was constructed.
        //!
        //! Taken here rather than on the first record so that write() reads it and never writes
        //! it. A "set it on the first call" static would be a data race between two threads
        //! logging at once, and would also be shared by every instance of this class.
        std::chrono::steady_clock::time_point mOrigin;
    };
}

#endif // QT_LIKE_SIGNAL_LOG_SINK_FILE_HPP
