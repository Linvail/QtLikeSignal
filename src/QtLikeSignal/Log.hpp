// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::Log - installing a sink, setting filter rules, and the entry point ~LogRecord
//! emits through.
//!
//! This is the header a caller includes. It pulls in the rest of the facility, so a file that
//! logs needs one include and not five:
//!
//! @code
//!   #include "QtLikeSignal/Log.hpp"
//!
//!   QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogTimer, "qtlikesignal.timer", "TIMR" )
//!
//!   qCWarning( gLogTimer ) << "startTimer: interval cannot be negative" << aMsec;
//! @endcode

#ifndef QT_LIKE_SIGNAL_LOG_HPP
#define QT_LIKE_SIGNAL_LOG_HPP

#include "QtLikeSignal/LogCategory.hpp"
#include "QtLikeSignal/LogHex.hpp"
#include "QtLikeSignal/LogLevel.hpp"
#include "QtLikeSignal/LogMessage.hpp"
#include "QtLikeSignal/LogRecord.hpp"
#include "QtLikeSignal/LogSink.hpp"
#include "QtLikeSignal/LogSinkFile.hpp"

#include <string>

namespace QtLikeSignal
{
    //! Process-wide state of the logging facility: which sink is installed, and which categories
    //! are switched on.
    //!
    //! A namespace rather than a singleton class, because there is nothing here to hold: the sink
    //! pointer and the rule string are the whole of it, and a class would only add a way to ask
    //! for the instance.
    namespace Log
    {
        //! Installs @p aSink as the destination for every record from now on.
        //!
        //! **The sink is not owned.** The caller keeps it alive, and must not destroy it while any
        //! thread might still be logging -- in practice, install it at startup and leave it. That
        //! is a real constraint and it is deliberate: reference-counting the sink would put an
        //! atomic increment on every log call to solve a problem that installing once at startup
        //! does not have.
        //!
        //! @return the sink installed before this call, or null if it was still the default.
        LogSink* setSink
            (
            LogSink* aSink  //!< New sink, or null to go back to the stderr default.
            );

        //! @return the installed sink, or the stderr default if none was installed. Never null.
        LogSink* sink();

        //! @return the shared sink that writes to stderr. This is the default.
        LogSinkFile& stderrSink();

        //! @return the shared sink that writes to stdout.
        LogSinkFile& stdoutSink();

        //! Applies @p aRules to every category, now and to every category created later.
        //!
        //! The syntax is a comma-separated list of `pattern=level`, where the level is one of the
        //! four names logLevelName() produces and the pattern is a category name that may end in
        //! `*`:
        //!
        //! ```
        //! qtlikesignal.*=warning,qtlikesignal.wayland=debug
        //! ```
        //!
        //! Later rules win over earlier ones, so that reads as "warnings from everything, except
        //! tell me everything about Wayland". A category matching no rule keeps
        //! LogCategory::defaultThreshold(). Malformed entries are skipped individually; one bad
        //! rule does not discard the rest of the string.
        //!
        //! Replaces any rules previously set, including those read from the environment.
        void setFilterRules
            (
            const char* aRules  //!< Rule string; null or empty resets everything to the default.
            );

        //! @return the rule string currently in force, or empty if there is none.
        std::string filterRules();

        //! Reads QTLIKESIGNAL_LOG_RULES and applies it, once per process.
        //!
        //! Called from LogCategory's constructor, which is what makes the environment work at all:
        //! the level check happens in the logging macro before any record is built, so a
        //! facility that waited until the first record to read the environment would have already
        //! suppressed it. A category is constructed before it can be checked, so this is the
        //! earliest hook there is.
        //!
        //! Safe to call from any thread and at any time; every call after the first does nothing.
        void ensureInitialised();

        //! Hands one finished record to the installed sink.
        //!
        //! Called by ~LogRecord and not normally by anything else. **Does not throw**, whatever the
        //! sink does -- see the implementation for what happens to a sink that tries.
        void emitRecord
            (
            const LogMessage& aMessage
            );

        //! Pushes anything the installed sink has buffered to its destination.
        //!
        //! Worth calling from a crash handler. It is the deferred emit mode, once that exists,
        //! that makes this more than a convenience.
        void flush();

        //! Flushes the installed sink and puts the default back.
        //!
        //! Does not stop logging: records after this go to stderr. There is no state in which this
        //! library refuses to report, because the most interesting warnings in a process's life
        //! arrive during its teardown.
        void shutdown();

        //! @return the calling thread's id, as the operating system numbers it.
        //!
        //! The kernel's thread id -- GetCurrentThreadId() on Windows, gettid(2) on Linux -- rather
        //! than a hash of std::thread::id, so that it matches what `top -H`, a debugger's thread
        //! list and the journal's own fields say about the same thread. Cached per thread, so the
        //! syscall happens once however much that thread logs.
        unsigned long long currentThreadId();

    }
}

#endif // QT_LIKE_SIGNAL_LOG_HPP
