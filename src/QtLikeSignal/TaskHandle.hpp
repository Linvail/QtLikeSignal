// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::TaskHandle -- what submit() gives back: a way to cancel a task, or wait for it.

#ifndef QT_LIKE_SIGNAL_TASK_HANDLE_HPP
#define QT_LIKE_SIGNAL_TASK_HANDLE_HPP

#include <climits>
#include <memory>

namespace QtLikeSignal
{
    class TaskState;

    //! One submitted task, seen from outside: cancel it, wait for it, or ask where it stands.
    //!
    //! @code
    //!   QtLikeSignal::TaskHandle handle = pool.submit( work );
    //!
    //!   if( !handle.cancel() )        // true only if no worker had started it
    //!   {
    //!       handle.wait();            // it is running; wait for it to finish
    //!   }
    //! @endcode
    //!
    //! **Cheap to ignore.** A caller that does not want one lets the returned handle die on the
    //! spot, and nothing is left behind: the task carries the same state whether anybody watches
    //! it or not.
    //!
    //! **It outlives the pool safely.** The state a handle reads belongs to the task, not to the
    //! pool, so a handle kept after the pool is destroyed answers about a task that was cancelled
    //! at shutdown rather than reading freed memory.
    //!
    //! **Never wait for a task from inside a task.** A task that waits for another task on a pool
    //! with one worker waits for a worker that is itself. The pool cannot see that happening and
    //! does not try to; the rule is the answer.
    //!
    //! **Copying a handle is copying the pointer.** Two handles to one task agree on everything,
    //! and either may cancel it; only the first cancel is the one that took the work back.
    class TaskHandle
    {
    public:
        TaskHandle();

        //! Returns true if the handle refers to a task. False for a default-made handle, and for
        //! one from a submit() that was refused.
        bool isValid() const
        {
            return static_cast<bool>( mState );
        }

        bool cancel();

        bool wait
            (
            unsigned long aTime = ULONG_MAX
            ) const;

        bool isFinished() const;

        bool isCancelled() const;

        bool isRunning() const;

    private:
        friend class ThreadPool;

        explicit TaskHandle
            (
            std::shared_ptr<TaskState> aState
            );

        //! The task's own state, shared with the pool and with every other handle to it. Null for
        //! a handle that refers to no task.
        std::shared_ptr<TaskState> mState;
    };
}

#endif // QT_LIKE_SIGNAL_TASK_HANDLE_HPP
