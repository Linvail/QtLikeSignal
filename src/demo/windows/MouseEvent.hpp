// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalDemo::MouseEvent -- one mouse press, release or move, as the demo sees it.

#ifndef QT_LIKE_SIGNAL_DEMO_MOUSEEVENT_HPP
#define QT_LIKE_SIGNAL_DEMO_MOUSEEVENT_HPP

namespace QtLikeSignalDemo
{
    //! A mouse press, release or move, in client coordinates.
    //!
    //! Deliberately carries no Windows types. GuiApplication translates the WM_ message into this
    //! before emitting it, so everything downstream -- the slots, the log, the painting -- is
    //! ordinary C++ that knows nothing about Win32. That is the same split Qt makes between
    //! QWindowSystemInterface and QMouseEvent.
    //!
    //! The button enum is nested rather than free so this header still declares exactly one public
    //! type.
    struct MouseEvent
    {
        //! Which button the event refers to.
        enum class Button
        {
            None,     //!< No button: a move with nothing held down.
            Left,     //!< Left button.
            Middle,   //!< Middle button.
            Right     //!< Right button.
        };

        int    mX { 0 };                     //!< X in client coordinates, pixels from the left.
        int    mY { 0 };                     //!< Y in client coordinates, pixels from the top.
        Button mButton { Button::None };     //!< Button pressed or released; None for a move.
    };
}

#endif // QT_LIKE_SIGNAL_DEMO_MOUSEEVENT_HPP
