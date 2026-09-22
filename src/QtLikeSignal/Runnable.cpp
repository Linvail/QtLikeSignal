// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Runnable implementation.

#include "QtLikeSignal/Runnable.hpp"

namespace QtLikeSignal
{
    //! Constructs work that the caller deletes. See setAutoDelete().
    Runnable::Runnable()
    {
    }

    //! Destroys the work.
    //!
    //! Out of line, and virtual, so that a pool can delete a Runnable through this pointer without
    //! knowing what it is, and so that the vtable has one place to live.
    Runnable::~Runnable()
    {
    }
}
