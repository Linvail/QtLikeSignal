// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Out-of-line members of AbstractEventDispatcher.

#include "QtLikeSignal/AbstractEventDispatcher.hpp"

namespace QtLikeSignal
{
    //! Constructs an event dispatcher.
    AbstractEventDispatcher::AbstractEventDispatcher() = default;

    //! Destroys the event dispatcher and cleans up pending events.
    AbstractEventDispatcher::~AbstractEventDispatcher() = default;

}
