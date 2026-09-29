// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! SettingsFile implementation: the shared cache, the three key maps, and sync().
//!
//! Ported from QConfFile and QConfFileSettingsPrivate in Qt 6's qsettings.cpp: fromName(),
//! mergedKeyMap(), set(), remove(), clear(), get(), children() and syncConfFile().

#include "QtLikeSignal/SettingsFile.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/SettingsLockFile.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace QtLikeSignal
{
    namespace
    {
        //! How long sync() waits for another process to release the write lock, in milliseconds.
        //!
        //! A writer holds the lock only while it reads, merges and replaces one small file, which
        //! takes milliseconds. Five seconds is far more than that. It is not "for ever", as it is
        //! for QLockFile::lock(), because sync() can run on a thread with an event loop, and a
        //! stuck lock -- for example a file system that does not support locks -- must not stop
        //! the loop for good. After the time, sync() reports AccessError, and the changes stay in
        //! memory for the next sync().
        const unsigned int kLockTimeoutMs = 5000;

        //! The process-wide cache of open files: from cacheKey() to the file.
        //!
        //! Weak, so that the cache does not keep a file alive. When the last Settings object that
        //! names a file is destroyed, the file is destroyed, and the next Settings object reads it
        //! again. Qt keeps a few unused files in a second cache as well; that saves a read of a
        //! small file and is left out.
        struct FileCache
        {
            //! Guards mFiles.
            std::mutex mMutex;

            //! The files, by cacheKey().
            std::map<std::string, std::weak_ptr<SettingsFile> > mFiles;
        };

        //! Returns the one cache.
        //!
        //! Made on first use and never destroyed. A Settings object with static storage duration
        //! can be destroyed after every other static, and its SettingsFile destructor then still
        //! needs the cache. A function-local static object would already be gone at that point.
        //! The pointer stays reachable, so a leak checker does not report it.
        FileCache& fileCache()
        {
            static FileCache* const cache = new FileCache();
            return *cache;
        }

        //! Returns true if @p aKey starts with @p aPrefix.
        bool startsWith
            (
            const std::string& aKey,    //!< The key.
            const std::string& aPrefix  //!< The prefix.
            )
        {
            return aKey.compare( 0, aPrefix.size(), aPrefix ) == 0;
        }

        //! Adds the part of @p aRelativeKey that @p aSpec asks for to @p aResult.
        //!
        //! This is QSettingsPrivate::processChild(). For ChildKeys, a key with a '/' is in a
        //! sub-group and is skipped. For ChildGroups, a key without one is skipped, and a key with
        //! one gives the part before it.
        void processChild
            (
            const std::string& aRelativeKey,     //!< The key without the prefix.
            SettingsFile::ChildSpec aSpec,       //!< What to collect.
            std::vector<std::string>& aResult    //!< Where to add it.
            )
        {
            if( aSpec == SettingsFile::ChildSpec::AllKeys )
            {
                aResult.push_back( aRelativeKey );
                return;
            }
            const std::size_t slash = aRelativeKey.find( '/' );
            if( slash == std::string::npos )
            {
                if( aSpec == SettingsFile::ChildSpec::ChildKeys )
                {
                    aResult.push_back( aRelativeKey );
                }
            }
            else if( aSpec == SettingsFile::ChildSpec::ChildGroups )
            {
                aResult.push_back( aRelativeKey.substr( 0, slash ) );
            }
        }

        //! Adds the keys of @p aKeys that start with @p aPrefix to @p aResult, through
        //! processChild(). A key in @p aSkip is not added.
        void collectChildren
            (
            const SettingsCodec::KeyMap& aKeys,   //!< The keys to search.
            const std::set<std::string>* aSkip,   //!< Keys to leave out. Can be null.
            const std::string& aPrefix,           //!< The prefix, with a trailing '/' or empty.
            SettingsFile::ChildSpec aSpec,        //!< What to collect.
            std::vector<std::string>& aResult     //!< Where to add the names.
            )
        {
            for( auto it = aKeys.lower_bound( aPrefix );
                it != aKeys.end() && startsWith( it->first, aPrefix ); ++it )
            {
                if( aSkip != nullptr && aSkip->count( it->first ) != 0 )
                {
                    continue;
                }
                processChild( it->first.substr( aPrefix.size() ), aSpec, aResult );
            }
        }
    }

    //! Constructs an empty file for @p aFileName. Only open() calls it.
    //!
    //! The file is not read here. The caller calls sync(), which reads it.
    SettingsFile::SettingsFile
        (
        std::string aFileName,   //!< The absolute path.
        SettingsFormat aFormat   //!< The format.
        )
        : mFileName( std::move( aFileName ) )
        , mFormat( aFormat )
    {
    }

    //! Returns the shared file for @p aFileName and @p aFormat, and makes it if no Settings object
    //! has it open.
    //!
    //! @p aFileName should be absolute; see SettingsFileIo::absolutePath(). Two names for one file
    //! that are not the same text, for example through a symbolic link, give two instances. They
    //! still do not lose each other's changes, because sync() merges with the file on disk; they
    //! only do not see each other's changes before a sync.
    std::shared_ptr<SettingsFile> SettingsFile::open
        (
        const std::string& aFileName,  //!< The absolute path of the file.
        SettingsFormat aFormat         //!< The format of the file.
        )
    {
        FileCache& cache = fileCache();
        const std::string key = cacheKey( aFileName, aFormat );
        std::lock_guard<std::mutex> lock( cache.mMutex );

        std::weak_ptr<SettingsFile>& entry = cache.mFiles[key];
        std::shared_ptr<SettingsFile> file = entry.lock();
        if( !file )
        {
            // Not make_shared: the constructor is private, so that open() is the only way in.
            file.reset( new SettingsFile( aFileName, aFormat ) );
            entry = file;
        }
        return file;
    }

    //! Destroys the file, and drops its entry from the cache.
    //!
    //! The entry is dropped only if it is expired. open() can have put a new instance for the same
    //! file in the entry between the last reference to this one going and this destructor taking
    //! the cache mutex; that entry is not expired and must stay.
    SettingsFile::~SettingsFile()
    {
        FileCache& cache = fileCache();
        std::lock_guard<std::mutex> lock( cache.mMutex );
        const auto it = cache.mFiles.find( cacheKey( mFileName, mFormat ) );
        if( it != cache.mFiles.end() && it->second.expired() )
        {
            cache.mFiles.erase( it );
        }
    }

    //! Returns the key of @p aFileName and @p aFormat in the cache. The format is part of it, so
    //! that the same path opened in two formats does not share one set of keys.
    std::string SettingsFile::cacheKey
        (
        const std::string& aFileName,  //!< The absolute path.
        SettingsFormat aFormat         //!< The format.
        )
    {
        return std::to_string( static_cast<int>( aFormat ) ) + ":" + aFileName;
    }

    //! Finds @p aKey. A key set since the last write wins over the file's own key, and a removed
    //! key is not found.
    //!
    //! @return true if the key exists. Then @p aValue receives its value.
    bool SettingsFile::get
        (
        const std::string& aKey,  //!< The full key.
        SettingsValue& aValue     //!< Receives the value.
        ) const
    {
        std::lock_guard<std::mutex> lock( mMutex );
        const auto added = mAddedKeys.find( aKey );
        if( added != mAddedKeys.end() )
        {
            aValue = added->second.mValue;
            return true;
        }
        const auto original = mOriginalKeys.find( aKey );
        if( original != mOriginalKeys.end() && mRemovedKeys.count( aKey ) == 0 )
        {
            aValue = original->second.mValue;
            return true;
        }
        return false;
    }

    //! Sets @p aKey to @p aValue in memory. The next sync() writes it.
    //!
    //! The key gets a new position from mNextPosition. If the file already has the key, the write
    //! keeps the old position instead; see mergedKeys().
    void SettingsFile::set
        (
        const std::string& aKey,       //!< The full key.
        const SettingsValue& aValue    //!< The value.
        )
    {
        std::lock_guard<std::mutex> lock( mMutex );
        mRemovedKeys.erase( aKey );
        SettingsCodec::Entry& entry = mAddedKeys[aKey];
        entry.mValue = aValue;
        entry.mPosition = mNextPosition++;
    }

    //! Removes @p aKey and every key under it, `aKey/...`, in memory. The next sync() writes it.
    void SettingsFile::remove
        (
        const std::string& aKey  //!< The full key. Not empty; see clear().
        )
    {
        std::lock_guard<std::mutex> lock( mMutex );
        const std::string prefix = aKey + "/";

        auto added = mAddedKeys.lower_bound( prefix );
        while( added != mAddedKeys.end() && startsWith( added->first, prefix ) )
        {
            added = mAddedKeys.erase( added );
        }
        mAddedKeys.erase( aKey );

        for( auto original = mOriginalKeys.lower_bound( prefix );
            original != mOriginalKeys.end() && startsWith( original->first, prefix ); ++original )
        {
            mRemovedKeys.insert( original->first );
        }
        if( mOriginalKeys.count( aKey ) != 0 )
        {
            mRemovedKeys.insert( aKey );
        }
    }

    //! Removes every key, in memory. The next sync() writes an empty file.
    void SettingsFile::clear()
    {
        std::lock_guard<std::mutex> lock( mMutex );
        mAddedKeys.clear();
        mRemovedKeys.clear();
        for( const auto& original : mOriginalKeys )
        {
            mRemovedKeys.insert( original.first );
        }
    }

    //! Returns the keys or groups under @p aPrefix, relative to it, sorted and without repeats.
    std::vector<std::string> SettingsFile::children
        (
        const std::string& aPrefix,  //!< The group, with a trailing '/', or empty for the top.
        ChildSpec aSpec              //!< What to collect.
        ) const
    {
        std::vector<std::string> result;
        {
            std::lock_guard<std::mutex> lock( mMutex );
            collectChildren( mOriginalKeys, &mRemovedKeys, aPrefix, aSpec, result );
            collectChildren( mAddedKeys, nullptr, aPrefix, aSpec, result );
        }
        std::sort( result.begin(), result.end() );
        result.erase( std::unique( result.begin(), result.end() ), result.end() );
        return result;
    }

    //! Returns the keys that the file will hold after the next write: mOriginalKeys without
    //! mRemovedKeys, with mAddedKeys on top.
    //!
    //! A key that the file had and the program set again keeps the file's position, so changing a
    //! value does not move its line to the end of the file. The caller holds mMutex.
    SettingsCodec::KeyMap SettingsFile::mergedKeys() const
    {
        SettingsCodec::KeyMap result = mOriginalKeys;
        for( const std::string& removed : mRemovedKeys )
        {
            result.erase( removed );
        }
        for( const auto& added : mAddedKeys )
        {
            const auto existing = result.find( added.first );
            if( existing != result.end() )
            {
                existing->second.mValue = added.second.mValue;
            }
            else
            {
                result.insert( added );
            }
        }
        return result;
    }

    //! Brings the memory and the file into agreement: reads the file again if another process
    //! changed it, and writes the changes made here.
    //!
    //! This is syncConfFile() from qsettings.cpp, in the same order:
    //! 1. With no changes to write, and a file that is the same as at the last read, return.
    //! 2. With changes to write, check that the file is writable, and take the write lock.
    //! 3. If the file changed on disk -- always, with no changes -- read it again. Its keys become
    //!    the new mOriginalKeys. The changes stay as they are, on top of the new keys.
    //! 4. With changes, write mergedKeys() through an atomic replace, and make it the new
    //!    mOriginalKeys.
    //!
    //! A file that does not exist reads as empty. A file that exists but cannot be read is an
    //! AccessError, and nothing is written, because a write would lose the keys in it.
    //!
    //! @return the first error, or NoError. After an error the changes stay in memory, and the
    //!         next sync() tries again.
    SettingsStatus SettingsFile::sync()
    {
        std::lock_guard<std::mutex> lock( mMutex );
        const SettingsCodec& codec = SettingsCodec::forFormat( mFormat );
        const bool readOnly = mAddedKeys.empty() && mRemovedKeys.empty();

        SettingsFileIo::Stamp current = SettingsFileIo::stamp( mFileName );
        if( readOnly && mStamp.mSize > 0 && current == mStamp )
        {
            return SettingsStatus::NoError;
        }

        SettingsLockFile lockFile( mFileName + ".lock" );
        if( !readOnly )
        {
            if( mFileName.empty() || !SettingsFileIo::isWritable( mFileName ) )
            {
                return SettingsStatus::AccessError;
            }
            if( !lockFile.tryLock( kLockTimeoutMs ) )
            {
                qCWarning( gLogSettings ) << "Settings: could not lock" << mFileName
                                          << "for writing within" << kLockTimeoutMs << "ms";
                return SettingsStatus::AccessError;
            }
            current = SettingsFileIo::stamp( mFileName );
        }

        SettingsStatus status = SettingsStatus::NoError;
        if( readOnly || current != mStamp )
        {
            mOriginalKeys.clear();
            if( current.mExists )
            {
                std::string data;
                if( !SettingsFileIo::readAll( mFileName, data ) )
                {
                    return SettingsStatus::AccessError;
                }
                if( !codec.read( data, mOriginalKeys ) )
                {
                    status = SettingsStatus::FormatError;
                }
            }
            mStamp = current;
        }

        if( !readOnly )
        {
            SettingsCodec::KeyMap merged = mergedKeys();
            std::string data;
            codec.write( merged, data );
            if( !SettingsFileIo::writeAtomically( mFileName, data ) )
            {
                return status == SettingsStatus::NoError ? SettingsStatus::AccessError : status;
            }
            mOriginalKeys = std::move( merged );
            mAddedKeys.clear();
            mRemovedKeys.clear();
            mStamp = SettingsFileIo::stamp( mFileName );
        }
        return status;
    }

    //! Returns true if the file can be written. See SettingsFileIo::isWritable().
    bool SettingsFile::isWritable() const
    {
        return SettingsFileIo::isWritable( mFileName );
    }

}
