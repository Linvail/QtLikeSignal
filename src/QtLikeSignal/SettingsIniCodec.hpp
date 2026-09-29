// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsIniCodec - reads and writes the INI format of QSettings::IniFormat.

#ifndef QT_LIKE_SIGNAL_SETTINGS_INI_CODEC_HPP
#define QT_LIKE_SIGNAL_SETTINGS_INI_CODEC_HPP

#include "QtLikeSignal/SettingsCodec.hpp"
#include "QtLikeSignal/SettingsValue.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace QtLikeSignal
{
    //! The INI format, as QSettings writes it.
    //!
    //! This is a port of the INI part of Qt 6's qsettings.cpp, not a new INI dialect. A file that
    //! QSettings wrote can be read here, and a file written here can be read by QSettings, with
    //! the limits below.
    //!
    //! @code
    //!   [General]
    //!   title=Chart plotter
    //!
    //!   [window]
    //!   width=800
    //!   geometry\x=10
    //!   recent="a, b", c
    //! @endcode
    //!
    //! **The rules, in short.**
    //! - A key "window/width" is written as `width=` in a section `[window]`. The top level is the
    //!   section `[General]`. A group that is itself called "general" is written `[%general]`.
    //! - Further '/' in a key are written as '\'. So "window/geometry/x" is `geometry\x=` in
    //!   `[window]`.
    //! - A byte in a key or a section name that is not an ASCII letter, a digit, '_', '-' or '.' is
    //!   written as `%XX`, or as `%UXXXX` for a character above U+00FF.
    //! - A value is quoted if it contains ';', ',' or '=', or starts or ends with a space. Control
    //!   characters, '"' and '\' are escaped with a backslash. Other text, including non-ASCII
    //!   text, is written as UTF-8.
    //! - A list is its elements, joined with ", ". An empty list is `@Invalid()`, so that it does
    //!   not look the same as a list of one empty string.
    //! - A string that starts with '@' is written with a second '@' in front, because '@' starts a
    //!   type marker such as `@Invalid()`.
    //! - A line that starts with ';' is a comment. Comments are not kept when the file is written.
    //!
    //! **Limits against QSettings.**
    //! - `@Variant(...)`, `@DateTime(...)`, `@Rect(...)`, `@Size(...)` and `@Point(...)` hold a
    //!   serialised Qt type. They are read as plain strings, marker and all, and never written.
    //! - `@ByteArray(...)` is read as a string, without the marker.
    //! - One change, to fix a case loss in Qt: Qt writes a group called "general" as
    //!   `[%General]`, and reads it back as "General". This codec writes `[%general]`, with the
    //!   case of the group, and Qt reads that correctly too.
    //!
    //! The static functions are public so that the test suite can check each rule on its own.
    class SettingsIniCodec : public SettingsCodec
    {
    public:
        bool read
            (
            const std::string& aData,
            KeyMap& aKeys
            ) const override;

        void write
            (
            const KeyMap& aKeys,
            std::string& aData
            ) const override;

        static void escapeKey
            (
            const std::string& aKey,
            std::string& aResult
            );

        static std::string unescapeKey
            (
            const std::string& aKey
            );

        static void escapeString
            (
            const std::string& aText,
            std::string& aResult
            );

        static void escapeStringList
            (
            const std::vector<std::string>& aList,
            std::string& aResult
            );

        static bool unescapeStringList
            (
            const std::string& aText,
            std::string& aString,
            std::vector<std::string>& aList
            );

        static std::string valueToString
            (
            const SettingsValue& aValue
            );

        static SettingsValue stringToValue
            (
            const std::string& aText
            );

        static SettingsValue stringListToValue
            (
            const std::vector<std::string>& aList
            );

    private:
        static bool readLine
            (
            const std::string& aData,
            std::size_t& aDataPos,
            std::size_t& aLineStart,
            std::size_t& aLineLength,
            std::size_t& aEqualsPos
            );

        static void writeValue
            (
            const SettingsValue& aValue,
            std::string& aResult
            );

    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_INI_CODEC_HPP
