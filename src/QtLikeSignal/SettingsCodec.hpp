// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsCodec - the interface between the settings store and one file format.

#ifndef QT_LIKE_SIGNAL_SETTINGS_CODEC_HPP
#define QT_LIKE_SIGNAL_SETTINGS_CODEC_HPP

#include "QtLikeSignal/SettingsFormat.hpp"
#include "QtLikeSignal/SettingsValue.hpp"

#include <cstddef>
#include <map>
#include <string>

namespace QtLikeSignal
{
    //! Turns the bytes of a settings file into keys and values, and back.
    //!
    //! This is the only part of the settings store that knows the file format. SettingsFile does
    //! the caching, the locking and the atomic replace, and asks a codec only for these two
    //! conversions. So a new format, for example TOML, is one new subclass and one new
    //! SettingsFormat enumerator. Nothing else changes.
    //!
    //! A codec has no state. The one instance of each is shared by every thread, which is safe
    //! because read() and write() are const and touch only their arguments.
    class SettingsCodec
    {
    public:
        //! One key in a file: its value, and where it was in the file.
        //!
        //! The position keeps the order of a file that a person wrote by hand. When a file is
        //! written back, the sections and the keys come out in the order they were read, and a
        //! key that the program added comes after them. Without it, every write would sort the
        //! file, and a person who reads the file would lose the order that they chose.
        struct Entry
        {
            //! The value.
            SettingsValue mValue;

            //! The order of the key in the file. A lower number is written first. The codec sets
            //! it when it reads a file. SettingsFile sets it for a key that the program adds.
            std::size_t mPosition { 0 };
        };

        //! All the keys of one file, by their full key: the group, a '/', and the name, as in
        //! "window/width". A key at the top level has no '/'.
        //!
        //! A std::map, so the keys are sorted by their bytes. That makes the keys under one group
        //! a contiguous range, which is how Settings::childKeys() and remove() find them.
        using KeyMap = std::map<std::string, Entry>;

        virtual ~SettingsCodec();

        //! Reads @p aData into @p aKeys.
        //!
        //! @return false if a part of the data is not in the format. The keys that could be read
        //!         are still in @p aKeys, so a caller that ignores the error still gets as much of
        //!         the file as possible. This is what QSettings does.
        virtual bool read
            (
            const std::string& aData,  //!< The whole file.
            KeyMap& aKeys              //!< Receives the keys. Must be empty on entry.
            ) const = 0;

        //! Writes @p aKeys into @p aData, in the order of their positions.
        virtual void write
            (
            const KeyMap& aKeys,  //!< The keys to write.
            std::string& aData    //!< Receives the whole file. Must be empty on entry.
            ) const = 0;

        static const SettingsCodec& forFormat
            (
            SettingsFormat aFormat
            );

    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_CODEC_HPP
