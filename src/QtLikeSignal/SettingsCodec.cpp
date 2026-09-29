// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! SettingsCodec implementation: the choice of codec for a format.

#include "QtLikeSignal/SettingsCodec.hpp"

#include "QtLikeSignal/SettingsIniCodec.hpp"

namespace QtLikeSignal
{
    //! Destroys the codec. Virtual, because a codec is used through this interface.
    SettingsCodec::~SettingsCodec()
    {
    }

    //! Returns the codec for @p aFormat.
    //!
    //! Each codec is one function-local static. It has no state, so one instance serves every
    //! Settings object in every thread, and C++11 makes its construction thread-safe.
    const SettingsCodec& SettingsCodec::forFormat
        (
        SettingsFormat aFormat  //!< The format.
        )
    {
        static const SettingsIniCodec iniCodec;
        switch( aFormat )
        {
        case SettingsFormat::Ini:
        default:
            return iniCodec;
        }
    }

}
