// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Windows half of the settings file system: SettingsFileIo and SettingsLockFile, over the wide
//! file API, MoveFileExW() and a lock file that has no sharing.

#include "QtLikeSignal/SettingsFileIo.hpp"
#include "QtLikeSignal/SettingsLockFile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace QtLikeSignal
{
    namespace
    {
        //! How long tryLock() waits between two attempts. See the POSIX file.
        const std::chrono::milliseconds kLockRetryInterval( 10 );

        //! Converts UTF-8 to UTF-16 for the wide API. Returns an empty string for an empty or
        //! invalid input.
        std::wstring toWide
            (
            const std::string& aText  //!< UTF-8 text.
            )
        {
            if( aText.empty() )
            {
                return std::wstring();
            }
            const int length = ::MultiByteToWideChar( CP_UTF8, 0, aText.data(),
                static_cast<int>( aText.size() ), nullptr, 0 );
            if( length <= 0 )
            {
                return std::wstring();
            }
            std::wstring result( static_cast<std::size_t>( length ), L'\0' );
            ::MultiByteToWideChar( CP_UTF8, 0, aText.data(), static_cast<int>( aText.size() ),
                &result[0], length );
            return result;
        }

        //! Converts UTF-16 from the wide API to UTF-8.
        std::string toUtf8
            (
            const std::wstring& aText  //!< UTF-16 text.
            )
        {
            if( aText.empty() )
            {
                return std::string();
            }
            const int length = ::WideCharToMultiByte( CP_UTF8, 0, aText.data(),
                static_cast<int>( aText.size() ), nullptr, 0, nullptr, nullptr );
            if( length <= 0 )
            {
                return std::string();
            }
            std::string result( static_cast<std::size_t>( length ), '\0' );
            ::WideCharToMultiByte( CP_UTF8, 0, aText.data(), static_cast<int>( aText.size() ),
                &result[0], length, nullptr, nullptr );
            return result;
        }

        //! Returns true if @p aChar separates the parts of a path. Windows accepts both.
        bool isSeparator
            (
            char aChar  //!< The character.
            )
        {
            return aChar == '\\' || aChar == '/';
        }

        //! Returns the directory part of @p aPath, without the last separator. Empty if there is
        //! none.
        std::string parentOf
            (
            const std::string& aPath  //!< The path.
            )
        {
            const std::size_t separator = aPath.find_last_of( "\\/" );
            if( separator == std::string::npos )
            {
                return std::string();
            }
            return aPath.substr( 0, separator );
        }

        //! Returns true if @p aPath is a directory that exists.
        bool isDirectory
            (
            const std::string& aPath  //!< The path.
            )
        {
            const DWORD attributes = ::GetFileAttributesW( toWide( aPath ).c_str() );
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   ( attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
        }

        //! Returns the length of the root of @p aPath, which cannot be made: "C:\" is 3, a UNC
        //! "\\server\share\" is its whole length, and a relative path is 0.
        std::size_t rootLength
            (
            const std::string& aPath  //!< The path.
            )
        {
            if( aPath.size() >= 2 && aPath[1] == ':' )
            {
                return ( aPath.size() >= 3 && isSeparator( aPath[2] ) ) ? 3 : 2;
            }
            if( aPath.size() >= 2 && isSeparator( aPath[0] ) && isSeparator( aPath[1] ) )
            {
                // Skip "\\server\share\".
                std::size_t position = 2;
                for( int part = 0; part < 2 && position < aPath.size(); ++part )
                {
                    while( position < aPath.size() && !isSeparator( aPath[position] ) )
                    {
                        ++position;
                    }
                    if( position < aPath.size() )
                    {
                        ++position;
                    }
                }
                return position;
            }
            return isSeparator( aPath[0] ) ? 1 : 0;
        }

        //! Returns the value of the environment variable @p aName, in UTF-8. Empty if not set.
        std::string environment
            (
            const wchar_t* aName  //!< The variable.
            )
        {
            const DWORD length = ::GetEnvironmentVariableW( aName, nullptr, 0 );
            if( length == 0 )
            {
                return std::string();
            }
            std::wstring value( length, L'\0' );
            const DWORD written = ::GetEnvironmentVariableW( aName, &value[0], length );
            value.resize( written );
            return toUtf8( value );
        }

        //! Writes all of @p aData to @p aHandle. Returns false on any error.
        bool writeAll
            (
            HANDLE aHandle,           //!< The open file.
            const std::string& aData  //!< The bytes.
            )
        {
            std::size_t written = 0;
            while( written < aData.size() )
            {
                const std::size_t remaining = aData.size() - written;
                const DWORD chunk = remaining > 0x40000000 ? 0x40000000 :
                    static_cast<DWORD>( remaining );
                DWORD done = 0;
                if( !::WriteFile( aHandle, aData.data() + written, chunk, &done, nullptr ) )
                {
                    return false;
                }
                written += done;
            }
            return true;
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
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if( !aPath.empty() &&
            ::GetFileAttributesExW( toWide( aPath ).c_str(), GetFileExInfoStandard, &data ) &&
            ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) == 0 )
        {
            result.mExists = true;
            result.mSize = ( static_cast<unsigned long long>( data.nFileSizeHigh ) << 32 ) |
                data.nFileSizeLow;
            result.mModified = static_cast<long long>(
                ( static_cast<unsigned long long>( data.ftLastWriteTime.dwHighDateTime ) << 32 ) |
                data.ftLastWriteTime.dwLowDateTime );
        }
        return result;
    }

    //! Reads the whole file at @p aPath into @p aData.
    //!
    //! The file is opened with every sharing flag, including FILE_SHARE_DELETE, so that a reader
    //! never stops a writer from replacing the file with MoveFileExW().
    //!
    //! @return false if the file cannot be opened or read. @p aData is then empty.
    bool SettingsFileIo::readAll
        (
        const std::string& aPath,  //!< The file.
        std::string& aData         //!< Receives the bytes.
        )
    {
        aData.clear();
        const HANDLE handle = ::CreateFileW( toWide( aPath ).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            return false;
        }

        char buffer[16384];
        bool ok = true;
        for( ;; )
        {
            DWORD done = 0;
            if( !::ReadFile( handle, buffer, sizeof( buffer ), &done, nullptr ) )
            {
                ok = false;
                break;
            }
            if( done == 0 )
            {
                break;
            }
            aData.append( buffer, done );
        }
        ::CloseHandle( handle );
        if( !ok )
        {
            aData.clear();
        }
        return ok;
    }

    //! Replaces the file at @p aPath with @p aData, in one atomic step.
    //!
    //! The data goes to `<path>.<pid>.tmp` first, which is flushed and then moved over the file
    //! with MoveFileExW( MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ). On NTFS that
    //! replaces the file in one step, so a reader sees the whole old file or the whole new one.
    //!
    //! It fails if another program has the file open without FILE_SHARE_DELETE, for example an
    //! editor. The old file is then unchanged.
    //!
    //! @return false if any step fails. The temporary file is then removed.
    bool SettingsFileIo::writeAtomically
        (
        const std::string& aPath,  //!< The file to replace or create.
        const std::string& aData   //!< Its new contents.
        )
    {
        const std::wstring target = toWide( aPath );
        const std::wstring temporary = toWide( aPath + "." +
            std::to_string( ::GetCurrentProcessId() ) + ".tmp" );
        const HANDLE handle = ::CreateFileW( temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            return false;
        }

        bool ok = writeAll( handle, aData ) && ::FlushFileBuffers( handle );
        ::CloseHandle( handle );
        if( ok )
        {
            ok = ::MoveFileExW( temporary.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0;
        }
        if( !ok )
        {
            ::DeleteFileW( temporary.c_str() );
        }
        return ok;
    }

    //! Makes every missing directory above the file @p aPath, as `mkdir -p` does. Both '\' and
    //! '/' separate parts. A drive root or a UNC share is not made.
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

        std::size_t position = rootLength( directory );
        while( position <= directory.size() )
        {
            std::size_t separator = directory.find_first_of( "\\/", position );
            if( separator == std::string::npos )
            {
                separator = directory.size();
            }
            if( separator > position )
            {
                const std::string part = directory.substr( 0, separator );
                if( !::CreateDirectoryW( toWide( part ).c_str(), nullptr ) &&
                    ::GetLastError() != ERROR_ALREADY_EXISTS )
                {
                    return false;
                }
            }
            position = separator + 1;
        }
        return isDirectory( directory );
    }

    //! Returns true if the settings file at @p aPath can be written.
    //!
    //! An existing file must open for writing. For a new file, its directory is made if needed,
    //! and a probe file is made there and deleted, because Windows gives no cheap answer to "may
    //! I make a file in this directory" -- the access rules are ACLs, not mode bits.
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
        if( stamp( aPath ).mExists )
        {
            const HANDLE handle = ::CreateFileW( toWide( aPath ).c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr );
            if( handle == INVALID_HANDLE_VALUE )
            {
                return false;
            }
            ::CloseHandle( handle );
            return true;
        }

        if( !makeParentDirectories( aPath ) )
        {
            return false;
        }
        const std::wstring probe = toWide( aPath + "." +
            std::to_string( ::GetCurrentProcessId() ) + ".probe" );
        const HANDLE handle = ::CreateFileW( probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr );
        if( handle == INVALID_HANDLE_VALUE )
        {
            return false;
        }
        ::CloseHandle( handle );
        return true;
    }

    //! Returns @p aPath as an absolute path, through GetFullPathNameW(). This also resolves "."
    //! and "..", and turns '/' into '\'. Returns @p aPath if the call fails.
    std::string SettingsFileIo::absolutePath
        (
        const std::string& aPath  //!< The path.
        )
    {
        if( aPath.empty() )
        {
            return aPath;
        }
        const std::wstring wide = toWide( aPath );
        const DWORD length = ::GetFullPathNameW( wide.c_str(), 0, nullptr, nullptr );
        if( length == 0 )
        {
            return aPath;
        }
        std::wstring result( length, L'\0' );
        const DWORD written = ::GetFullPathNameW( wide.c_str(), length, &result[0], nullptr );
        if( written == 0 || written >= length )
        {
            return aPath;
        }
        result.resize( written );
        return toUtf8( result );
    }

    //! Returns the directory for a user's settings files, without a trailing '\'.
    //!
    //! %APPDATA%, which is the roaming application data folder, FOLDERID_RoamingAppData. Qt asks
    //! SHGetKnownFolderPath() for the same folder; the environment variable gives the same answer
    //! in a normal session and needs no link to shell32 and ole32, which every application that
    //! builds this library would otherwise have to add. If %APPDATA% is not set, as in some
    //! services, it is %USERPROFILE%\AppData\Roaming. Empty if neither is set.
    std::string SettingsFileIo::userConfigDirectory()
    {
        std::string result = environment( L"APPDATA" );
        if( result.empty() )
        {
            const std::string profile = environment( L"USERPROFILE" );
            if( profile.empty() )
            {
                return std::string();
            }
            result = profile + "\\AppData\\Roaming";
        }
        while( result.size() > 3 && isSeparator( result.back() ) )
        {
            result.pop_back();
        }
        return result;
    }

    //! Returns the separator between the parts of a path: '\'.
    char SettingsFileIo::separator()
    {
        return '\\';
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
    //! The lock is an open handle to the lock file with no sharing. A second CreateFileW() fails
    //! with ERROR_SHARING_VIOLATION while the handle is open. ERROR_ACCESS_DENIED is also a reason
    //! to wait: it is what CreateFileW() reports for a file that is being deleted, which is the
    //! moment just after the holder closed it.
    //!
    //! @return true if the lock is held. false if the time ran out, or if the lock file cannot be
    //!         made.
    bool SettingsLockFile::tryLock
        (
        unsigned int aTimeoutMs  //!< The longest time to wait, in milliseconds. 0 tries once.
        )
    {
        if( isLocked() )
        {
            return true;
        }

        const std::wstring path = toWide( mPath );
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds( aTimeoutMs );
        for( ;; )
        {
            const HANDLE handle = ::CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr );
            if( handle != INVALID_HANDLE_VALUE )
            {
                mHandle = reinterpret_cast<std::intptr_t>( handle );
                return true;
            }

            const DWORD error = ::GetLastError();
            if( error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED )
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

    //! Releases the lock, if it is held. Closing the handle also deletes the lock file, because
    //! it was opened with FILE_FLAG_DELETE_ON_CLOSE.
    void SettingsLockFile::unlock()
    {
        if( !isLocked() )
        {
            return;
        }
        ::CloseHandle( reinterpret_cast<HANDLE>( mHandle ) );
        mHandle = kNoHandle;
    }

}
