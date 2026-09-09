// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogSink - the destination a finished log record is handed to.

#ifndef QT_LIKE_SIGNAL_LOG_SINK_HPP
#define QT_LIKE_SIGNAL_LOG_SINK_HPP

#include "QtLikeSignal/LogMessage.hpp"

namespace QtLikeSignal
{
    //! Destination for emitted log records.
    //!
    //! Two sinks ship with QtLikeSignal itself, both LogSinkFile: one on stderr, which is the
    //! default, and one on stdout. The sinks that matter in a vehicle -- journald and DLT -- live
    //! in the separate QtLikeSignalLog target instead, because QtLikeSignal depends on nothing but
    //! the C++ runtime and the platform's threading and linking libsystemd or libdlt into it would
    //! end that.
    //!
    //! Three obligations on an implementation, all of them consequences of where write() is
    //! called from:
    //!
    //! - **It must be callable concurrently from any thread.** A sink is installed once and used
    //!   by every thread in the process; nothing serialises the calls for it.
    //! - **It must not throw.** write() is reached from ~LogRecord, which can run while an
    //!   exception is already propagating.
    //! - **It must not log.** A record emitted from inside write() re-enters the same sink.
    //!
    //! A fourth is a strong preference rather than an obligation: write() should not block for
    //! long. On the thread holding a frame budget, a sink that waits on a full socket buffer is
    //! a dropped frame. A sink that cannot promise that is the reason the deferred emit mode
    //! exists.
    class LogSink
    {
    public:
        LogSink() = default;

        virtual ~LogSink();

        //! A sink is referred to by pointer from every logging thread at once; copying one would
        //! make it ambiguous which of the copies is installed.
        LogSink
            (
            const LogSink&
            ) = delete;

        LogSink& operator=
            (
            const LogSink&
            ) = delete;

        //! Writes one record.
        //!
        //! @p aMessage and everything it points at are valid only for the duration of the call.
        //! See LogMessage.
        virtual void write
            (
            const LogMessage& aMessage
            ) = 0;

        //! Pushes anything this sink has buffered to its destination.
        //!
        //! Called from Log::flush() and Log::shutdown(). The default does nothing, which is right
        //! for a sink that never buffers. Like write(), it must not throw.
        virtual void flush();

    };
}

#endif // QT_LIKE_SIGNAL_LOG_SINK_HPP
