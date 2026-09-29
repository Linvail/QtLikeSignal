// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Definitions of the logging categories declared in LogCategories.hpp.
//!
//! One translation unit for all seven, because each macro below defines an object and an object
//! must be defined exactly once. Which file that is does not matter to anything except that it is
//! only one, so it is this one rather than whichever of the seven parts happened to be written
//! first.
//!
//! The four-character ids are what a DLT sink registers a context under -- DLT context ids are
//! exactly that long -- so they are chosen to stay distinct in four characters, which the dotted
//! names do not: "qtlikesignal.thread" and "qtlikesignal.timer" would both begin "QT" and could not
//! both be shortened by truncation.

#include "QtLikeSignal/LogCategories.hpp"

QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogApp, "qtlikesignal.app", "QAPP" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogObject, "qtlikesignal.object", "QOBJ" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogThread, "qtlikesignal.thread", "QTHR" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogTimer, "qtlikesignal.timer", "QTMR" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogDispatcher, "qtlikesignal.dispatcher", "QDSP" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogSignalWatcher, "qtlikesignal.signalwatcher", "QSGW" )
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogSettings, "qtlikesignal.settings", "QSET" )
