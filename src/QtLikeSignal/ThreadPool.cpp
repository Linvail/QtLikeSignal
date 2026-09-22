// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! ThreadPool implementation: the workers, the queue, and shutdown.

#include "QtLikeSignal/ThreadPool.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"
#include "QtLikeSignal/TaskState.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace QtLikeSignal
{
    namespace
    {
        //! The most workers defaultWorkerCount() asks for, whatever the machine reports.
        //!
        //! A machine with 32 cores does not want 31 threads decoding images: the work this pool is
        //! for is bounded by the disk and by memory long before it is bounded by cores, and every
        //! thread is a stack. A program that knows better gives the count it wants.
        const int kMaxDefaultWorkers = 8;
    }

    //! One worker thread: a Thread that runs the pool's queue instead of an event loop.
    //!
    //! A nested class rather than a free one, because it exists only to give ThreadPool a run()
    //! and needs the pool's private queue. Thread::threadBody() sets currentThread(), makes the
    //! dispatcher and binds the affinity before it calls run(), so work sees a thread that is
    //! complete in every way except that nothing runs an event loop on it.
    class ThreadPool::Worker : public Thread
    {
    public:
        //! Constructs a worker of @p aPool, named @p aName. Does not start it.
        Worker
            (
            ThreadPool& aPool,          //!< The pool whose queue this worker drains.
            const std::string& aName    //!< The thread name, for a debugger and for `ps -L`.
            )
            : Thread( aName )
            , mPool( aPool )
        {
        }

    protected:
        //! Drains the pool's queue until the pool stops.
        virtual void run() override
        {
            mPool.workerLoop();
        }

    private:
        //! The pool. It outlives every worker: the destructor joins them all.
        ThreadPool& mPool;
    };

    //! Returns the worker count for a pool that was given none.
    //!
    //! One less than the machine reports, so the thread that asked for the work keeps a core to
    //! run on, clamped to 1 to 8. A machine that reports nothing -- `hardware_concurrency()` may
    //! answer 0 -- gets one worker, because a pool with no workers is a queue nothing drains.
    int ThreadPool::defaultWorkerCount()
    {
        const unsigned int reported = std::thread::hardware_concurrency();
        const int usable = ( reported > 1U ) ? static_cast<int>( reported ) - 1 : 1;
        return std::min( usable, kMaxDefaultWorkers );
    }

    //! Constructs a pool with @p aWorkerCount workers and starts them.
    //!
    //! The workers are running, and waiting, when the constructor returns. A count of 0 asks for
    //! defaultWorkerCount(); a negative count is that too, with a log record, because a pool with
    //! no worker accepts work and never runs it.
    ThreadPool::ThreadPool
        (
        int aWorkerCount   //!< How many workers, or 0 for defaultWorkerCount().
        )
    {
        int wanted = ( aWorkerCount == 0 ) ? defaultWorkerCount() : aWorkerCount;
        if( wanted < 1 )
        {
            qCWarning( gLogThread ) << "ThreadPool: a worker count of" << aWorkerCount
                                    << "is not usable; one worker is started instead";
            wanted = 1;
        }

        mWorkers.reserve( static_cast<std::size_t>( wanted ) );
        for( int index = 0; index < wanted; ++index )
        {
            // 14 characters at two digits, which is inside the 15 Linux allows for a thread name.
            const std::string name = "QtLikeSignal pool " + std::to_string( index );
            std::unique_ptr<Worker> worker( new( std::nothrow ) Worker( *this, name ) );
            if( !worker )
            {
                qCCritical( gLogThread ) << "ThreadPool: worker" << index << "could not be made";
                break;
            }

            worker->start();
            mWorkers.push_back( std::move( worker ) );
        }
    }

    //! Drops the queued work and waits for the running work, then joins the workers.
    //!
    //! Waiting is the whole point: a destructor that returned while a worker still ran would leave
    //! that worker holding pointers into a program that has moved on. Work that has not started is
    //! dropped rather than run, because nobody is left who wants its result.
    ThreadPool::~ThreadPool()
    {
        static_cast<void>( clear() );

        {
            std::lock_guard<std::mutex> guard( mMutex );
            mStopping = true;
        }
        mWorkArrived.notify_all();

        // Each worker leaves workerLoop() when it sees mStopping, so ~Thread's own quit() and
        // wait() find a thread that is already on its way out.
        mWorkers.clear();
    }

    //! Submits @p aRunnable to the queue, to run on a worker as soon as one is free.
    //!
    //! @p aPriority ranks this work against the work already waiting: higher runs first, and work
    //! of one priority runs in the order it was submitted. It changes nothing about a task that
    //! has started. The default, 0, is what most work wants; a loader that must answer a scroll
    //! now raises the tile the user is looking at rather than lowering the hundred it is not.
    //!
    //! The pool deletes the work afterwards only if Runnable::autoDelete() says so, which is false
    //! unless the caller asked. A null pointer, work submitted while the pool is shutting down,
    //! and a task whose state could not be allocated are each refused with a log record; the work
    //! never runs, and the pool deletes it if it owns it.
    //!
    //! @return a handle to the task, or a handle that refers to none if the work was refused.
    TaskHandle ThreadPool::submit
        (
        Runnable* aRunnable,   //!< The work. Not owned unless it says autoDelete().
        int aPriority          //!< Higher runs first. Equal priorities keep their order.
        )
    {
        if( aRunnable == nullptr )
        {
            qCWarning( gLogThread ) << "ThreadPool::submit: refused; the work is null";
            return TaskHandle();
        }

        Task task;
        task.mRunnable   = aRunnable;
        task.mPriority   = aPriority;
        task.mAutoDelete = aRunnable->autoDelete();

        // nothrow rather than make_shared, because make_shared throws when it cannot allocate and
        // this library is built to compile without exceptions.
        task.mState.reset( new( std::nothrow ) TaskState() );
        if( !task.mState )
        {
            qCCritical( gLogThread ) << "ThreadPool::submit: refused; no memory for the task state";
            dropTask( task );
            return TaskHandle();
        }

        {
            std::lock_guard<std::mutex> guard( mMutex );
            if( mStopping || mWorkers.empty() )
            {
                qCWarning( gLogThread ) <<
                    "ThreadPool::submit: refused; the pool is stopping or has no worker";
                static_cast<void>( task.mState->cancel() );
                dropTask( task );
                return TaskHandle();
            }

            enqueue( task );
        }

        mWorkArrived.notify_one();
        return TaskHandle( task.mState );
    }

    //! Waits until the queue is empty and no task is running, or @p aTime milliseconds pass.
    //!
    //! **Never call this from inside a task.** A task that waits for the pool it is running on
    //! waits for itself, and on a pool of one worker that is for ever.
    //!
    //! @return true if the pool went quiet; false if the time passed first.
    bool ThreadPool::waitForDone
        (
        unsigned long aTime   //!< How long to wait, in milliseconds. The default is for ever.
        )
    {
        std::unique_lock<std::mutex> lock( mMutex );
        const auto quiet = [this]()
            {
                return mQueue.empty() && mActiveCount == 0U;
            };

        if( aTime == ULONG_MAX )
        {
            mWentQuiet.wait( lock, quiet );
            return true;
        }

        return mWentQuiet.wait_for( lock, std::chrono::milliseconds( aTime ), quiet );
    }

    //! Drops the work that has not started yet. Work already running is left to finish.
    //!
    //! @return how many tasks were dropped.
    std::size_t ThreadPool::clear()
    {
        std::deque<Task> dropped;
        {
            std::lock_guard<std::mutex> guard( mMutex );
            dropped.swap( mQueue );
        }

        // Outside the lock: a Runnable's destructor is the caller's code, and no lock of this
        // library is held while a caller's code runs.
        for( const Task& task : dropped )
        {
            static_cast<void>( task.mState->cancel() );
            dropTask( task );
        }

        if( !dropped.empty() )
        {
            std::lock_guard<std::mutex> guard( mMutex );
            if( mQueue.empty() && mActiveCount == 0U )
            {
                mWentQuiet.notify_all();
            }
        }

        return dropped.size();
    }

    //! Returns how many tasks are waiting for a worker. For tests and diagnostics.
    std::size_t ThreadPool::queuedCount() const
    {
        std::lock_guard<std::mutex> guard( mMutex );
        return mQueue.size();
    }

    //! Returns how many tasks are running right now. For tests and diagnostics.
    std::size_t ThreadPool::activeCount() const
    {
        std::lock_guard<std::mutex> guard( mMutex );
        return mActiveCount;
    }

    //! Puts @p aTask in the queue, after the work that runs before it.
    //!
    //! The queue is sorted by priority, highest first, and upper_bound puts a task after the ones
    //! of its own priority -- so equal priorities keep the order they were submitted in. Qt's
    //! QThreadPoolPrivate::enqueueTask() ranks its pages of runnables the same way.
    //!
    //! Called with the lock held. Work of the lowest priority, which is most work, lands at the
    //! end with no elements moved.
    void ThreadPool::enqueue
        (
        const Task& aTask   //!< The work to queue.
        )
    {
        const auto ranksBefore = []( int aWanted, const Task& aQueued )
            {
                return aQueued.mPriority < aWanted;
            };

        const auto at = std::upper_bound( mQueue.begin(), mQueue.end(), aTask.mPriority,
            ranksBefore );
        mQueue.insert( at, aTask );
    }

    //! The body of every worker: take work, run it, repeat, until the pool stops.
    //!
    //! The lock is held for the bookkeeping only. A task runs with nothing locked, so a task may
    //! submit more work, and a long task never blocks another worker.
    void ThreadPool::workerLoop()
    {
        std::unique_lock<std::mutex> lock( mMutex );
        for(;;)
        {
            mWorkArrived.wait( lock, [this]()
                {
                    return mStopping || !mQueue.empty();
                } );

            if( mQueue.empty() )
            {
                // Only the stopping case is left: the destructor has dropped the queue already.
                return;
            }

            const Task task = mQueue.front();
            mQueue.pop_front();

            // A task cancelled while it waited is dropped here rather than being erased from the
            // queue when cancel() was called: cancel() belongs to the handle, which may outlive
            // this pool and must not reach into it.
            if( !task.mState->markRunning() )
            {
                lock.unlock();
                dropTask( task );
                lock.lock();

                // The same check the end of a task makes. Dropping the last queued task is a way
                // for the pool to become quiet, and a waitForDone() that was waiting for exactly
                // that would otherwise wait until its timeout, or for ever.
                if( mQueue.empty() && mActiveCount == 0U )
                {
                    mWentQuiet.notify_all();
                }
                continue;
            }

            ++mActiveCount;

            lock.unlock();
            runTask( task );
            lock.lock();

            --mActiveCount;
            if( mQueue.empty() && mActiveCount == 0U )
            {
                mWentQuiet.notify_all();
            }
        }
    }

    //! Runs one task and deletes it if the pool owns it.
    //!
    //! Called with no lock held. An exception that escaped run() would pass through here and end
    //! the program; Runnable::run() says that work must not let one out, and the builds that
    //! matter compile with exceptions disabled anyway.
    void ThreadPool::runTask
        (
        const Task& aTask   //!< The work to run.
        )
    {
        aTask.mRunnable->run();
        aTask.mState->markFinished();

        if( aTask.mAutoDelete )
        {
            delete aTask.mRunnable;
        }
    }

    //! Deletes the work of @p aTask if the pool owns it. For work that will not run.
    //!
    //! Called with no lock held, for the reason clear() gives: a Runnable's destructor is the
    //! caller's code.
    void ThreadPool::dropTask
        (
        const Task& aTask   //!< The work that will not run.
        )
    {
        if( aTask.mAutoDelete )
        {
            delete aTask.mRunnable;
        }
    }
}
