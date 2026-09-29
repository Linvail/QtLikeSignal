// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for CallStack. It reads the stack of the calling thread, and the stack of a
//! different thread: a busy thread, a thread blocked in the kernel, and the reader itself. It also
//! checks what comes back when there is nothing to read.

#include "gtest/gtest.h"
#include "QtLikeSignalDebug/CallStack.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>

#if defined( __linux__ )
    #include <signal.h>
#endif

//! Defined when a capture of a different thread cannot work as three tests expect: on Linux,
//! under ThreadSanitizer.
//!
//! ThreadSanitizer installs its own handler for each signal, and runs the real handler later, when
//! the thread next goes into the runtime of the sanitizer. Then the stack of a busy thread has
//! frames of that runtime above the frames of the thread. A thread that is blocked in a system call
//! that the runtime does not intercept, such as the futex wait of std::future, does not answer.
//! Clang tells about the sanitizer through __has_feature, and GCC through a predefined macro; the
//! lines below check both.
#if defined( __linux__ )
    #if defined( __has_feature )
        #if __has_feature( thread_sanitizer )
            #define QT_LIKE_SIGNAL_SIGNALS_DEFERRED 1
        #endif
    #endif
    #if defined( __SANITIZE_THREAD__ ) && !defined( QT_LIKE_SIGNAL_SIGNALS_DEFERRED )
        #define QT_LIKE_SIGNAL_SIGNALS_DEFERRED 1
    #endif
#endif

using QtLikeSignal::CallStack;

// The tests look for each probe below in a stack, so each probe must keep a frame of its own. It
// must not be inlined, and it must not end in a tail call that gives its frame to the callee.
#if defined( _MSC_VER )
    #define PROBE_NOINLINE __declspec( noinline )
#else
    #define PROBE_NOINLINE __attribute__( ( noinline ) )
#endif

//! Functions with names that the tests can find in a stack.
//!
//! A named namespace, not an anonymous one, and no static functions. Linux names a frame only from
//! the dynamic symbol table, and that table never holds a function with internal linkage.
namespace CallStackProbe
{
    //! Each probe increments this after its real work, so that the work is never its last step.
    std::atomic<int> gAfter { 0 };

    //! Captures the stack of the calling thread from inside a frame that the test can name.
    PROBE_NOINLINE void captureHere
        (
        CallStack& aOut  //!< Gets the stack.
        )
    {
        aOut = CallStack::capture();
        gAfter.fetch_add( 1 );
    }

    //! Captures @p aTarget from inside a frame that the test can name.
    PROBE_NOINLINE void captureTargetHere
        (
        const CallStack::Target& aTarget,  //!< The thread to read.
        CallStack& aOut                    //!< Gets the stack.
        )
    {
        aOut = CallStack::capture( aTarget );
        gAfter.fetch_add( 1 );
    }

    //! Spins until @p aRelease is true: busy, as a stuck handler usually is.
    PROBE_NOINLINE void spinUntilReleased
        (
        std::atomic<bool>& aSpinning,       //!< Set to true when the spin starts.
        const std::atomic<bool>& aRelease   //!< The test sets it to true to stop the spin.
        )
    {
        aSpinning.store( true );
        while( !aRelease.load() )
        {
        }
        gAfter.fetch_add( 1 );
    }

    //! Reads the clock until @p aRelease is true: busy, and mostly inside the code of the system,
    //! which usually has no symbols.
    PROBE_NOINLINE void readTheClockUntilReleased
        (
        std::atomic<bool>& aSpinning,       //!< Set to true when the loop starts.
        const std::atomic<bool>& aRelease   //!< The test sets it to true to stop the loop.
        )
    {
        aSpinning.store( true );
        while( !aRelease.load() )
        {
            ( void )std::chrono::steady_clock::now();
        }
        gAfter.fetch_add( 1 );
    }

    //! Runs @p aBody: a frame that the tests can find at the bottom of the stack of a probe
    //! thread.
    PROBE_NOINLINE void runBody
        (
        const std::function<void()>& aBody  //!< What the probe thread runs.
        )
    {
        aBody();
        gAfter.fetch_add( 1 );
    }

    //! Blocks in the kernel until @p aRelease is ready: it waits, as a deadlocked thread does.
    PROBE_NOINLINE void waitUntilReleased
        (
        std::atomic<bool>& aWaiting,        //!< Set to true just before the wait.
        std::shared_future<void> aRelease   //!< The test makes it ready to stop the wait.
        )
    {
        aWaiting.store( true );
        aRelease.wait();
        gAfter.fetch_add( 1 );
    }
}

namespace
{
    //! @return the first line of @p aText: the innermost frame in the text of a stack.
    std::string firstLine
        (
        const std::string& aText  //!< Text from CallStack::toString().
        )
    {
        return aText.substr( 0, aText.find( '\n' ) );
    }

    //! Checks that @p aName is one of the innermost frames of @p aText. No frame above it can
    //! belong to the reader or to the mechanism: no capture call, and on Linux no signal handler
    //! or trampoline.
    //!
    //! "One of the innermost", not "the innermost": a thread that stops in a loop can stop inside
    //! a function that the loop calls. In a build without optimisation, even atomic<bool>::load()
    //! is a call with a frame of its own. With 32-bit MSVC in debug, that call goes down three
    //! frames: load(), then _Check_load_memory_order(), then __RTC_CheckEsp, the stack check of
    //! /RTC that exists only on x86. Thus three frames can be above the probe.
    ::testing::AssertionResult isNearTheTop
        (
        const std::string& aText,  //!< Text from CallStack::toString().
        const char* aName          //!< The probe that must be near the top.
        )
    {
        static const int kMostAbove = 3;
        static const char* const kMechanism[] = { "CallStack", "onCaptureSignal", "__restore_rt",
                                                  "captureTargetHere" };

        std::size_t start = 0;
        for( int frame = 0; start < aText.size(); ++frame )
        {
            const std::size_t end = aText.find( '\n', start );
            const std::string line = aText.substr( start, end - start );
            if( line.find( aName ) != std::string::npos )
            {
                return ::testing::AssertionSuccess();
            }
            for( const char* mechanism : kMechanism )
            {
                if( line.find( mechanism ) != std::string::npos )
                {
                    return ::testing::AssertionFailure() << "frame " << frame << " is " << mechanism
                                                         << ", above " << aName << ":\n" << aText;
                }
            }
            if( frame == kMostAbove || end == std::string::npos )
            {
                break;
            }
            start = end + 1;
        }
        return ::testing::AssertionFailure() << aName << " is not in the innermost "
                                             << kMostAbove + 1 << " frames:\n" << aText;
    }

    //! A thread that makes a Target for itself, gives it to the test, then runs a body until the
    //! test releases it.
    //!
    //! The destructor joins the thread, after the last capture of the test: a test captures a
    //! target only while its thread runs.
    class ProbeThread
    {
    public:
        //! Starts the thread, then waits for its target and for @p aStarted to be true.
        ProbeThread
            (
            std::function<void()> aBody,        //!< What the thread runs after it makes its target.
            const std::atomic<bool>& aStarted   //!< The body sets it when it is in position.
            )
        {
            std::promise<CallStack::Target> handOver;
            std::future<CallStack::Target> target = handOver.get_future();
            mThread = std::thread( [&handOver, aBody]()
                {
                    handOver.set_value( CallStack::Target::currentThread() );
                    CallStackProbe::runBody( aBody );
                } );
            mTarget = target.get();
            while( !aStarted.load() )
            {
                std::this_thread::yield();
            }
        }

        //! Joins the thread. The test releases it before this runs.
        ~ProbeThread()
        {
            mThread.join();
        }

        //! @return the target of the thread.
        const CallStack::Target& target() const
        {
            return mTarget;
        }

    private:
        std::thread mThread;        //!< The thread.
        CallStack::Target mTarget;  //!< The target that the thread made for itself.
    };
}

//! The stack of the calling thread starts in its caller, and goes out through the body of the
//! test.
TEST( CallStackTest, CapturesTheCallingThread )
{
    CallStack stack;
    CallStackProbe::captureHere( stack );

    ASSERT_FALSE( stack.empty() ) << stack.error();
    EXPECT_TRUE( stack.error().empty() ) << stack.error();
    const std::string text = stack.toString();
    EXPECT_NE( firstLine( text ).find( "captureHere" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "CapturesTheCallingThread" ), std::string::npos ) << text;
}

//! The capture reads a busy thread where it spins. Above that frame there is nothing from the
//! reader or from the mechanism: on Linux, no handler and no signal trampoline.
TEST( CallStackTest, CapturesABusyThread )
{
    #if defined( QT_LIKE_SIGNAL_SIGNALS_DEFERRED )
        GTEST_SKIP() << "ThreadSanitizer defers the signal; see QT_LIKE_SIGNAL_SIGNALS_DEFERRED";
    #endif
    std::atomic<bool> spinning { false };
    std::atomic<bool> release { false };
    CallStack stack;
    {
        ProbeThread busy( [&spinning, &release]()
            {
                CallStackProbe::spinUntilReleased( spinning, release );
            }, spinning );
        ASSERT_TRUE( busy.target().isValid() );

        CallStackProbe::captureTargetHere( busy.target(), stack );
        release.store( true );
    }

    ASSERT_FALSE( stack.empty() ) << stack.error();
    const std::string text = stack.toString();
    EXPECT_TRUE( isNearTheTop( text, "spinUntilReleased" ) );
    EXPECT_EQ( text.find( "captureTargetHere" ), std::string::npos ) << text;
}

//! The walk goes to the bottom of a thread that stopped inside system code, which usually has no
//! symbols here. Each capture finds the frame at the bottom of the probe thread, not only the
//! frames near the top, and the frame that called into the clock.
//!
//! The probe calls the clock in a tight loop, so many captures stop at the first instruction of a
//! small clock function, before its prologue. There, on x86, the return address to the probe's
//! frame is only at the stack pointer, and a walk that starts from EBP skips that frame. Checking
//! readTheClockUntilReleased as well as runBody is what catches a walk that loses it.
TEST( CallStackTest, WalksPastSystemCodeToTheBottom )
{
    const int kCaptures = 20;
    std::atomic<bool> spinning { false };
    std::atomic<bool> release { false };
    int complete = 0;
    std::string incomplete;
    {
        ProbeThread busy( [&spinning, &release]()
            {
                CallStackProbe::readTheClockUntilReleased( spinning, release );
            }, spinning );

        for( int i = 0; i < kCaptures; ++i )
        {
            const std::string text = CallStack::capture( busy.target() ).toString();
            if( text.find( "runBody" ) != std::string::npos
                && text.find( "readTheClockUntilReleased" ) != std::string::npos )
            {
                ++complete;
            }
            else if( incomplete.empty() || text.size() < incomplete.size() )
            {
                incomplete = text;
            }
        }
        release.store( true );
    }

    EXPECT_EQ( complete, kCaptures ) << "one that lost a frame:\n" << incomplete;
}

//! The capture also reads a thread that is blocked in the kernel, through the frames of the
//! system to the caller that blocked.
TEST( CallStackTest, CapturesAThreadBlockedInTheKernel )
{
    #if defined( QT_LIKE_SIGNAL_SIGNALS_DEFERRED )
        GTEST_SKIP() << "ThreadSanitizer defers the signal; see QT_LIKE_SIGNAL_SIGNALS_DEFERRED";
    #endif
    std::atomic<bool> waiting { false };
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    CallStack stack;
    {
        ProbeThread blocked( [&waiting, released]()
            {
                CallStackProbe::waitUntilReleased( waiting, released );
            }, waiting );

        // The probe sets the flag just before the wait, so give the thread time to start the
        // wait.
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        CallStackProbe::captureTargetHere( blocked.target(), stack );
        release.set_value();
    }

    ASSERT_FALSE( stack.empty() ) << stack.error();
    const std::string text = stack.toString();
    EXPECT_NE( text.find( "waitUntilReleased" ), std::string::npos ) << text;
}

//! A thread can capture its own target. It reads itself directly, and does not stop itself.
TEST( CallStackTest, CapturesItsOwnTarget )
{
    const CallStack::Target self = CallStack::Target::currentThread();
    ASSERT_TRUE( self.isValid() );

    CallStack stack;
    CallStackProbe::captureTargetHere( self, stack );

    ASSERT_FALSE( stack.empty() ) << stack.error();
    const std::string text = stack.toString();
    EXPECT_NE( firstLine( text ).find( "captureTargetHere" ), std::string::npos ) << text;
}

//! When two threads capture at the same time, the captures run one at a time, and each one gets
//! a stack.
TEST( CallStackTest, CapturesFromTwoReadersAtOnce )
{
    static constexpr int kEach = 20;
    std::atomic<bool> spinning { false };
    std::atomic<bool> release { false };
    int goodA = 0;
    int goodB = 0;
    {
        ProbeThread busy( [&spinning, &release]()
            {
                CallStackProbe::spinUntilReleased( spinning, release );
            }, spinning );

        const auto reader = [&busy]( int& aGood )
            {
                for( int i = 0; i < kEach; ++i )
                {
                    if( !CallStack::capture( busy.target() ).empty() )
                    {
                        ++aGood;
                    }
                }
            };
        std::thread readerA( reader, std::ref( goodA ) );
        std::thread readerB( reader, std::ref( goodB ) );
        readerA.join();
        readerB.join();
        release.store( true );
    }

    EXPECT_EQ( goodA, kEach );
    EXPECT_EQ( goodB, kEach );
}

//! Each frame of the text opens with the file name of the module that holds it, which is the part
//! a reader uses to tell the program from a library that it loaded. The frames of this test are in
//! the test program itself, so its name has to be there. This also guards the search for the path
//! of a module, which gives up and writes "?" when it cannot read one.
TEST( CallStackTest, EachFrameNamesItsModule )
{
    const std::string text = CallStack::capture( CallStack::Target::currentThread() ).toString();

    ASSERT_FALSE( text.empty() );
    EXPECT_NE( text.find( "QtLikeSignal-test" ), std::string::npos ) << text;
}

//! An empty target gives an empty stack, and its text tells why.
TEST( CallStackTest, EmptyTargetIsAnError )
{
    const CallStack stack = CallStack::capture( CallStack::Target() );

    EXPECT_TRUE( stack.empty() );
    ASSERT_FALSE( stack.error().empty() );
    EXPECT_NE( stack.toString().find( stack.error() ), std::string::npos ) << stack.toString();
}

//! On Windows, a target that lives after its thread causes no damage: the capture fails and tells
//! why.
TEST( CallStackTest, EndedThreadIsAnError )
{
    #if !defined( _WIN32 )
        GTEST_SKIP() << "a Linux target must not outlive its thread: its id may name a new one";
    #else
        CallStack::Target ended;
        std::thread worker( [&ended]()
            {
                ended = CallStack::Target::currentThread();
            } );
        worker.join();
        ASSERT_TRUE( ended.isValid() );

        const CallStack stack = CallStack::capture( ended );
        EXPECT_TRUE( stack.empty() ) << stack.toString();
        EXPECT_FALSE( stack.error().empty() );
    #endif
}

//! A move of a target moves the thread that it names, and makes the source empty.
TEST( CallStackTest, MovingATargetEmptiesTheSource )
{
    CallStack::Target first = CallStack::Target::currentThread();
    ASSERT_TRUE( first.isValid() );

    CallStack::Target second( std::move( first ) );
    EXPECT_FALSE( first.isValid() );
    EXPECT_TRUE( second.isValid() );

    first = std::move( second );
    EXPECT_TRUE( first.isValid() );
    EXPECT_FALSE( second.isValid() );
}

//! On Linux, a thread that blocks the signal does not answer, and the capture stops after
//! kAnswerTimeoutMs. The kernel delivers the signal late, when the thread unblocks it, and the
//! handler finds nothing to answer. Thus the next capture of the same thread still gets its own
//! correct answer.
TEST( CallStackTest, ThreadThatBlocksTheSignalDoesNotAnswer )
{
    #if !defined( __linux__ )
        GTEST_SKIP() << "only Linux reads another thread through a signal";
    #elif defined( QT_LIKE_SIGNAL_SIGNALS_DEFERRED )
        GTEST_SKIP() << "ThreadSanitizer defers the signal; see QT_LIKE_SIGNAL_SIGNALS_DEFERRED";
    #else
        std::atomic<bool> blocked { false };
        std::atomic<bool> unblock { false };
        std::atomic<bool> spinning { false };
        std::atomic<bool> release { false };
        CallStack refused;
        CallStack answered;
        std::chrono::steady_clock::duration waited {};
        {
            ProbeThread blocker( [&blocked, &unblock, &spinning, &release]()
                {
                    sigset_t signals;
                    sigemptyset( &signals );
                    sigaddset( &signals, SIGRTMIN + CallStack::kSignalOffset );
                    pthread_sigmask( SIG_BLOCK, &signals, nullptr );
                    blocked.store( true );

                    while( !unblock.load() )
                    {
                        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                    }
                    // Here the kernel delivers the signal that the refused capture left pending.
                    pthread_sigmask( SIG_UNBLOCK, &signals, nullptr );
                    CallStackProbe::spinUntilReleased( spinning, release );
                }, blocked );

            const auto start = std::chrono::steady_clock::now();
            refused = CallStack::capture( blocker.target() );
            waited = std::chrono::steady_clock::now() - start;

            unblock.store( true );
            while( !spinning.load() )
            {
                std::this_thread::yield();
            }
            answered = CallStack::capture( blocker.target() );
            release.store( true );
        }

        EXPECT_TRUE( refused.empty() );
        EXPECT_NE( refused.error().find( "did not answer" ), std::string::npos ) << refused.error();
        EXPECT_GE( waited, std::chrono::milliseconds( CallStack::kAnswerTimeoutMs - 50 ) );

        ASSERT_FALSE( answered.empty() ) << answered.error();
        EXPECT_TRUE( isNearTheTop( answered.toString(), "spinUntilReleased" ) );
    #endif
}
