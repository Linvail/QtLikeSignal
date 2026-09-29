// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsStatus - the first error a Settings object met.

#ifndef QT_LIKE_SIGNAL_SETTINGS_STATUS_HPP
#define QT_LIKE_SIGNAL_SETTINGS_STATUS_HPP

namespace QtLikeSignal
{
    //! The result of the file operations of a Settings object.
    //!
    //! Settings::status() keeps the first error, not the last one, as QSettings::status() does.
    //! A later success does not clear it.
    enum class SettingsStatus
    {
        //! No error occurred.
        NoError,

        //! The file could not be read, written, or locked. For example, the directory is read-only.
        AccessError,

        //! The file has a line that the parser does not accept. The keys that it could read are
        //! still available.
        FormatError
    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_STATUS_HPP
