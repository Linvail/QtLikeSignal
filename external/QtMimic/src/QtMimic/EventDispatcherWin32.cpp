//! @file
//!
//! Win32 event dispatcher implementation.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic/EventDispatcherWin32.hpp"

#include <cstdio>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace QtMimic
{
    namespace
    {
        //! Private wakeup message, posted by wakeWaiter() and swallowed by processPlatformEvents().
        //!
        //! WM_USER is the first value reserved for an application's own use on its own windows,
        //! which is exactly what this is. Qt uses WM_USER + 1 for the same purpose.
        constexpr UINT kWakeUpMessage = WM_USER + 1;

        //! Class name of the hidden message-only window.
        const char* const kWindowClassName = "QtMimic_EventDispatcher";

        //! Registers the window class once per process.
        //!
        //! Every dispatcher on every thread shares one class; RegisterClassA is process-wide, so
        //! registering per-instance would fail with ERROR_CLASS_ALREADY_EXISTS after the first.
        //! call_once also makes it safe for two threads to start their loops simultaneously.
        void ensureWindowClassRegistered()
        {
            static std::once_flag sOnce;
            std::call_once( sOnce, []()
                {
                    WNDCLASSA windowClass {};
                    windowClass.lpfnWndProc   = &DefWindowProcA;
                    windowClass.hInstance     = GetModuleHandleA( nullptr );
                    windowClass.lpszClassName = kWindowClassName;
                    if( RegisterClassA( &windowClass ) == 0 )
                    {
                        std::fprintf( stderr,
                            "EventDispatcherWin32: RegisterClass() failed (%lu)\n",
                            GetLastError() );
                    }
                } );
        }
    }

    //! Constructs the dispatcher and its message-only window on the calling thread.
    EventDispatcherWin32::EventDispatcherWin32()
    {
        ensureWindowClassRegistered();

        // HWND_MESSAGE creates a message-only window: never visible, never enumerated, and not a
        // child of the desktop -- it exists purely to own a message queue endpoint we can post to.
        const HWND window = CreateWindowExA(
            0,
            kWindowClassName,
            kWindowClassName,
            0,
            0, 0, 0, 0,
            HWND_MESSAGE,
            nullptr,
            GetModuleHandleA( nullptr ),
            nullptr );

        if( window == nullptr )
        {
            // Not fatal: waitForEvents() falls back to the inherited condition-variable wait, which
            // still delivers our own events correctly. Only OS messages stop being serviced, so say
            // so rather than degrading silently.
            std::fprintf( stderr,
                "EventDispatcherWin32: CreateWindowEx() failed (%lu); falling back to the "
                "cross-platform wait, so OS messages will not be dispatched\n", GetLastError() );
        }

        mMessageWindow = window;
    }

    //! Destroys the dispatcher and its message-only window.
    EventDispatcherWin32::~EventDispatcherWin32()
    {
        if( mMessageWindow != nullptr )
        {
            DestroyWindow( static_cast<HWND>( mMessageWindow ) );
            mMessageWindow = nullptr;
        }
    }

    //! Blocks on the thread's message queue until a message arrives or @p aTimeoutMs elapses.
    void EventDispatcherWin32::waitForEvents
        (
        std::unique_lock<std::mutex>& aLock,  //!< Lock on mMutex, held on entry and on return.
        int aTimeoutMs                          //!< Milliseconds to wait, or -1 to wait indefinitely.
        )
    {
        if( mMessageWindow == nullptr )
        {
            // No window, so nothing can post a wakeup message to this thread. Use the inherited
            // condition-variable wait instead of blocking on a queue nobody can signal.
            EventDispatcherDefault::waitForEvents( aLock, aTimeoutMs );
            return;
        }

        // Block with the lock released: waiting on the message queue cannot hold a std::mutex, and
        // holding it would stop every other thread from posting -- including the post meant to wake
        // us.
        aLock.unlock();

        const DWORD timeout = ( aTimeoutMs < 0 )
                              ? INFINITE
                              : static_cast<DWORD>( aTimeoutMs );

        // Returns as soon as any message is queued -- our wakeup, input, or anything else. Zero
        // handles: the message queue is the only thing being waited on.
        //
        // MWMO_INPUTAVAILABLE makes it return for input already sitting in the queue rather than
        // only for newly-arrived input; without it a message that arrived between the PeekMessage
        // drain and this call would be slept through until the next one. MWMO_ALERTABLE lets APCs
        // (and therefore alertable I/O completion) run.
        MsgWaitForMultipleObjectsEx( 0, nullptr, timeout, QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE );

        aLock.lock();
    }

    //! Wakes a thread blocked on the message queue. Thread-safe and non-blocking.
    void EventDispatcherWin32::wakeWaiter()
    {
        // Keep the base behaviour too: waitForEvents() falls back to the condition variable when the
        // window could not be created, and a waiter there still has to be notified.
        EventDispatcherDefault::wakeWaiter();

        if( mMessageWindow == nullptr )
        {
            return;
        }

        // Only the first wake of a batch posts a message; processPlatformEvents() clears the flag
        // once the loop has consumed it. The thread message queue has a fixed capacity, so
        // collapsing a burst matters here rather than being a mere optimisation.
        if( mWakePending.exchange( true ) )
        {
            return;
        }

        if( PostMessageA( static_cast<HWND>( mMessageWindow ), kWakeUpMessage, 0, 0 ) == FALSE )
        {
            // The post failed, so no wakeup is coming and the flag would wedge the fast path shut
            // forever. Clear it so the next caller retries rather than assuming a wake is pending.
            mWakePending.store( false );
        }
    }

    //! Drains and dispatches the thread's pending OS messages.
    void EventDispatcherWin32::processPlatformEvents()
    {
        if( mMessageWindow == nullptr )
        {
            return;
        }

        MSG message;
        while( PeekMessageA( &message, nullptr, 0, 0, PM_REMOVE ) != FALSE )
        {
            if( message.message == kWakeUpMessage
                && message.hwnd == static_cast<HWND>( mMessageWindow ) )
            {
                // Ours, not the application's: consume it and re-arm the collapsing flag. Cleared
                // only after the message has actually been taken off the queue, so a wakeWaiter()
                // racing this point either posted before the peek (already accounted for) or finds
                // the flag clear and posts its own -- no wakeup can be dropped.
                mWakePending.store( false );
                continue;
            }

            if( message.message == WM_QUIT )
            {
                // Stop this loop rather than the whole application. Qt quits the QCoreApplication
                // here, but this dispatcher may belong to a worker thread, and tearing down the
                // process because one worker's queue saw WM_QUIT would be a surprising amount of
                // action at a distance. interrupt() ends the current pass; the owning Thread's own
                // exit flag still decides whether the loop resumes.
                interrupt();
                return;
            }

            TranslateMessage( &message );
            DispatchMessageA( &message );
        }
    }
}
