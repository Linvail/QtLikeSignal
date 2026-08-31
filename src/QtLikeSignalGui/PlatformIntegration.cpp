// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Platform selection, the backend factory, and the writes a backend makes into a Window.

#include "QtLikeSignalGui/PlatformIntegration.hpp"

#if defined( _WIN32 )
    #include "QtLikeSignalGui/PlatformIntegrationWin32.hpp"
#endif
// HAVE_X11, HAVE_LIBINPUT and HAVE_LIBUDEV are defined by check_cfg() at configure time, for the
// toolchains where pkg-config found the package -- see tools/toolchain-linux.py. The build script
// compiles each backend under exactly the same condition, so these guards and the source list
// cannot disagree; they used to be a pair of project-specific defines that restated the same facts
// under different names, and keeping two names in step is work with nothing to show for it.
//
// The DRM backend needs both of libinput's halves, which is why its guard is a conjunction rather
// than a name: libinput reads the devices and libudev is how they are found in the first place.
#if defined( HAVE_X11 )
    #include "QtLikeSignalGui/PlatformIntegrationX11.hpp"
#endif
#if defined( HAVE_LIBINPUT ) && defined( HAVE_LIBUDEV )
    #include "QtLikeSignalGui/PlatformIntegrationDrm.hpp"
#endif
#if defined( HAVE_WAYLAND_CLIENT )
    #include "QtLikeSignalGui/PlatformIntegrationWayland.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace QtLikeSignalGui
{
    namespace
    {
        //! Lower-cases an ASCII platform name, so "-p X11" and "-p x11" mean the same thing.
        std::string toLower
            (
            std::string aText   //!< Text to fold. Taken by value; the copy is the result.
            )
        {
            std::transform( aText.begin(), aText.end(), aText.begin(),
                []( unsigned char aCharacter )
                {
                    return static_cast<char>( std::tolower( aCharacter ) );
                } );
            return aText;
        }

        //! Reads an environment variable, returning an empty string when it is unset or empty.
        //!
        //! std::getenv rather than the _s variant: this has to compile on MSVC, clang and gcc, and
        //! only the first of those has getenv_s. MSVC's deprecation warning for it is silenced by
        //! the project's own settings rather than by picking a compiler-specific spelling here.
        std::string environmentValue
            (
            const char* aName   //!< Variable to read.
            )
        {
            const char* const value = std::getenv( aName );
            return ( value != nullptr ) ? std::string( value ) : std::string();
        }

        //! Turns a platform name from the command line or the environment into a PlatformType.
        //!
        //! **The only string comparison against a platform name in the library.** Everything else
        //! holds a PlatformType, so this is the single boundary where a human-typed word becomes a
        //! value the rest of the code can switch on.
        //!
        //! Folded to lower case first, so "-p X11" and "-p x11" mean the same thing.
        //!
        //! An unrecognised name is reported here rather than passed on, because this is the last
        //! point at which the text still exists to put in the message: the enum has no way to carry
        //! "the user typed nosuchplatform".
        //!
        //! @return the platform, or Unknown for a name nothing recognises.
        PlatformType platformFromName
            (
            const std::string& aName   //!< The name as it was written.
            )
        {
            const std::string folded = toLower( aName );

            if( folded == "windows" )
            {
                return PlatformType::Windows;
            }
            if( folded == "x11" )
            {
                return PlatformType::X11;
            }
            if( folded == "wayland" )
            {
                return PlatformType::Wayland;
            }
            if( folded == "drm" )
            {
                return PlatformType::Drm;
            }

            std::fprintf( stderr,
                "QtLikeSignalGui: \"%s\" is not a platform this library knows; expected one of windows, "
                "x11, wayland, drm\n", aName.c_str() );

            return PlatformType::Unknown;
        }
    }

    //! Decides which backend to use, from the command line, the environment, then the session.
    //!
    //! **This is the only place a platform is ever spelled out.** Everything downstream compares
    //! PlatformType values, so the text of a `-p x11` reaches exactly one function and stops there.
    //!
    //! The order mirrors Qt's, which takes -platform over QT_QPA_PLATFORM over a compiled-in
    //! preference, and for the same reason: the most specific statement of intent wins. The switch
    //! is spelled `-p` rather than `-platform` because that is what the external library that
    //! creates the windows already takes, so one flag drives both halves of the program;
    //! `-platform` and `--platform` are accepted as well for the reader who expects Qt's.
    //!
    //! On Windows the answer is always Windows: there is one window system and no choice to make,
    //! so a `-p` meant for the Linux build is ignored rather than being an error.
    //!
    //! **Detection asks the window system, not the environment.** Where a platform is not named,
    //! each backend is asked whether it can reach its server -- X11 opens a connection, Wayland
    //! connects to the compositor -- and only a backend built into this binary is asked at all.
    //! Reading WAYLAND_DISPLAY and DISPLAY was cheaper and wrong in both directions: a variable
    //! survives the session it described, and its absence says nothing about a socket that is
    //! reachable anyway. The graphics layer's EGL display provider has always probed rather than
    //! read, and two detections that could disagree about which window system is running would be
    //! worse than either alone.
    //!
    //! @return the chosen platform, or Unknown for a name nothing recognises.
    PlatformType PlatformIntegration::choosePlatform
        (
        const std::vector<std::string>& aArgs   //!< The program's arguments, argv[0] included.
        )
    {
        #if defined( _WIN32 )
            static_cast<void>( aArgs );
            return PlatformType::Windows;
        #else
            for( std::size_t i = 1; i < aArgs.size(); ++i )
            {
                const std::string& argument = aArgs[i];

                if( ( argument == "-p" || argument == "-platform" || argument == "--platform" )
                    && ( i + 1 ) < aArgs.size() )
                {
                    return platformFromName( aArgs[i + 1] );
                }

                const std::string prefix( "--platform=" );
                if( argument.compare( 0, prefix.size(), prefix ) == 0 )
                {
                    return platformFromName( argument.substr( prefix.size() ) );
                }
            }

            const std::string fromEnvironment = environmentValue( "QTLIKESIGNAL_PLATFORM" );
            if( !fromEnvironment.empty() )
            {
                return platformFromName( fromEnvironment );
            }

            // Wayland before X11: a session running both has XWayland available, and picking the
            // translation layer over the compositor the session actually runs would be the wrong
            // way round. Qt makes the same choice.
            //
            // Guarded on the same defines the factory uses, so a platform this binary has no
            // backend for is never chosen. That is not only tidiness: the probe below *is* the
            // backend's own library, so asking a backend that was not built is not possible, and
            // answering with a platform that create() would then refuse would only move the failure
            // one step further from its cause.
            #if defined( HAVE_WAYLAND_CLIENT )
                if( PlatformIntegrationWayland::isAvailable() )
                {
                    return PlatformType::Wayland;
                }
            #endif

            #if defined( HAVE_X11 )
                if( PlatformIntegrationX11::isAvailable() )
                {
                    return PlatformType::X11;
                }
            #endif

            // No server answered, which on Linux is the DRM/KMS console: the display is driven
            // directly and input comes from libinput. Answering Unknown here would be a claim that
            // nothing can run, when in fact the one platform that needs no session is exactly the
            // one left. A binary built without the DRM backend still reports the absence, from
            // create() rather than from here.
            return PlatformType::Drm;
        #endif
    }

    //! Creates the backend for @p aPlatform.
    //!
    //! Statically linked rather than loaded, so a platform this binary does not have is reported
    //! here and the caller carries on without a window system instead of the process dying. Qt calls
    //! qFatal() at this point; a library that also serves headless programs should not.
    //!
    //! A switch rather than a chain of comparisons, so adding a platform to the enum makes every
    //! compiler that warns on an unhandled enumerator point straight at this function.
    //!
    //! @return the backend, or nullptr when there is none for that platform in this binary.
    std::unique_ptr<PlatformIntegration> PlatformIntegration::create
        (
        PlatformType aPlatform   //!< The platform, as choosePlatform() decided.
        )
    {
        switch( aPlatform )
        {
        case PlatformType::Windows:
            #if defined( _WIN32 )
                return std::unique_ptr<PlatformIntegration>( new PlatformIntegrationWin32() );
            #else
                break;
            #endif

        case PlatformType::X11:
            #if defined( HAVE_X11 )
                return std::unique_ptr<PlatformIntegration>( new PlatformIntegrationX11() );
            #else
                break;
            #endif

        case PlatformType::Drm:
            #if defined( HAVE_LIBINPUT ) && defined( HAVE_LIBUDEV )
                return std::unique_ptr<PlatformIntegration>( new PlatformIntegrationDrm() );
            #else
                break;
            #endif

        case PlatformType::Wayland:
            #if defined( HAVE_WAYLAND_CLIENT )
                return std::unique_ptr<PlatformIntegration>( new PlatformIntegrationWayland() );
            #else
                break;
            #endif

        case PlatformType::Unknown:
            break;
        }

        if( aPlatform == PlatformType::Unknown )
        {
            std::fprintf( stderr,
                "QtLikeSignalGui: no window system found; pass -p x11, -p wayland or -p drm, or set "
                "QTLIKESIGNAL_PLATFORM\n" );
        }
        else
        {
            std::fprintf( stderr, "QtLikeSignalGui: no \"%s\" backend is built into this binary\n",
                platformTypeName( aPlatform ) );
        }

        return nullptr;
    }

    //! Creates a native window. Refused unless the backend overrides it.
    //!
    //! Virtual with a default rather than pure, so that a backend which only adopts windows -- X11,
    //! and Wayland after it -- does not carry a stub that exists to say no. canCreateWindows() is
    //! how a caller asks in advance; this is what happens if it did not.
    //!
    //! @return nullptr, always.
    Window* PlatformIntegration::createWindow
        (
        const WindowSettings& aSettings   //!< Ignored.
        )
    {
        static_cast<void>( aSettings );
        return nullptr;
    }

    //! Adopts a native window created elsewhere. Refused unless the backend overrides it.
    //!
    //! @return nullptr, always.
    Window* PlatformIntegration::adoptWindow
        (
        const NativeWindow& aNative   //!< Ignored.
        )
    {
        static_cast<void>( aNative );
        return nullptr;
    }

    //! Gets the connection this backend uses. Null unless the backend overrides it.
    //!
    //! Non-const, and deliberately: on X11 asking for the connection is what opens it, because a GL
    //! library has to choose its FBConfig on a live Display before there is a window to choose it
    //! for. Win32 has no such object and answers null.
    //!
    //! @return nullptr, always.
    void* PlatformIntegration::nativeDisplay()
    {
        return nullptr;
    }

    //! Attaches a menu bar. Does nothing unless the backend overrides it.
    //!
    //! Virtual with a default rather than pure, because a menu bar is not a thing every window
    //! system has: Wayland has no server-side menus at all, and on X11 one is drawn by the client.
    //! Requiring each backend to write an empty override would only spread the same silence across
    //! more files. Window::setMenu() documents the reach.
    void PlatformIntegration::setMenu
        (
        Window* aWindow,     //!< Window to attach the menu to.
        void* aMenuHandle    //!< The platform's menu handle.
        )
    {
        static_cast<void>( aWindow );
        static_cast<void>( aMenuHandle );
    }

    //! Constructs a Window fronting @p aNative, for a backend that has just created or adopted one.
    //!
    //! Window's constructor is private, and a backend deriving from this class is not a friend of
    //! Window -- friendship does not inherit. This is the one door through, and it exists so that
    //! the invariant "a Window always has a backend and a native window" is enforced in one place
    //! rather than trusted to each backend.
    //!
    //! @return the new Window. Never null; ownership passes to the caller.
    Window* PlatformIntegration::newWindow
        (
        PlatformIntegration* aIntegration,   //!< The backend that created or adopted it.
        const NativeWindow& aNative,         //!< The native window it just took charge of.
        int aWidth,                          //!< Client-area width in pixels.
        int aHeight                          //!< Client-area height in pixels.
        )
    {
        return new Window( aIntegration, aNative, aWidth, aHeight );
    }

    //! Records the native window behind @p aWindow, or an all-zero one once it has been released.
    void PlatformIntegration::assignNative
        (
        Window* aWindow,             //!< Window to update.
        const NativeWindow& aNative  //!< The native window, or a default-constructed one to clear.
        )
    {
        aWindow->mNative = aNative;
    }

    //! Records a new size on @p aWindow without emitting anything.
    //!
    //! For a backend correcting its own bookkeeping -- the size the window system actually granted
    //! at creation, say. Reporting a resize to the application is WindowSystemInterface::
    //! handleResize()'s job, and it records the size too.
    void PlatformIntegration::assignSize
        (
        Window* aWindow,   //!< Window to update.
        int aWidth,        //!< Client-area width in pixels.
        int aHeight        //!< Client-area height in pixels.
        )
    {
        aWindow->mWidth  = aWidth;
        aWindow->mHeight = aHeight;
    }

    //! Records whether @p aWindow is currently shown.
    void PlatformIntegration::assignVisible
        (
        Window* aWindow,   //!< Window to update.
        bool aVisible      //!< True once it has been shown.
        )
    {
        aWindow->mVisible = aVisible;
    }
}
