// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Win32 event dispatcher implementation.

#include "QtLikeSignal/EventDispatcherWin32.hpp"

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <tchar.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace QtLikeSignal
{
    namespace
    {
        //! Private wakeup message, posted by wakeWaiter() and swallowed by processPlatformEvents().
        //!
        //! WM_USER is the first value reserved for an application's own use on its own windows,
        //! which is exactly what this is. Qt uses WM_USER + 1 for the same purpose.
        constexpr UINT kWakeUpMessage = WM_USER + 1;

        //! Clears the owning dispatcher's wake flag when the wakeup message is dispatched.
        //!
        //! This exists because the flag cannot be cleared only where we drain. wakeWaiter() posts
        //! kWakeUpMessage at most once and leaves mWakePending set until somebody consumes it, and
        //! processPlatformEvents() is not the only thing on this thread that can consume it: any
        //! foreign message loop -- MessageBox(), a modal dialog, a menu or drag loop, a COM modal
        //! loop, any third-party GetMessage loop -- will PeekMessage it off the queue and
        //! DispatchMessage it here. With DefWindowProc in this slot, which is what used to be
        //! here, the message was discarded and the flag stayed set forever, so every later
        //! wakeWaiter() returned early without posting and the loop could never be woken again.
        //! quit() went with it, since that wakes through the same path, so the application also
        //! could not be shut down. That was R34, and this is its fix.
        //!
        //! Qt does the same thing in the same place: qeventdispatcher_win.cpp handles
        //! WM_QT_SENDPOSTEDEVENTS in qt_internal_proc() and clears its `wakeUps` there, under the
        //! comment "if the window procedure was invoked by the foreign event loop (e.g. from the
        //! native modal dialog)". Attaching the clear to the *message* rather than to the drain
        //! that happened to consume it is what makes it correct whoever dispatches it.
        //!
        //! What is stored in GWLP_USERDATA is the flag itself, not the dispatcher, so this stays a
        //! plain function in an anonymous namespace with no access to EventDispatcherWin32's
        //! privates and no Windows types in the header.
        LRESULT CALLBACK messageWindowProc
            (
            HWND aWindow,     //!< Window the message was dispatched to.
            UINT aMessage,    //!< Message id.
            WPARAM aWParam,   //!< First message parameter.
            LPARAM aLParam    //!< Second message parameter.
            )
        {
            if( aMessage == kWakeUpMessage )
            {
                // Null until the constructor has stored the flag: CreateWindowEx sends WM_NCCREATE
                // and WM_CREATE through here before it returns the handle we would set it on.
                auto* wakePending = reinterpret_cast<std::atomic<bool>*>(
                    GetWindowLongPtr( aWindow, GWLP_USERDATA ) );
                if( wakePending != nullptr )
                {
                    wakePending->store( false );
                }
                return 0;
            }

            return DefWindowProc( aWindow, aMessage, aWParam, aLParam );
        }

        //! Class name of the hidden message-only window, unique to this copy of the library.
        //!
        //! The address of messageWindowProc is appended for the reason Qt appends
        //! quintptr(qt_internal_proc) to its own: "make sure that multiple Qt's can coexist in the
        //! same process". RegisterClass is process-wide, so two copies of this library in one
        //! process -- a static library linked into both an executable and a DLL -- would otherwise
        //! collide on one name, and the loser would silently create its windows against the
        //! winner's class. That was harmless while the class had no window procedure of its own.
        //! It stopped being harmless the moment the one above started carrying the R34 fix, because
        //! the surviving registration decides which copy's code runs.
        //!
        //! Built as TCHAR, like every string this file hands to Win32, because the calls here are
        //! the generic-text ones -- RegisterClass, not RegisterClassA -- and those take LPCTSTR.
        //! A TCHAR ostringstream rather than a printf into a fixed buffer: it needs no size guess,
        //! and it avoids the _sntprintf family, whose truncation behaviour differs between the two
        //! toolchains that build this file.
        const TCHAR* windowClassName()
        {
            static const std::basic_string<TCHAR> sName = []()
                {
                    std::basic_ostringstream<TCHAR> name;
                    name << TEXT( "QtLikeSignal_EventDispatcher_" )
                         << reinterpret_cast<const void*>( &messageWindowProc );
                    return name.str();
                }();
            return sName.c_str();
        }

        //! Registers the window class once per process.
        //!
        //! Every dispatcher on every thread shares one class; RegisterClass is process-wide, so
        //! registering per-instance would fail with ERROR_CLASS_ALREADY_EXISTS after the first.
        //! call_once also makes it safe for two threads to start their loops simultaneously.
        void ensureWindowClassRegistered()
        {
            static std::once_flag sOnce;
            std::call_once( sOnce, []()
                {
                    WNDCLASS windowClass {};
                    windowClass.lpfnWndProc   = &messageWindowProc;
                    windowClass.hInstance     = GetModuleHandle( nullptr );
                    windowClass.lpszClassName = windowClassName();
                    if( RegisterClass( &windowClass ) == 0 )
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
        const HWND window = CreateWindowEx(
            0,
            windowClassName(),
            windowClassName(),
            0,
            0, 0, 0, 0,
            HWND_MESSAGE,
            nullptr,
            GetModuleHandle( nullptr ),
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
        else
        {
            // How messageWindowProc() finds the flag to clear. Stored after creation rather than
            // passed through CreateWindowEx's lpParam because the window procedure only needs it
            // for kWakeUpMessage, which cannot arrive before this line: nothing can post to a
            // handle that has not been returned yet.
            SetWindowLongPtr( window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>( &mWakePending ) );
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

        if( PostMessage( static_cast<HWND>( mMessageWindow ), kWakeUpMessage, 0, 0 ) == FALSE )
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
        while( PeekMessage( &message, nullptr, 0, 0, PM_REMOVE ) != FALSE )
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
                // Stop a dispatch pass rather than the whole application. Qt quits the
                // QCoreApplication here -- qeventdispatcher_win.cpp calls
                // QCoreApplication::instance()->quit() and returns false -- but this dispatcher may
                // belong to a worker thread, and tearing down the process because one worker's
                // queue saw WM_QUIT would be a surprising amount of action at a distance.
                //
                // Which pass it stops is not the one this reads like, and the earlier wording here
                // ("interrupt() ends the current pass") was wrong. processPlatformEvents() runs
                // from processEvents() *after* both of its interrupt checks and after the event
                // batch has been taken, so the pass that saw WM_QUIT dispatches its whole batch and
                // reports success; the pass after it is the one that returns false. Thread::exec()
                // then loops on its own mExiting flag without consulting that return value, so on a
                // running loop the whole effect of WM_QUIT is one idle pass.
                //
                // Both halves of that are pinned, in QtLikeSignal-test-eventdispatcher-win32.cpp:
                // WmQuitEndsTheFollowingPassAndNotTheProcess and WmQuitDoesNotStopAnExecLoop.
                interrupt();
                return;
            }

            TranslateMessage( &message );
            DispatchMessage( &message );
        }
    }
}
