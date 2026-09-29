// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! POSIX half of the settings file system: SettingsFileIo and SettingsLockFile, over open(2),
//! rename(2) and flock(2).

#include "QtLikeSignal/SettingsFileIo.hpp"
#include "QtLikeSignal/SettingsLockFile.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace QtLikeSignal
{
    namespace
    {
        //! How long tryLock() waits between two attempts. Short, because a writer holds the lock
        //! only while it reads, merges and replaces one small file.
        const std::chrono::milliseconds kLockRetryInterval( 10 );

        //! Returns the directory part of @p aPath, without the last '/'. Empty if there is no '/'.
        //! "/" for a file in the root directory.
        std::string parentOf
            (
            const std::string& aPath  //!< The path.
            )
        {
            const std::size_t slash = aPath.rfind( '/' );
            if( slash == std::string::npos )
            {
                return std::string();
            }
            return slash == 0 ? std::string( "/" ) : aPath.substr( 0, slash );
        }

        //! Returns true if @p aPath is a directory that exists.
        bool isDirectory
            (
            const std::string& aPath  //!< The path.
            )
        {
            struct stat info;
            return ::stat( aPath.c_str(), &info ) == 0 && S_ISDIR( info.st_mode );
        }

        //! Closes @p aFd. It does not retry after EINTR: on Linux the descriptor is closed even
        //! then, and a second close() could close a descriptor that another thread just opened.
        void closeFd
            (
            int aFd  //!< The descriptor.
            )
        {
            static_cast<void>( ::close( aFd ) );
        }

        //! Writes all of @p aData to @p aFd. Retries a short write and a write that a signal
        //! interrupts. Returns false on any other error.
        bool writeAll
            (
            int aFd,                  //!< The descriptor.
            const std::string& aData  //!< The bytes.
            )
        {
            std::size_t written = 0;
            while( written < aData.size() )
            {
                const ssize_t result = ::write( aFd, aData.data() + written, aData.size() - written
                                              );
                if( result < 0 )
                {
                    if( errno == EINTR )
                    {
                        continue;
                    }
                    return false;
                }
                written += static_cast<std::size_t>( result );
            }
            return true;
        }

        //! Returns the home directory: $HOME, or the password database if $HOME is not set.
        std::string homeDirectory()
        {
            const char* const home = std::getenv( "HOME" );
            if( home != nullptr && home[0] != '\0' )
            {
                return home;
            }

            long size = ::sysconf( _SC_GETPW_R_SIZE_MAX );
            if( size <= 0 )
            {
                size = 16384;
            }
            std::vector<char> buffer( static_cast<std::size_t>( size ) );
            struct passwd entry;
            struct passwd* found = nullptr;
            if( ::getpwuid_r( ::getuid(), &entry, buffer.data(), buffer.size(), &found ) == 0 &&
                found != nullptr && found->pw_dir != nullptr )
            {
                return found->pw_dir;
            }
            return std::string();
        }
    }

    //! Returns the current stamp of the file at @p aPath. A file that cannot be examined is
    //! reported as one that does not exist.
    SettingsFileIo::Stamp SettingsFileIo::stamp
        (
        const std::string& aPath  //!< The file.
        )
    {
        Stamp result;
        struct stat info;
        if( ::stat( aPath.c_str(), &info ) == 0 )
        {
            result.mExists = true;
            result.mSize = static_cast<unsigned long long>( info.st_size );
            #if defined( __APPLE__ )
                result.mModified = static_cast<long long>( info.st_mtimespec.tv_sec ) * 1000000000LL
                    +
                    info.st_mtimespec.tv_nsec;
            #else
                result.mModified = static_cast<long long>( info.st_mtim.tv_sec ) * 1000000000LL +
                    info.st_mtim.tv_nsec;
            #endif
        }
        return result;
    }

    //! Reads the whole file at @p aPath into @p aData.
    //!
    //! @return false if the file cannot be opened or read. @p aData is then empty.
    bool SettingsFileIo::readAll
        (
        const std::string& aPath,  //!< The file.
        std::string& aData         //!< Receives the bytes.
        )
    {
        aData.clear();
        const int fd = ::open( aPath.c_str(), O_RDONLY | O_CLOEXEC );
        if( fd < 0 )
        {
            return false;
        }

        char buffer[16384];
        bool ok = true;
        for( ;; )
        {
            const ssize_t result = ::read( fd, buffer, sizeof( buffer ) );
            if( result < 0 )
            {
                if( errno == EINTR )
                {
                    continue;
                }
                ok = false;
                break;
            }
            if( result == 0 )
            {
                break;
            }
            aData.append( buffer, static_cast<std::size_t>( result ) );
        }
        closeFd( fd );
        if( !ok )
        {
            aData.clear();
        }
        return ok;
    }

    //! Replaces the file at @p aPath with @p aData, in one atomic step.
    //!
    //! The data goes to `<path>.<pid>.tmp` first, which is flushed to the disk and then renamed
    //! over the file. rename(2) is atomic, so a reader, or a crash, sees either the whole old file
    //! or the whole new one, never a part. This is what QSaveFile does for QSettings.
    //!
    //! The new file gets the permission bits of the old one. Without this it would get the
    //! default bits of a new file, so a file that a person made private with chmod 600 would
    //! become readable by others after the first write.
    //!
    //! The directory must exist. See makeParentDirectories().
    //!
    //! @return false if any step fails. The old file is then unchanged, and the temporary file is
    //!         removed.
    bool SettingsFileIo::writeAtomically
        (
        const std::string& aPath,  //!< The file to replace or create.
        const std::string& aData   //!< Its new contents.
        )
    {
        const std::string temporary = aPath + "." + std::to_string( ::getpid() ) + ".tmp";
        const int fd = ::open( temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666 );
        if( fd < 0 )
        {
            return false;
        }

        bool ok = writeAll( fd, aData );
        if( ok )
        {
            struct stat original;
            if( ::stat( aPath.c_str(), &original ) == 0 )
            {
                ok = ::fchmod( fd, original.st_mode & 07777 ) == 0;
            }
        }
        if( ok )
        {
            ok = ::fsync( fd ) == 0;
        }
        closeFd( fd );
        if( ok )
        {
            ok = ::rename( temporary.c_str(), aPath.c_str() ) == 0;
        }
        if( !ok )
        {
            static_cast<void>( ::unlink( temporary.c_str() ) );
        }
        return ok;
    }

    //! Makes every missing directory above the file @p aPath, as `mkdir -p` does.
    //!
    //! @return true if the directory of @p aPath exists afterwards.
    bool SettingsFileIo::makeParentDirectories
        (
        const std::string& aPath  //!< A file path. The last component is not made.
        )
    {
        const std::string directory = parentOf( aPath );
        if( directory.empty() || isDirectory( directory ) )
        {
            return true;
        }

        std::size_t position = 1;
        while( position <= directory.size() )
        {
            const std::size_t slash = directory.find( '/', position );
            const std::string part = directory.substr( 0,
                slash == std::string::npos ? directory.size() : slash );
            if( ::mkdir( part.c_str(), 0777 ) != 0 && errno != EEXIST )
            {
                return false;
            }
            if( slash == std::string::npos )
            {
                break;
            }
            position = slash + 1;
        }
        return isDirectory( directory );
    }

    //! Returns true if the settings file at @p aPath can be written.
    //!
    //! Both the file and its directory must be writable: the file so that it can be opened, and
    //! the directory because writeAtomically() makes a temporary file there and renames it. A file
    //! that does not exist yet is writable if its directory can be made and written.
    //!
    //! Like QSettings::isWritable(), the answer can change as soon as it is given.
    bool SettingsFileIo::isWritable
        (
        const std::string& aPath  //!< The file.
        )
    {
        if( aPath.empty() )
        {
            return false;
        }
        std::string directory = parentOf( aPath );
        if( directory.empty() )
        {
            directory = ".";
        }

        if( stamp( aPath ).mExists )
        {
            const int fd = ::open( aPath.c_str(), O_WRONLY | O_CLOEXEC );
            if( fd < 0 )
            {
                return false;
            }
            closeFd( fd );
            return ::access( directory.c_str(), W_OK ) == 0;
        }

        return makeParentDirectories( aPath ) && ::access( directory.c_str(), W_OK ) == 0;
    }

    //! Returns @p aPath as an absolute path. A relative path is taken from the current directory.
    //!
    //! The result is the key of the process-wide file cache, so two Settings objects that name
    //! one file by a relative and by an absolute path share one cache entry. "." and ".." are not
    //! resolved, and symbolic links are not followed.
    std::string SettingsFileIo::absolutePath
        (
        const std::string& aPath  //!< The path.
        )
    {
        if( aPath.empty() || aPath[0] == '/' )
        {
            return aPath;
        }
        std::vector<char> buffer( 4096 );
        while( ::getcwd( buffer.data(), buffer.size() ) == nullptr )
        {
            if( errno != ERANGE )
            {
                return aPath;
            }
            buffer.resize( buffer.size() * 2 );
        }
        std::string result( buffer.data() );
        if( result.empty() || result.back() != '/' )
        {
            result += '/';
        }
        return result + aPath;
    }

    //! Returns the directory for a user's settings files, without a trailing '/'.
    //!
    //! $XDG_CONFIG_HOME if it is set. A relative value is taken from the home directory, as Qt
    //! does. Otherwise `~/.config`. Empty if there is no home directory.
    std::string SettingsFileIo::userConfigDirectory()
    {
        const char* const configHome = std::getenv( "XDG_CONFIG_HOME" );
        std::string result;
        if( configHome != nullptr && configHome[0] == '/' )
        {
            result = configHome;
        }
        else
        {
            const std::string home = homeDirectory();
            if( home.empty() )
            {
                return std::string();
            }
            result = home + "/" + ( ( configHome != nullptr && configHome[0] != '\0' ) ?
                std::string( configHome ) : std::string( ".config" ) );
        }
        while( result.size() > 1 && result.back() == '/' )
        {
            result.pop_back();
        }
        return result;
    }

    //! Returns the separator between the parts of a path: '/'.
    char SettingsFileIo::separator()
    {
        return '/';
    }

    //! Constructs an unlocked lock for the lock file at @p aPath.
    SettingsLockFile::SettingsLockFile
        (
        std::string aPath  //!< The lock file, usually `<settings file>.lock`.
        )
        : mPath( std::move( aPath ) )
    {
    }

    //! Unlocks, if the lock is held.
    SettingsLockFile::~SettingsLockFile()
    {
        unlock();
    }

    //! Takes the lock, and waits up to @p aTimeoutMs milliseconds for another process to release
    //! it.
    //!
    //! After flock() succeeds, the open file is compared with the file at the path. They differ
    //! when the previous holder unlocked -- which deletes the file -- after this process opened
    //! it. The lock is then on a deleted file and protects nothing, so it is dropped and the loop
    //! tries again with the new file.
    //!
    //! @return true if the lock is held. false if the time ran out, or if the lock file cannot be
    //!         made, for example because the directory is read-only.
    bool SettingsLockFile::tryLock
        (
        unsigned int aTimeoutMs  //!< The longest time to wait, in milliseconds. 0 tries once.
        )
    {
        if( isLocked() )
        {
            return true;
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds( aTimeoutMs );
        for( ;; )
        {
            const int fd = ::open( mPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666 );
            if( fd < 0 )
            {
                if( errno == EINTR )
                {
                    continue;
                }
                return false;
            }

            if( ::flock( fd, LOCK_EX | LOCK_NB ) == 0 )
            {
                struct stat opened;
                struct stat current;
                if( ::fstat( fd, &opened ) == 0 && ::stat( mPath.c_str(), &current ) == 0 &&
                    opened.st_dev == current.st_dev && opened.st_ino == current.st_ino )
                {
                    mHandle = fd;
                    return true;
                }
                closeFd( fd );
                continue;
            }

            const int error = errno;
            closeFd( fd );
            if( error != EWOULDBLOCK && error != EINTR )
            {
                return false;
            }
            if( std::chrono::steady_clock::now() >= deadline )
            {
                return false;
            }
            std::this_thread::sleep_for( kLockRetryInterval );
        }
    }

    //! Releases the lock, if it is held, and deletes the lock file.
    //!
    //! The file is deleted while the lock is still held, so no other process can take a lock on
    //! it between the two steps. A process that is waiting on the old file finds out in tryLock().
    void SettingsLockFile::unlock()
    {
        if( !isLocked() )
        {
            return;
        }
        static_cast<void>( ::unlink( mPath.c_str() ) );
        closeFd( static_cast<int>( mHandle ) );
        mHandle = kNoHandle;
    }

}
