// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Settings implementation: the constructors, the group stack, and the deferred write.
//!
//! Ported from QSettings and QSettingsPrivate in Qt 6's qsettings.cpp. The store underneath is
//! SettingsFile.

#include "QtLikeSignal/Settings.hpp"

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/SettingsFile.hpp"
#include "QtLikeSignal/SettingsFileIo.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

namespace QtLikeSignal
{
    namespace
    {
        //! The directories that setPath() set, by format.
        struct CustomPaths
        {
            //! Guards mPaths.
            std::mutex mMutex;

            //! The directory for each format, as setPath() gave it. A format with no entry uses
            //! SettingsFileIo::userConfigDirectory().
            std::map<SettingsFormat, std::string> mPaths;
        };

        //! Returns the one set of custom paths. Never destroyed; see fileCache() in
        //! SettingsFile.cpp for why.
        CustomPaths& customPaths()
        {
            static CustomPaths* const paths = new CustomPaths();
            return *paths;
        }

        //! Returns the directory where the settings files of @p aFormat go, without a trailing
        //! separator: the setPath() directory if there is one, the user's folder if not.
        std::string configDirectory
            (
            SettingsFormat aFormat  //!< The format.
            )
        {
            {
                CustomPaths& paths = customPaths();
                std::lock_guard<std::mutex> lock( paths.mMutex );
                const auto it = paths.mPaths.find( aFormat );
                if( it != paths.mPaths.end() )
                {
                    return it->second;
                }
            }
            return SettingsFileIo::userConfigDirectory();
        }

        //! Returns the file name extension of @p aFormat, with the '.'.
        const char* extensionOf
            (
            SettingsFormat aFormat  //!< The format.
            )
        {
            switch( aFormat )
            {
            case SettingsFormat::Ini:
            default:
                return ".ini";
            }
        }
    }

    //! Constructs a Settings object for the application that CoreApplication names.
    //!
    //! This is the organization constructor, with CoreApplication::organizationName() and
    //! CoreApplication::applicationName(). Set the two names before the first Settings object is
    //! made.
    Settings::Settings
        (
        Object* aParent  //!< The parent, or null.
        )
        : Settings( CoreApplication::organizationName(), CoreApplication::applicationName(),
            aParent )
    {
    }

    //! Constructs a Settings object for the settings of @p aApplication by @p aOrganization.
    //!
    //! The file is `<dir>/<organization>/<application>.ini`, or `<dir>/<organization>.ini` for an
    //! empty application. See the class comment for `<dir>`. An empty organization is an error, as
    //! in Qt: status() is AccessError, and the organization "Unknown Organization" is used, so the
    //! values still go somewhere that can be found.
    //!
    //! The file is read here. It does not have to exist; its directory is made at the first write.
    Settings::Settings
        (
        const std::string& aOrganization,  //!< The organization, for example "Garmin".
        const std::string& aApplication,   //!< The application, for example "Chartplotter".
        Object* aParent                    //!< The parent, or null.
        )
        : Object( aParent )
        , mOrganization( aOrganization )
        , mApplication( aApplication )
    {
        std::string organization = aOrganization;
        if( organization.empty() )
        {
            setStatus( SettingsStatus::AccessError );
            organization = "Unknown Organization";
        }

        const SettingsFormat format = SettingsFormat::Ini;
        const std::string directory = configDirectory( format );
        std::string fileName;
        if( directory.empty() )
        {
            qCWarning( gLogSettings ) << "Settings: no folder for user settings was found;"
                                      << "the settings cannot be saved";
            setStatus( SettingsStatus::AccessError );
        }
        else
        {
            const char separator = SettingsFileIo::separator();
            fileName = directory + separator + organization;
            fileName += aApplication.empty() ? std::string() : separator + aApplication;
            fileName += extensionOf( format );
        }
        openFile( fileName, format );
    }

    //! Constructs a Settings object for the file @p aFileName, in @p aFormat.
    //!
    //! A relative name is taken from the current directory at the time of this call. The file is
    //! read here. It does not have to exist; it and its directory are made at the first write.
    Settings::Settings
        (
        const std::string& aFileName,  //!< The file, in UTF-8.
        SettingsFormat aFormat,        //!< Its format.
        Object* aParent                //!< The parent, or null.
        )
        : Object( aParent )
    {
        openFile( SettingsFileIo::absolutePath( aFileName ), aFormat );
    }

    //! Writes any change that is not yet written, and destroys the object.
    //!
    //! A write that fails here is not reported to anyone except the log, because nobody can ask
    //! status() afterwards. Call sync() first and check status() to know.
    Settings::~Settings()
    {
        if( mPendingChanges )
        {
            // The result of this write, not status(): status() keeps the first error, which can be
            // an earlier one while this write succeeded.
            const SettingsStatus result = mFile->sync();
            mPendingChanges = false;
            if( result != SettingsStatus::NoError )
            {
                qCWarning( gLogSettings ) << "Settings: could not write" << fileName()
                                          << "when the Settings object was destroyed";
            }
        }
    }

    //! Opens the shared store for @p aFileName and reads the file. All constructors end here.
    void Settings::openFile
        (
        const std::string& aFileName,  //!< The absolute path, or empty if there is none.
        SettingsFormat aFormat         //!< The format.
        )
    {
        mFile = SettingsFile::open( aFileName, aFormat );
        setStatus( mFile->sync() );
    }

    //! Removes every key in the file, including those outside the current group.
    //!
    //! To remove only the keys of the current group, call remove( "" ).
    void Settings::clear()
    {
        mFile->clear();
        requestUpdate();
    }

    //! Writes the changes that are not yet written, and reads the changes that another process
    //! wrote.
    //!
    //! It is not usually necessary to call this: see "When the file is written" in the class
    //! comment. Call it to write at a time that the program chooses, or to see whether the write
    //! worked, in status().
    void Settings::sync()
    {
        setStatus( mFile->sync() );
        mPendingChanges = false;
    }

    //! Returns the first error since this object was made, or NoError.
    //!
    //! The first, not the last: a later success does not clear it. This is what QSettings does,
    //! because a program that checks status() only once must still see a failure that happened
    //! before.
    SettingsStatus Settings::status() const
    {
        return mStatus;
    }

    //! Puts @p aPrefix in front of every key until the matching endGroup(). Groups nest.
    //!
    //! @code
    //!   settings.beginGroup( "window" );
    //!   settings.setValue( "width", 800 );   // the key "window/width"
    //!   settings.endGroup();
    //! @endcode
    void Settings::beginGroup
        (
        const std::string& aPrefix  //!< The group. Can contain '/' to open more than one level.
        )
    {
        Group group;
        group.mName = normalizedKey( aPrefix );
        beginGroupOrArray( group );
    }

    //! Closes the group that the last beginGroup() opened.
    //!
    //! Misuse is reported and does not crash: an endGroup() with no open group does nothing, and
    //! an endGroup() that closes an array closes it without the work that endArray() does.
    void Settings::endGroup()
    {
        if( mGroupStack.empty() )
        {
            qCWarning( gLogSettings ) << "Settings::endGroup: no matching beginGroup()";
            return;
        }

        const Group group = mGroupStack.back();
        mGroupStack.pop_back();
        const std::size_t length = group.toString().size();
        if( length > 0 )
        {
            mGroupPrefix.erase( mGroupPrefix.size() - ( length + 1 ) );
        }

        if( group.isArray() )
        {
            qCWarning( gLogSettings ) << "Settings::endGroup: expected endArray() instead";
        }
    }

    //! Returns the current group, without a trailing '/'. Empty at the top level.
    std::string Settings::group() const
    {
        return mGroupPrefix.empty() ? std::string() :
               mGroupPrefix.substr( 0, mGroupPrefix.size() - 1 );
    }

    //! Opens the array @p aPrefix for reading, and returns its size.
    //!
    //! @code
    //!   const int count = settings.beginReadArray( "recent" );
    //!   for( int i = 0; i < count; ++i )
    //!   {
    //!       settings.setArrayIndex( i );
    //!       paths.push_back( settings.value( "path", std::string() ) );
    //!   }
    //!   settings.endArray();
    //! @endcode
    //!
    //! @return the value of `<prefix>/size`, or 0 if there is none.
    int Settings::beginReadArray
        (
        const std::string& aPrefix  //!< The array.
        )
    {
        Group group;
        group.mName = normalizedKey( aPrefix );
        group.mNumber = 0;
        beginGroupOrArray( group );
        return value( "size" ).toInt();
    }

    //! Opens the array @p aPrefix for writing.
    //!
    //! With @p aSize -1, the default, the size is counted: endArray() writes the highest index
    //! used, plus one, as `<prefix>/size`. With a size of 0 or more, that size is written at once.
    //! Elements are one-based in the file: element 0 is `<prefix>/1/...`, as in Qt.
    //!
    //! Old elements above the new size are not removed. Call remove( prefix ) first to replace an
    //! array that can get shorter.
    void Settings::beginWriteArray
        (
        const std::string& aPrefix,  //!< The array.
        int aSize                    //!< The size, or -1 to count it.
        )
    {
        Group group;
        group.mName = normalizedKey( aPrefix );
        group.mNumber = 0;
        group.mMaxNumber = aSize < 0 ? 0 : -1;
        beginGroupOrArray( group );

        if( aSize < 0 )
        {
            remove( "size" );
        }
        else
        {
            setValue( "size", aSize );
        }
    }

    //! Closes the array that the last beginReadArray() or beginWriteArray() opened. For an array
    //! written with no size, it writes the size.
    void Settings::endArray()
    {
        if( mGroupStack.empty() )
        {
            qCWarning( gLogSettings ) << "Settings::endArray: no matching beginArray()";
            return;
        }

        const Group group = mGroupStack.back();
        mGroupStack.pop_back();
        const std::size_t length = group.toString().size();
        if( length > 0 )
        {
            mGroupPrefix.erase( mGroupPrefix.size() - ( length + 1 ) );
        }

        if( group.mMaxNumber != -1 )
        {
            setValue( group.mName + "/size", group.mMaxNumber );
        }
        if( !group.isArray() )
        {
            qCWarning( gLogSettings ) << "Settings::endArray: expected endGroup() instead";
        }
    }

    //! Moves to element @p aIndex of the open array. The keys that follow are in that element.
    //!
    //! A negative index is taken as 0. Without an open array, this is reported and does nothing.
    void Settings::setArrayIndex
        (
        int aIndex  //!< The zero-based index.
        )
    {
        if( mGroupStack.empty() || !mGroupStack.back().isArray() )
        {
            qCWarning( gLogSettings ) << "Settings::setArrayIndex: missing beginArray()";
            return;
        }

        Group& top = mGroupStack.back();
        const std::size_t length = top.toString().size();
        top.mNumber = std::max( aIndex, 0 ) + 1;
        if( top.mMaxNumber != -1 && top.mNumber > top.mMaxNumber )
        {
            top.mMaxNumber = top.mNumber;
        }
        mGroupPrefix.replace( mGroupPrefix.size() - length - 1, length, top.toString() );
    }

    //! Returns every key in the current group, at any depth, relative to the group. Sorted.
    std::vector<std::string> Settings::allKeys() const
    {
        return mFile->children( mGroupPrefix, SettingsFile::ChildSpec::AllKeys );
    }

    //! Returns the keys directly in the current group, not in a group below it. Sorted.
    std::vector<std::string> Settings::childKeys() const
    {
        return mFile->children( mGroupPrefix, SettingsFile::ChildSpec::ChildKeys );
    }

    //! Returns the groups directly in the current group. Sorted.
    std::vector<std::string> Settings::childGroups() const
    {
        return mFile->children( mGroupPrefix, SettingsFile::ChildSpec::ChildGroups );
    }

    //! Returns true if the file can be written. The answer can change as soon as it is given, for
    //! example if a person changes the permissions of the file.
    bool Settings::isWritable() const
    {
        return mFile->isWritable();
    }

    //! Sets @p aKey to @p aValue. A key that already exists gets the new value.
    //!
    //! The change is in memory at once, and in the file later; see the class comment. An empty
    //! key is reported and ignored.
    void Settings::setValue
        (
        const std::string& aKey,      //!< The key, relative to the current group.
        const SettingsValue& aValue   //!< The value.
        )
    {
        const std::string key = normalizedKey( aKey );
        if( key.empty() )
        {
            qCWarning( gLogSettings ) << "Settings::setValue: empty key passed";
            return;
        }
        mFile->set( mGroupPrefix + key, aValue );
        requestUpdate();
    }

    //! Returns the value of @p aKey, or @p aDefaultValue if the key does not exist.
    //!
    //! This is the QSettings form. For a typed value with a default, the template overload is
    //! usually better; see there.
    SettingsValue Settings::value
        (
        const std::string& aKey,              //!< The key, relative to the current group.
        const SettingsValue& aDefaultValue    //!< What to return if the key does not exist.
        ) const
    {
        SettingsValue stored;
        return lookup( aKey, "Settings::value", stored ) ? stored : aDefaultValue;
    }

    //! Returns the value of @p aKey as a string, or @p aDefaultValue if the key does not exist or
    //! its value is not a single string.
    //!
    //! This overload exists for a string literal default. Without it, `value( "name", "Evan" )`
    //! would make the template deduce an array type.
    std::string Settings::value
        (
        const std::string& aKey,      //!< The key, relative to the current group.
        const char* aDefaultValue     //!< What to return. Null is taken as an empty string.
        ) const
    {
        return value<std::string>( aKey,
            aDefaultValue != nullptr ? std::string( aDefaultValue ) : std::string() );
    }

    //! Removes @p aKey and every key below it.
    //!
    //! An empty key removes every key in the current group, and at the top level every key in the
    //! file. The change is written later, as for setValue().
    void Settings::remove
        (
        const std::string& aKey  //!< The key, relative to the current group, or empty.
        )
    {
        std::string key = normalizedKey( aKey );
        if( key.empty() )
        {
            key = group();
        }
        else
        {
            key.insert( 0, mGroupPrefix );
        }

        if( key.empty() )
        {
            mFile->clear();
        }
        else
        {
            mFile->remove( key );
        }
        requestUpdate();
    }

    //! Returns true if @p aKey exists. It is relative to the current group.
    bool Settings::contains
        (
        const std::string& aKey  //!< The key.
        ) const
    {
        SettingsValue stored;
        return lookup( aKey, "Settings::contains", stored );
    }

    //! Returns the absolute path of the file. Empty if no folder for user settings was found.
    std::string Settings::fileName() const
    {
        return mFile->fileName();
    }

    //! Returns the format of the file.
    SettingsFormat Settings::format() const
    {
        return mFile->format();
    }

    //! Returns the organization name that this object was made with. Empty for the file
    //! constructor.
    std::string Settings::organizationName() const
    {
        return mOrganization;
    }

    //! Returns the application name that this object was made with. Empty for the file
    //! constructor.
    std::string Settings::applicationName() const
    {
        return mApplication;
    }

    //! Sets the directory in which the organization and application constructors put the files
    //! of @p aFormat. Thread-safe.
    //!
    //! Only Settings objects made after the call use it. This is what a test calls before it makes
    //! a Settings object, so that it never writes in the real user folder. It is also how an
    //! application puts its settings in a place of its own, such as a directory next to the
    //! program on a device with no home directory.
    void Settings::setPath
        (
        SettingsFormat aFormat,   //!< The format.
        const std::string& aPath  //!< The directory, in UTF-8. Empty goes back to the default.
        )
    {
        CustomPaths& paths = customPaths();
        std::lock_guard<std::mutex> lock( paths.mMutex );
        if( aPath.empty() )
        {
            paths.mPaths.erase( aFormat );
            return;
        }
        std::string path = aPath;
        while( path.size() > 1 && ( path.back() == '/' || path.back() == '\\' ) )
        {
            path.pop_back();
        }
        paths.mPaths[aFormat] = path;
    }

    //! Returns @p aKey without repeated, leading or trailing '/'.
    //!
    //! "foo" stays "foo", "/foo//bar///" becomes "foo/bar", and "///" becomes "". This is
    //! QSettingsPrivate::normalizedKey().
    std::string Settings::normalizedKey
        (
        const std::string& aKey  //!< The key.
        )
    {
        std::string result;
        result.reserve( aKey.size() );
        std::size_t i = 0;
        while( i < aKey.size() )
        {
            while( i < aKey.size() && aKey[i] == '/' )
            {
                ++i;
            }
            if( i == aKey.size() )
            {
                break;
            }
            const std::size_t mark = i;
            while( i < aKey.size() && aKey[i] != '/' )
            {
                ++i;
            }
            if( !result.empty() )
            {
                result += '/';
            }
            result.append( aKey, mark, i - mark );
        }
        return result;
    }

    //! Finds @p aKey in the current group. An empty key is reported, in the name of @p aCaller,
    //! and is not found.
    //!
    //! @return true if the key exists. Then @p aValue receives its value.
    bool Settings::lookup
        (
        const std::string& aKey,  //!< The key, relative to the current group.
        const char* aCaller,      //!< The public function, for the warning.
        SettingsValue& aValue     //!< Receives the value.
        ) const
    {
        const std::string key = normalizedKey( aKey );
        if( key.empty() )
        {
            // One string, because the record puts a space between the parts of a message.
            qCWarning( gLogSettings ) << std::string( aCaller ) + ": empty key passed";
            return false;
        }
        return mFile->get( mGroupPrefix + key, aValue );
    }

    //! Pushes @p aGroup onto the group stack, and adds its name to the key prefix.
    void Settings::beginGroupOrArray
        (
        const Group& aGroup  //!< The group or array.
        )
    {
        mGroupStack.push_back( aGroup );
        if( !aGroup.mName.empty() )
        {
            mGroupPrefix += aGroup.mName + "/";
        }
    }

    //! Records @p aStatus, unless an error is already recorded. See status().
    void Settings::setStatus
        (
        SettingsStatus aStatus  //!< The result of an operation.
        ) const
    {
        if( mStatus == SettingsStatus::NoError )
        {
            mStatus = aStatus;
        }
    }

    //! Schedules a write for when control returns to this object's event loop.
    //!
    //! Only the first change after a write schedules one; later changes are written by the same
    //! call. This is QSettings posting QEvent::UpdateRequest to itself.
    //!
    //! If this object's thread has no event loop, callLater() cannot deliver the call, and the
    //! changes are written at sync() or at destruction instead.
    void Settings::requestUpdate()
    {
        if( mPendingChanges )
        {
            return;
        }
        mPendingChanges = true;
        Object::callLater( this, &Settings::update );
    }

    //! Writes the changes. The deferred call from requestUpdate() ends here.
    void Settings::update()
    {
        sync();
    }

}
