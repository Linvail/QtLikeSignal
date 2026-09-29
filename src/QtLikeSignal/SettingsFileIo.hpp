// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsFileIo - the file-system calls that the settings store needs, one set per
//! platform.

#ifndef QT_LIKE_SIGNAL_SETTINGS_FILE_IO_HPP
#define QT_LIKE_SIGNAL_SETTINGS_FILE_IO_HPP

#include <string>

namespace QtLikeSignal
{
    //! The file-system operations of the settings store.
    //!
    //! A class of static functions, so that one header declares them all, and so that
    //! SettingsFileIoPosix.cpp and SettingsFileIoWin.cpp each define the whole set. This is the
    //! same split as ThreadPosix.cpp and ThreadWin.cpp. `<filesystem>` is not used, because nothing
    //! else in the library uses it and it adds nothing that the platform calls do not give.
    //!
    //! Every path is UTF-8. On Windows it is converted to UTF-16 for the wide API, so a path with
    //! non-ASCII characters works on both platforms.
    //!
    //! Every function is safe to call from any thread at the same time, because each works only
    //! on its arguments and on the file system.
    class SettingsFileIo
    {
    public:
        //! What the store remembers about a file, to see whether another process changed it.
        //!
        //! The size and the modification time together, as QSettings uses them. A change that
        //! keeps both the size and the time the same is not seen. The time has nanosecond
        //! resolution where the platform gives it, which makes that case rare.
        struct Stamp
        {
            //! True if the file exists.
            bool mExists { false };

            //! The size of the file in bytes. 0 if it does not exist.
            unsigned long long mSize { 0 };

            //! The last modification time, in platform units: nanoseconds on POSIX, 100-nanosecond
            //! ticks on Windows. Only compared for equality, so the unit does not matter.
            long long mModified { 0 };

            //! Returns true if both stamps describe the same state of the file.
            bool operator==
                (
                const Stamp& aOther  //!< The stamp to compare with.
                ) const
            {
                return mExists == aOther.mExists && mSize == aOther.mSize &&
                       mModified == aOther.mModified;
            }

            //! Returns true if the stamps differ. See operator==().
            bool operator!=
                (
                const Stamp& aOther  //!< The stamp to compare with.
                ) const
            {
                return !( *this == aOther );
            }

        };

        static Stamp stamp
            (
            const std::string& aPath
            );

        static bool readAll
            (
            const std::string& aPath,
            std::string& aData
            );

        static bool writeAtomically
            (
            const std::string& aPath,
            const std::string& aData
            );

        static bool makeParentDirectories
            (
            const std::string& aPath
            );

        static bool isWritable
            (
            const std::string& aPath
            );

        static std::string absolutePath
            (
            const std::string& aPath
            );

        static std::string userConfigDirectory();

        static char separator();

    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_FILE_IO_HPP
