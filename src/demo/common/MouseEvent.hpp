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
    //! Deliberately carries no platform types. Each demo's application class translates the native
    //! event into this before emitting it -- a WM_LBUTTONDOWN on Windows, an XButtonPressedEvent on
    //! X11 -- so everything downstream, the slots and the painting, is ordinary C++ that knows
    //! nothing about either window system. That is the same split Qt makes between
    //! QWindowSystemInterface and QMouseEvent, and it is why this header is shared by both demos
    //! rather than duplicated per platform.
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
