// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The priority a post can carry, and the three values worth naming.

#ifndef QT_LIKE_SIGNAL_EVENTPRIORITY_HPP
#define QT_LIKE_SIGNAL_EVENTPRIORITY_HPP

namespace QtLikeSignal
{
    //! Named priorities for Object::postEvent(), mirroring Qt::EventPriority.
    //!
    //! A plain int, not an enum, and deliberately: three named values are what most programs need,
    //! but an application that wants five bands should not have to fight the type to get them. Qt
    //! draws the same line -- Qt::EventPriority names three values and
    //! QCoreApplication::postEvent() takes an int.
    //!
    //! **Higher runs first, and equal priorities keep posting order.** The default is kNormal, so a
    //! program that never mentions priority sees a plain FIFO queue, which is what it had before
    //! priorities existed.
    //!
    //! **Prefer demoting to promoting.** Qt is the evidence: in the whole of qtbase,
    //! Qt::HighEventPriority is used exactly zero times and Qt::LowEventPriority exactly once, to
    //! push widget repaints below everything else. Demoting the work that can wait is safer than
    //! promoting the work that cannot, because a demotion cannot starve anything, and it reaches
    //! the same end -- a telltale overtakes a repaint either way.
    namespace EventPriority
    {
        //! Above the default. Qt::HighEventPriority.
        //!
        //! For work that must not queue behind a backlog: a warning the driver has to see, a
        //! display losing its device, a shutdown that has a hard deadline behind it.
        constexpr int kHigh = 1;

        //! The default, and what every post in this library uses.
        constexpr int kNormal = 0;

        //! Below the default. Qt::LowEventPriority.
        //!
        //! For work whose value decays: a repaint, a telemetry flush, a persistence write. The one
        //! Qt itself uses, and the one to reach for first.
        constexpr int kLow = -1;
    }
}

#endif // QT_LIKE_SIGNAL_EVENTPRIORITY_HPP
