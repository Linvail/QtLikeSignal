
#include "Thread.h"

#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>


// Priority scheduling is an optional part of POSIX. Where it is absent there is no portable
// way to ask for a priority at all, so setPriority() records the value and does nothing else,
// which is what Qt does behind its own QT_HAS_THREAD_PRIORITY_SCHEDULING guard.
#if defined( _POSIX_THREAD_PRIORITY_SCHEDULING )
    #define G_HAS_THREAD_PRIORITY_SCHEDULING
#endif

namespace QtLikeSignal
{

    #if defined( G_HAS_THREAD_PRIORITY_SCHEDULING )

        namespace
        {

            //! Maps a Thread priority onto a scheduler policy and priority number.
            //!
            //! This is Qt's mapping from qthread_unix.cpp, including its deliberately coarse scaling: the
            //! divisor is TimeCriticalPriority rather than the span between the lowest and highest values, so
            //! the enum lands on the low end of the platform's range rather than spreading across it. Kept as
            //! Qt has it so behaviour matches; the alternative would be a library that claims to mimic QThread
            //! and then schedules differently. Returns true if a priority could be calculated; false if the
            //! platform would not report a range.
            bool calculateUnixPriority
                (
                int aPriority,      //!< The Thread priority to convert.
                int* aSchedPolicy,  //!< In: the thread's current policy. Out: the policy to apply, which
                                    //!< only changes when IdlePriority selects SCHED_IDLE.
                int* aSchedPriority //!< Out: the priority number to apply under that policy.
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
            }  // end calculateUnixPriority()

        } // namespace

    #endif

    void Thread::startPlatformSpecific()
    {
        pthread_attr_t attr;
        pthread_attr_init( &attr );

        #if defined( G_HAS_THREAD_PRIORITY_SCHEDULING )
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
                        // The attributes were refused. Go back to inheriting and let the
                        // thread apply the priority to itself once it is running, which is
                        // Qt's fallback for exactly this case.
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
            #if defined( G_HAS_THREAD_PRIORITY_SCHEDULING )
                pthread_attr_setinheritsched( &attr, PTHREAD_INHERIT_SCHED );
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
    //! must already have established that the native handle is valid, because this uses it.
    void Thread::applyPriority
        (
        Priority aPriority  //!< The priority to apply. InheritPriority is meaningful only on Windows
                            //!< and only from start(), where it means "the priority of the thread
                            //!< calling start()"; UNIX expresses inheritance through the pthread
                            //!< attributes instead and never comes here with it.
        )
    {
        // No priority scheduling on this platform; the value is recorded and nothing else.
        ( void )aPriority;
    }

    //! Blocks until the thread has finished executing or timeout expires. Thread-safe. Returns
    //! true if thread finished, false if timeout occurred.
    bool Thread::wait
        (
        unsigned long aTime  //!< Maximum time to wait in milliseconds.
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

        // pthread_join() has no portable timed form -- pthread_timedjoin_np() is a glibc
        // extension Qt guards with a configure test -- so the timeout is served by the same
        // condition variable the thread ends by notifying, and the join that follows is only
        // ever the already-finished kind.
        {
            std::unique_lock<std::mutex> lock( mWaitMutex );
            const auto hasFinished = [this]
                {
                    return mFinished.load();
                };

            // The untimed overload rather than wait_for() with a huge duration: what a
            // wait_for() has to do with it is add it to the clock's current time, which
            // overflows.
            if( aTime == ULONG_MAX )
            {
                mWaitCv.wait( lock, hasFinished );
            }
            else if( !mWaitCv.wait_for( lock, std::chrono::milliseconds( aTime ), hasFinished )
                   )
            {
                return false;
            }
        }

        std::lock_guard<std::mutex> lock( mPriorityMutex );
        if( mJoinable )
        {
            // Under the lock and gated on the flag because two threads joining the same
            // thread is undefined; whichever gets here second finds nothing left to reap.
            pthread_join( mThreadId, nullptr );
            mJoinable = false;
        }
        return true;
    }

}