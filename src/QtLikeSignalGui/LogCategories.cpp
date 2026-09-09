// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Definitions of the logging categories declared in LogCategories.hpp.
//!
//! One translation unit for all five, because each macro below defines an object and an object
//! must be defined exactly once.
//!
//! Every backend is defined here, including the ones this build cannot compile. A category costs a
//! pointer, an atomic and a link into a list, and only when something first asks for it -- so the
//! alternative, four more conditions matching the ones in the wscript, would buy nothing and give
//! the build a second place to get the same answer wrong.

#include "QtLikeSignalGui/LogCategories.hpp"

QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogGui, "qtlikesignal.gui", "GGUI" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogGuiWin32, "qtlikesignal.gui.win32", "GWIN" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogGuiX11, "qtlikesignal.gui.x11", "GX11" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogGuiWayland, "qtlikesignal.gui.wayland", "GWAY" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogGuiDrm, "qtlikesignal.gui.drm", "GDRM" )
