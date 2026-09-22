// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! TaskState implementation: the four stages, and the wait.

#include "QtLikeSignal/TaskState.hpp"

#include <chrono>
#include <climits>

namespace QtLikeSignal
{
    //! Takes the task back, if no worker has started it.
    //!
    //! @return true if the task was queued and is now cancelled; false if it is already running,
    //!         finished, or cancelled.
    bool TaskState::cancel()
    {
        {
            std::lock_guard<std::mutex> guard( mMutex );
            if( mStage != Stage::Queued )
            {
                return false;
            }
            mStage = Stage::Cancelled;
        }

        mChanged.notify_all();
        return true;
    }

    //! Moves the task from Queued to Running, for the worker that picked it up.
    //!
    //! @return true if this worker may run it; false if it was cancelled first, in which case the
    //!         worker drops it.
    bool TaskState::markRunning()
    {
        std::lock_guard<std::mutex> guard( mMutex );
        if( mStage != Stage::Queued )
        {
            return false;
        }

        mStage = Stage::Running;
        return true;
    }

    //! Records that run() has returned, and wakes whoever waits.
    void TaskState::markFinished()
    {
        {
            std::lock_guard<std::mutex> guard( mMutex );
            mStage = Stage::Finished;
        }

        mChanged.notify_all();
    }

    //! Waits until the task is finished or cancelled, or until @p aTime milliseconds have passed.
    //!
    //! @return true if the task is finished or cancelled; false if the time passed first.
    bool TaskState::wait
        (
        unsigned long aTime   //!< How long to wait, in milliseconds. ULONG_MAX is for ever.
        ) const
    {
        std::unique_lock<std::mutex> lock( mMutex );
        const auto settled = [this]()
            {
                return mStage == Stage::Finished || mStage == Stage::Cancelled;
            };

        if( aTime == ULONG_MAX )
        {
            mChanged.wait( lock, settled );
            return true;
        }

        return mChanged.wait_for( lock, std::chrono::milliseconds( aTime ), settled );
    }
}
