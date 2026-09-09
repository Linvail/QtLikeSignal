// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogLevel - the severity of a log record, and its printable name.
//!
//! Its own header because it is the one piece of the logging facility that everything else in it
//! needs: a category is switched per level, a record carries one, and a sink dispatches on one.

#ifndef QT_LIKE_SIGNAL_LOG_LEVEL_HPP
#define QT_LIKE_SIGNAL_LOG_LEVEL_HPP

namespace QtLikeSignal
{
    //! Severity of a log record, ordered least severe to most.
    //!
    //! The order is load-bearing and not merely conventional: a category stores a threshold and
    //! answers isEnabled() by comparing against it, so anything that reorders these enumerators
    //! or opens a gap between them silently changes what every filter rule means. Add new levels
    //! at the ends, never in the middle.
    //!
    //! Four rather than Qt's five: there is no Fatal. A level whose documented behaviour is to
    //! abort the process is a control-flow decision wearing a logging costume, and this library
    //! does not terminate its host. Report at Critical and let the caller decide.
    enum class LogLevel
    {
        //! Detail useful while working on the code itself. Off by default.
        Debug,

        //! Something a reader of the log would want to know went normally.
        Info,

        //! Misuse of the library, or a condition it recovered from. This is what the warnings
        //! that used to go to stderr by hand are.
        Warning,

        //! A failure the caller is unlikely to be able to work around.
        Critical
    };

    //! Returns the lower-case name of @p aLevel, as filter rules spell it and sinks print it.
    //!
    //! Never null, and never allocates: the returned pointer is to a string literal with static
    //! storage duration, so it outlives any use a caller can make of it.
    const char* logLevelName
        (
        LogLevel aLevel  //!< The level to name.
        );

    //! Parses a level name as a filter rule spells it, into @p aLevel. Returns false and leaves
    //! @p aLevel untouched when @p aName is not one of the four names logLevelName() produces.
    //!
    //! Case-sensitive and lower-case only, deliberately: accepting "Warning" and "WARNING" as well
    //! invites rule strings that differ between two configurations of the same product and read as
    //! though they should behave differently.
    bool logLevelFromName
        (
        const char* aName,  //!< Candidate name; null is rejected rather than treated as empty.
        LogLevel& aLevel    //!< Set to the parsed level on success, untouched on failure.
        );

}

#endif // QT_LIKE_SIGNAL_LOG_LEVEL_HPP
