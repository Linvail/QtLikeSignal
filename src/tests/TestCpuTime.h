#ifndef TEST_CPU_TIME_H
#define TEST_CPU_TIME_H

//! @file
//!
//! Process CPU time, for tests that need to tell "blocked" apart from "spinning".
//!
//! Deliberately **not** std::clock(). C specifies clock() as processor time, but MSVC documents and
//! implements it as *wall-clock* time since CRT initialisation -- so on Windows a cpu/wall ratio
//! computed from it is always ~1.0 and a spin check passes or fails regardless of what the loop
//! actually did. That is not a hypothetical: it made
//! CoreApplicationTest.ReExecAfterQuitBlocksInsteadOfSpinning fail on Windows while passing on
//! Linux, where glibc's clock() is conforming.
//!
//! There is no portable standard-library call for this, so each platform gets its own.

#if defined( _WIN32 )
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        // windows.h defines min/max macros that collide with std::min/std::max, which gtest uses.
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/resource.h>
#endif

namespace TestSupport
{
    //! Gets CPU time consumed by this process across all its threads, in seconds.
    //!
    //! Compare against elapsed wall time: a loop that blocks correctly consumes almost none, while
    //! one that spins consumes roughly one core's worth per second of wall time.
    inline double processCpuSeconds()
    {
        #if defined( _WIN32 )
            FILETIME creationTime {};
            FILETIME exitTime {};
            FILETIME kernelTime {};
            FILETIME userTime {};
            if( GetProcessTimes( GetCurrentProcess(), &creationTime, &exitTime, &kernelTime,
                &userTime ) == 0 )
            {
                return 0.0;
            }

            // FILETIME counts 100-nanosecond intervals. Both halves matter: a spin burns user time,
            // but a badly-behaved wait can burn kernel time instead.
            auto toSeconds = []( const FILETIME& aTime )
                {
                    ULARGE_INTEGER value;
                    value.LowPart  = aTime.dwLowDateTime;
                    value.HighPart = aTime.dwHighDateTime;
                    return static_cast<double>( value.QuadPart ) * 1e-7;
                };
            return toSeconds( kernelTime ) + toSeconds( userTime );
        #else
            rusage usage {};
            if( getrusage( RUSAGE_SELF, &usage ) != 0 )
            {
                return 0.0;
            }

            auto toSeconds = []( const timeval& aTime )
                {
                    return static_cast<double>( aTime.tv_sec )
                           + static_cast<double>( aTime.tv_usec ) * 1e-6;
                };
            return toSeconds( usage.ru_utime ) + toSeconds( usage.ru_stime );
        #endif
    }
}

#endif // TEST_CPU_TIME_H
