#include <gtest/gtest.h>
#include "Thread.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#if defined( _WIN32 )
    #include <windows.h>
#endif

using namespace QtLikeSignal;

//! A thread that stays in its event loop until told to quit.
//!
//! Priority can only be set on a running thread, so every test here needs one that is reliably
//! alive while the call is made. Using the default run()/exec() gives that without a sleep-based
//! guess: the thread sits in the loop until quit() lands.
class PriorityTestThread : public Thread
{
public:
    //! Blocks until the thread's event loop is up and able to accept work. Returns true if the
    //! thread is running and has a dispatcher within the timeout.
    //!
    //! Deliberately stronger than isRunning() alone. start() sets the running flag on the CALLING
    //! thread before the new thread has executed anything, so isRunning() can be true while the
    //! worker has not yet created its dispatcher -- and post() rejects work until it has. Waiting
    //! on the dispatcher as well is what makes that window impossible to land in.
    bool waitUntilRunning
        (
        int aTimeoutMs = 5000  //!< How long to wait before giving up.
        )
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds( aTimeoutMs );
        while( std::chrono::steady_clock::now() < deadline )
        {
            if( isRunning() && eventDispatcher() )
            {
                return true;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return isRunning() && eventDispatcher() != nullptr;
    }

};

//! A thread that is not running reports InheritPriority.
TEST( ThreadPriority, ReportsInheritPriorityBeforeStart )
{
    PriorityTestThread thread;
    EXPECT_EQ( thread.priority(), Thread::InheritPriority );
}

//! setPriority() before start() is refused and leaves the reported priority alone.
//!
//! This is Qt's contract and the surprising half of it: the value is not remembered for the
//! upcoming run, so the thread starts at InheritPriority regardless.
TEST( ThreadPriority, SetBeforeStartIsRefused )
{
    PriorityTestThread thread;
    thread.setPriority( Thread::HighPriority );
    EXPECT_EQ( thread.priority(), Thread::InheritPriority );

    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );
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

    PriorityTestThread thread;
    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );

    for( const Thread::Priority priority : priorities )
    {
        thread.setPriority( priority );
        EXPECT_EQ( thread.priority(), priority );
    }

    thread.quit();
    thread.wait();
}

//! InheritPriority is rejected by the setter and does not disturb the current value.
TEST( ThreadPriority, InheritPriorityIsRejected )
{
    PriorityTestThread thread;
    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );

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
    PriorityTestThread thread;
    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );
    thread.setPriority( Thread::HighPriority );
    ASSERT_EQ( thread.priority(), Thread::HighPriority );

    thread.quit();
    ASSERT_TRUE( thread.wait() );

    EXPECT_EQ( thread.priority(), Thread::InheritPriority );
}

//! A second run does not inherit the priority set on the first.
TEST( ThreadPriority, RestartResetsToInheritPriority )
{
    PriorityTestThread thread;
    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );
    thread.setPriority( Thread::HighestPriority );
    ASSERT_EQ( thread.priority(), Thread::HighestPriority );
    thread.quit();
    ASSERT_TRUE( thread.wait() );

    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );
    EXPECT_EQ( thread.priority(), Thread::InheritPriority )
        << "the previous run's priority said nothing about this one";

    thread.quit();
    thread.wait();
}

//! start( Priority ) gives the thread its priority without a separate setter call.
TEST( ThreadPriority, StartWithPriorityIsReported )
{
    PriorityTestThread thread;
    thread.start( Thread::HighPriority );
    ASSERT_TRUE( thread.waitUntilRunning() );

    EXPECT_EQ( thread.priority(), Thread::HighPriority );

    thread.quit();
    thread.wait();
}

//! start() with no argument still means InheritPriority, as it always did.
TEST( ThreadPriority, StartWithoutArgumentInherits )
{
    PriorityTestThread thread;
    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );

    EXPECT_EQ( thread.priority(), Thread::InheritPriority );

    thread.quit();
    thread.wait();
}

//! The priority is in effect before run() begins, not merely by the time start() returns.
//!
//! This is the point of applying it from inside the new thread. Overriding run() to sample the
//! priority as its first statement is the only way to observe the ordering the documentation
//! promises; checking after the fact would pass even if the priority arrived late.
TEST( ThreadPriority, PriorityIsInEffectBeforeRunBegins )
{
    class SamplingThread : public Thread
    {
    public:
        std::atomic<int> mSampled { -1 };

    protected:
        virtual void run() override
        {
            mSampled.store( static_cast<int>( priority() ) );
            Thread::run();
        }

    };

    SamplingThread thread;
    thread.start( Thread::HighestPriority );
    ASSERT_TRUE( thread.isRunning() || thread.isFinished() );

    // Give run() a chance to record the value, then stop the loop.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( thread.mSampled.load() == -1 && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    EXPECT_EQ( thread.mSampled.load(), static_cast<int>( Thread::HighestPriority ) )
        << "run() started before the requested priority was in effect";

    thread.quit();
    thread.wait();
}

//! A restart with no argument clears a priority the previous run was started with.
TEST( ThreadPriority, RestartWithoutPriorityClearsPrevious )
{
    PriorityTestThread thread;
    thread.start( Thread::TimeCriticalPriority );
    ASSERT_TRUE( thread.waitUntilRunning() );
    ASSERT_EQ( thread.priority(), Thread::TimeCriticalPriority );
    thread.quit();
    ASSERT_TRUE( thread.wait() );

    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );
    EXPECT_EQ( thread.priority(), Thread::InheritPriority );

    thread.quit();
    thread.wait();
}

//! setPriority() racing the thread's own exit must not crash or touch a dead handle.
//!
//! The interesting window is between the run body finishing and the OS thread being joined. This
//! hammers setPriority() from another thread while the target quits underneath it, which is the
//! case mPriorityMutex exists to make safe. Under a sanitizer build this is the test that would
//! catch a use-after-exit on the native handle.
TEST( ThreadPriority, SetPriorityRacingThreadExitIsSafe )
{
    for( int round = 0; round < 20; ++round )
    {
        PriorityTestThread thread;
        thread.start();
        ASSERT_TRUE( thread.waitUntilRunning() );

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
    PriorityTestThread thread;
    thread.start();
    ASSERT_TRUE( thread.waitUntilRunning() );

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
    EXPECT_TRUE( finalPriority == Thread::LowPriority || finalPriority == Thread::HighPriority );

    thread.quit();
    thread.wait();
}

#if defined( _WIN32 )

    //! On Windows the request actually reaches the OS thread.
    //!
    //! Windows honours all seven levels, so the mapping can be checked for real rather than only
    //! through our own getter. There is no equivalent assertion on Linux: the default SCHED_OTHER
    //! policy reports a single-value priority range, so every level maps to the same number and
    //! nothing observable changes.
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

        PriorityTestThread thread;
        thread.start();
        ASSERT_TRUE( thread.waitUntilRunning() );

        for( const Expectation& expectation : expectations )
        {
            thread.setPriority( expectation.mPriority );
            ASSERT_EQ( thread.priority(), expectation.mPriority );

            // Ask the thread what the OS thinks its own priority is. Reading it from here would need
            // the native handle, which is private; posting the query onto the thread itself gets the
            // answer from the only place GetCurrentThread() means the right thread.
            std::promise<int> reported;
            std::future<int> answer = reported.get_future();
            ASSERT_TRUE( thread.post(
                [&reported]()
                {
                    reported.set_value( GetThreadPriority( GetCurrentThread() ) );
                } ) );
            ASSERT_EQ( answer.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
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
        PriorityTestThread thread;
        thread.start( Thread::HighestPriority );
        ASSERT_TRUE( thread.waitUntilRunning() );
        ASSERT_EQ( thread.priority(), Thread::HighestPriority );

        std::promise<int> reported;
        std::future<int> answer = reported.get_future();
        ASSERT_TRUE( thread.post(
            [&reported]()
            {
                reported.set_value( GetThreadPriority( GetCurrentThread() ) );
            } ) );
        ASSERT_EQ( answer.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

        EXPECT_EQ( answer.get(), THREAD_PRIORITY_HIGHEST )
            << "start( Priority ) did not reach the OS thread";

        thread.quit();
        thread.wait();
    }

    //! A thread that records what the OS says its priority is the instant run() is entered, then
    //! returns.
    class PriorityAtRunEntryThread : public Thread
    {
    public:
        std::atomic<int> mPriorityAtRunEntry { THREAD_PRIORITY_ERROR_RETURN };

    protected:
        //! Samples the OS priority and returns immediately, without entering an event loop.
        virtual void run() override
        {
            mPriorityAtRunEntry.store( GetThreadPriority( GetCurrentThread() ) );
        }

    };

    //! The priority is already in force when run() begins, not applied from inside it.
    //!
    //! This is what creating the thread suspended buys, and it is the reason start() uses the OS
    //! thread API directly. Sampling from inside run() is the earliest point this test can reach;
    //! the sequence that would fail it -- thread created running at normal priority, priority
    //! applied afterwards -- is exactly what a std::thread-backed start() is forced to do.
    TEST( ThreadPriority, WindowsPriorityIsInForceBeforeRunBegins )
    {
        PriorityAtRunEntryThread thread;
        thread.start( Thread::HighestPriority );

        ASSERT_TRUE( thread.wait( 5000 ) );
        EXPECT_EQ( thread.mPriorityAtRunEntry.load(), THREAD_PRIORITY_HIGHEST )
            << "run() began before start()'s priority reached the OS thread";
    }

    //! InheritPriority means the creating thread's priority, and has to be made to happen.
    //!
    //! The OS hands every new thread NormalPriority regardless of who created it, so inheritance
    //! is not the default it sounds like: start() has to read the creating thread's priority and
    //! apply it. Qt does this too. Raising this test's own thread first is what makes the
    //! difference between inheriting and defaulting visible at all.
    TEST( ThreadPriority, WindowsInheritPriorityFollowsTheCreatingThread )
    {
        const HANDLE self = GetCurrentThread();
        const int originalPriority = GetThreadPriority( self );
        ASSERT_NE( originalPriority, THREAD_PRIORITY_ERROR_RETURN );
        ASSERT_TRUE( SetThreadPriority( self, THREAD_PRIORITY_HIGHEST ) );

        PriorityAtRunEntryThread thread;
        thread.start();
        const bool finished = thread.wait( 5000 );
        const int reported = thread.mPriorityAtRunEntry.load();

        // Put this thread back before asserting: a failed assertion returns from the test, and
        // leaving the whole gtest main thread at HIGHEST would follow into every test after it.
        SetThreadPriority( self, originalPriority );

        ASSERT_TRUE( finished );
        EXPECT_EQ( reported, THREAD_PRIORITY_HIGHEST )
            << "the new thread did not inherit the creating thread's priority";
        EXPECT_EQ( thread.priority(), Thread::InheritPriority )
            << "an inherited priority is still reported as InheritPriority";
    }

#endif // _WIN32
