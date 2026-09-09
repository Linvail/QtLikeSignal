// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogMessage - one emitted log record, as a sink sees it.

#ifndef QT_LIKE_SIGNAL_LOG_MESSAGE_HPP
#define QT_LIKE_SIGNAL_LOG_MESSAGE_HPP

#include "QtLikeSignal/LogLevel.hpp"

#include <chrono>
#include <cstddef>

namespace QtLikeSignal
{
    class LogCategory;

    //! One finished log record, handed to a sink.
    //!
    //! A view, not an owner. mText points into the LogRecord's own buffer, which lives on the
    //! stack of the frame that logged and is gone the moment write() returns; mFile and mCategory
    //! point at storage that outlives the process. **A sink that keeps the text past the call must
    //! copy it.** That is the whole reason this is a struct of raw pointers rather than something
    //! more comfortable: a log record must not allocate, and handing a sink an owning string would
    //! allocate once per line on whatever thread was unlucky enough to log.
    struct LogMessage
    {
        LogLevel mLevel;  //!< Severity the record was emitted at.

        //! Category it was emitted on. Never null.
        const LogCategory* mCategory;

        //! When the record was created, on the monotonic clock.
        //!
        //! Monotonic and not the wall clock, because a vehicle's wall clock jumps when GPS time
        //! arrives -- usually a few seconds into a boot, which is exactly the window whose log a
        //! reader is most likely to be reading. Timestamps that go backwards mid-boot are worse
        //! than no timestamps. A sink that must print an absolute time pairs this with a wall
        //! clock reading it takes once at startup.
        std::chrono::steady_clock::time_point mTime;

        //! Native id of the thread that logged, as the OS numbers it.
        //!
        //! The kernel's thread id, not a hash of std::thread::id, so that it matches what
        //! `top -H`, a debugger's thread list and the journal's own fields say. That
        //! correspondence is the only thing this field is for.
        unsigned long long mThreadId;

        const char* mFile;  //!< Source file the record came from. Never null.
        int mLine;          //!< Line in that file.

        //! The formatted text, NUL-terminated. Never null; may be empty.
        const char* mText;

        //! Length of mText in bytes, not counting the terminator.
        std::size_t mLength;

        //! True when the text did not fit the record's fixed buffer and was cut short.
        //!
        //! The text itself already ends in an ellipsis when this is set, so a sink that ignores
        //! this flag still produces something honest. It is here for sinks that carry structured
        //! fields and would rather record the fact than embed it in the message.
        bool mTruncated;
    };
}

#endif // QT_LIKE_SIGNAL_LOG_MESSAGE_HPP
