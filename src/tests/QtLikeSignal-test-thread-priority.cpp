//! @file
//!
//! GoogleTest suite for QtLikeSignal::Thread's scheduling-priority setter/getter, start(Priority),
//! and the native-OS-thread creation that backs them.
//!
//! Deliberately parallel to QtMimic's QtMimic-test-thread-priority.cpp -- same tests, same order,
//! same names -- so the two can be diffed against each other. See
//! history/TEST-UNIFICATION-PLAN-20260810.md.

#include "QtLikeSignal-test-types.hpp"

#include "Thread.hpp"
#include "Object.hpp"

#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#if defined( _WIN32 )
    #include <windows.h>
#elif defined( __linux__ )
    #include <pthread.h>
    #include <sched.h>
#endif

namespace
{
    using namespace std::chrono_literals;
    using namespace QtLikeSignal;

    //! A thread that is not running reports InheritPriority.
    TEST( ThreadPriority, ReportsInheritPriorityBeforeStart )
    {
        Thread thread( "prio-before-start" );
        EXPECT_EQ( thread.priority(), Thread::InheritPriority );
    }

    //! setPriority() before start() is refused and leaves the reported priority alone.
    //!
    //! This is Qt's contract and the surprising half of it: the value is not remembered for the
    //! upcoming run, so the thread starts at InheritPriority regardless.
    TEST( ThreadPriority, SetBeforeStartIsRefused )
    {
        Thread thread( "prio-set-before-start" );
        thread.setPriority( Thread::HighPriority );
        EXPECT_EQ( thread.priority(), Thread::InheritPriority );

        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );
        EXPECT_EQ( thread.priority(), Thread::InheritPriority )
            << "a priority set before start() must not leak into the run";

        thread.quit();
        thread.wait();
    }

    //! Every settable priority round-trips through the getter on a running thread.
    TEST( ThreadPriority, SetAndGetOnRunningThread )
    {
        const Thread::Priority priorities[] =
        {
            Thread::IdlePriority,
            Thread::LowestPriority,
            Thread::LowPriority,
            Thread::NormalPriority,
            Thread::HighPriority,
            Thread::HighestPriority,
            Thread::TimeCriticalPriority
        };

        Thread thread( "prio-set-and-get" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        for( const Thread::Priority priority : priorities )
        {
            thread.setPriority( priority );
            EXPECT_EQ( thread.priority(), priority );
        }

        thread.quit();
        thread.wait();
    }

    #if defined( __linux__ ) && defined( SCHED_IDLE )
        //! The POSIX backend must apply priority to the native thread, not only cache the enum.
        TEST( ThreadPriority, IdlePriorityReachesPosixScheduler )
        {
            Thread thread( "prio-posix-idle" );
            thread.start();
            ASSERT_TRUE( waitUntilRunning( thread ) );

            thread.setPriority( Thread::IdlePriority );

            std::promise<int> sampledPolicy;
            auto sampledPolicyFuture = sampledPolicy.get_future();
            ASSERT_TRUE( thread.post( [&sampledPolicy]()
                {
                    int policy = 0;
                    sched_param param {};
                    EXPECT_EQ( pthread_getschedparam( pthread_self(), &policy, &param ), 0 );
                    sampledPolicy.set_value( policy );
                } ) );

            ASSERT_EQ( sampledPolicyFuture.wait_for( 5s ), std::future_status::ready );
            EXPECT_EQ( sampledPolicyFuture.get(), SCHED_IDLE );

            thread.quit();
            thread.wait();
        }
    #endif

    //! InheritPriority is rejected by the setter and does not disturb the current value.
    TEST( ThreadPriority, InheritPriorityIsRejected )
    {
        Thread thread( "prio-inherit-rejected" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        thread.setPriority( Thread::HighPriority );
        ASSERT_EQ( thread.priority(), Thread::HighPriority );

        thread.setPriority( Thread::InheritPriority );
        EXPECT_EQ( thread.priority(), Thread::HighPriority )
            << "a rejected setPriority() must not clear the priority already in effect";

        thread.quit();
        thread.wait();
    }

    //! Once the thread has finished, the priority reverts to InheritPriority.
    TEST( ThreadPriority, ReportsInheritPriorityAfterFinish )
    {
        Thread thread( "prio-after-finish" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );
        thread.setPriority( Thread::HighPriority );
        ASSERT_EQ( thread.priority(), Thread::HighPriority );

        thread.quit();
        thread.wait();

        EXPECT_EQ( thread.priority(), Thread::InheritPriority );
    }

    //! A second run does not inherit the priority set on the first via setPriority().
    TEST( ThreadPriority, RestartResetsToInheritPriority )
    {
        Thread thread( "prio-restart-setter" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );
        thread.setPriority( Thread::HighestPriority );
        ASSERT_EQ( thread.priority(), Thread::HighestPriority );
        thread.quit();
        thread.wait();

        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );
        EXPECT_EQ( thread.priority(), Thread::InheritPriority )
            << "the previous run's priority said nothing about this one";

        thread.quit();
        thread.wait();
    }

    //! start( Priority ) gives the thread its priority without a separate setter call.
    TEST( ThreadPriority, StartWithPriorityIsReported )
    {
        Thread thread( "prio-start-with-priority" );
        thread.start( Thread::HighPriority );
        ASSERT_TRUE( waitUntilRunning( thread ) );

        EXPECT_EQ( thread.priority(), Thread::HighPriority );

        thread.quit();
        thread.wait();
    }

    //! start() with no argument still means InheritPriority, as it always did.
    TEST( ThreadPriority, StartWithoutArgumentInherits )
    {
        Thread thread( "prio-start-no-argument" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        EXPECT_EQ( thread.priority(), Thread::InheritPriority );

        thread.quit();
        thread.wait();
    }

    //! A restart with no argument clears a priority the previous run was started with.
    TEST( ThreadPriority, RestartWithoutPriorityClearsPrevious )
    {
        Thread thread( "prio-restart-start-arg" );
        thread.start( Thread::TimeCriticalPriority );
        ASSERT_TRUE( waitUntilRunning( thread ) );
        ASSERT_EQ( thread.priority(), Thread::TimeCriticalPriority );
        thread.quit();
        thread.wait();

        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );
        EXPECT_EQ( thread.priority(), Thread::InheritPriority );

        thread.quit();
        thread.wait();
    }

    //! The priority is in effect before the loop's started signal fires, not merely by the time
    //! start() returns.
    //!
    //! mStarted is emitted from inside loop(), which run() only reaches after its priority
    //! fix-up step, so sampling from a connectStarted() callback observes the same ordering
    //! guarantee QtLikeSignal's tests observe by overriding a virtual run() -- QtMimic has no
    //! such override point, since a Thread's work is defined by posted tasks rather than a
    //! subclassed run(). Checking after the fact (e.g. from a posted task) would pass even if the
    //! priority arrived late, because posted tasks only run after mStarted has already fired.
    TEST( ThreadPriority, PriorityIsInEffectBeforeStartedSignal )
    {
        Thread thread( "prio-before-started-signal" );
        Thread localThread;
        Object local( &localThread );
        std::mutex mutex;
        std::condition_variable cv;
        bool sampled = false;
        Thread::Priority sampledPriority = Thread::InheritPriority;

        localThread.start();

        // Direct, and not by preference: it is what makes this test test anything. Auto would
        // resolve to Queued here (local lives on localThread, started is emitted on thread), so the
        // sample would be taken by localThread's loop some time after mStarted had already fired --
        // the "checking after the fact" the comment above rules out. It would still pass with a
        // late-arriving priority.
        auto connection = Object::connect( thread.getStarted(), &local, [&]()
            {
                std::lock_guard<std::mutex> locker( mutex );
                sampledPriority = thread.priority();
                sampled = true;
                cv.notify_all();
            }, ConnectionType::Direct );

        thread.start( Thread::HighestPriority );

        {
            std::unique_lock<std::mutex> locker( mutex );
            ASSERT_TRUE( cv.wait_for( locker, 5s, [&]
                {
                    return sampled;
                } ) ) << "started signal never fired";
        }
        EXPECT_EQ( sampledPriority, Thread::HighestPriority )
            << "the loop started before the requested priority was in effect";

        thread.quit();
        thread.wait();
        localThread.quit();
        localThread.wait();
    }

    //! setPriority() racing the thread's own exit must not crash or touch a dead handle.
    //!
    //! The interesting window is between the loop finishing and the OS thread being joined. This
    //! hammers setPriority() from another thread while the target quits underneath it, which is
    //! the case mPriorityMutex exists to make safe. Under a sanitizer build this is the test that
    //! would catch a use-after-exit on the native handle.
    TEST( ThreadPriority, SetPriorityRacingThreadExitIsSafe )
    {
        for( int round = 0; round < 20; ++round )
        {
            Thread thread( "prio-race-exit" );
            thread.start();
            ASSERT_TRUE( waitUntilRunning( thread ) );

            std::atomic<bool> stop { false };
            std::thread hammer(
                [&thread, &stop]()
                {
                    while( !stop.load() )
                    {
                        thread.setPriority( Thread::HighPriority );
                        thread.priority();
                    }
                } );

            thread.quit();
            thread.wait();

            stop.store( true );
            hammer.join();

            EXPECT_EQ( thread.priority(), Thread::InheritPriority );
        }
    }

    //! Concurrent setters from many threads leave a coherent value behind.
    TEST( ThreadPriority, ConcurrentSettersAreSerialised )
    {
        Thread thread( "prio-concurrent-setters" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        std::vector<std::thread> setters;
        for( int i = 0; i < 8; ++i )
        {
            setters.emplace_back(
                [&thread]()
                {
                    for( int n = 0; n < 200; ++n )
                    {
                        thread.setPriority( Thread::LowPriority );
                        thread.setPriority( Thread::HighPriority );
                    }
                } );
        }
        for( std::thread& setter : setters )
        {
            setter.join();
        }

        const Thread::Priority finalPriority = thread.priority();
        EXPECT_TRUE( finalPriority == Thread::LowPriority || finalPriority == Thread::HighPriority )
        ;

        thread.quit();
        thread.wait();
    }

    #if defined( _WIN32 )

        //! On Windows the request actually reaches the OS thread.
        //!
        //! Windows honours all seven levels, so the mapping can be checked for real rather than
        //! only through our own getter. There is no equivalent assertion on Linux: the default
        //! SCHED_OTHER policy reports a single-value priority range, so every level maps to the
        //! same number and nothing observable changes.
        TEST( ThreadPriority, WindowsAppliesPriorityToOsThread )
        {
            struct Expectation
            {
                Thread::Priority mPriority;
                int mNativePriority;
            };

            const Expectation expectations[] =
            {
                { Thread::IdlePriority, THREAD_PRIORITY_IDLE },
                { Thread::LowestPriority, THREAD_PRIORITY_LOWEST },
                { Thread::LowPriority, THREAD_PRIORITY_BELOW_NORMAL },
                { Thread::NormalPriority, THREAD_PRIORITY_NORMAL },
                { Thread::HighPriority, THREAD_PRIORITY_ABOVE_NORMAL },
                { Thread::HighestPriority, THREAD_PRIORITY_HIGHEST },
                { Thread::TimeCriticalPriority, THREAD_PRIORITY_TIME_CRITICAL }
            };

            Thread thread( "prio-win-applies" );
            thread.start();
            ASSERT_TRUE( waitUntilRunning( thread ) );

            for( const Expectation& expectation : expectations )
            {
                thread.setPriority( expectation.mPriority );
                ASSERT_EQ( thread.priority(), expectation.mPriority );

                // Ask the thread what the OS thinks its own priority is. Reading it from here
                // would need the native handle, which is private; posting the query onto the
                // thread itself gets the answer from the only place GetCurrentThread() means the
                // right thread.
                std::promise<int> reported;
                std::future<int> answer = reported.get_future();
                ASSERT_TRUE( thread.post(
                    [&reported]()
                    {
                        reported.set_value( GetThreadPriority( GetCurrentThread() ) );
                    } ) );
                ASSERT_EQ( answer.wait_for( 5s ), std::future_status::ready )
                    << "the thread never ran the posted query";

                EXPECT_EQ( answer.get(), expectation.mNativePriority )
                    << "setPriority() did not reach the OS thread";
            }

            thread.quit();
            thread.wait();
        }

        //! A priority passed to start() reaches the OS thread, not just our own bookkeeping.
        TEST( ThreadPriority, WindowsStartWithPriorityAppliesToOsThread )
        {
            Thread thread( "prio-win-start-applies" );
            thread.start( Thread::HighestPriority );
            ASSERT_TRUE( waitUntilRunning( thread ) );
            ASSERT_EQ( thread.priority(), Thread::HighestPriority );

            std::promise<int> reported;
            std::future<int> answer = reported.get_future();
            ASSERT_TRUE( thread.post(
                [&reported]()
                {
                    reported.set_value( GetThreadPriority( GetCurrentThread() ) );
                } ) );
            ASSERT_EQ( answer.wait_for( 5s ), std::future_status::ready );

            EXPECT_EQ( answer.get(), THREAD_PRIORITY_HIGHEST )
                << "start( Priority ) did not reach the OS thread";

            thread.quit();
            thread.wait();
        }

        //! The priority is already in force by the time the loop's started signal fires, on the
        //! real OS thread -- not merely by our own bookkeeping (PriorityIsInEffectBeforeStartedSignal
        //! above already covers that half).
        //!
        //! This is what creating the thread suspended buys, and it is the reason start() uses the
        //! OS thread API directly rather than std::thread: a thread born already running would be
        //! at NormalPriority for the window between creation and the priority landing.
        TEST( ThreadPriority, WindowsPriorityIsInForceBeforeStartedSignal )
        {
            Thread thread( "prio-win-before-started" );
            Thread localThread;
            Object local( &localThread );
            std::mutex mutex;
            std::condition_variable cv;
            bool sampled = false;
            int sampledNativePriority = THREAD_PRIORITY_ERROR_RETURN;

            localThread.start();

            // Direct is mandatory here: GetCurrentThread() samples whichever thread runs the slot,
            // so the delivery thread *is* the measurement. Under Auto this resolves to Queued and
            // the sample comes from localThread, which sits at NormalPriority -- a wrong answer
            // rather than a timeout.
            auto connection = Object::connect( thread.getStarted(), &local, [&]()
                {
                    std::lock_guard<std::mutex> locker( mutex );
                    sampledNativePriority = GetThreadPriority( GetCurrentThread() );
                    sampled = true;
                    cv.notify_all();
                }, ConnectionType::Direct );

            thread.start( Thread::HighestPriority );

            {
                std::unique_lock<std::mutex> locker( mutex );
                ASSERT_TRUE( cv.wait_for( locker, 5s, [&]
                    {
                        return sampled;
                    } ) ) << "started signal never fired";
            }
            EXPECT_EQ( sampledNativePriority, THREAD_PRIORITY_HIGHEST )
                << "the loop started before start()'s priority reached the OS thread";

            thread.quit();
            thread.wait();
            localThread.quit();
            localThread.wait();
        }

        //! InheritPriority means the creating thread's priority, and has to be made to happen.
        //!
        //! The OS hands every new thread NormalPriority regardless of who created it, so
        //! inheritance is not the default it sounds like: start() has to read the creating
        //! thread's priority and apply it. Qt does this too. Raising this test's own thread first
        //! is what makes the difference between inheriting and defaulting visible at all.
        TEST( ThreadPriority, WindowsInheritPriorityFollowsTheCreatingThread )
        {
            const HANDLE self = GetCurrentThread();
            const int originalPriority = GetThreadPriority( self );
            ASSERT_NE( originalPriority, THREAD_PRIORITY_ERROR_RETURN );
            ASSERT_TRUE( SetThreadPriority( self, THREAD_PRIORITY_HIGHEST ) );

            Thread thread( "prio-win-inherit" );

            std::mutex mutex;
            std::condition_variable cv;
            bool sampled = false;
            int sampledNativePriority = THREAD_PRIORITY_ERROR_RETURN;

            Thread localThread;
            Object local( &localThread );
            localThread.start();

            // Direct is mandatory here: GetCurrentThread() samples whichever thread runs the slot,
            // so the delivery thread *is* the measurement. Under Auto this resolves to Queued and
            // the sample comes from localThread, which sits at NormalPriority -- a wrong answer
            // rather than a timeout.
            auto connection = Object::connect( thread.getStarted(), &local, [&]()
                {
                    std::lock_guard<std::mutex> locker( mutex );
                    sampledNativePriority = GetThreadPriority( GetCurrentThread() );
                    sampled = true;
                    cv.notify_all();
                }, ConnectionType::Direct );

            thread.start();

            bool started;
            {
                std::unique_lock<std::mutex> locker( mutex );
                started = cv.wait_for( locker, 5s, [&]
                    {
                        return sampled;
                    } );
            }

            // Put this thread back before asserting: a failed assertion returns from the test,
            // and leaving the whole gtest main thread at HIGHEST would follow into every test
            // after it.
            SetThreadPriority( self, originalPriority );

            ASSERT_TRUE( started );
            EXPECT_EQ( sampledNativePriority, THREAD_PRIORITY_HIGHEST )
                << "the new thread did not inherit the creating thread's priority";
            EXPECT_EQ( thread.priority(), Thread::InheritPriority )
                << "an inherited priority is still reported as InheritPriority";

            thread.quit();
            thread.wait();
            localThread.quit();
            localThread.wait();
        }

    #endif // _WIN32

} // namespace
