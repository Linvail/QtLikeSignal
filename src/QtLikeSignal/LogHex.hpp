// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogHex - an integer streamed into a log record as hexadecimal.

#ifndef QT_LIKE_SIGNAL_LOG_HEX_HPP
#define QT_LIKE_SIGNAL_LOG_HEX_HPP

#include "QtLikeSignal/LogRecord.hpp"

namespace QtLikeSignal
{
    //! An integer to be written as hexadecimal rather than in base ten.
    //!
    //! A wrapper type and not a manipulator, because a manipulator would have to change the state
    //! of the record and then change it back, and a record that is midway through a chain is
    //! exactly where that gets forgotten. This affects one value and nothing after it.
    //!
    //! Not decoration. An EGL error is documented as 0x3002, a poll(2) revents is a bitmask, and
    //! an X11 window id is quoted in hex by every tool that prints one; rendering any of those in
    //! base ten hands the reader a number they then have to convert before they can look it up.
    //!
    //! @code
    //!   qCWarning( gLogGl ) << "eglCreateContext failed; EGL error"
    //!                                 << QtLikeSignal::logHex( eglGetError(), 4 );
    //!   // ... eglCreateContext failed; EGL error 0x3002
    //! @endcode
    struct LogHex
    {
        unsigned long long mValue;  //!< The value to write.

        //! Least number of digits to write, zero-padded on the left. Zero for natural width.
        //!
        //! A fixed width is worth asking for wherever the value is really a field rather than a
        //! count -- an EGL error is always four digits, so padding one keeps a column of them
        //! aligned and makes an unfamiliar code recognisable as the same shape as a familiar one.
        int mMinDigits;
    };

    //! Wraps @p aValue so that streaming it into a record writes hexadecimal.
    //!
    //! Takes the widest unsigned integer there is, so every integral type reaches it by an
    //! implicit widening and no call site needs a cast. A negative value should be cast by the
    //! caller to the unsigned type it wants to see the bits of, rather than being sign-extended
    //! into sixteen digits by accident.
    inline LogHex logHex
        (
        unsigned long long aValue,  //!< Value to write.
        int aMinDigits = 0          //!< Least digits, zero-padded; 0 for natural width.
        )
    {
        return LogHex { aValue, aMinDigits };
    }

    //! Appends @p aValue as lower-case hexadecimal with an 0x prefix.
    inline LogRecord& operator<<
        (
        LogRecord& aRecord,
        LogHex aValue
        )
    {
        return aRecord.appendHex( aValue.mValue, aValue.mMinDigits );
    }
}

#endif // QT_LIKE_SIGNAL_LOG_HEX_HPP
