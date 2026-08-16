//! @file
//!
//! Tests for the Win32 event dispatcher's OS-message handling -- mission stage 5, "I want to
//! receive OS/platform's messages. 100% cpu-spin is not allowed."
//!
//! The whole file is Windows-only; it compiles to nothing elsewhere so the cross-build stays clean.
//!
//! **Scope, and why this file is smaller than its Linux sibling.** Every test in the suite already
//! runs on EventDispatcherWin32 -- CoreApplication and Thread construct it unconditionally on
//! Windows -- so the constructor, the destructor, the MsgWaitForMultipleObjectsEx wait, the
//! wakeWaiter() post and the PeekMessage drain are exercised several hundred times over before this
//! file is reached. What none of them touch is the part of processPlatformEvents() that deals with
//! a message the dispatcher did not send itself: TranslateMessage/DispatchMessage, and the WM_QUIT
//! branch. That is what these tests are for.

#if defined( _WIN32 )

#include <gtest/gtest.h>
#include "QtMimic/CoreApplication.hpp"
#include "QtMimic/EventDispatcherWin32.hpp"
#include "QtMimic/Object.hpp"
#include "TestCpuTime.hpp"
#include "QtMimic/Thread.hpp"
#include "QtMimic/Timer.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    // windows.h defines min/max macros that collide with std::min/std::max, which gtest uses.
    #define NOMINMAX
#endif
#include <windows.h>

using namespace QtMimic;

namespace
{
    //! Application-defined message the tests post, standing in for any OS or component message.
    //!
    //! WM_APP rather than WM_USER: WM_USER+1 is what the dispatcher posts to its own window for
    //! wakeups, and a test message must not be mistaken for one. The WM_APP range is reserved for
    //! exactly this -- messages exchanged within an application rather than within a window class.
    constexpr UINT kTestMessage = WM_APP + 1;

    //! Class name of the tests' own window, distinct from the dispatcher's.
    //!
    //! TCHAR, matching the generic-text Win32 calls this file makes.
    const TCHAR* const kTestWindowClassName = TEXT( "QtMimicTest_Window" );

    //! A message-only window belonging to the thread that constructs it, recording what it receives.
    //!
    //! This is the Windows counterpart of the Linux tests' TestPipe: something the OS will deliver
    //! to, which the dispatcher knows nothing about, so that "the message reached its handler" can
    //! only be true if the dispatcher really called DispatchMessage on it.
    //!
    //! Must be constructed *and* destroyed on the thread whose loop is under test -- a window
    //! belongs to its creating thread, and DestroyWindow() from any other thread fails.
    class TestMessageWindow
    {
    public:
        //! Creates the message-only window, owned by the calling thread.
        TestMessageWindow()
        {
            registerClassOnce();

            mWindow = CreateWindowEx( 0, kTestWindowClassName, kTestWindowClassName, 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, GetModuleHandle( nullptr ), nullptr );

            if( mWindow != nullptr )
            {
                // How the window procedure, which is static, finds the instance that owns it.
                SetWindowLongPtr( mWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( this ) );
            }
        }

        //! Destroys the window. Must run on the thread that constructed this object.
        ~TestMessageWindow()
        {
            if( mWindow != nullptr )
            {
                DestroyWindow( mWindow );
                mWindow = nullptr;
            }
        }

        TestMessageWindow
            (
            const TestMessageWindow&
            ) = delete;

        TestMessageWindow& operator=
            (
            const TestMessageWindow&
            ) = delete;

        //! The window handle, or nullptr if it could not be created.
        HWND handle() const
        {
            return mWindow;
        }

        //! Counts how many times @p aMessage was delivered through the window procedure.
        int countOf
            (
            UINT aMessage   //!< Message to count.
            ) const
        {
            std::lock_guard<std::mutex> lock( mMutex );
            int                         count = 0;
            for( const auto& received : mReceived )
            {
                if( received.mMessage == aMessage )
                {
                    ++count;
                }
            }
            return count;
        }

        //! Gets the wParam of the first @p aMessage delivered, or 0 if there was none.
        WPARAM firstParamOf
            (
            UINT aMessage   //!< Message to look for.
            ) const
        {
            std::lock_guard<std::mutex> lock( mMutex );
            for( const auto& received : mReceived )
            {
                if( received.mMessage == aMessage )
                {
                    return received.mWParam;
                }
            }
            return 0;
        }

        //! The thread the window procedure last ran on, or nullptr if it has not run.
        Thread* lastRanOn() const
        {
            return mRanOn.load();
        }

    private:
        //! One message as the window procedure saw it.
        struct Received
        {
            UINT   mMessage;   //!< Message id.
            WPARAM mWParam;    //!< Its first parameter.
        };

        //! Registers the test window class once per process.
        //!
        //! RegisterClass is process-wide, so a per-instance call would fail with
        //! ERROR_CLASS_ALREADY_EXISTS after the first window; call_once also makes it safe for the
        //! main thread and a worker to build their windows at the same time.
        static void registerClassOnce()
        {
            static std::once_flag sOnce;
            std::call_once( sOnce, []()
                {
                    WNDCLASS windowClass {};
                    windowClass.lpfnWndProc   = &windowProc;
                    windowClass.hInstance     = GetModuleHandle( nullptr );
                    windowClass.lpszClassName = kTestWindowClassName;
                    RegisterClass( &windowClass );
                } );
        }

        //! Records the message, then hands it to the default handler.
        static LRESULT CALLBACK windowProc
            (
            HWND aWindow,      //!< Window the message was dispatched to.
            UINT aMessage,     //!< Message id.
            WPARAM aWParam,    //!< First message parameter.
            LPARAM aLParam     //!< Second message parameter.
            )
        {
            auto* self = reinterpret_cast<TestMessageWindow*>(
                GetWindowLongPtr( aWindow, GWLP_USERDATA ) );
            if( self != nullptr )
            {
                {
                    std::lock_guard<std::mutex> lock( self->mMutex );
                    self->mReceived.push_back( { aMessage, aWParam } );
                }
                self->mRanOn.store( Thread::currentThread() );
            }
            return DefWindowProc( aWindow, aMessage, aWParam, aLParam );
        }

        HWND                  mWindow { nullptr };   //!< The message-only window, or nullptr.
        mutable std::mutex    mMutex;                //!< Guards mReceived.
        std::vector<Received> mReceived;             //!< Every message the procedure saw, in order.
        std::atomic<Thread*>  mRanOn { nullptr };    //!< Thread the procedure last ran on.
    };

    //! Gets the running thread's dispatcher as an EventDispatcherWin32, or null if it is not one.
    std::shared_ptr<EventDispatcherWin32> currentWin32Dispatcher()
    {
        Thread* current = Thread::currentThread();
        if( !current )
        {
            return nullptr;
        }
        return std::dynamic_pointer_cast<EventDispatcherWin32>( current->eventDispatcher() );
    }

    //! Quits the application if it is still running after a deadline, and reports that it had to.
    //!
    //! The loop under test blocks in MsgWaitForMultipleObjectsEx with no deadline when nothing is
    //! scheduled, so a dispatcher that failed to deliver would hang the suite rather than fail it.
    //! fired() turns that hang into an ordinary failure with a reason attached.
    //!
    //! Cancelled by its own destructor, and sleeping in short slices rather than one long one, for
    //! two reasons: a test that ends on time pays nothing for the watchdog, and a watchdog left
    //! running past its test cannot quit the *next* test's application out from under it.
    //!
    //! It posts a raw thread message as well as calling quit(), and that is not belt and braces.
    //! quit() wakes the loop through wakeWaiter(), so on a dispatcher whose wake flag has wedged --
    //! exactly what WakeSurvivesAForeignMessageLoopDrainingTheQueue below tests for -- quit() sets
    //! the exit flag and the loop never notices. WM_NULL goes straight to the message queue, which
    //! is the one thing that still ends MsgWaitForMultipleObjectsEx, so a broken build fails here
    //! instead of hanging the suite.
    class Watchdog
    {
    public:
        explicit Watchdog
            (
            int aMs   //!< Milliseconds to allow before quitting the application.
            )
            : mLoopThreadId( GetCurrentThreadId() )
        {
            mThread = std::thread( [this, aMs]()
                {
                    for( int waited = 0; waited < aMs; waited += kSliceMs )
                    {
                        if( mCancelled.load() )
                        {
                            return;
                        }
                        std::this_thread::sleep_for( std::chrono::milliseconds( kSliceMs ) );
                    }
                    mFired.store( true );
                    CoreApplication::quit();
                    PostThreadMessage( mLoopThreadId, WM_NULL, 0, 0 );
                } );
        }

        ~Watchdog()
        {
            mCancelled.store( true );
            mThread.join();
        }

        Watchdog
            (
            const Watchdog&
            ) = delete;

        Watchdog& operator=
            (
            const Watchdog&
            ) = delete;

        //! True if the deadline expired and the watchdog, rather than the test, stopped the loop.
        bool fired() const
        {
            return mFired.load();
        }

    private:
        //! How long each sleep slice is, and so how promptly a cancel is noticed.
        static constexpr int kSliceMs = 5;

        DWORD             mLoopThreadId;            //!< Thread whose message queue to poke on firing.
        std::thread       mThread;                  //!< Runs the deadline.
        std::atomic<bool> mCancelled { false };     //!< Set by the destructor to end mThread early.
        std::atomic<bool> mFired { false };         //!< Set when the deadline expired and quit ran.
    };
}

//! Verifies a real window message wakes the blocked loop and reaches its window procedure.
//!
//! This is mission stage 5 on Windows: the loop is asleep in MsgWaitForMultipleObjectsEx with no
//! deadline, and a message posted by something else in the process -- another component, or the OS
//! itself -- has to bring it back and be dispatched to the window that owns it. Nothing but
//! DispatchMessage in processPlatformEvents() can make the window procedure run.
TEST( EventDispatcherWin32Test, PostedWindowMessageIsDispatchedToItsWindowProc )
{
    CoreApplication app;
    auto            dispatcher = currentWin32Dispatcher();
    ASSERT_NE( dispatcher, nullptr ) << "the main thread should be running EventDispatcherWin32";

    TestMessageWindow window;
    ASSERT_NE( window.handle(), nullptr );

    Thread* mainThread = Thread::currentThread();

    // Posted from another thread, once the loop is definitely blocked -- the same shape as the
    // Linux sibling signalling a descriptor from outside the loop.
    std::thread poster( [&window]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            PostMessage( window.handle(), kTestMessage, 1234, 0 );
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            CoreApplication::quit();
        } );

    Watchdog watchdog( 5000 );

    EXPECT_EQ( app.exec(), 0 );
    poster.join();

    EXPECT_FALSE( watchdog.fired() ) << "the loop had to be stopped by the watchdog.";
    EXPECT_EQ( window.countOf( kTestMessage ), 1 )
        << "a message posted to a window on the loop's own thread never reached its window "
        "procedure; processPlatformEvents() is not dispatching messages it did not send itself.";
    EXPECT_EQ( window.firstParamOf( kTestMessage ), static_cast<WPARAM>( 1234 ) );
    EXPECT_EQ( window.lastRanOn(), mainThread )
        << "a window procedure must run on the thread that owns the window.";
}

//! Verifies a key-down message is translated into a character message.
//!
//! TranslateMessage() and DispatchMessage() sit on the same line of processPlatformEvents() and
//! removing either leaves the other looking correct, so they need separate tests. Only
//! TranslateMessage can turn a posted WM_KEYDOWN into the WM_CHAR that text input depends on; it
//! posts that WM_CHAR back onto the same thread's queue, which the same drain then dispatches.
TEST( EventDispatcherWin32Test, KeyDownIsTranslatedIntoACharacterMessage )
{
    CoreApplication app;
    ASSERT_NE( currentWin32Dispatcher(), nullptr );

    TestMessageWindow window;
    ASSERT_NE( window.handle(), nullptr );

    // Scan code in bits 16-23 and a repeat count of 1, as a real key-down carries. TranslateMessage
    // consults the scan code, so a bare wParam is not enough to rely on.
    const UINT   scanCode = MapVirtualKey( 'A', MAPVK_VK_TO_VSC );
    const LPARAM keyParam = static_cast<LPARAM>( 1 | ( scanCode << 16 ) );

    std::thread poster( [&window, keyParam]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            PostMessage( window.handle(), WM_KEYDOWN, 'A', keyParam );
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            CoreApplication::quit();
        } );

    Watchdog watchdog( 5000 );

    EXPECT_EQ( app.exec(), 0 );
    poster.join();

    EXPECT_FALSE( watchdog.fired() ) << "the loop had to be stopped by the watchdog.";
    ASSERT_EQ( window.countOf( WM_KEYDOWN ), 1 ) << "the key-down itself was never dispatched.";
    EXPECT_GE( window.countOf( WM_CHAR ), 1 )
        << "a dispatched WM_KEYDOWN produced no WM_CHAR, so TranslateMessage() is not being "
        "called; keyboard input would reach a native handler as key codes and never as text.";

    const WPARAM character = window.firstParamOf( WM_CHAR );
    EXPECT_TRUE( character == 'a' || character == 'A' )
        << "WM_CHAR carried " << character << " rather than the letter that was pressed.";
}

//! Verifies WM_QUIT stops a dispatch pass without stopping the process.
//!
//! Pins which pass it stops, because that is not the pass one would guess.
//! processPlatformEvents() runs from processEvents() *after* both of its interrupt checks and after
//! the event batch has been taken, so the interrupt this branch raises cannot affect the pass that
//! saw the message: that pass runs its whole batch and reports success. The pass after it returns
//! false without doing anything, and work queued in between waits one further pass.
//!
//! Written against processEvents() directly rather than exec(), so each pass is observed on its
//! own. See WmQuitDoesNotStopAnExecLoop for what that adds up to in a running loop.
TEST( EventDispatcherWin32Test, WmQuitEndsTheFollowingPassAndNotTheProcess )
{
    CoreApplication app;
    auto            dispatcher = currentWin32Dispatcher();
    ASSERT_NE( dispatcher, nullptr );

    std::atomic<bool> firstTaskRan { false };
    std::atomic<bool> secondTaskRan { false };

    CoreApplication::post( [&firstTaskRan]()
        {
            firstTaskRan.store( true );
        } );

    // A thread message, which PeekMessage( hwnd = nullptr ) picks up exactly as it would one posted
    // by any other component in the process.
    PostQuitMessage( 0 );

    EXPECT_TRUE( dispatcher->processEvents() )
        << "the pass that saw WM_QUIT should still have dispatched its own batch.";
    EXPECT_TRUE( firstTaskRan.load() )
        << "WM_QUIT discarded work that was already in the batch being dispatched.";

    CoreApplication::post( [&secondTaskRan]()
        {
            secondTaskRan.store( true );
        } );

    // ASSERT rather than EXPECT, and this is the reason: if either of these is wrong then the
    // second task has already been dispatched, and the final pass below would find an empty queue,
    // no timers, and block in MsgWaitForMultipleObjectsEx forever. A test that hangs reports
    // nothing; stopping here reports which expectation broke.
    ASSERT_FALSE( dispatcher->processEvents() )
        << "the pass after the one that saw WM_QUIT should return false; the WM_QUIT branch is not "
        "interrupting anything.";
    ASSERT_FALSE( secondTaskRan.load() )
        << "the interrupted pass ran work anyway, so it did not return where it is expected to.";

    // Nothing was lost: the deferred work runs on the pass after that, and the dispatcher is usable
    // again, because interrupt() is consumed rather than merely tested.
    EXPECT_TRUE( dispatcher->processEvents() );
    EXPECT_TRUE( secondTaskRan.load() );
}

//! Verifies WM_QUIT does not stop a running event loop -- the deliberate divergence from Qt.
//!
//! Qt quits the application here: qeventdispatcher_win.cpp calls
//! `QCoreApplication::instance()->quit()` and returns false from processEvents(). This dispatcher
//! does not, because it may belong to a worker thread, and ending the process because one worker's
//! queue saw WM_QUIT would be action at a distance. What it does instead is interrupt(), and
//! Thread::exec() loops on its own mExiting flag without consulting what processEvents() returned
//! -- so the practical effect of WM_QUIT on a running loop is one wasted pass.
//!
//! This test pins that, rather than asserting it is right. If the divergence is ever revisited,
//! this is the test that has to change, and it names what changing it would mean.
TEST( EventDispatcherWin32Test, WmQuitDoesNotStopAnExecLoop )
{
    CoreApplication app;
    ASSERT_NE( currentWin32Dispatcher(), nullptr );

    std::atomic<bool> timerRan { false };

    PostQuitMessage( 0 );

    // Fires well after the WM_QUIT has been drained. It can only run if the loop outlived it.
    Timer stopper;
    stopper.setSingleShot( true );
    Object::connect( stopper.getTimeout(), &stopper, [&timerRan]()
        {
            timerRan.store( true );
            CoreApplication::quit();
        }, ConnectionType::Direct );
    stopper.start( 80 );

    Watchdog watchdog( 5000 );

    EXPECT_EQ( app.exec(), 0 );

    EXPECT_FALSE( watchdog.fired() ) << "the loop had to be stopped by the watchdog.";
    EXPECT_TRUE( timerRan.load() )
        << "the loop stopped when it saw WM_QUIT. That is Qt's behaviour, but not this "
        "dispatcher's: it interrupts a pass and leaves the decision to quit with the owning "
        "Thread. If this was changed on purpose, this test is the record of what changed.";
}

//! Verifies a worker thread dispatches the messages of a window created on that worker.
//!
//! Windows delivers a message to the queue of the thread that created the window, which is why the
//! dispatcher builds its own message-only window on the loop's thread rather than sharing one. A
//! worker therefore has to service its own messages, on its own thread, with no help from the main
//! loop -- and nothing in the suite has ever created a second window to check it.
TEST( EventDispatcherWin32Test, AWorkerThreadDispatchesTheMessagesOfItsOwnWindow )
{
    CoreApplication app;

    Thread worker;
    worker.start();

    // isRunning() is set by start() before the run body creates the dispatcher, so wait on the
    // dispatcher itself.
    for( int i = 0; i < 500 && worker.eventDispatcher() == nullptr; ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    ASSERT_NE( worker.eventDispatcher(), nullptr );

    // Created on the worker, because a window belongs to its creating thread.
    std::unique_ptr<TestMessageWindow> window;
    std::atomic<bool>                  windowReady { false };
    ASSERT_TRUE( worker.post( [&window, &windowReady]()
        {
            window = std::unique_ptr<TestMessageWindow>( new TestMessageWindow() );
            windowReady.store( true );
        } ) );

    for( int i = 0; i < 500 && !windowReady.load(); ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    ASSERT_TRUE( windowReady.load() );
    ASSERT_NE( window->handle(), nullptr );

    EXPECT_TRUE( PostMessage( window->handle(), kTestMessage, 99, 0 ) != FALSE );

    for( int i = 0; i < 500 && window->countOf( kTestMessage ) == 0; ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

    EXPECT_EQ( window->countOf( kTestMessage ), 1 )
        << "a message posted to a worker's window was never dispatched; the worker's loop is not "
        "servicing its own message queue.";
    EXPECT_EQ( window->lastRanOn(), &worker )
        << "the worker's window procedure ran somewhere other than the worker.";

    // Destroyed on its own thread too: DestroyWindow() from another one fails.
    std::atomic<bool> windowGone { false };
    ASSERT_TRUE( worker.post( [&window, &windowGone]()
        {
            window.reset();
            windowGone.store( true );
        } ) );
    for( int i = 0; i < 500 && !windowGone.load(); ++i )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

    worker.quit();
    EXPECT_TRUE( worker.wait( 5000 ) );
}

//! Verifies the loop can still be woken after a foreign message loop has drained the queue.
//!
//! wakeWaiter() posts its wakeup message at most once, and leaves mWakePending set until something
//! consumes it. processPlatformEvents() is not the only consumer on this thread: MessageBox(), a
//! modal dialog, a menu or drag loop, a COM modal loop, or any third-party GetMessage loop will
//! take that message off the queue and dispatch it. If nothing clears the flag on that path, it
//! stays set forever, every later wakeWaiter() returns early without posting, and the loop can
//! never be woken again -- including by quit(), which wakes through the same path, so the
//! application cannot even be shut down. That was R34.
//!
//! The fix is the window procedure clearing the flag, so it does not matter who dispatches the
//! message. This test drives the whole sequence: wake, foreign drain, then a fresh post that only
//! arrives if the collapse was re-armed.
TEST( EventDispatcherWin32Test, WakeSurvivesAForeignMessageLoopDrainingTheQueue )
{
    CoreApplication app;
    auto            dispatcher = currentWin32Dispatcher();
    ASSERT_NE( dispatcher, nullptr );

    // 1. Cause a wakeup, so a wakeup message is queued and the collapse flag is set.
    std::atomic<bool> firstRan { false };
    CoreApplication::post( [&firstRan]()
        {
            firstRan.store( true );
        } );

    // 2. The foreign loop. This is the only part of the test that is not our own code's doing, and
    //    it is deliberately the plainest possible drain -- exactly what a modal dialog runs.
    MSG message;
    int drained = 0;
    while( PeekMessage( &message, nullptr, 0, 0, PM_REMOVE ) != FALSE )
    {
        TranslateMessage( &message );
        DispatchMessage( &message );
        ++drained;
    }
    EXPECT_GE( drained, 1 ) << "no wakeup message was queued, so this test proved nothing.";

    // 3. Let the dispatcher run the work it already had. The queue is not empty, so this pass does
    //    not block and finds no message of its own to clear the flag with.
    dispatcher->processEvents();
    EXPECT_TRUE( firstRan.load() );

    // 4. The question: does a new post still reach a blocked loop?
    std::atomic<bool> secondRan { false };
    std::thread       poster( [&secondRan]()
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            CoreApplication::post( [&secondRan]()
                {
                    secondRan.store( true );
                    CoreApplication::quit();
                } );
        } );

    Watchdog watchdog( 5000 );

    EXPECT_EQ( app.exec(), 0 );
    poster.join();

    EXPECT_TRUE( secondRan.load() )
        << "a task posted after a foreign message loop drained the queue never ran: the wake "
        "collapse flag was left set, so wakeWaiter() stopped posting and the loop slept through "
        "it.";
    EXPECT_FALSE( watchdog.fired() )
        << "the loop had to be stopped by the watchdog's raw thread message, which means quit() "
        "could not wake it either -- the dispatcher was wedged, not merely slow.";
}

//! Verifies an idle loop with a window consumes essentially no CPU -- stage 5's "no 100% cpu-spin".
//!
//! The Windows shape of this is its own risk rather than a copy of the Linux one.
//! MsgWaitForMultipleObjectsEx is asked for QS_ALLINPUT with MWMO_INPUTAVAILABLE, which returns for
//! input already sitting in the queue -- so any message the drain fails to remove makes the wait
//! return immediately, forever, at full CPU. A window whose procedure is reached only through
//! DispatchMessage is what makes that reachable here.
TEST( EventDispatcherWin32Test, IdleLoopWithAWindowDoesNotSpin )
{
    CoreApplication app;
    ASSERT_NE( currentWin32Dispatcher(), nullptr );

    TestMessageWindow window;
    ASSERT_NE( window.handle(), nullptr );

    // One message before the measurement, so the loop has drained and dispatched something and is
    // idling afterwards rather than never having woken at all.
    ASSERT_TRUE( PostMessage( window.handle(), kTestMessage, 0, 0 ) != FALSE );

    // Here the deadline is the point of the test rather than a safety net: the loop is meant to sit
    // idle for the whole of it, and this is what ends it.
    constexpr int kRunMs = 300;
    Watchdog      watchdog( kRunMs );

    const double cpuBefore  = TestSupport::processCpuSeconds();
    const auto   wallBefore = std::chrono::steady_clock::now();

    EXPECT_EQ( app.exec(), 0 );

    const double cpuSeconds = TestSupport::processCpuSeconds() - cpuBefore;
    const double wallSeconds
        = std::chrono::duration<double>( std::chrono::steady_clock::now() - wallBefore ).count();

    ASSERT_GT( wallSeconds, 0.1 ) << "the loop returned far too early to have blocked at all";
    EXPECT_EQ( window.countOf( kTestMessage ), 1 );
    EXPECT_LT( cpuSeconds / wallSeconds, 0.1 )
        << "an idle loop burned " << cpuSeconds << "s of CPU over " << wallSeconds
        << "s of wall time (ratio " << ( cpuSeconds / wallSeconds )
        << "); MsgWaitForMultipleObjectsEx is returning immediately instead of blocking.";
}

#endif // _WIN32
