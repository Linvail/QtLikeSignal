// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::Settings - mimics Qt's QSettings: keeps an application's settings in an INI file
//! between two runs.

#ifndef QT_LIKE_SIGNAL_SETTINGS_HPP
#define QT_LIKE_SIGNAL_SETTINGS_HPP

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/SettingsFormat.hpp"
#include "QtLikeSignal/SettingsStatus.hpp"
#include "QtLikeSignal/SettingsValue.hpp"

#include <memory>
#include <string>
#include <vector>

namespace QtLikeSignal
{
    class SettingsFile;

    //! Reads and writes an application's settings, in an INI file that QSettings can also read.
    //!
    //! @code
    //!   QtLikeSignal::CoreApplication::setOrganizationName( "Example" );
    //!   QtLikeSignal::CoreApplication::setApplicationName( "Chartplotter" );
    //!
    //!   QtLikeSignal::Settings settings;                 // ~/.config/Example/Chartplotter.ini
    //!
    //!   settings.beginGroup( "window" );
    //!   const int width = settings.value( "width", 800 );      // 800 if the key is not set
    //!   settings.setValue( "height", 600 );
    //!   settings.endGroup();
    //!
    //!   settings.beginWriteArray( "recent" );
    //!   for( int i = 0; i < 2; ++i )
    //!   {
    //!       settings.setArrayIndex( i );
    //!       settings.setValue( "path", i == 0 ? "/charts/a.kap" : "/charts/b.kap" );
    //!   }
    //!   settings.endArray();                        // writes recent/size=2
    //! @endcode
    //!
    //! **Where the file is.** One of three constructors picks it:
    //! - `Settings( organization, application )`: `<dir>/<organization>/<application>.ini`, or
    //!   `<dir>/<organization>.ini` if the application name is empty.
    //! - `Settings()`: the same, with the names from CoreApplication::setOrganizationName() and
    //!   CoreApplication::setApplicationName().
    //! - `Settings( fileName, SettingsFormat::Ini )`: that file.
    //!
    //! `<dir>` is `$XDG_CONFIG_HOME`, or `~/.config`, on Linux, and `%APPDATA%` on Windows -- the
    //! same folders that QSettings uses. setPath() changes it, which is what a test must do so
    //! that it does not write into the real user folder. An empty organization name gives
    //! AccessError, and the file `Unknown Organization/...`, as in Qt.
    //!
    //! **Keys.** A key is a path of names joined by '/', such as "window/width". Repeated and
    //! outer slashes are removed, so "/window//width/" is the same key. beginGroup() puts a prefix
    //! in front of every key until endGroup(). Keys are **case-sensitive on every platform**; this
    //! is a deliberate difference from Qt, which ignores case on Windows because the registry does.
    //! Here a key that differs only in case is a different key everywhere, so a program that works
    //! on Linux works the same way on Windows.
    //!
    //! **Values.** A value is a SettingsValue: a bool, an integer, a double, a string, a list of
    //! strings, or nothing. value() gives back a SettingsValue. value( key, default ) with a typed
    //! default gives back that type, and gives the default if the key is missing or its text does
    //! not convert. A value read from the file is text, because the file is text; see
    //! SettingsValue.
    //!
    //! **When the file is written.** setValue() and remove() change memory at once. The file is
    //! written later: when control next returns to the event loop of this object's thread, when
    //! sync() is called, or when this object is destroyed. So many changes in a row cost one
    //! write. The write is an atomic replace under a lock between processes, and it merges with
    //! the file on disk, so two programs that change different keys keep both changes. A thread
    //! with no event loop writes only at sync() and at destruction.
    //!
    //! **Reentrant, not thread-safe**, in the sense Global.hpp gives these words. Two Settings
    //! objects in two threads are safe, also when they name the same file: the store that they
    //! share is thread-safe. One Settings object used from two threads at the same time is not,
    //! because the group stack and the status are its own.
    //!
    //! **Not in this class, against QSettings:** the Windows registry and macOS property lists
    //! (every platform writes INI), the system-wide scope and the fallback files (one file per
    //! object), custom formats, and types other than the six of SettingsValue. See SettingsIniCodec
    //! for the file format.
    class Settings : public Object
    {
    public:
        explicit Settings
            (
            Object* aParent = nullptr
            );

        Settings
            (
            const std::string& aOrganization,
            const std::string& aApplication = std::string(),
            Object* aParent = nullptr
            );

        Settings
            (
            const std::string& aFileName,
            SettingsFormat aFormat,
            Object* aParent = nullptr
            );

        ~Settings() override;

        void clear();

        void sync();

        SettingsStatus status() const;

        void beginGroup
            (
            const std::string& aPrefix
            );

        void endGroup();

        std::string group() const;

        int beginReadArray
            (
            const std::string& aPrefix
            );

        void beginWriteArray
            (
            const std::string& aPrefix,
            int aSize = -1
            );

        void endArray();

        void setArrayIndex
            (
            int aIndex
            );

        std::vector<std::string> allKeys() const;

        std::vector<std::string> childKeys() const;

        std::vector<std::string> childGroups() const;

        bool isWritable() const;

        void setValue
            (
            const std::string& aKey,
            const SettingsValue& aValue
            );

        SettingsValue value
            (
            const std::string& aKey,
            const SettingsValue& aDefaultValue = SettingsValue()
            ) const;

        //! Returns the value of @p aKey as T. See the definition below.
        template <typename T>
        T value
            (
            const std::string& aKey,
            const T& aDefaultValue
            ) const;

        std::string value
            (
            const std::string& aKey,
            const char* aDefaultValue
            ) const;

        void remove
            (
            const std::string& aKey
            );

        bool contains
            (
            const std::string& aKey
            ) const;

        std::string fileName() const;

        SettingsFormat format() const;

        std::string organizationName() const;

        std::string applicationName() const;

        static void setPath
            (
            SettingsFormat aFormat,
            const std::string& aPath
            );

    private:
        //! One entry of the group stack: a group from beginGroup(), or an array from
        //! beginReadArray() or beginWriteArray().
        //!
        //! This is QSettingsGroup. An array element is a group whose name is the array's name and a
        //! one-based index: element 0 of "recent" is the group "recent/1".
        struct Group
        {
            //! The normalized name, as given to beginGroup() or to the array function.
            std::string mName;

            //! -1 for a plain group. For an array, the current index plus one, or 0 before the
            //! first setArrayIndex().
            int mNumber { -1 };

            //! For an array written with no size: the highest index plus one that was used, which
            //! endArray() writes as the size. -1 otherwise.
            int mMaxNumber { -1 };

            //! Returns true if this entry is an array.
            bool isArray() const
            {
                return mNumber != -1;
            }

            //! Returns the text that this entry adds to the key prefix: the name, and for an array
            //! element also '/' and the one-based index.
            std::string toString() const
            {
                return mNumber > 0 ? mName + "/" + std::to_string( mNumber ) : mName;
            }

        };

        void openFile
            (
            const std::string& aFileName,
            SettingsFormat aFormat
            );

        static std::string normalizedKey
            (
            const std::string& aKey
            );

        bool lookup
            (
            const std::string& aKey,
            const char* aCaller,
            SettingsValue& aValue
            ) const;

        void beginGroupOrArray
            (
            const Group& aGroup
            );

        void setStatus
            (
            SettingsStatus aStatus
            ) const;

        void requestUpdate();

        void update();

        //! The store for the file, shared with every other Settings object that names it.
        std::shared_ptr<SettingsFile> mFile;

        //! The organization name that the constructor was given, or that it read from
        //! CoreApplication. Empty for the file constructor.
        std::string mOrganization;

        //! The application name that the constructor was given, or that it read from
        //! CoreApplication. Empty for the file constructor.
        std::string mApplication;

        //! The groups and arrays that are open, innermost last.
        std::vector<Group> mGroupStack;

        //! The prefix that the open groups put in front of every key: each group's toString() and
        //! a '/'. Empty when no group is open.
        std::string mGroupPrefix;

        //! The first error, or NoError. Mutable, because value() and the other const reads can
        //! report an error too. See setStatus().
        mutable SettingsStatus mStatus { SettingsStatus::NoError };

        //! True when a change was made and a write is scheduled but has not run.
        bool mPendingChanges { false };
    };

    //! Returns the value of @p aKey, converted to T, or @p aDefaultValue if the key does not exist
    //! or its value does not convert to T.
    //!
    //! @code
    //!   const int width      = settings.value( "window/width", 800 );
    //!   const bool maximized = settings.value( "window/maximized", false );
    //!   const std::vector<std::string> recent =
    //!       settings.value( "recent", std::vector<std::string>() );
    //! @endcode
    //!
    //! This is the form to use for a typed value. The QSettings form, value( key ).toInt(), is
    //! available too, but gives 0 and not the default when the text in the file is not a number.
    //!
    //! @tparam T Any type that SettingsValue::to<T>() accepts: bool, an integer type, a
    //!         floating-point type, std::string, std::vector<std::string>. It is deduced from the
    //!         default, so `value( "width", 800 )` gives an int. A string literal default uses the
    //!         `const char*` overload and gives a std::string.
    template <typename T>
    T Settings::value
        (
        const std::string& aKey,       //!< The key, relative to the current group.
        const T& aDefaultValue         //!< What to return if the key is missing or not a T.
        ) const
    {
        SettingsValue stored;
        if( !lookup( aKey, "Settings::value", stored ) )
        {
            return aDefaultValue;
        }
        bool ok = false;
        T result = stored.template to<T>( &ok );
        return ok ? result : aDefaultValue;
    }

}

#endif // QT_LIKE_SIGNAL_SETTINGS_HPP
