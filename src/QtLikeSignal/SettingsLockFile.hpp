// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsLockFile - a lock that stops two processes from writing one settings file
//! at the same time.

#ifndef QT_LIKE_SIGNAL_SETTINGS_LOCK_FILE_HPP
#define QT_LIKE_SIGNAL_SETTINGS_LOCK_FILE_HPP

#include <cstdint>
#include <string>

namespace QtLikeSignal
{
    //! An exclusive lock between processes, held on a file `<settings file>.lock`.
    //!
    //! @code
    //!   QtLikeSignal::SettingsLockFile lock( "/home/evan/.config/Example/Chartplotter.ini.lock" );
    //!   if( lock.tryLock( 5000 ) )
    //!   {
    //!       // read, merge and replace the settings file
    //!   }                                  // the destructor unlocks
    //! @endcode
    //!
    //! **The operating system releases the lock when the process ends.** This is the important
    //! property, and it is why this is not Qt's QLockFile. QLockFile writes the owner's process id
    //! into the file and must guess later whether a lock is stale: the owner can crash, and after
    //! a restart its process id can belong to a different program. Here the lock is not the file
    //! but a lock that the kernel holds for an open file, so a crash or a power cut can never
    //! leave a lock that nobody holds:
    //! - POSIX: flock( LOCK_EX ) on the lock file. After the lock is taken, the file is checked to
    //!   be the one at the path, because a process that unlocks deletes the file, and a process
    //!   that opened the old file just before that would otherwise hold a lock on a deleted file.
    //! - Windows: the lock file is opened with no sharing and FILE_FLAG_DELETE_ON_CLOSE. A second
    //!   open fails with a sharing violation until the handle is closed, and closing it deletes
    //!   the file.
    //!
    //! Only a write takes the lock. A read does not need one, because the writer replaces the file
    //! in one atomic step, so a reader sees either the old file or the new one.
    //!
    //! Not thread-safe. One thread uses one instance. Threads in one process that write the same
    //! file are kept apart before this lock, by the mutex in SettingsFile.
    class SettingsLockFile
    {
    public:
        explicit SettingsLockFile
            (
            std::string aPath
            );

        ~SettingsLockFile();

        SettingsLockFile
            (
            const SettingsLockFile&
            ) = delete;

        SettingsLockFile& operator=
            (
            const SettingsLockFile&
            ) = delete;

        bool tryLock
            (
            unsigned int aTimeoutMs
            );

        void unlock();

        //! Returns true if this instance holds the lock.
        bool isLocked() const
        {
            return mHandle != kNoHandle;
        }

    private:
        //! The value of mHandle when no lock is held. -1 is not a valid file descriptor, and it is
        //! INVALID_HANDLE_VALUE on Windows.
        static const std::intptr_t kNoHandle = -1;

        //! The path of the lock file, in UTF-8.
        std::string mPath;

        //! The open lock file while the lock is held: a file descriptor on POSIX, a HANDLE on
        //! Windows. kNoHandle when no lock is held.
        std::intptr_t mHandle { kNoHandle };
    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_LOCK_FILE_HPP
