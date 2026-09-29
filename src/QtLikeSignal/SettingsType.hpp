// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsType - which of its six kinds of data a SettingsValue holds.

#ifndef QT_LIKE_SIGNAL_SETTINGS_TYPE_HPP
#define QT_LIKE_SIGNAL_SETTINGS_TYPE_HPP

namespace QtLikeSignal
{
    //! The kind of data in a SettingsValue.
    //!
    //! These are the types that an INI file can give back, and no others. A value read from a file
    //! is always a String, a StringList or Invalid, because the file holds text. A value is a Bool,
    //! an Int or a Double only if the program set it and the file was not read again since then.
    //! The conversions on SettingsValue hide this difference: toInt() reads a String as well as it
    //! reads an Int.
    enum class SettingsType
    {
        //! No value. A default-constructed SettingsValue, and a value that the file wrote as
        //! `@Invalid()`.
        Invalid,

        //! A bool.
        Bool,

        //! An integer, held as a long long.
        Int,

        //! A double.
        Double,

        //! A UTF-8 string.
        String,

        //! A list of UTF-8 strings.
        StringList
    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_TYPE_HPP
