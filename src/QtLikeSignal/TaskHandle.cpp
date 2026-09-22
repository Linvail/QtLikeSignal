// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! TaskHandle implementation: four questions and one answer, all asked of the task's state.

#include "QtLikeSignal/TaskHandle.hpp"

#include "QtLikeSignal/TaskState.hpp"

#include <utility>

namespace QtLikeSignal
{
    //! Constructs a handle that refers to no task.
    //!
    //! Every question about it answers false, and cancel() and wait() do nothing. A program that
    //! keeps handles in a container gets this from a default-made slot.
    TaskHandle::TaskHandle()
    {
    }

    //! Constructs a handle to @p aState. Made by ThreadPool::submit() only.
    TaskHandle::TaskHandle
        (
        std::shared_ptr<TaskState> aState   //!< The task's state.
        )
        : mState( std::move( aState ) )
    {
    }

    //! Takes the work back, if no worker has started it.
    //!
    //! A task that is already running is never abandoned half-way: this refuses, and wait() is
    //! what a caller uses then.
    //!
    //! @return true if the task will not run; false if it is running, finished, cancelled
    //!         already, or if this handle refers to no task.
    bool TaskHandle::cancel()
    {
        return mState && mState->cancel();
    }

    //! Waits until the task is finished or cancelled, or until @p aTime milliseconds have passed.
    //!
    //! **Never from inside a task.** See the class comment.
    //!
    //! @return true if the task is finished or cancelled, and for a handle that refers to no
    //!         task; false if the time passed first.
    bool TaskHandle::wait
        (
        unsigned long aTime   //!< How long to wait, in milliseconds. The default is for ever.
        ) const
    {
        return !mState || mState->wait( aTime );
    }

    //! Returns true if run() has returned. False for a cancelled task, which never ran.
    bool TaskHandle::isFinished() const
    {
        return mState && mState->stage() == TaskState::Stage::Finished;
    }

    //! Returns true if the task was taken back before it started.
    bool TaskHandle::isCancelled() const
    {
        return mState && mState->stage() == TaskState::Stage::Cancelled;
    }

    //! Returns true if a worker is inside run() right now.
    bool TaskHandle::isRunning() const
    {
        return mState && mState->stage() == TaskState::Stage::Running;
    }
}
