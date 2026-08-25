// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::PlatformType -- which window system a program is running on.

#ifndef QT_LIKE_SIGNAL_GUI_PLATFORMTYPE_HPP
#define QT_LIKE_SIGNAL_GUI_PLATFORMTYPE_HPP

namespace QtLikeSignalGui
{
    //! The window systems this library knows about.
    //!
    //! An enum rather than a string, and everything downstream of the command line compares these
    //! rather than text. A backend name only ever exists as a string in two places: the `-p`
    //! argument and the environment variable it comes from, which PlatformIntegration::
    //! choosePlatform() turns into one of these once; and a diagnostic message, which
    //! platformTypeName() renders on demand. Nothing else has any reason to spell a platform out,
    //! and a typo in a comparison that never happens cannot silently select the wrong backend.
    enum class PlatformType
    {
        //! No window system: none was found, or the one named is not built into this binary.
        //!
        //! Zero and first, so a default-constructed value means "none" rather than naming a
        //! platform the program may not even be running on.
        Unknown = 0,

        Windows,   //!< Win32. The only one on Windows, and always the answer there.
        X11,       //!< X11, through Xlib.
        Wayland,   //!< Wayland. Selectable and detectable, but no backend implements it yet.
        Drm        //!< DRM/KMS with no window system at all; input comes from libinput.
    };

    //! Gets a name for @p aType, for a diagnostic or a log line. Never null.
    //!
    //! For printing only. Comparing these strings would put back exactly what the enum exists to
    //! remove, so compare the enum instead.
    inline const char* platformTypeName
        (
        PlatformType aType   //!< The platform to name.
        )
    {
        switch( aType )
        {
        case PlatformType::Windows:
            return "windows";

        case PlatformType::X11:
            return "x11";

        case PlatformType::Wayland:
            return "wayland";

        case PlatformType::Drm:
            return "drm";

        case PlatformType::Unknown:
            break;
        }

        return "none";
    }
}

#endif // QT_LIKE_SIGNAL_GUI_PLATFORMTYPE_HPP
