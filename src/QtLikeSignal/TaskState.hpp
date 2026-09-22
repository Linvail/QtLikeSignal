// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::TaskState -- what a ThreadPool task and its TaskHandle both look at.

#ifndef QT_LIKE_SIGNAL_TASK_STATE_HPP
#define QT_LIKE_SIGNAL_TASK_STATE_HPP

#include <condition_variable>
#include <mutex>

namespace QtLikeSignal
{
    //! Where one submitted task stands, shared by the pool and by every handle to it.
    //!
    //! **Internal to the library.** A program uses TaskHandle, which is a pointer to one of these
    //! and the four questions worth asking about it.
    //!
    //! **It carries its own lock, rather than borrowing the pool's.** A handle may outlive the
    //! pool -- a caller can keep one, and the pool can be destroyed first -- so the thing a handle
    //! reads must not belong to the pool. The cost is a mutex and a condition variable for each
    //! task; the alternative is a handle that reads freed memory.
    //!
    //! **Only two moves are allowed:** Queued to Running, and either of those to Finished or
    //! Cancelled. cancel() succeeds only from Queued, which is what makes "cancel takes the work
    //! back" a promise rather than a hope: a task that has started is never abandoned half-way.
    class TaskState
    {
    public:
        //! Where the task stands.
        enum class Stage
        {
            Queued,      //!< Waiting for a worker.
            Running,     //!< A worker is inside run().
            Finished,    //!< run() returned.
            Cancelled    //!< Taken back before a worker started it.
        };

        //! Returns the stage. Cheap, and never blocks for long.
        Stage stage() const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            return mStage;
        }

        bool cancel();

        bool markRunning();

        void markFinished();

        bool wait
            (
            unsigned long aTime
            ) const;

    private:
        //! Guards mStage, and is what mChanged waits on.
        mutable std::mutex mMutex;

        //! Wakes wait() when the stage becomes Finished or Cancelled.
        mutable std::condition_variable mChanged;

        //! Where the task stands.
        Stage mStage { Stage::Queued };
    };
}

#endif // QT_LIKE_SIGNAL_TASK_STATE_HPP
