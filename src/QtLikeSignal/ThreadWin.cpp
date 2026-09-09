// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Windows-specific half of QtLikeSignal::Thread: native OS thread creation, priority, and join.

#include "QtLikeSignal/Thread.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"

#include <windows.h>
// _beginthreadex() rather than CreateThread(): it is the same OS thread either way, but it
// also initialises and, on return, releases the CRT's per-thread state. Qt reaches for it
// only in static-CRT builds and uses CreateThread elsewhere; using it unconditionally costs
// nothing, is correct for both CRT flavours, and its signature matches the entry point
// exactly, so no function-pointer cast is needed.
#include <process.h>

#include <string>

namespace QtLikeSignal
{
    namespace
    {
        //! Signature of SetThreadDescription, resolved at run time. See threadDescriptionApi().
        using SetThreadDescriptionFn = HRESULT ( WINAPI* )( HANDLE, PCWSTR );

        //! Signature of GetThreadDescription, resolved at run time. See threadDescriptionApi().
        using GetThreadDescriptionFn = HRESULT ( WINAPI* )( HANDLE, PWSTR* );

        //! The two thread-naming entry points, or null where the system does not have them.
        //!
        //! **Resolved with GetProcAddress rather than linked**, because they arrived in Windows 10
        //! version 1607. Calling them directly would put a load-time dependency on that version
        //! into every program that links this library, and a program that never names a thread
        //! would fail to start on an older system for a diagnostic it does not use.
        //!
        //! Kernel32 is where the loader finds them; the implementation lives in KernelBase and is
        //! forwarded. Looked up once -- the module is already loaded in every process, so no
        //! reference is taken and none is released.
        struct ThreadDescriptionApi
        {
            SetThreadDescriptionFn mSet;   //!< SetThreadDescription, or null.
            GetThreadDescriptionFn mGet;   //!< GetThreadDescription, or null.
        };

        //! Gets the resolved entry points, looking them up on the first call.
        //!
        //! A function-local static, so the lookup happens once and the standard guarantees the
        //! initialisation is thread-safe without a lock of ours.
        const ThreadDescriptionApi& threadDescriptionApi()
        {
            static const ThreadDescriptionApi api = []()
                {
                    ThreadDescriptionApi resolved { nullptr, nullptr };
                    const HMODULE kernel = GetModuleHandleW( L"kernel32.dll" );
                    if( kernel != nullptr )
                    {
                        resolved.mSet = reinterpret_cast<SetThreadDescriptionFn>(
                            GetProcAddress( kernel, "SetThreadDescription" ) );
                        resolved.mGet = reinterpret_cast<GetThreadDescriptionFn>(
                            GetProcAddress( kernel, "GetThreadDescription" ) );
                    }
                    return resolved;
                }();
            return api;
        }

        //! Converts UTF-8 to the UTF-16 Windows wants. Returns an empty string if it cannot.
        std::wstring toWide
            (
            const std::string& aText  //!< UTF-8 text to convert.
            )
        {
            if( aText.empty() )
            {
                return std::wstring();
            }

            const int length = MultiByteToWideChar( CP_UTF8, 0, aText.c_str(),
                static_cast<int>( aText.size() ), nullptr, 0 );
            if( length <= 0 )
            {
                return std::wstring();
            }

            std::wstring wide( static_cast<std::size_t>( length ), L'\0' );
            MultiByteToWideChar( CP_UTF8, 0, aText.c_str(), static_cast<int>( aText.size() ),
                &wide[0], length );
            return wide;
        }

        //! Converts UTF-16 back to the UTF-8 the rest of this library speaks.
        std::string toUtf8
            (
            const wchar_t* aText  //!< NUL-terminated UTF-16 text, or null.
            )
        {
            if( aText == nullptr || aText[0] == L'\0' )
            {
                return std::string();
            }

            const int length = WideCharToMultiByte( CP_UTF8, 0, aText, -1, nullptr, 0,
                nullptr, nullptr );
            if( length <= 1 )
            {
                return std::string();
            }

            // One less than the count, because that count includes the terminator and a
            // std::string carries its own.
            std::string utf8( static_cast<std::size_t>( length - 1 ), '\0' );
            WideCharToMultiByte( CP_UTF8, 0, aText, -1, &utf8[0], length, nullptr, nullptr );
            return utf8;
        }
    }

    //! Sets the calling thread's name, as a debugger and Task Manager report it.
    void Thread::setNativeName
        (
        const std::string& aName  //!< New name, UTF-8. Empty does nothing.
        )
    {
        if( aName.empty() )
        {
            return;
        }

        const SetThreadDescriptionFn setter = threadDescriptionApi().mSet;
        if( setter == nullptr )
        {
            return;
        }

        const std::wstring wide = toWide( aName );
        if( !wide.empty() )
        {
            // The failure is deliberately not reported. A refused name costs a label in a debugger
            // and nothing else, and a warning here would be noise about a diagnostic.
            setter( GetCurrentThread(), wide.c_str() );
        }
    }

    //! Gets the calling thread's name from the OS, or an empty string.
    std::string Thread::nativeName()
    {
        const GetThreadDescriptionFn getter = threadDescriptionApi().mGet;
        if( getter == nullptr )
        {
            return std::string();
        }

        PWSTR description = nullptr;
        if( FAILED( getter( GetCurrentThread(), &description ) ) || description == nullptr )
        {
            return std::string();
        }

        const std::string name = toUtf8( description );

        // LocalFree and not delete: the buffer comes from the system's local heap, which is what
        // GetThreadDescription documents and the only thing that may release it.
        LocalFree( description );
        return name;
    }

    //! Labels the OS thread this object created. Called from start(), on the calling thread.
    void Thread::applyNativeName
        (
        const std::string& aName  //!< The name to give the OS thread, UTF-8.
        )
    {
        if( aName.empty() || mHandle == nullptr )
        {
            return;
        }

        const SetThreadDescriptionFn setter = threadDescriptionApi().mSet;
        if( setter == nullptr )
        {
            return;
        }

        // SetThreadDescription names any thread by handle, not only the caller, which is what lets
        // this run on the owning thread rather than inside the new one. See the declaration.
        const std::wstring wide = toWide( aName );
        if( !wide.empty() )
        {
            setter( static_cast<HANDLE>( mHandle ), wide.c_str() );
        }
    }

    //! Creates the OS thread, already at mPriority when it executes its first instruction.
    //!
    //! Created suspended, given its priority, then resumed. A new thread otherwise starts at
    //! normal priority, so a low-priority thread that starts another low-priority thread would
    //! be preempted by its own child for the window between creation and the priority landing.
    //! Called by start() with mPriorityMutex held.
    void Thread::startPlatformSpecific()
    {
        // The zero is the stack size, meaning the default the image was linked with.
        const unsigned int flags = CREATE_SUSPENDED;
        const auto handle = _beginthreadex( nullptr, 0, &threadEntry, this, flags, nullptr );
        if( handle == 0 )
        {
            qCCritical( gLogThread ) << "Thread::start: failed to create thread";
            mData->setThreadRunning( false );
            return;
        }

        mHandle = reinterpret_cast<void*>( handle );

        // Unconditional, InheritPriority included: the OS hands out NormalPriority regardless of
        // what the creating thread is running at, so inheriting is something that has to be done
        // rather than something that happens.
        applyPriority( mPriority );

        if( ResumeThread( static_cast<HANDLE>( mHandle ) ) == static_cast<DWORD>( -1 ) )
        {
            qCCritical( gLogThread )
                << "Thread::start: failed to resume new thread";
        }
    }

    //! Entry point handed to _beginthreadex(). Returns 0 always; nothing reads a per-thread
    //! exit code back through wait().
    unsigned int __stdcall Thread::threadEntry
        (
        void* aArg      //!< The Thread that is starting, as a void*.
        )
    {
        static_cast<Thread*>( aArg )->threadBody();
        return 0;
    }

    //! Pushes a priority down to the OS thread.
    //!
    //! Split out so the platform code sits in one place. The caller must hold mPriorityMutex and
    //! must already have established that mHandle is valid, because this uses it.
    void Thread::applyPriority
        (
        Priority aPriority  //!< The priority to apply. InheritPriority is meaningful only on Windows
                            //!< and only from start(), where it means "the priority of the thread
                            //!< calling start()"; UNIX expresses inheritance through the pthread
                            //!< attributes instead and never comes here with it.
        )
    {
        int prio;
        switch( aPriority )
        {
        case IdlePriority:
            prio = THREAD_PRIORITY_IDLE;
            break;

        case LowestPriority:
            prio = THREAD_PRIORITY_LOWEST;
            break;

        case LowPriority:
            prio = THREAD_PRIORITY_BELOW_NORMAL;
            break;

        case NormalPriority:
            prio = THREAD_PRIORITY_NORMAL;
            break;

        case HighPriority:
            prio = THREAD_PRIORITY_ABOVE_NORMAL;
            break;

        case HighestPriority:
            prio = THREAD_PRIORITY_HIGHEST;
            break;

        case TimeCriticalPriority:
            prio = THREAD_PRIORITY_TIME_CRITICAL;
            break;

        case InheritPriority:
        default:
            // Only reachable from start(), where the calling thread is the creating thread, so
            // this really is the priority being inherited. Qt resolves it the same way.
            prio = GetThreadPriority( GetCurrentThread() );
            break;
        }

        if( !SetThreadPriority( static_cast<HANDLE>( mHandle ), prio ) )
        {
            qCWarning( gLogThread ) << "Thread: failed to set thread priority";
        }
    }

    //! Blocks until the event loop has exited and the OS thread has been reaped, or @p aTime
    //! milliseconds have passed. Returns true if the thread finished (or there was nothing to
    //! wait for); false on timeout.
    //!
    //! Thread-safe: WaitForSingleObject() supports any number of concurrent waiters on the same
    //! handle, so this only needs to track who closes it, via mWaiters.
    //!
    //! Deliberately does NOT hold mPriorityMutex across the actual wait: run()'s priority
    //! fix-up (relevant only on the UNIX side, but the code path is shared) needs that same
    //! mutex to get past its very first step, and this thread cannot finish -- and so signal the
    //! handle -- until it does. Holding the mutex across the wait would be a self-inflicted
    //! deadlock against a thread that has barely started.
    bool Thread::wait
        (
        unsigned long aTime  //!< Maximum time to wait in milliseconds; ULONG_MAX blocks indefinitely.
        )
    {
        HANDLE handle = nullptr;
        {
            std::lock_guard<std::mutex> lock( mPriorityMutex );
            handle = static_cast<HANDLE>( mHandle );
            if( !handle )
            {
                // Never started, or already reaped by an earlier wait().
                return true;
            }
            // Registered before the lock is dropped so no other caller can close the handle
            // while this call is inside WaitForSingleObject() on it.
            ++mWaiters;
        }

        // The OS thread object is the wait primitive, as in Qt: it is signalled by the thread
        // ending, which is strictly later than the mHasFinished store at the tail of
        // threadBody(), so a true return always implies isFinished().
        const DWORD timeout = ( aTime == ULONG_MAX ) ? INFINITE : static_cast<DWORD>( aTime );
        const DWORD result = WaitForSingleObject( handle, timeout );
        if( result == WAIT_FAILED )
        {
            qCCritical( gLogThread ) << "Thread::wait: thread wait failure";
        }
        const bool completed = ( result == WAIT_OBJECT_0 );

        {
            std::lock_guard<std::mutex> lock( mPriorityMutex );
            --mWaiters;
            // Only once the thread has actually ended: a timed-out waiter must leave the handle
            // for the run that is still using it.
            if( completed && mWaiters == 0 )
            {
                CloseHandle( handle );
                // Unless start() has already put a new run's handle there, in which case that
                // one is not ours to forget.
                if( static_cast<HANDLE>( mHandle ) == handle )
                {
                    mHandle = nullptr;
                }
            }
        }
        return completed;
    }

}
