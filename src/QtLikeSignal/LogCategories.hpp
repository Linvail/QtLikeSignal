// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The logging categories QtLikeSignal itself reports on.
//!
//! Seven of them, one for each part of the library, so that a reader can turn up the thread
//! machinery without also turning up every parent-child warning:
//!
//! ```
//! QTLIKESIGNAL_LOG_RULES="qtlikesignal.*=warning,qtlikesignal.thread=debug"
//! ```
//!
//! Declared in one header rather than each in the file that uses it, because four of the seven are
//! shared: the thread category is reported on by Thread.cpp and by both of its platform halves,
//! the dispatcher category by both dispatchers, and the settings category by the value type, the
//! file store and the Settings class. A category is one object, so it has to be
//! declared somewhere both halves can see and defined exactly once -- which is LogCategories.cpp.
//!
//! An application's own categories do not belong here. Define them with
//! QTLIKESIGNAL_DEFINE_LOG_CATEGORY in the application, under a name of its own rather than under
//! "qtlikesignal.", so that a rule aimed at this library does not silently catch them too.

#ifndef QT_LIKE_SIGNAL_LOG_CATEGORIES_HPP
#define QT_LIKE_SIGNAL_LOG_CATEGORIES_HPP

#include "QtLikeSignal/LogCategory.hpp"

//! The application object: construction, exec() and quit(). "QAPP" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogApp )

//! Objects: lifetime, thread affinity, parent-child, and the object tree dump. "QOBJ" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogObject )

//! Threads: start, wait, priority, and the platform calls behind them. "QTHR" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogThread )

//! Timers, at both the Object and Timer levels. "QTMR" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogTimer )

//! Event dispatchers, including the platform wait each one is built on. "QDSP" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogDispatcher )

//! The opt-in shutdown-signal watcher: what it installed, and what arrived. "QSGW" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogSignalWatcher )

//! Settings: reading, writing and locking settings files. "QSET" to DLT.
QTLIKESIGNAL_DECLARE_LOG_CATEGORY( gLogSettings )

#endif // QT_LIKE_SIGNAL_LOG_CATEGORIES_HPP
