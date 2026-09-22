// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::Runnable -- one piece of work a ThreadPool runs on a worker thread.

#ifndef QT_LIKE_SIGNAL_RUNNABLE_HPP
#define QT_LIKE_SIGNAL_RUNNABLE_HPP

namespace QtLikeSignal
{
    //! Work to run on another thread: derive from this and write run(). Qt's QRunnable.
    //!
    //! **A class, because work usually has state.** A decode has a path to read and pixels to hand
    //! back, and a member is a plainer place for them than a lambda capture. Work that is two
    //! lines and no state does not need this class at all: ThreadPool::submit() takes any callable
    //! and wraps it in one of these.
    //!
    //! **Who deletes it is the caller's choice, and the default is the caller.** This is the one
    //! place this class disagrees with QRunnable, whose autoDelete() is true by default. A pool
    //! that deletes by default surprises the caller that kept a pointer to read the result from;
    //! here the caller keeps what it made, and setAutoDelete( true ) hands that job to the pool.
    //! A Runnable the pool made for a callable is always deleted by the pool.
    //!
    //! **Nothing may escape run().** The library compiles with exceptions disabled in the builds
    //! that matter, so a pool cannot catch one for the caller, and a worker thread has nowhere to
    //! report it to. Work that can fail reports the failure in its own members, which the caller
    //! reads after the task is finished.
    //!
    //! **run() is called on a worker thread**, which is a QtLikeSignal::Thread:
    //! Thread::currentThread() inside it is that worker, an Object made there belongs to it, and
    //! Object::callLater() from it reaches another thread's loop. What that worker does not have
    //! is an event loop of its own, so a Timer started inside run() never fires.
    class Runnable
    {
    public:
        Runnable();

        virtual ~Runnable();

        Runnable
            (
            const Runnable&
            ) = delete;

        Runnable& operator=
            (
            const Runnable&
            ) = delete;

        //! The work. Called once, on a worker thread.
        virtual void run() = 0;

        //! Returns true if the pool deletes this object after run(). False by default.
        bool autoDelete() const
        {
            return mAutoDelete;
        }

        //! Sets whether the pool deletes this object after run().
        //!
        //! Read once, when the task is taken from the queue, so a change after submit() may or may
        //! not be seen. Decide before submitting.
        void setAutoDelete
            (
            bool aAutoDelete   //!< True to give the pool the job of deleting this object.
            )
        {
            mAutoDelete = aAutoDelete;
        }

    private:
        //! True if the pool deletes this object after run(). See setAutoDelete().
        bool mAutoDelete { false };
    };
}

#endif // QT_LIKE_SIGNAL_RUNNABLE_HPP
