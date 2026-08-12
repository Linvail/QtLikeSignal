//! @file
//!
//! Out-of-line members of AbstractEventDispatcher.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "AbstractEventDispatcher.hpp"

namespace QtMimic
{
    //! Constructs an event dispatcher.
    AbstractEventDispatcher::AbstractEventDispatcher() = default;

    //! Destroys the event dispatcher and cleans up pending events.
    AbstractEventDispatcher::~AbstractEventDispatcher() = default;

}
