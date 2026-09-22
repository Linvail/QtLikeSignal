// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for SignalWatcher: the opt-in watcher that turns a shutdown request from the
//! operating system into an ordinary signal on the event loop.
//!
//! **Three suites.** The first is portable and covers what a watcher does before any signal
//! arrives: it installs, it refuses to be the second one in a process, and it lets go when it is
//! destroyed. The second is Linux-only and covers delivery, both modes, and the state the watcher
//! leaves the process in afterwards. The third is Windows-only and covers delivery there.
//!
//! **The Windows delivery test uses a child process, and has to.** The only way to make the console
//! control handler run is GenerateConsoleCtrlEvent(), which addresses a process *group* rather than
//! one process: called here it would send Ctrl+Break to every program sharing the console, the test
//! runner included. So the test starts this same binary again with CREATE_NEW_PROCESS_GROUP, which
//! puts the child in a group of its own, and signals that group by id. See the two tests at the end
//! of this file.
//!
//! **Each delivery test raises the signal exactly once.** The second one is meant to terminate the
//! process -- that is the guarantee the class makes -- so a test that raised twice would take the
//! whole binary with it. The "the next one terminates" property is asserted by reading the
//! disposition back instead, which is the same fact without the crater.

#include "QtLikeSignal/SignalWatcher.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategory.hpp"
#include "QtLikeSignal/LogMessage.hpp"
#include "QtLikeSignal/LogSink.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined( __linux__ )
    #include <csignal>
    #include <pthread.h>
#endif

#if defined( _WIN32 )
    #include <csignal>
    #include <cstdlib>

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        // windows.h defines min/max macros that collide with std::min/std::max, which gtest uses.
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

using namespace QtLikeSignal;

namespace
{
    //! How long a delivery test lets the loop run before it gives up.
    //!
    //! Every delivery here is already pending before exec() is entered, so the loop should return
    //! on its first pass. The timer exists so that a broken watcher fails the test in a second
    //! instead of hanging the suite: a signal that was blocked and never read leaves the loop with
    //! nothing at all to wake it.
    const int kGiveUpMs = 2000;

    //! A sink that counts records and flushes, and remembers how many records each flush had seen.
    //!
    //! The last part is what makes the ordering testable. SignalWatcher promises to flush *after*
    //! the slots have run, because the records that explain a shutdown are the ones the slots
    //! write; a flush that ran first would leave exactly those behind.
    class ShutdownSink : public LogSink
    {
    public:
        //! Records the text, so a test can look for the line one of its slots wrote.
        void write
            (
            const LogMessage& aMessage
            ) override
        {
            std::lock_guard<std::mutex> guard( mMutex );
            mTexts.push_back( std::string( aMessage.mText, aMessage.mLength ) );
        }

        //! Counts the flush, and notes how much had been written when it happened.
        void flush() override
        {
            std::lock_guard<std::mutex> guard( mMutex );
            ++mFlushCount;
            mRecordsAtLastFlush = mTexts.size();
        }

        //! @return how many times flush() was called.
        int flushCount() const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            return mFlushCount;
        }

        //! @return how many records had been written when the last flush happened.
        std::size_t recordsAtLastFlush() const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            return mRecordsAtLastFlush;
        }

        //! @return the one-based position of the first record containing @p aNeedle, or 0.
        std::size_t positionOf
            (
            const std::string& aNeedle  //!< Text to look for inside a record.
            ) const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            for( std::size_t i = 0; i < mTexts.size(); ++i )
            {
                if( mTexts[i].find( aNeedle ) != std::string::npos )
                {
                    return i + 1;
                }
            }
            return 0;
        }

    private:
        mutable std::mutex mMutex;          //!< Guards everything below; sinks are called anywhere.

        std::vector<std::string> mTexts;    //!< Every record written, in order.

        int mFlushCount { 0 };              //!< How many flushes have arrived.

        std::size_t mRecordsAtLastFlush { 0 };  //!< mTexts.size() as of the last flush.
    };

    //! Installs a sink for as long as it lives, and puts the previous one back.
    //!
    //! Several tests here provoke a critical record on purpose. Without this they would be correct
    //! and still print a wall of alarming text into a passing run.
    class ScopedSink
    {
    public:
        //! Installs @p aSink, keeping whatever was installed before.
        explicit ScopedSink
            (
            LogSink* aSink  //!< Sink to install. Must outlive this object.
            )
        {
            mPrevious = Log::setSink( aSink );
        }

        //! Puts the previous sink back.
        ~ScopedSink()
        {
            Log::setSink( mPrevious );
        }

        ScopedSink
            (
            const ScopedSink&
            ) = delete;

        ScopedSink& operator=
            (
            const ScopedSink&
            ) = delete;

    private:
        LogSink* mPrevious { nullptr };   //!< What was installed before, put back on destruction.
    };

    //! Starts a single-shot timer that quits the loop, so no test can hang the suite.
    //!
    //! The timer is the caller's, because it has to outlive this call and be stopped by going out
    //! of scope with the rest of the test.
    void armGiveUpTimer
        (
        Timer& aTimer  //!< Timer to arm. Anything already set on it is replaced.
        )
    {
        aTimer.setSingleShot( true );
        Object::connect( aTimer.getTimeout(), &aTimer, []()
            {
                CoreApplication::quit();
            }, ConnectionType::Direct );
        aTimer.start( kGiveUpMs );
    }
}

//! Verifies a watcher constructed on the loop's own thread installs itself.
TEST( SignalWatcherTest, AWatcherInstallsItselfOnTheLoopThread )
{
    CoreApplication app;

    SignalWatcher watcher;
    EXPECT_TRUE( watcher.isWatching() );
    EXPECT_EQ( watcher.mode(), SignalWatchMode::Handler )
        << "SignalWatchMode::Handler is the default because it constrains nothing.";
}

//! Verifies the second watcher in a process refuses, and says why.
//!
//! The handlers and the saved dispositions are process-global. Two watchers would each put back
//! what the other installed, and the loser would be the one that had already been asked to run the
//! shutdown -- so the second one has to decline rather than take over.
TEST( SignalWatcherTest, ASecondWatcherRefusesAndSaysSo )
{
    CoreApplication app;

    ShutdownSink sink;
    ScopedSink installed( &sink );

    SignalWatcher first;
    ASSERT_TRUE( first.isWatching() );

    SignalWatcher second;
    EXPECT_FALSE( second.isWatching() );
    EXPECT_GT( sink.positionOf( "a watcher already exists" ), 0u )
        << "a refusal has to be reported; a silent one is found only during a shutdown.";
}

//! Verifies destroying a watcher lets the next one install.
//!
//! The refusal above must be a claim on a living object, not a one-shot latch on the process. A
//! program that builds a watcher, tears its application down and builds another -- which is what
//! every test in this file does -- would otherwise stop being watched after the first.
TEST( SignalWatcherTest, AWatcherReleasesItsHoldWhenDestroyed )
{
    CoreApplication app;

    {
        SignalWatcher first;
        ASSERT_TRUE( first.isWatching() );
    }

    SignalWatcher second;
    EXPECT_TRUE( second.isWatching() );
}

#if defined( __linux__ )

namespace
{
    //! The category the flush-ordering test logs on, so that it can find its own record again.
    //!
    //! Under "qtlikesignal.test." rather than "qtlikesignal.", so that a filter rule aimed at the
    //! library does not catch it and a rule aimed at it does not catch the library.
    QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gShutdownTest, "qtlikesignal.test.shutdown", "TSHD" )
}

//! Verifies a raised SIGTERM comes back as the watcher's signal, on the loop's own thread.
//!
//! The point of the class in one test: a handler may do almost nothing, so the notification takes
//! the long way round -- one byte down a pipe, out of the poll(2) the loop was asleep in, and into
//! a slot on the loop's thread where locking and allocating are legal again.
TEST( SignalWatcherLinuxTest, AWatchedSignalArrivesAsASignalOnTheLoopThread )
{
    CoreApplication app;
    Thread* const mainThread = Thread::currentThread();

    SignalWatcher watcher;
    ASSERT_TRUE( watcher.isWatching() );

    std::atomic<int> seenSignal { 0 };
    std::atomic<Thread*> ranOn { nullptr };
    std::atomic<int> callCount { 0 };

    Object::connect( watcher.getTriggered(), &watcher,
        [&]( int aSignal )
        {
            seenSignal.store( aSignal );
            ranOn.store( Thread::currentThread() );
            callCount.fetch_add( 1 );
            CoreApplication::quit();
        } );

    ASSERT_EQ( ::raise( SIGTERM ), 0 );

    Timer giveUp;
    armGiveUpTimer( giveUp );

    EXPECT_EQ( app.exec(), 0 );

    EXPECT_EQ( callCount.load(), 1 );
    EXPECT_EQ( seenSignal.load(), SIGTERM );
    EXPECT_EQ( ranOn.load(), mainThread )
        << "the shutdown slot has to run on the loop's thread, not in the handler.";
}

//! Verifies the watched signals are left at their default once one has arrived.
//!
//! This is "the second signal kills", asserted without raising a second signal. SA_RESETHAND puts
//! the default disposition back as the kernel enters the handler, and the watcher deliberately
//! does not undo that afterwards -- a graceful shutdown that hangs must not make the program
//! unkillable.
//!
//! **A custom disposition is installed first, and that is what makes the test mean anything.**
//! Without it the disposition the watcher saves is already SIG_DFL, so a watcher that wrongly
//! restored the saved one would write SIG_DFL over SIG_DFL and the assertion below would pass
//! either way. SIG_IGN is a value the two branches cannot both produce.
TEST( SignalWatcherLinuxTest, TheWatchedSignalsAreLeftAtTheirDefaultAfterOneArrives )
{
    CoreApplication app;

    struct sigaction ignoring;
    sigemptyset( &ignoring.sa_mask );
    ignoring.sa_flags   = 0;
    ignoring.sa_handler = SIG_IGN;

    struct sigaction original;
    ASSERT_EQ( sigaction( SIGTERM, &ignoring, &original ), 0 );

    {
        SignalWatcher watcher;
        ASSERT_TRUE( watcher.isWatching() );

        Object::connect( watcher.getTriggered(), &watcher, []( int )
            {
                CoreApplication::quit();
            } );

        ASSERT_EQ( ::raise( SIGTERM ), 0 );

        Timer giveUp;
        armGiveUpTimer( giveUp );
        EXPECT_EQ( app.exec(), 0 );
    }

    struct sigaction current;
    ASSERT_EQ( sigaction( SIGTERM, nullptr, &current ), 0 );
    EXPECT_EQ( current.sa_handler, SIG_DFL )
        << "after one signal the next must terminate the process, even from the destructor; "
        << "SIG_IGN here means the destructor restored what it saved and should not have.";

    sigaction( SIGTERM, &original, nullptr );
}

//! Verifies the earlier disposition is put back when no signal ever arrived.
//!
//! sigaction(2) is process-global, so a watcher that had replaced an application's own handler and
//! then quietly kept it replaced would be a library breaking a program that never asked it to.
TEST( SignalWatcherLinuxTest, TheDestructorPutsTheEarlierHandlerBackWhenNothingArrived )
{
    CoreApplication app;

    struct sigaction ours;
    sigemptyset( &ours.sa_mask );
    ours.sa_flags   = 0;
    ours.sa_handler = SIG_IGN;

    struct sigaction original;
    ASSERT_EQ( sigaction( SIGTERM, &ours, &original ), 0 );

    {
        SignalWatcher watcher;
        ASSERT_TRUE( watcher.isWatching() );
    }

    struct sigaction current;
    ASSERT_EQ( sigaction( SIGTERM, nullptr, &current ), 0 );
    EXPECT_EQ( current.sa_handler, SIG_IGN )
        << "what the program had installed before must be what it has afterwards.";

    sigaction( SIGTERM, &original, nullptr );
}

//! Verifies the log is flushed after the slots have run, not before.
//!
//! A sink with a buffer loses whatever it still holds when the process goes. The records worth
//! keeping are the ones the shutdown slots write, so the flush has to come last.
TEST( SignalWatcherLinuxTest, TheLogIsFlushedAfterTheSlotsHaveRun )
{
    CoreApplication app;

    ShutdownSink sink;
    ScopedSink installed( &sink );

    SignalWatcher watcher;
    ASSERT_TRUE( watcher.isWatching() );

    Object::connect( watcher.getTriggered(), &watcher, [&]( int )
        {
            qCCritical( gShutdownTest ) << "the-slot-was-here";
            CoreApplication::quit();
        } );

    ASSERT_EQ( ::raise( SIGTERM ), 0 );

    Timer giveUp;
    armGiveUpTimer( giveUp );
    EXPECT_EQ( app.exec(), 0 );

    const std::size_t slotRecord = sink.positionOf( "the-slot-was-here" );
    ASSERT_GT( slotRecord, 0u ) << "the slot's own record never reached the sink.";
    EXPECT_GT( sink.flushCount(), 0 );
    EXPECT_GE( sink.recordsAtLastFlush(), slotRecord )
        << "the flush ran before the slot did, which is the one ordering that loses records.";
}

//! Verifies a watcher refuses on a thread whose loop cannot poll a descriptor.
//!
//! The notification is a file descriptor, and only EventDispatcherLinux can wait on one. A raw
//! std::thread is adopted with EventDispatcherDefault, which cannot -- so a watcher built there
//! would install handlers whose byte nobody ever reads, and swallow the shutdown.
TEST( SignalWatcherLinuxTest, AWatcherRefusesOnAThreadWithoutTheLinuxDispatcher )
{
    ShutdownSink sink;
    ScopedSink installed( &sink );

    std::atomic<bool> watching { true };
    std::thread worker( [&watching]()
        {
            SignalWatcher watcher;
            watching.store( watcher.isWatching() );
        } );
    worker.join();

    EXPECT_FALSE( watching.load() );
    EXPECT_GT( sink.positionOf( "not running EventDispatcherLinux" ), 0u );
}

//! Verifies SignalFd mode refuses when the signals were not blocked first.
//!
//! signalfd(2) reads signals that are blocked; an unblocked one is delivered the ordinary way and
//! never reaches the descriptor. The block has to happen in main() before any thread starts, which
//! is a constraint on the application that this library cannot impose -- so it checks and declines
//! rather than looking installed and reporting nothing.
TEST( SignalWatcherLinuxTest, SignalFdModeRefusesWhenTheSignalsAreNotBlocked )
{
    CoreApplication app;

    ShutdownSink sink;
    ScopedSink installed( &sink );

    SignalWatcher watcher( SignalWatchMode::SignalFd );
    EXPECT_FALSE( watcher.isWatching() );
    EXPECT_EQ( watcher.mode(), SignalWatchMode::SignalFd )
        << "mode() reports what was asked for, whether or not it could be honoured.";
    EXPECT_GT( sink.positionOf( "blocked in every thread" ), 0u );
}

//! Verifies SignalFd mode reports a signal once the caller has blocked it.
//!
//! The alternative construction mode, end to end: no handler exists, so nothing here has to be
//! async-signal-safe, and the signal arrives as bytes on a readable descriptor like any other
//! platform event source.
TEST( SignalWatcherLinuxTest, SignalFdModeReportsASignalWhenTheyAreBlocked )
{
    CoreApplication app;

    sigset_t watched;
    sigemptyset( &watched );
    sigaddset( &watched, SIGINT );
    sigaddset( &watched, SIGTERM );

    sigset_t previous;
    ASSERT_EQ( pthread_sigmask( SIG_BLOCK, &watched, &previous ), 0 );

    std::atomic<int> seenSignal { 0 };

    {
        SignalWatcher watcher( SignalWatchMode::SignalFd );
        ASSERT_TRUE( watcher.isWatching() );

        Object::connect( watcher.getTriggered(), &watcher, [&]( int aSignal )
            {
                seenSignal.store( aSignal );
                CoreApplication::quit();
            } );

        // raise() targets the calling thread, so the signal is pending and blocked here and the
        // descriptor is the only way it can be read. That makes the test deterministic rather than
        // dependent on which thread the kernel picks.
        ASSERT_EQ( ::raise( SIGTERM ), 0 );

        Timer giveUp;
        armGiveUpTimer( giveUp );
        EXPECT_EQ( app.exec(), 0 );
    }

    EXPECT_EQ( seenSignal.load(), SIGTERM );

    // The watcher unblocked the watched signals on delivery, which is how the next one terminates
    // the process. Put the mask this test found back, so nothing after it inherits the change.
    pthread_sigmask( SIG_SETMASK, &previous, nullptr );
}

#endif // defined( __linux__ )

#if defined( _WIN32 )

namespace
{
    //! Name of the environment variable naming the event the child sets when it is ready.
    //!
    //! An environment variable rather than an argument, because the child is this same binary run
    //! by GoogleTest, and GoogleTest owns the command line. The environment is inherited by
    //! CreateProcess with no plumbing at all.
    const char* const kReadyEventVariable = "QTLIKESIGNAL_TEST_READY_EVENT";

    //! The GoogleTest name of the child half of the delivery test.
    const char* const kChildTestName =
        "SignalWatcherWinChildTest.DISABLED_QuitsOnAConsoleControlEvent";

    //! How long either half waits for the other before it gives up, in milliseconds.
    const unsigned long kChildTimeoutMs = 30000;
}

//! The child half of the delivery test. Run only by the parent below.
//!
//! DISABLED_ so that an ordinary run of the suite skips it: on its own it would sit in exec() until
//! its timer fired, waiting for an event nobody is going to send. The parent runs it by name with
//! --gtest_also_run_disabled_tests, and reads the exit code GoogleTest produces -- 0 only if every
//! expectation below held.
TEST( SignalWatcherWinChildTest, DISABLED_QuitsOnAConsoleControlEvent )
{
    const char* const readyEventName = std::getenv( kReadyEventVariable );
    ASSERT_NE( readyEventName, nullptr )
        << "this test is run by its parent, which sets " << kReadyEventVariable;

    CoreApplication app;
    Thread* const mainThread = Thread::currentThread();

    SignalWatcher watcher;
    ASSERT_TRUE( watcher.isWatching() );

    std::atomic<int> seenSignal { 0 };
    std::atomic<Thread*> ranOn { nullptr };

    Object::connect( watcher.getTriggered(), &watcher, [&]( int aSignal )
        {
            seenSignal.store( aSignal );
            ranOn.store( Thread::currentThread() );
            CoreApplication::quit();
        } );

    // Readiness is reported from inside the loop, not from here, and that is what makes the test
    // deterministic. By the time a zero-delay timer fires, the handler is installed and the loop is
    // running -- so a control event sent after this point cannot arrive too early to be seen.
    Timer announce;
    announce.setSingleShot( true );
    Object::connect( announce.getTimeout(), &announce, [readyEventName]()
        {
            HANDLE const ready = ::OpenEventA( EVENT_MODIFY_STATE, FALSE, readyEventName );
            if( ready != nullptr )
            {
                ::SetEvent( ready );
                ::CloseHandle( ready );
            }
        }, ConnectionType::Direct );
    announce.start( 0 );

    Timer giveUp;
    giveUp.setSingleShot( true );
    Object::connect( giveUp.getTimeout(), &giveUp, []()
        {
            CoreApplication::quit();
        }, ConnectionType::Direct );
    giveUp.start( static_cast<int>( kChildTimeoutMs ) );

    EXPECT_EQ( app.exec(), 0 );
    EXPECT_EQ( seenSignal.load(), SIGBREAK );
    EXPECT_EQ( ranOn.load(), mainThread )
        << "the shutdown slot has to run on the loop's thread, not on the handler's.";
}

//! Verifies a real console control event reaches the loop and quits it.
//!
//! **This is why it needs a child process.** GenerateConsoleCtrlEvent() addresses a process
//! *group*, not a process, so calling it here with group 0 would send Ctrl+Break to everything
//! sharing this console -- the test runner included. A child launched with CREATE_NEW_PROCESS_GROUP
//! is a group of its own, and can be signalled by its own process id while this process, in the
//! group it inherited, is untouched.
//!
//! Ctrl+Break and not Ctrl+C: CREATE_NEW_PROCESS_GROUP disables Ctrl+C in the child, which is
//! documented behaviour and not a fault of the watcher.
//!
//! It covers what the three portable tests cannot: that the handler queues the call, that the
//! queued call reaches the loop's own thread, and that the handler's wait ends because the loop
//! acknowledged it rather than because it timed out.
TEST( SignalWatcherWinTest, AConsoleControlEventQuitsTheChildsLoop )
{
    // GetConsoleProcessList() and not GetConsoleWindow(). A console opened by a terminal that uses
    // a pseudoconsole -- Windows Terminal, an IDE panel, anything driving ConPTY -- has no window
    // of its own, so GetConsoleWindow() answers null for a process that is perfectly well attached
    // and would have skipped this test everywhere except a classic console window.
    DWORD consoleOwner = 0;
    if( ::GetConsoleProcessList( &consoleOwner, 1 ) == 0 )
    {
        GTEST_SKIP() << "no console is attached, so no control event can be generated.";
    }

    char exePath[MAX_PATH] = { 0 };
    ASSERT_NE( ::GetModuleFileNameA( nullptr, exePath, MAX_PATH ), 0u );

    // The process id keeps the name unique, so two copies of this suite running at once do not
    // hand each other's children the same event.
    const std::string readyEventName =
        "Local\\QtLikeSignal-SignalWatcher-ready-" + std::to_string( ::GetCurrentProcessId() );

    HANDLE const ready = ::CreateEventA( nullptr, TRUE, FALSE, readyEventName.c_str() );
    ASSERT_NE( ready, nullptr );

    ASSERT_NE( ::SetEnvironmentVariableA( kReadyEventVariable, readyEventName.c_str() ), 0 );

    std::string commandLine = std::string( "\"" ) + exePath + "\" --gtest_filter="
        + kChildTestName + " --gtest_also_run_disabled_tests";

    STARTUPINFOA startup = { 0 };
    startup.cb = sizeof( startup );
    PROCESS_INFORMATION child = { 0 };

    const BOOL started = ::CreateProcessA( nullptr, &commandLine[0], nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &child );

    ::SetEnvironmentVariableA( kReadyEventVariable, nullptr );

    ASSERT_NE( started, 0 ) << "could not start the child; error " << ::GetLastError();

    const DWORD readyResult = ::WaitForSingleObject( ready, kChildTimeoutMs );
    if( readyResult != WAIT_OBJECT_0 )
    {
        ::TerminateProcess( child.hProcess, 1 );
    }
    EXPECT_EQ( readyResult, static_cast<DWORD>( WAIT_OBJECT_0 ) )
        << "the child never reported that its loop was running.";

    if( readyResult == WAIT_OBJECT_0 )
    {
        EXPECT_NE( ::GenerateConsoleCtrlEvent( CTRL_BREAK_EVENT, child.dwProcessId ), 0 )
            << "GenerateConsoleCtrlEvent() failed; error " << ::GetLastError();

        if( ::WaitForSingleObject( child.hProcess, kChildTimeoutMs ) != WAIT_OBJECT_0 )
        {
            ::TerminateProcess( child.hProcess, 2 );
            ADD_FAILURE() << "the child did not exit after the control event.";
        }
    }

    DWORD exitCode = 0xffffffff;
    EXPECT_NE( ::GetExitCodeProcess( child.hProcess, &exitCode ), 0 );
    EXPECT_EQ( exitCode, 0u )
        << "the child's own expectations failed; run " << kChildTestName
        << " with --gtest_also_run_disabled_tests to see them.";

    ::CloseHandle( child.hThread );
    ::CloseHandle( child.hProcess );
    ::CloseHandle( ready );
}

#endif // defined( _WIN32 )
