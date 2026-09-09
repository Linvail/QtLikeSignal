// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::logLevelName() and QtLikeSignal::logLevelFromName().

#include "QtLikeSignal/LogLevel.hpp"

#include <cstring>

namespace QtLikeSignal
{
    namespace
    {
        //! One level and the name filter rules spell it with.
        struct LevelName
        {
            LogLevel mLevel;      //!< The level.
            const char* mName;    //!< Its lower-case name.
        };

        //! Every level, exactly once, in declaration order.
        //!
        //! One table drives both directions, so a level added to the enum without a name here is
        //! caught by the static_assert below rather than by a log line that prints "?" months
        //! later. Both functions walk it; four entries is not worth a lookup structure.
        constexpr LevelName kLevelNames[] =
        {
            { LogLevel::Debug, "debug" },
            { LogLevel::Info, "info" },
            { LogLevel::Warning, "warning" },
            { LogLevel::Critical, "critical" }
        };

        //! Catches a level added to the enum but not to the table. Critical is the last enumerator,
        //! so its value is one less than the number of levels there are.
        static_assert(
            sizeof( kLevelNames ) / sizeof( kLevelNames[ 0 ] )
            == static_cast<std::size_t>( LogLevel::Critical ) + 1,
            "kLevelNames must have one entry per LogLevel enumerator" );
    }

    //! Returns the lower-case name of @p aLevel.
    //!
    //! An unrecognised value cannot arise from the enum itself and only reaches here through a cast
    //! of some out-of-range integer. It gets a printable answer rather than an assertion, because a
    //! logging call is the wrong place to take a process down.
    const char* logLevelName
        (
        LogLevel aLevel  //!< The level to name.
        )
    {
        for( const LevelName& entry : kLevelNames )
        {
            if( entry.mLevel == aLevel )
            {
                return entry.mName;
            }
        }
        return "unknown";
    }

    //! Parses a level name into @p aLevel, returning whether it was one of the four.
    bool logLevelFromName
        (
        const char* aName,  //!< Candidate name; null is rejected rather than treated as empty.
        LogLevel& aLevel    //!< Set to the parsed level on success, untouched on failure.
        )
    {
        if( aName == nullptr )
        {
            return false;
        }

        for( const LevelName& entry : kLevelNames )
        {
            if( std::strcmp( entry.mName, aName ) == 0 )
            {
                aLevel = entry.mLevel;
                return true;
            }
        }
        return false;
    }
}
