// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! When a Property tells its subscribers that it changed.

#ifndef QT_LIKE_SIGNAL_PROPERTYNOTIFYMODE_HPP
#define QT_LIKE_SIGNAL_PROPERTYNOTIFYMODE_HPP

namespace QtLikeSignal
{
    //! When a Property tells its subscribers that it changed.
    //!
    //! The value itself is written immediately either way, so a reader always sees the newest one.
    //! What this chooses is when the *notification* goes out.
    enum class PropertyNotifyMode
    {
        //! Emit from inside set(), before it returns.
        //!
        //! The default, because it is what a caller expects and the only mode a property with no
        //! owner can offer. Every write that changes the value produces exactly one emission.
        Immediate,

        //! Emit once, later in this pass of the event loop, however many writes there were.
        //!
        //! For a value written far more often than it is worth reporting -- a speed reading
        //! updated per bus frame, a layout touched from six places, an animated value three other
        //! things watch. A thousand writes in one pass become one emission carrying the last of
        //! them.
        //!
        //! **A subscriber sees the final value, not each one.** That is the point rather than a
        //! limitation, and it makes this mode wrong for anything that has to observe every
        //! intermediate value -- a recorder, or a counter of how often something moved.
        //!
        //! **Needs an owner.** The deduplication is Object::callLater(), which keys on a context
        //! object, so a property constructed without one cannot offer this and says so.
        Coalesced
    };
}

#endif // QT_LIKE_SIGNAL_PROPERTYNOTIFYMODE_HPP
