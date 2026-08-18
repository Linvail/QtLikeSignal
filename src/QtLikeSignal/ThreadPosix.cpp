// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! POSIX-specific half of QtLikeSignal::Thread: native OS thread creation, priority, and join.


#include "QtLikeSignal/Thread.hpp"

#include <cerrno>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>


// Priority scheduling is an optional part of POSIX. Where it is absent there is no portable
// way to ask for a priority at all, so setPriority() records the value and does nothing else,
// which is what Qt does behind its own QT_HAS_THREAD_PRIORITY_SCHEDULING guard.
#if defined( _POSIX_THREAD_PRIORITY_SCHEDULING )
    #define QT_LIKE_SIGNAL_HAS_THREAD_PRIORITY_SCHEDULING
#endif

namespace QtLikeSignal
{

    #if defined( QT_LIKE_SIGNAL_HAS_THREAD_PRIORITY_SCHEDULING )

    namespace
    {

        //! Maps a Thread priority onto a scheduler policy and priority number.
        //!
        //! This is Qt's mapping from qthread_unix.cpp, including its deliberately coarse scaling: the
        //! divisor is TimeCriticalPriority rather than the span between the lowest and highest values, so
        //! the enum lands on the low end of the platform's range rather than spreading across it. Kept as
        //! Qt has it so behaviour matches. Returns true if a priority could be calculated; false if the
        //! platform would not report a range.
        // The alternative would be a library that claims to mimic QThread and then schedules
        // differently.
        bool calculateUnixPriority
            (
            int aPriority,          //!< The Thread priority to convert.
            int* aSchedPolicy,      //!< In: the thread's current policy. Out: the policy to apply, which
                                    //!< only changes when IdlePriority selects SCHED_IDLE.
            int* aSchedPriority     //!< Out: the priority number to apply under that policy.
            )
        {
            #ifdef SCHED_IDLE
                if( aPriority == Thread::IdlePriority )
                {
                    *aSchedPolicy = SCHED_IDLE;
                    *aSchedPriority = 0;
                    return true;
                }
                const int lowestPriority = Thread::LowestPriority;
            #else
                const int lowestPriority = Thread::IdlePriority;
            #endif
            const int highestPriority = Thread::TimeCriticalPriority;

            const int prioMin = sched_get_priority_min( *aSchedPolicy );
            const int prioMax = sched_get_priority_max( *aSchedPolicy );
            if( prioMin == -1 || prioMax == -1 )
            {
                return false;
            }

            int prio = ( ( aPriority - lowestPriority ) * ( prioMax - prioMin ) /
                highestPriority
                       ) +
                prioMin;
            if( prio < prioMin )
            {
                prio = prioMin;
            }
            if( prio > prioMax )
            {
                prio = prioMax;
            }

            *aSchedPriority = prio;
            return true;
        }      // end calculateUnixPriority()
    }     // namespace

    #endif

    //! Creates the OS thread, already at mPriority when it executes its first instruction, by
    //! passing it in the pthread_create() attributes.
    //!
    //! The one exception is a kernel that refuses the scheduling attributes outright, where the
    //! thread is created inheriting the caller's priority and applies the requested one to
    //! itself as its first action, in run(). Qt falls back the same way. Called by start() with
    //! mPriorityMutex held.
    void Thread::startPlatformSpecific()
    {
        pthread_attr_t attr;
        pthread_attr_init( &attr );

        #if defined( QT_LIKE_SIGNAL_HAS_THREAD_PRIORITY_SCHEDULING )
            if( mPriority != InheritPriority )
            {
                int schedPolicy = 0;
                int prio = 0;
                if( pthread_attr_getschedpolicy( &attr, &schedPolicy ) != 0 )
                {
                    std::fprintf( stderr,
                        "Thread::start: cannot determine default scheduler policy\n" );
                }
                else if( !calculateUnixPriority( mPriority, &schedPolicy, &prio ) )
                {
                    std::fprintf( stderr,
                        "Thread::start: cannot determine scheduler priority range\n" );
                }
                else
                {
                    sched_param sp {};
                    sp.sched_priority = prio;

                    if( pthread_attr_setinheritsched( &attr, PTHREAD_EXPLICIT_SCHED ) != 0 ||
                        pthread_attr_setschedpolicy( &attr, schedPolicy ) != 0 ||
                        pthread_attr_setschedparam( &attr, &sp ) != 0 )
                    {
                        // The attributes were refused. Go back to inheriting and let the thread
                        // apply the priority to itself once it is running, which is Qt's
                        // fallback for exactly this case.
                        pthread_attr_setinheritsched( &attr, PTHREAD_INHERIT_SCHED );
                        mPriorityNeedsReset = true;
                    }
                }
            }
        #endif

        int code = pthread_create( &mThreadId, &attr, &threadEntry, this );
        if( code == EPERM )
        {
            // Not permitted to select those scheduling parameters. Retry inheriting them
            // instead of failing the start outright, as Qt does; the thread runs, just not at
            // the requested priority.
            #if defined( QT_LIKE_SIGNAL_HAS_THREAD_PRIORITY_SCHEDULING )
                pthread_attr_setinheritsched( &attr, PTHREAD_INHERIT_SCHED );

                // The thread now starts at the caller's priority, so it has to apply the requested
                // one to itself as its first action. Without this the request is silently dropped
                // for the whole run, which is what the comment above already promised not to do.
                mPriorityNeedsReset = ( mPriority != InheritPriority );
            #endif
            code = pthread_create( &mThreadId, &attr, &threadEntry, this );
        }

        pthread_attr_destroy( &attr );

        if( code != 0 )
        {
            std::fprintf( stderr, "Thread::start: thread creation error\n" );
            mData->setThreadRunning( false );
            return;
        }

        mJoinable = true;
    }  // end Thread::startPlatformSpecific()

    //! Entry point handed to pthread_create(). Returns nullptr always; nothing is passed back
    //! through pthread_join().
    void* Thread::threadEntry
        (
        void* aArg      //!< The Thread that is starting, as a void*.
        )
    {
        static_cast<Thread*>( aArg )->threadBody();
        return nullptr;
    }

    //! Pushes a priority down to the OS thread.
    //!
    //! Split out so the platform code sits in one place. The caller must hold mPriorityMutex and
    //! must already have established that mThreadId is valid, because this uses it.
    void Thread::applyPriority
        (
        Priority aPriority  //!< The priority to apply. InheritPriority is meaningful only on Windows
                            //!< and only from start(), where it means "the priority of the thread
                            //!< calling start()"; UNIX expresses inheritance through the pthread
                            //!< attributes instead and never comes here with it.
        )
    {
        #if defined( QT_LIKE_SIGNAL_HAS_THREAD_PRIORITY_SCHEDULING )
            int schedPolicy = 0;
            sched_param param {};
            if( pthread_getschedparam( mThreadId, &schedPolicy, &param ) != 0 )
            {
                std::fprintf( stderr,
                    "Thread::setPriority: cannot get scheduler parameters\n" );
                return;
            }

            int prio = 0;
            if( !calculateUnixPriority( aPriority, &schedPolicy, &prio ) )
            {
                std::fprintf( stderr,
                    "Thread::setPriority: cannot determine scheduler priority range\n" );
                return;
            }

            param.sched_priority = prio;
            const int status = pthread_setschedparam( mThreadId, schedPolicy, &param );

            #ifdef SCHED_IDLE
                // SCHED_IDLE is optional and some kernels reject it. Fall back to the lowest
                // priority the thread's existing policy allows, which is as close as we can get.
                //
                // NOTE: this condition deliberately differs from Qt, which tests
                // `status == -1 && errno == EINVAL`. pthread_setschedparam() returns the error
                // number directly and does not touch errno, so Qt's test can never be true and its
                // fallback is dead code. Testing the return value is what actually reaches it.
                if( status == EINVAL && schedPolicy == SCHED_IDLE )
                {
                    if( pthread_getschedparam( mThreadId, &schedPolicy, &param ) == 0 )
                    {
                        param.sched_priority = sched_get_priority_min( schedPolicy );
                        pthread_setschedparam( mThreadId, schedPolicy, &param );
                    }
                }
            #else
                ( void )status;
            #endif
        #else
            // No priority scheduling on this platform; the value is recorded and nothing else.
            ( void )aPriority;
        #endif
    }

    //! Blocks until the event loop has exited and the OS thread has been reaped, or @p aTime
    //! milliseconds have passed. Returns true if the thread finished (or there was nothing to
    //! wait for); false on timeout. Thread-safe.
    //!
    //! pthread_join() has no portable timed form -- pthread_timedjoin_np() is a glibc extension
    //! Qt guards with a configure test -- so the timeout is served by the same condition variable
    //! threadBody() ends by notifying, and the pthread_join() that follows is only ever the
    //! already-finished kind. Windows can pass its timeout straight to WaitForSingleObject().
    //!
    //! Only the caller that actually claims mJoinable does the real pthread_join() -- calling
    //! pthread_join() on the same pthread_t concurrently from two threads is undefined, unlike
    //! WaitForSingleObject() on Windows, which is fine with multiple waiters. A second
    //! concurrent caller simply finds nothing left to claim and returns.
    //!
    //! Deliberately does NOT hold mPriorityMutex across the actual wait: run()'s priority fix-up
    //! needs that same mutex to get past its very first step, and this thread cannot finish --
    //! and so let the wait return -- until it does. Holding the mutex across the wait would be a
    //! self-inflicted deadlock against a thread that has barely started.
    bool Thread::wait
        (
        unsigned long aTime  //!< Maximum time to wait in milliseconds; ULONG_MAX blocks indefinitely.
        )
    {
        {
            std::lock_guard<std::mutex> lock( mPriorityMutex );
            if( !mJoinable )
            {
                // Never started, or already reaped by an earlier wait().
                return true;
            }
        }

        {
            std::unique_lock<std::mutex> lock( mWaitMutex );
            const auto hasFinished = [this]
                {
                    return mHasFinished.load();
                };

            // The untimed overload rather than wait_for() with a huge duration: what a wait_for()
            // has to do with it is add it to the clock's current time, which overflows.
            if( aTime == ULONG_MAX )
            {
                mWaitCv.wait( lock, hasFinished );
            }
            else if( !mWaitCv.wait_for( lock, std::chrono::milliseconds( aTime ), hasFinished ) )
            {
                return false;
            }
        }

        std::lock_guard<std::mutex> lock( mPriorityMutex );
        if( mJoinable )
        {
            // Under the lock and gated on the flag because two threads joining the same thread is
            // undefined; whichever gets here second finds nothing left to reap.
            pthread_join( mThreadId, nullptr );
            mJoinable = false;
        }
        return true;
    }

}