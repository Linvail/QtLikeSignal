// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsFormat - the file format a Settings object reads and writes.

#ifndef QT_LIKE_SIGNAL_SETTINGS_FORMAT_HPP
#define QT_LIKE_SIGNAL_SETTINGS_FORMAT_HPP

namespace QtLikeSignal
{
    //! The file format of a settings file.
    //!
    //! There is one format now. The enum exists for two reasons. First, it is the argument that
    //! tells the file constructor, `Settings( fileName, SettingsFormat::Ini )`, apart from the
    //! organization constructor, `Settings( organization, application )`. Qt uses its own Format
    //! argument in the same way. Second, a new format, for example TOML, is added here as a new
    //! enumerator and a new SettingsCodec, and no signature changes.
    //!
    //! Qt's NativeFormat is not here, because this library does not use the Windows registry or
    //! macOS property lists. Every platform writes INI.
    enum class SettingsFormat
    {
        //! The INI format that QSettings::IniFormat writes. A file written by one can be read by
        //! the other, with the limits that the Settings class comment gives.
        Ini
    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_FORMAT_HPP
