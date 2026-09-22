// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for QtLikeSignal::ThreadPool and QtLikeSignal::Runnable: the workers, the
//! queue, who deletes the work, and what shutdown waits for.

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogMessage.hpp"
#include "QtLikeSignal/LogSink.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Runnable.hpp"
#include "QtLikeSignal/TaskHandle.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/ThreadPool.hpp"

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using namespace QtLikeSignal;

    //! How long a test waits for something that should happen at once, before it gives up.
    const auto kPatience = 5s;

    //! A gate a test opens when it chooses, so a task can be held inside run().
    class Gate
    {
    public:
        //! Blocks until open() is called, or the patience runs out.
        void wait()
        {
            std::unique_lock<std::mutex> lock( mMutex );
            mChanged.wait_for( lock, kPatience, [this]()
                {
                    return mOpen;
                } );
        }

        //! Lets everybody through, now and later.
        void open()
        {
            {
                std::lock_guard<std::mutex> guard( mMutex );
                mOpen = true;
            }
            mChanged.notify_all();
        }

    private:
        std::mutex mMutex;                 //!< Guards mOpen.
        std::condition_variable mChanged;  //!< Wakes the waiters.
        bool mOpen { false };              //!< True once open() was called.
    };

    //! A sink that counts the records containing one phrase, installed for as long as it lives.
    //!
    //! It lets a test assert on a record the library writes, and it keeps every other record out of
    //! a passing run. The previous sink is put back on destruction.
    class PhraseCountingSink : public LogSink
    {
    public:
        //! Installs this sink, which counts the records that contain @p aPhrase.
        explicit PhraseCountingSink
            (
            const std::string& aPhrase   //!< The text to look for inside a record.
            )
            : mPhrase( aPhrase )
        {
            mPrevious = Log::setSink( this );
        }

        //! Puts the previous sink back.
        virtual ~PhraseCountingSink() override
        {
            Log::setSink( mPrevious );
        }

        PhraseCountingSink
            (
            const PhraseCountingSink&
            ) = delete;

        PhraseCountingSink& operator=
            (
            const PhraseCountingSink&
            ) = delete;

        //! Counts the record if it contains the phrase.
        virtual void write
            (
            const LogMessage& aMessage   //!< The record.
            ) override
        {
            const std::string text( aMessage.mText, aMessage.mLength );
            if( text.find( mPhrase ) != std::string::npos )
            {
                ++mCount;
            }
        }

        //! @return how many records contained the phrase.
        int count() const
        {
            return mCount.load();
        }

    private:
        const std::string mPhrase;         //!< The text to look for.
        std::atomic<int> mCount { 0 };     //!< Records that contained it.
        LogSink* mPrevious { nullptr };    //!< What was installed before, put back on destruction.
    };

    //! Counts how often it ran, and on which threads, and says when it is destroyed.
    class CountingWork : public Runnable
    {
    public:
        //! Constructs work that counts into @p aCount.
        explicit CountingWork
            (
            std::atomic<int>& aCount   //!< Incremented by each run.
            )
            : mCount( aCount )
        {
        }

        //! Counts the run, and remembers the thread it ran on and how many ran before it.
        virtual void run() override
        {
            mRanOn = Thread::currentThread();
            mOrder = ++mCount;
        }

        std::atomic<int>& mCount;             //!< Where the runs are counted.
        Thread* mRanOn { nullptr };           //!< The worker that ran it, or null.
        int mOrder { 0 };                     //!< 1 for the first of a batch to run.
    };

    //! Work that says when it is destroyed, so a test can see who deleted it.
    class ReportingWork : public Runnable
    {
    public:
        //! Constructs work that sets @p aDeleted when it is destroyed.
        explicit ReportingWork
            (
            std::atomic<bool>& aDeleted   //!< Set true by the destructor.
            )
            : mDeleted( aDeleted )
        {
        }

        //! Reports the destruction.
        virtual ~ReportingWork() override
        {
            mDeleted = true;
        }

        //! Does nothing; this work is about its lifetime.
        virtual void run() override
        {
        }

    private:
        std::atomic<bool>& mDeleted;   //!< Set true by the destructor.
    };

    //! Waits until @p aCondition is true, or the patience runs out.
    //!
    //! @return true if the condition came true.
    template <typename Condition>
    bool waitFor
        (
        Condition aCondition   //!< Asked again and again.
        )
    {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while( std::chrono::steady_clock::now() < deadline )
        {
            if( aCondition() )
            {
                return true;
            }
            std::this_thread::sleep_for( 1ms );
        }
        return aCondition();
    }
}

//! Verifies a pool starts the workers it was asked for, and names them.
TEST( ThreadPoolTest, TheWorkersStartWithThePool )
{
    ThreadPool pool( 3 );
    EXPECT_EQ( pool.workerCount(), 3 );
    EXPECT_EQ( pool.queuedCount(), 0U );
    EXPECT_EQ( pool.activeCount(), 0U );

    EXPECT_GE( ThreadPool::defaultWorkerCount(), 1 );
    EXPECT_LE( ThreadPool::defaultWorkerCount(), 8 );

    ThreadPool automatic;
    EXPECT_EQ( automatic.workerCount(), ThreadPool::defaultWorkerCount() );
}

//! Verifies a worker count that cannot be used gives one worker, not none.
TEST( ThreadPoolTest, AnUnusableWorkerCountGivesOneWorker )
{
    ThreadPool pool( -4 );
    EXPECT_EQ( pool.workerCount(), 1 );

    std::atomic<int> count { 0 };
    pool.submit( [&count]()
        {
            ++count;
        } );
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( count.load(), 1 );
}

//! Verifies every task of a batch runs, on the pool's threads rather than the caller's.
TEST( ThreadPoolTest, EveryTaskRunsOnAWorker )
{
    ThreadPool pool( 2 );
    std::atomic<int> count { 0 };
    std::mutex threadsMutex;
    std::set<std::thread::id> threads;

    for( int index = 0; index < 64; ++index )
    {
        pool.submit( [&]()
            {
                ++count;
                std::lock_guard<std::mutex> guard( threadsMutex );
                threads.insert( std::this_thread::get_id() );
            } );
    }

    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( count.load(), 64 );
    EXPECT_EQ( pool.queuedCount(), 0U );
    EXPECT_EQ( pool.activeCount(), 0U );

    std::lock_guard<std::mutex> guard( threadsMutex );
    EXPECT_FALSE( threads.count( std::this_thread::get_id() ) == 1U );
    EXPECT_LE( threads.size(), 2U );
}

//! Verifies work that arrives at an idle pool wakes a worker.
//!
//! The first batch is submitted while the workers are still starting, so they find it without
//! being woken. This test waits for the pool to go quiet first, which parks every worker in its
//! wait, and only then submits: the task runs if, and only if, submit() wakes somebody.
TEST( ThreadPoolTest, WorkThatArrivesLaterWakesAWorker )
{
    ThreadPool pool( 2 );
    std::atomic<int> count { 0 };

    pool.submit( [&count]()
        {
            ++count;
        } );
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    ASSERT_EQ( count.load(), 1 );

    // Both workers are parked by now: the pool went quiet, and nothing has been submitted since.
    std::this_thread::sleep_for( 20ms );

    pool.submit( [&count]()
        {
            ++count;
        } );
    EXPECT_TRUE( pool.waitForDone( 2000 ) );
    EXPECT_EQ( count.load(), 2 );
}

//! Verifies a task runs on a QtLikeSignal::Thread, so currentThread() is the worker it runs on.
TEST( ThreadPoolTest, ATaskRunsOnAQtLikeSignalThread )
{
    ThreadPool pool( 1 );
    std::atomic<int> count { 0 };
    CountingWork work( count );

    pool.submit( &work );
    ASSERT_TRUE( pool.waitForDone( 5000 ) );

    EXPECT_EQ( count.load(), 1 );
    ASSERT_NE( work.mRanOn, nullptr );
    EXPECT_NE( work.mRanOn, Thread::currentThread() );
    EXPECT_TRUE( work.mRanOn->isRunning() );
}

//! Verifies the caller keeps the Runnable it made, unless it asked the pool to take it.
TEST( ThreadPoolTest, AutoDeleteDecidesWhoDeletesTheWork )
{
    ThreadPool pool( 1 );

    std::atomic<bool> keptDeleted { false };
    {
        ReportingWork kept( keptDeleted );
        pool.submit( &kept );
        ASSERT_TRUE( pool.waitForDone( 5000 ) );
        EXPECT_FALSE( keptDeleted.load() );   // the pool left it alone
    }
    EXPECT_TRUE( keptDeleted.load() );        // the scope deleted it

    std::atomic<bool> givenDeleted { false };
    ReportingWork* const given = new ReportingWork( givenDeleted );
    given->setAutoDelete( true );
    pool.submit( given );
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_TRUE( waitFor( [&givenDeleted]()
        {
            return givenDeleted.load();
        } ) );
}

//! Verifies one worker keeps the order work was submitted in.
TEST( ThreadPoolTest, OneWorkerKeepsSubmissionOrder )
{
    ThreadPool pool( 1 );
    std::mutex orderMutex;
    std::vector<int> order;

    for( int index = 0; index < 32; ++index )
    {
        pool.submit( [&order, &orderMutex, index]()
            {
                std::lock_guard<std::mutex> guard( orderMutex );
                order.push_back( index );
            } );
    }

    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    ASSERT_EQ( order.size(), 32U );
    for( int index = 0; index < 32; ++index )
    {
        EXPECT_EQ( order[static_cast<std::size_t>( index )], index );
    }
}

//! Verifies clear() drops what has not started, and leaves what is running alone.
TEST( ThreadPoolTest, ClearDropsWhatHasNotStarted )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    std::atomic<int> finished { 0 };

    pool.submit( [&]()
        {
            ++started;
            gate.wait();
            ++finished;
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    std::atomic<int> laterRuns { 0 };
    for( int index = 0; index < 8; ++index )
    {
        pool.submit( [&laterRuns]()
            {
                ++laterRuns;
            } );
    }

    EXPECT_EQ( pool.clear(), 8U );
    EXPECT_EQ( pool.queuedCount(), 0U );
    EXPECT_EQ( pool.activeCount(), 1U );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( finished.load(), 1 );
    EXPECT_EQ( laterRuns.load(), 0 );
}

//! Verifies clear() deletes the work it drops, but only the work the pool owns.
TEST( ThreadPoolTest, ClearDeletesOnlyWhatThePoolOwns )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );

    // The one worker must be inside that task before the next two are submitted, or clear()
    // drops three tasks rather than two.
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    std::atomic<bool> ownedDeleted { false };
    ReportingWork* const owned = new ReportingWork( ownedDeleted );
    owned->setAutoDelete( true );
    pool.submit( owned );

    std::atomic<bool> keptDeleted { false };
    ReportingWork kept( keptDeleted );
    pool.submit( &kept );

    EXPECT_EQ( pool.clear(), 2U );
    EXPECT_TRUE( ownedDeleted.load() );
    EXPECT_FALSE( keptDeleted.load() );

    gate.open();
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
}

//! Verifies waitForDone() reports a timeout rather than waiting for ever.
TEST( ThreadPoolTest, WaitForDoneReportsATimeout )
{
    ThreadPool pool( 1 );
    Gate gate;
    pool.submit( [&gate]()
        {
            gate.wait();
        } );

    EXPECT_FALSE( pool.waitForDone( 20 ) );

    gate.open();
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
}

//! Verifies the destructor waits for the task that is running, and drops the rest.
TEST( ThreadPoolTest, TheDestructorWaitsForRunningWork )
{
    Gate gate;
    std::atomic<int> started { 0 };
    std::atomic<int> finished { 0 };
    std::atomic<int> neverRan { 0 };

    // Opened from another thread, after this one is already inside the destructor. Opening it
    // here would let the worker finish the first task and start the second before the destructor
    // had dropped it, which is a race in the test rather than in the pool.
    std::thread opener( [&gate]()
        {
            std::this_thread::sleep_for( 50ms );
            gate.open();
        } );

    {
        ThreadPool pool( 1 );
        pool.submit( [&]()
            {
                ++started;
                gate.wait();
                ++finished;
            } );
        ASSERT_TRUE( waitFor( [&started]()
            {
                return started.load() == 1;
            } ) );

        pool.submit( [&neverRan]()
            {
                ++neverRan;
            } );

        // The destructor runs here: it drops the queued task, then waits for the running one.
    }

    opener.join();

    EXPECT_EQ( finished.load(), 1 );
    EXPECT_EQ( neverRan.load(), 0 );
}

//! Verifies a task may submit more work to its own pool.
TEST( ThreadPoolTest, ATaskCanSubmitMoreWork )
{
    ThreadPool pool( 2 );
    std::atomic<int> count { 0 };

    pool.submit( [&pool, &count]()
        {
            ++count;
            pool.submit( [&count]()
            {
                ++count;
            } );
        } );

    EXPECT_TRUE( waitFor( [&count]()
        {
            return count.load() == 2;
        } ) );
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
}

//! Verifies null work is refused with no effect.
TEST( ThreadPoolTest, NullWorkIsRefused )
{
    ThreadPool pool( 1 );
    pool.submit( static_cast<Runnable*>( nullptr ) );
    EXPECT_EQ( pool.queuedCount(), 0U );
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
}

//! Verifies submissions from several threads at once all arrive.
TEST( ThreadPoolTest, SubmitIsSafeFromManyThreads )
{
    ThreadPool pool( 4 );
    std::atomic<int> count { 0 };
    std::vector<std::thread> producers;

    for( int producer = 0; producer < 4; ++producer )
    {
        producers.emplace_back( [&pool, &count]()
            {
                for( int index = 0; index < 100; ++index )
                {
                    pool.submit( [&count]()
                    {
                        ++count;
                    } );
                }
            } );
    }

    for( std::thread& producer : producers )
    {
        producer.join();
    }

    ASSERT_TRUE( pool.waitForDone( 10000 ) );
    EXPECT_EQ( count.load(), 400 );
}

//! Verifies a handle reports where its task stands, from queued to finished.
TEST( ThreadPoolTest, AHandleFollowsItsTask )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };

    TaskHandle running = pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( running.isValid() );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );
    EXPECT_TRUE( running.isRunning() );
    EXPECT_FALSE( running.isFinished() );
    EXPECT_FALSE( running.isCancelled() );

    gate.open();
    EXPECT_TRUE( running.wait( 5000 ) );
    EXPECT_TRUE( running.isFinished() );
    EXPECT_FALSE( running.isRunning() );

    // A finished task is waited for again with no delay.
    EXPECT_TRUE( running.wait( 0 ) );
}

//! Verifies cancel() takes back work that no worker has started, and the work never runs.
TEST( ThreadPoolTest, CancelTakesBackQueuedWork )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    std::atomic<int> laterRan { 0 };

    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    TaskHandle queued = pool.submit( [&laterRan]()
        {
            ++laterRan;
        } );

    EXPECT_TRUE( queued.cancel() );
    EXPECT_TRUE( queued.isCancelled() );
    EXPECT_FALSE( queued.isFinished() );
    EXPECT_TRUE( queued.wait( 0 ) );   // a cancelled task is settled
    EXPECT_FALSE( queued.cancel() );   // and cancelling it again changes nothing

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( laterRan.load(), 0 );
}

//! Verifies cancel() is refused once a worker is inside the task, and wait() still returns.
TEST( ThreadPoolTest, CancelIsRefusedOnceTheWorkStarted )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    std::atomic<int> finished { 0 };

    TaskHandle handle = pool.submit( [&]()
        {
            ++started;
            gate.wait();
            ++finished;
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    EXPECT_FALSE( handle.cancel() );
    EXPECT_FALSE( handle.isCancelled() );

    gate.open();
    EXPECT_TRUE( handle.wait( 5000 ) );
    EXPECT_EQ( finished.load(), 1 );
}

//! Verifies the work the pool owns is deleted when a cancelled task is dropped.
TEST( ThreadPoolTest, CancelledWorkThePoolOwnsIsDeleted )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    std::atomic<bool> deleted { false };
    ReportingWork* const owned = new ReportingWork( deleted );
    owned->setAutoDelete( true );
    TaskHandle handle = pool.submit( owned );
    EXPECT_TRUE( handle.cancel() );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_TRUE( waitFor( [&deleted]()
        {
            return deleted.load();
        } ) );
}

//! Verifies a handle kept after the pool is gone answers rather than reading freed memory.
TEST( ThreadPoolTest, AHandleOutlivesItsPool )
{
    TaskHandle finished;
    TaskHandle cancelled;
    Gate gate;
    std::thread opener;

    {
        ThreadPool pool( 1 );
        std::atomic<int> started { 0 };
        finished = pool.submit( [&]()
            {
                ++started;
                gate.wait();
            } );
        ASSERT_TRUE( waitFor( [&started]()
            {
                return started.load() == 1;
            } ) );

        cancelled = pool.submit( []()
            {
            } );

        // The destructor drops the queued task and waits for the running one, so the gate must be
        // opened from somewhere that is not this thread. Joined rather than detached: a detached
        // thread can still be inside Gate::open() when this function returns and destroys the
        // gate, which ThreadSanitizer reports as a race between notify_all() and the destructor.
        opener = std::thread( [&gate]()
            {
                std::this_thread::sleep_for( 20ms );
                gate.open();
            } );
    }

    opener.join();

    EXPECT_TRUE( finished.isFinished() );
    EXPECT_TRUE( cancelled.isCancelled() );
    EXPECT_TRUE( cancelled.wait( 0 ) );
    EXPECT_FALSE( cancelled.cancel() );
}

//! Verifies two threads cancelling one task agree on which of them took the work back.
TEST( ThreadPoolTest, OnlyOneCancelWins )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    std::atomic<int> ran { 0 };
    TaskHandle handle = pool.submit( [&ran]()
        {
            ++ran;
        } );

    std::atomic<int> wins { 0 };
    std::vector<std::thread> cancellers;
    for( int index = 0; index < 4; ++index )
    {
        cancellers.emplace_back( [handle, &wins]() mutable
            {
                if( handle.cancel() )
                {
                    ++wins;
                }
            } );
    }
    for( std::thread& canceller : cancellers )
    {
        canceller.join();
    }

    EXPECT_EQ( wins.load(), 1 );
    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( ran.load(), 0 );
}

//! Verifies waitForDone() returns when the work it was waiting for is cancelled.
//!
//! Dropping the last queued task is one of the ways a pool becomes quiet, and a waiter that is
//! already inside waitForDone() has to be told about that one as well as about a task finishing.
TEST( ThreadPoolTest, WaitForDoneReturnsWhenTheLastTaskIsCancelled )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };

    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    std::atomic<int> neverRan { 0 };
    TaskHandle queued = pool.submit( [&neverRan]()
        {
            ++neverRan;
        } );

    // A waiter that is already inside waitForDone() when the queue empties. How long it takes is
    // what this test is about, not what it returns: wait_for() asks its predicate again when the
    // time runs out, so a waiter nobody wakes still reports the pool quiet -- late.
    std::atomic<bool> quiet { false };
    std::atomic<long long> waitedMs { 0 };
    std::thread waiter( [&pool, &quiet, &waitedMs]()
        {
            const auto started = std::chrono::steady_clock::now();
            quiet   = pool.waitForDone( 30000 );
            waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started ).count();
        } );

    std::this_thread::sleep_for( 20ms );
    EXPECT_TRUE( queued.cancel() );
    gate.open();

    waiter.join();
    EXPECT_TRUE( quiet.load() );
    EXPECT_LT( waitedMs.load(), 5000 );
    EXPECT_EQ( neverRan.load(), 0 );
}

//! Verifies a default-made handle answers without a task behind it.
TEST( ThreadPoolTest, AHandleWithNoTaskIsHarmless )
{
    TaskHandle empty;
    EXPECT_FALSE( empty.isValid() );
    EXPECT_FALSE( empty.cancel() );
    EXPECT_FALSE( empty.isFinished() );
    EXPECT_FALSE( empty.isCancelled() );
    EXPECT_FALSE( empty.isRunning() );
    EXPECT_TRUE( empty.wait( 0 ) );
}

//! Verifies work refused by a pool that has no room gives a handle that refers to no task.
TEST( ThreadPoolTest, RefusedWorkGivesAnEmptyHandle )
{
    ThreadPool pool( 1 );
    const TaskHandle handle = pool.submit( static_cast<Runnable*>( nullptr ) );
    EXPECT_FALSE( handle.isValid() );
}

//! Verifies waiting work runs in priority order, and equal priorities keep their order.
TEST( ThreadPoolTest, PriorityRanksTheWaitingWork )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    std::mutex orderMutex;
    std::vector<std::string> order;

    // The one worker is held inside this task, so everything below waits in the queue.
    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    const auto record = [&order, &orderMutex]( const char* aName )
        {
            std::lock_guard<std::mutex> guard( orderMutex );
            order.push_back( aName );
        };

    pool.submit( [record]()
        {
            record( "normal-first" );
        } );
    pool.submit( [record]()
        {
            record( "low" );
        }, -5 );
    pool.submit( [record]()
        {
            record( "high-first" );
        }, 5 );
    pool.submit( [record]()
        {
            record( "normal-second" );
        } );
    pool.submit( [record]()
        {
            record( "high-second" );
        }, 5 );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );

    std::lock_guard<std::mutex> guard( orderMutex );
    const std::vector<std::string> expected = { "high-first", "high-second", "normal-first",
                                                "normal-second", "low" };
    EXPECT_EQ( order, expected );
}

//! Verifies priority does not interrupt work that has already started.
TEST( ThreadPoolTest, PriorityNeverInterruptsRunningWork )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    std::atomic<bool> lowFinished { false };
    std::atomic<bool> highRan { false };

    pool.submit( [&]()
        {
            ++started;
            gate.wait();
            lowFinished = true;
        }, -10 );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    pool.submit( [&]()
        {
            // The low-priority task must have finished before this one starts: the worker inside
            // it is not taken away from it.
            EXPECT_TRUE( lowFinished.load() );
            highRan = true;
        }, 100 );

    std::this_thread::sleep_for( 20ms );
    EXPECT_FALSE( highRan.load() );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_TRUE( highRan.load() );
}

//! Verifies a Runnable is ranked by the priority given to submit(), like a callable.
TEST( ThreadPoolTest, ARunnableTakesAPriorityToo )
{
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    std::atomic<int> count { 0 };
    CountingWork low( count );
    CountingWork high( count );

    pool.submit( &low, -1 );
    pool.submit( &high, 1 );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( count.load(), 2 );
    EXPECT_EQ( high.mOrder, 1 );
    EXPECT_EQ( low.mOrder, 2 );
}

namespace
{
    //! A receiver that keeps what a continuation handed it, and the thread it arrived on.
    class Collector : public Object
    {
    public:
        //! Keeps @p aValue, and where it arrived.
        void take
            (
            std::string aValue   //!< What the work produced.
            )
        {
            mValue     = std::move( aValue );
            mArrivedOn = std::this_thread::get_id();
            ++mCalls;
        }

        std::string mValue;              //!< The last value handed over.
        std::thread::id mArrivedOn;      //!< Where the continuation ran.
        std::atomic<int> mCalls { 0 };   //!< How many continuations arrived.
    };

    //! Runs the calling thread's loop until @p aCondition is true, or the patience runs out.
    //!
    //! A continuation reaches its receiver as a posted event, so the receiver's loop has to run
    //! for it to arrive. A test has no exec(), so it pumps.
    template <typename Condition>
    bool pumpUntil
        (
        Condition aCondition   //!< Asked again after each pass.
        )
    {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while( std::chrono::steady_clock::now() < deadline )
        {
            Thread::currentThread()->processEvents();
            if( aCondition() )
            {
                return true;
            }
            std::this_thread::sleep_for( 1ms );
        }
        return aCondition();
    }

    //! A value that counts how often it was copied, to show that a result is moved home.
    struct CountedValue
    {
        //! Constructs a value that has not been copied.
        CountedValue() = default;

        //! Counts the copy.
        CountedValue
            (
            const CountedValue& aOther   //!< The value copied from.
            )
            : mCopies( aOther.mCopies + 1 )
        {
        }

        //! Moves without counting.
        CountedValue
            (
            CountedValue&& aOther   //!< The value moved from.
            ) noexcept
            : mCopies( aOther.mCopies )
        {
        }

        CountedValue& operator=
            (
            const CountedValue&
            ) = default;

        CountedValue& operator=
            (
            CountedValue&&
            ) = default;

        int mCopies { 0 };   //!< How often this value was copied on its way home.
    };
}

//! Verifies a result reaches the receiver's own thread, not the worker's.
TEST( ThreadPoolTest, AResultComesHomeToTheReceiversThread )
{
    CoreApplication app;
    ThreadPool pool( 2 );
    Collector collector;

    pool.submit( &collector,
        []()
        {
            return std::string( "decoded" );
        },
        [&collector]( std::string aValue )
        {
            collector.take( std::move( aValue ) );
        } );

    ASSERT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 1;
        } ) );
    EXPECT_EQ( collector.mValue, "decoded" );
    EXPECT_EQ( collector.mArrivedOn, std::this_thread::get_id() );
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
}

//! Verifies work that returns nothing still calls its continuation.
TEST( ThreadPoolTest, WorkThatReturnsNothingStillCallsBack )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Collector collector;
    std::atomic<int> ran { 0 };

    pool.submit( &collector,
        [&ran]()
        {
            ++ran;
        },
        [&collector]()
        {
            collector.take( "done" );
        } );

    ASSERT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 1;
        } ) );
    EXPECT_EQ( ran.load(), 1 );
    EXPECT_EQ( collector.mValue, "done" );
}

//! Verifies a receiver destroyed before the result arrives drops the continuation quietly.
TEST( ThreadPoolTest, AResultForADeadReceiverIsDropped )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    std::atomic<int> continuations { 0 };

    {
        Collector collector;
        pool.submit( &collector,
            [&]()
            {
                ++started;
                gate.wait();
                return std::string( "late" );
            },
            [&continuations]( std::string )
            {
                ++continuations;
            } );

        ASSERT_TRUE( waitFor( [&started]()
            {
                return started.load() == 1;
            } ) );
    }

    // The receiver is gone; the work finishes and its result has nowhere to go.
    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_FALSE( pumpUntil( [&continuations]()
        {
            return continuations.load() != 0;
        } ) );
    EXPECT_EQ( continuations.load(), 0 );
}

//! Verifies the result is moved home rather than copied.
TEST( ThreadPoolTest, TheResultMovesHome )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Collector collector;
    std::atomic<int> copies { -1 };

    pool.submit( &collector,
        []()
        {
            return CountedValue();
        },
        [&copies, &collector]( CountedValue aValue )
        {
            copies = aValue.mCopies;
            collector.take( "moved" );
        } );

    ASSERT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 1;
        } ) );
    EXPECT_EQ( copies.load(), 0 );
}

//! Verifies a result that cannot be copied at all comes home.
//!
//! This is the case the pool exists for: a decoded image is move-only, and a hand-off that copied
//! its result would not compile for one.
TEST( ThreadPoolTest, AMoveOnlyResultComesHome )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Collector collector;
    std::atomic<int> size { 0 };

    pool.submit( &collector,
        []()
        {
            return std::unique_ptr<std::string>( new std::string( "pixels" ) );
        },
        [&size, &collector]( std::unique_ptr<std::string> aValue )
        {
            size = static_cast<int>( aValue->size() );
            collector.take( *aValue );
        } );

    ASSERT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 1;
        } ) );
    EXPECT_EQ( size.load(), 6 );
    EXPECT_EQ( collector.mValue, "pixels" );
}

//! Verifies the handle of a hand-off follows the work, not the continuation.
TEST( ThreadPoolTest, TheHandleOfAHandOffFollowsTheWork )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Collector collector;

    const TaskHandle handle = pool.submit( &collector,
        []()
        {
            return std::string( "value" );
        },
        [&collector]( std::string aValue )
        {
            collector.take( std::move( aValue ) );
        } );

    ASSERT_TRUE( handle.isValid() );
    ASSERT_TRUE( handle.wait( 5000 ) );
    EXPECT_TRUE( handle.isFinished() );

    // The work is finished, and the continuation has not run: this thread has not yet looked at
    // its own queue, which is where the result is waiting.
    EXPECT_EQ( collector.mCalls.load(), 0 );
    EXPECT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 1;
        } ) );
}

//! Verifies a hand-off leaves nothing behind on a receiver that outlives it.
//!
//! The slot that carries a result captures the hand-off, and the signal it is connected to lives
//! inside that hand-off: without something to break the cycle, every submission would leave a
//! connection on the receiver and a hand-off in memory until the receiver died. Every other test
//! here hides that, because its receiver dies at the end of the test and unlinks the connections
//! on the way out. This one asks the receiver.
TEST( ThreadPoolTest, AHandOffLeavesNothingOnItsReceiver )
{
    CoreApplication app;
    ThreadPool pool( 2 );
    Collector collector;
    ASSERT_EQ( collector.incomingConnectionCount(), 0U );

    for( int index = 0; index < 50; ++index )
    {
        pool.submit( &collector,
            []()
            {
                return std::string( "value" );
            },
            [&collector]( std::string aValue )
            {
                collector.take( std::move( aValue ) );
            } );
    }

    ASSERT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 50;
        } ) );
    EXPECT_TRUE( pool.waitForDone( 5000 ) );

    EXPECT_TRUE( pumpUntil( [&collector]()
        {
            return collector.incomingConnectionCount() == 0U;
        } ) );
    EXPECT_EQ( collector.incomingConnectionCount(), 0U );
}

//! Verifies a hand-off that is cancelled before it runs leaves nothing behind either.
//!
//! The work never runs, so nothing is ever emitted; the connection made when it was submitted has
//! to go all the same.
TEST( ThreadPoolTest, ACancelledHandOffLeavesNothingBehind )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Collector collector;
    Gate gate;
    std::atomic<int> started { 0 };

    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    TaskHandle queued = pool.submit( &collector,
        []()
        {
            return std::string( "never" );
        },
        [&collector]( std::string aValue )
        {
            collector.take( std::move( aValue ) );
        } );
    EXPECT_EQ( collector.incomingConnectionCount(), 1U );
    EXPECT_TRUE( queued.cancel() );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );

    EXPECT_TRUE( waitFor( [&collector]()
        {
            return collector.incomingConnectionCount() == 0U;
        } ) );
    EXPECT_EQ( collector.mCalls.load(), 0 );
}

//! Verifies a hand-off with no receiver is refused rather than run for nothing.
TEST( ThreadPoolTest, AHandOffWithNoReceiverIsRefused )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    std::atomic<int> ran { 0 };

    const TaskHandle handle = pool.submit( static_cast<Object*>( nullptr ),
        [&ran]()
        {
            ++ran;
            return 1;
        },
        []( int )
        {
        } );

    EXPECT_FALSE( handle.isValid() );
    EXPECT_TRUE( pool.waitForDone( 5000 ) );
    EXPECT_EQ( ran.load(), 0 );
}

//! Verifies a hand-off is ranked by priority like any other work.
TEST( ThreadPoolTest, AHandOffTakesAPriority )
{
    CoreApplication app;
    ThreadPool pool( 1 );
    Gate gate;
    std::atomic<int> started { 0 };
    pool.submit( [&]()
        {
            ++started;
            gate.wait();
        } );
    ASSERT_TRUE( waitFor( [&started]()
        {
            return started.load() == 1;
        } ) );

    Collector collector;
    std::mutex orderMutex;
    std::vector<std::string> order;
    const auto record = [&order, &orderMutex]( const std::string& aName )
        {
            std::lock_guard<std::mutex> guard( orderMutex );
            order.push_back( aName );
        };

    pool.submit( &collector, [record]()
        {
            record( "low work" );
            return std::string( "low" );
        }, [&collector]( std::string aValue )
        {
            collector.take( std::move( aValue ) );
        }, -1 );

    pool.submit( &collector, [record]()
        {
            record( "high work" );
            return std::string( "high" );
        }, [&collector]( std::string aValue )
        {
            collector.take( std::move( aValue ) );
        }, 1 );

    gate.open();
    ASSERT_TRUE( pool.waitForDone( 5000 ) );
    ASSERT_TRUE( pumpUntil( [&collector]()
        {
            return collector.mCalls.load() == 2;
        } ) );

    std::lock_guard<std::mutex> guard( orderMutex );
    const std::vector<std::string> expected = { "high work", "low work" };
    EXPECT_EQ( order, expected );
}

//! Verifies a pool destroyed before its workers reach run() joins them before destroying them.
//!
//! The defect. ~ThreadPool cleared its vector of workers and left the joining to ~Thread, which
//! runs after ~Worker and therefore too late. Destroying an object rewrites its vptr -- once as
//! the derived destructor begins and again as the base one does -- while a worker that has
//! started but has not yet reached the virtual call that enters run() is reading that same vptr
//! to dispatch it. The thread could enter the base class's run() rather than the worker's.
//!
//! Any pool built and destroyed without being given work is the shape that shows it, because that
//! is the shortest life a worker can have. ThreadSanitizer detects the race itself, as a "data
//! race on vptr (ctor/dtor vs virtual call)"; without one the two writes and the read usually land
//! in an order that hides it.
//!
//! So the assertion does not depend on the race. ~Worker writes a critical record when its thread
//! is still running, which is true of a worker that was not joined whether or not the race hit
//! this time. The test fails in every build if the join is removed, and a sanitizer build still
//! reports the race as well.
TEST( ThreadPoolTest, APoolDestroyedBeforeItsWorkersStartJoinsThemFirst )
{
    PhraseCountingSink sink( "destroyed while its thread still runs" );

    for( int round = 0; round < 50; ++round )
    {
        ThreadPool pool( 4 );
        ASSERT_EQ( pool.workerCount(), 4 );
    }

    EXPECT_EQ( sink.count(), 0 );
}
