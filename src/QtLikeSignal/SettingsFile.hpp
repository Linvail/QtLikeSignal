// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::SettingsFile - the contents of one settings file, shared by every Settings object
//! that names it.

#ifndef QT_LIKE_SIGNAL_SETTINGS_FILE_HPP
#define QT_LIKE_SIGNAL_SETTINGS_FILE_HPP

#include "QtLikeSignal/SettingsCodec.hpp"
#include "QtLikeSignal/SettingsFileIo.hpp"
#include "QtLikeSignal/SettingsFormat.hpp"
#include "QtLikeSignal/SettingsStatus.hpp"
#include "QtLikeSignal/SettingsValue.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace QtLikeSignal
{
    //! One settings file in memory: the keys read from it, and the changes not yet written.
    //!
    //! This is QConfFile from qsettings.cpp. Settings is the public face; this is the store under
    //! it. Settings objects that name the same file share one instance, through open(), so a
    //! value that one of them sets is seen by the others at once, before any write.
    //!
    //! **Three key maps, not one.** The file's own keys are kept apart from the changes:
    //! - mOriginalKeys: the keys as the file had them when it was last read or written.
    //! - mAddedKeys: keys set since then.
    //! - mRemovedKeys: keys of mOriginalKeys removed since then.
    //! The split is what makes a write safe against another process. sync() reads the file again
    //! under the lock, and puts the changes on top of what the file holds *now*. So two processes
    //! that change two different keys both keep their change, and neither overwrites the other
    //! with an old copy.
    //!
    //! **Thread-safe.** Every public function takes mMutex, so Settings objects in different
    //! threads can share an instance. A Settings object itself is only reentrant; see Settings.
    class SettingsFile
    {
    public:
        //! What children() collects under a prefix.
        enum class ChildSpec
        {
            AllKeys,     //!< Every key, at any depth, relative to the prefix.
            ChildKeys,   //!< The keys directly under the prefix.
            ChildGroups  //!< The groups directly under the prefix.
        };

        static std::shared_ptr<SettingsFile> open
            (
            const std::string& aFileName,
            SettingsFormat aFormat
            );

        ~SettingsFile();

        SettingsFile
            (
            const SettingsFile&
            ) = delete;

        SettingsFile& operator=
            (
            const SettingsFile&
            ) = delete;

        //! Returns the absolute path of the file. Constant, so no lock is needed.
        const std::string& fileName() const
        {
            return mFileName;
        }

        //! Returns the format of the file. Constant, so no lock is needed.
        SettingsFormat format() const
        {
            return mFormat;
        }

        bool get
            (
            const std::string& aKey,
            SettingsValue& aValue
            ) const;

        void set
            (
            const std::string& aKey,
            const SettingsValue& aValue
            );

        void remove
            (
            const std::string& aKey
            );

        void clear();

        std::vector<std::string> children
            (
            const std::string& aPrefix,
            ChildSpec aSpec
            ) const;

        SettingsStatus sync();

        bool isWritable() const;

    private:
        SettingsFile
            (
            std::string aFileName,
            SettingsFormat aFormat
            );

        SettingsCodec::KeyMap mergedKeys() const;

        static std::string cacheKey
            (
            const std::string& aFileName,
            SettingsFormat aFormat
            );

        //! The absolute path of the file, in UTF-8. Empty if there is no place to put the file;
        //! every sync() then fails with AccessError.
        const std::string mFileName;

        //! The format of the file.
        const SettingsFormat mFormat;

        //! Guards every member below.
        mutable std::mutex mMutex;

        //! The keys as the file had them at the last read or write.
        SettingsCodec::KeyMap mOriginalKeys;

        //! Keys set since the last write. A key here hides the same key in mOriginalKeys.
        SettingsCodec::KeyMap mAddedKeys;

        //! Keys of mOriginalKeys removed since the last write.
        std::set<std::string> mRemovedKeys;

        //! The size and time of the file when it was last read or written. sync() compares it with
        //! the file on disk to see whether another process changed the file.
        SettingsFileIo::Stamp mStamp;

        //! The position that the next set() gives a key. It starts high, so that a key that the
        //! program adds is written after the keys that were in the file. See SettingsCodec::Entry.
        std::size_t mNextPosition { 0x40000000 };
    };

}

#endif // QT_LIKE_SIGNAL_SETTINGS_FILE_HPP
