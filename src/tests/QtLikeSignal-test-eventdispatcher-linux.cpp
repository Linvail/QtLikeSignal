// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Tests for the Linux event dispatcher's platform-event-source integration -- mission stage 5,
//! "I want to receive OS/platform's messages. 100% cpu-spin is not allowed."
//!
//! The whole file is Linux-only; it compiles to nothing elsewhere so the cross-build stays clean.

#if defined( __linux__ )

#include <gtest/gtest.h>
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Object.hpp"
#include "TestCpuTime.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <poll.h>
#include <unistd.h>

using namespace QtLikeSignal;

namespace
{
    //! A self-closing pipe, standing in for a platform event source such as a Wayland/X11 FD.
    class TestPipe
    {
    public:
        TestPipe()
        {
            if( ::pipe( mFds ) != 0 )
            {
                mFds[0] = -1;
                mFds[1] = -1;
            }
        }

        ~TestPipe()
        {
            closeRead();
            closeWrite();
        }

        TestPipe
            (
            const TestPipe&
            ) = delete;

        TestPipe& operator=
            (
            const TestPipe&
            ) = delete;

        //! Read end, to be registered with the dispatcher.
        int readFd() const
        {
            return mFds[0];
        }

        //! Makes the read end ready, as an OS event source would.
        void signal()
        {
            const char byte = 'x';
            const ssize_t written = ::write( mFds[1], &byte, 1 );
            ( void )written;
        }

        //! Consumes one byte, so the descriptor stops being ready.
        void drain()
        {
            char byte = 0;
            const ssize_t got = ::read( mFds[0], &byte, 1 );
            ( void )got;
        }

        //! Closes the read end.
        void closeRead()
        {
            if( mFds[0] >= 0 )
            {
                ::close( mFds[0] );
                mFds[0] = -1;
            }
        }

        //! Closes the write end.
        void closeWrite()
        {
            if( mFds[1] >= 0 )
            {
                ::close( mFds[1] );
                mFds[1] = -1;
            }
        }

    private:
        int mFds[2] { -1, -1 };
    };

    //! Gets the running thread's dispatcher as an EventDispatcherLinux, or null if it is not one.
    std::shared_ptr<EventDispatcherLinux> currentLinuxDispatcher()
    {
        Thread* current = Thread::currentThread();
        if( !current )
        {
            return nullptr;
        }
        return std::dynamic_pointer_cast<EventDispatcherLinux>( current->eventDispatcher() );
    }
}

//! Verifies a registered descriptor wakes the blocked loop and its callback runs on that thread.
//!
//! This is the whole point of the platform dispatcher: the loop is asleep in poll() with no
//! timeout, and an OS descriptor becoming readable -- signalled here from another thread, as the
//! compositor would -- has to bring it back and deliver the callback.
TEST( EventDispatcherLinuxTest, RegisteredDescriptorWakesTheLoopAndInvokesItsCallback )
{
    CoreApplication app;
    auto dispatcher = currentLinuxDispatcher();
    ASSERT_NE( dispatcher, nullptr ) << "the main thread should be running EventDispatcherLinux";

    TestPipe pipe;
    ASSERT_GE( pipe.readFd(), 0 );

    std::atomic<int> callbackCount { 0 };
    std::atomic<short> seenEvents { 0 };
    Thread* mainThread = Thread::currentThread();
    std::atomic<Thread*> ranOn { nullptr };

    ASSERT_TRUE( dispatcher->registerEventSource( pipe.readFd(), POLLIN,
        [&]( short aEvents )
        {
            seenEvents.store( aEvents );
            ranOn.store( Thread::currentThread() );
            pipe.drain();
            callbackCount.fetch_add( 1 );
            CoreApplication::quit();
        } ) );

    // Nothing is scheduled, so the loop will block indefinitely in poll() until this fires.
    std::thread signaller( [&pipe]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            pipe.signal();
        } );

    EXPECT_EQ( app.exec(), 0 );
    signaller.join();

    EXPECT_EQ( callbackCount.load(), 1 );
    EXPECT_TRUE( seenEvents.load() & POLLIN );
    EXPECT_EQ( ranOn.load(), mainThread )
        << "a platform callback must run on the dispatcher's own thread.";

    EXPECT_TRUE( dispatcher->unregisterEventSource( pipe.readFd() ) );
}

//! Verifies an unregistered descriptor stops producing callbacks.
TEST( EventDispatcherLinuxTest, UnregisteredDescriptorIsNoLongerPolled )
{
    CoreApplication app;
    auto dispatcher = currentLinuxDispatcher();
    ASSERT_NE( dispatcher, nullptr );

    TestPipe pipe;
    ASSERT_GE( pipe.readFd(), 0 );

    std::atomic<int> callbackCount { 0 };
    ASSERT_TRUE( dispatcher->registerEventSource( pipe.readFd(), POLLIN,
        [&callbackCount]( short )
        {
            callbackCount.fetch_add( 1 );
        } ) );

    EXPECT_TRUE( dispatcher->unregisterEventSource( pipe.readFd() ) );
    EXPECT_FALSE( dispatcher->unregisterEventSource( pipe.readFd() ) )
        << "unregistering twice should report that there was nothing to remove.";

    // Make the descriptor permanently ready. If it were still polled, the callback would fire and
    // -- worse -- poll() would return immediately forever, spinning the loop.
    pipe.signal();

    Timer stopper;
    stopper.setSingleShot( true );
    Object::connect( stopper.getTimeout(), &stopper, []()
        {
            CoreApplication::quit();
        }, ConnectionType::Direct );
    stopper.start( 60 );

    EXPECT_EQ( app.exec(), 0 );
    EXPECT_EQ( callbackCount.load(), 0 );
}

//! Verifies registering a source while the loop is already blocked takes effect immediately.
//!
//! The descriptor set is snapshotted at the top of each wait, so a loop already asleep in poll() is
//! watching a stale set. registerEventSource() has to wake it to rebuild; without that the new
//! source would go unnoticed until some unrelated event happened to end the wait.
TEST( EventDispatcherLinuxTest, RegisteringWhileBlockedTakesEffectWithoutOtherActivity )
{
    CoreApplication app;
    auto dispatcher = currentLinuxDispatcher();
    ASSERT_NE( dispatcher, nullptr );

    TestPipe pipe;
    ASSERT_GE( pipe.readFd(), 0 );
    pipe.signal();   // already readable before it is ever registered

    std::atomic<bool> fired { false };

    // Register from another thread once the main loop is definitely blocked in poll().
    std::thread late( [&]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            dispatcher->registerEventSource( pipe.readFd(), POLLIN,
            [&]( short )
            {
                pipe.drain();
                fired.store( true );
                CoreApplication::quit();
            } );
        } );

    // A watchdog bounds the test: if the late registration were missed the loop would block forever
    // rather than failing.
    std::thread watchdog( [&fired]()
        {
            for( int i = 0; i < 300 && !fired.load(); ++i )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
            }
            CoreApplication::quit();
        } );

    EXPECT_EQ( app.exec(), 0 );
    late.join();
    watchdog.join();

    EXPECT_TRUE( fired.load() )
        << "a source registered while the loop was blocked in poll() was not picked up until "
        "something else woke the loop.";

    dispatcher->unregisterEventSource( pipe.readFd() );
}

//! Verifies an idle loop consumes essentially no CPU -- mission stage 5's "no 100% cpu-spin".
//!
//! With no timers and no ready descriptors, poll() must block with no timeout. Measured as process
//! CPU time against wall time; see TestCpuTime.hpp for why that is not std::clock().
TEST( EventDispatcherLinuxTest, IdleLoopDoesNotSpin )
{
    CoreApplication app;
    auto dispatcher = currentLinuxDispatcher();
    ASSERT_NE( dispatcher, nullptr );

    // A registered-but-never-ready descriptor, so the poll set is non-trivial while still idle.
    TestPipe pipe;
    ASSERT_GE( pipe.readFd(), 0 );
    ASSERT_TRUE( dispatcher->registerEventSource( pipe.readFd(), POLLIN, []( short )
        {
        } ) );

    constexpr int kRunMs = 300;
    std::thread watchdog( [kRunMs]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( kRunMs ) );
            CoreApplication::quit();
        } );

    const double cpuBefore = TestSupport::processCpuSeconds();
    const auto wallBefore = std::chrono::steady_clock::now();

    EXPECT_EQ( app.exec(), 0 );

    const double cpuSeconds = TestSupport::processCpuSeconds() - cpuBefore;
    const double wallSeconds
        = std::chrono::duration<double>( std::chrono::steady_clock::now() - wallBefore ).count();
    watchdog.join();

    ASSERT_GT( wallSeconds, 0.1 ) << "the loop returned far too early to have blocked at all";
    EXPECT_LT( cpuSeconds / wallSeconds, 0.1 )
        << "an idle loop burned " << cpuSeconds << "s of CPU over " << wallSeconds
        << "s of wall time (ratio " << ( cpuSeconds / wallSeconds )
        << "); poll() is returning immediately instead of blocking.";

    dispatcher->unregisterEventSource( pipe.readFd() );
}

//! Verifies a source unregistered from inside a callback is not itself called in that same round.
//!
//! Both descriptors are ready before the loop wakes, so one poll() reports both and the dispatcher
//! is holding the readiness of the second when the first callback runs. Unregistering there has to
//! take effect immediately: the caller is entitled to close the descriptor, or to destroy whatever
//! the callback captured, the moment unregisterEventSource() returns.
//!
//! Copying the callbacks out with the descriptor set -- which is what the loop used to do -- makes
//! that impossible, because the copy outlives the registration it came from. The set is still
//! snapshotted, but each callback is now looked up again, under the lock, immediately before it is
//! invoked.
TEST( EventDispatcherLinuxTest, SourceUnregisteredFromACallbackIsNotCalledInThatRound )
{
    CoreApplication app;
    auto dispatcher = currentLinuxDispatcher();
    ASSERT_NE( dispatcher, nullptr ) << "the main thread should be running EventDispatcherLinux";

    TestPipe first;
    TestPipe second;
    ASSERT_GE( first.readFd(), 0 );
    ASSERT_GE( second.readFd(), 0 );

    std::atomic<int> firstCalls { 0 };
    std::atomic<int> secondCalls { 0 };

    // Registration order is the order the loop invokes ready callbacks in, so `first` is the one
    // that gets to unregister the other before the other has been reached.
    ASSERT_TRUE( dispatcher->registerEventSource( first.readFd(), POLLIN,
        [&]( short )
        {
            first.drain();
            firstCalls.fetch_add( 1 );

            // The sibling is ready in this very poll() round, and its callback has not run yet.
            dispatcher->unregisterEventSource( second.readFd() );
            CoreApplication::quit();
        } ) );

    ASSERT_TRUE( dispatcher->registerEventSource( second.readFd(), POLLIN,
        [&]( short )
        {
            second.drain();
            secondCalls.fetch_add( 1 );
        } ) );

    // Both ready before the loop looks, so one poll() returns both.
    second.signal();
    first.signal();

    EXPECT_EQ( app.exec(), 0 );

    EXPECT_EQ( firstCalls.load(), 1 );
    EXPECT_EQ( secondCalls.load(), 0 )
        << "the second source was unregistered from the first source's callback, but its own "
        "callback still ran in the same round -- the loop was holding a copy taken before the "
        "unregistration, so unregisterEventSource() did not take effect until the next round.";

    dispatcher->unregisterEventSource( first.readFd() );
}

#endif // __linux__
