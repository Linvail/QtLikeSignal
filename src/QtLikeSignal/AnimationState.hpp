// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Whether an animation is running, paused or stopped.

#ifndef QT_LIKE_SIGNAL_ANIMATIONSTATE_HPP
#define QT_LIKE_SIGNAL_ANIMATIONSTATE_HPP

namespace QtLikeSignal
{
    //! Whether an animation is running, paused or stopped.
    enum class AnimationState
    {
        //! Not advancing, and holding no position. start() begins from the beginning.
        //!
        //! The state an animation is constructed in and the one it returns to when it finishes or
        //! is stopped.
        Stopped,

        //! Advancing on every tick of its thread's animation clock.
        Running,

        //! Holding its position, not advancing, and still counted as in progress.
        //!
        //! Different from Stopped in the one way that matters: resume() continues from where it
        //! was, where start() after a stop begins again.
        Paused
    };
}

#endif // QT_LIKE_SIGNAL_ANIMATIONSTATE_HPP
