// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Out-of-line members of QtLikeSignal::LogSink.

#include "QtLikeSignal/LogSink.hpp"

namespace QtLikeSignal
{
    //! Destroys the sink.
    //!
    //! Defined here rather than in the header so the class has exactly one translation unit
    //! carrying its vtable, instead of a copy in every file that includes it.
    LogSink::~LogSink()
    {
    }

    //! Does nothing. Correct for any sink that does not buffer.
    void LogSink::flush()
    {
    }
}
