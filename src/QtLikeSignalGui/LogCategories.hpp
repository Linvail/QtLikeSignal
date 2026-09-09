// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The logging categories QtLikeSignalGui reports on.
//!
//! Five: one for the portable half, and one for each window-system backend, so that a reader
//! chasing a Wayland problem can turn that backend up without also turning up everything the
//! window and input layer says.
//!
//! ```
//! QTLIKESIGNAL_LOG_RULES="qtlikesignal.*=warning,qtlikesignal.gui.wayland=debug"
//! ```
//!
//! Under "qtlikesignal.gui." rather than a prefix of their own, because this is the same library
//! from a caller's point of view: a rule of "qtlikesignal.*" should mean all of it, and it does.
//!
//! Declared in one header rather than each in the file that uses it, for the reason
//! QtLikeSignal/LogCategories.hpp gives: a category is one object, so a category two files report
//! on has to be declared where both can see it and defined exactly once.

#ifndef QT_LIKE_SIGNAL_GUI_LOG_CATEGORIES_HPP
#define QT_LIKE_SIGNAL_GUI_LOG_CATEGORIES_HPP

#include "QtLikeSignal/LogCategory.hpp"

//! The portable half: the application object, window lifetime, and backend selection. "GGUI".
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogGui )

//! The Win32 backend. "GWIN" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogGuiWin32 )

//! The X11 backend. "GX11" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogGuiX11 )

//! The Wayland backend. "GWAY" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogGuiWayland )

//! The DRM/KMS backend, and the libinput device handling under it. "GDRM" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogGuiDrm )

#endif // QT_LIKE_SIGNAL_GUI_LOG_CATEGORIES_HPP
