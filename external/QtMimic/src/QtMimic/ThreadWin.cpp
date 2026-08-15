//! @file
//!
//! Windows-specific half of QtMimic::Thread: native OS thread creation, priority, and join.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic/Thread.hpp"

#include <cstdio>

#include <windows.h>
// _beginthreadex() rather than CreateThread(): it is the same OS thread either way, but it
// also initialises and, on return, releases the CRT's per-thread state. Using it
// unconditionally is correct for both static and dynamic CRT flavours, and its signature
// matches the entry point exactly, so no function-pointer cast is needed.
#include <process.h>

namespace QtMimic
{

    //! Creates the OS thread, already at mPriority when it executes its first instruction.
    //!
    //! Created suspended, given its priority, then resumed. A new thread otherwise starts at
    //! normal priority, so a low-priority thread that starts another low-priority thread would
    //! be preempted by its own child for the window between creation and the priority landing.
    //! Called by start() with mPriorityMutex held.
    void Thread::startPlatformSpecific()
    {
        const unsigned int flags = CREATE_SUSPENDED;
        const auto handle = _beginthreadex( nullptr, 0, &threadEntry, this, flags, nullptr );
        if( handle == 0 )
        {
            std::fprintf( stderr, "Thread::start: failed to create thread\n" );
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
            std::fprintf( stderr, "Thread::start: failed to resume new thread\n" );
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
            std::fprintf( stderr, "Thread: failed to set thread priority\n" );
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
            std::fprintf( stderr, "Thread::wait: thread wait failure\n" );
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
