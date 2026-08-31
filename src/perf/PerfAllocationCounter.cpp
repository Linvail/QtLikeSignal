// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Global operator new/delete replacements that count allocations for the benchmarks.
//!
//! Replacing them process-wide is the only way to count what a library allocates without changing
//! the library, and it is why this lives in its own translation unit: the replacement must be
//! linked into the benchmark binary and nowhere else.
//!
//! Counting is off unless a test turns it on, and it is per-thread, so an unrelated worker
//! allocating in the background cannot inflate a measurement.
//!
//! AddressSanitizer and ThreadSanitizer ship their own global operator new/delete in their runtime
//! library. A second, non-weak definition here makes the link fail with "multiple definition", so
//! this file backs out of the replacement under either sanitizer and reports itself unavailable.

#include "PerfHarness.hpp"

#include <cstdlib>
#include <new>

#if defined( __SANITIZE_ADDRESS__ ) || defined( __SANITIZE_THREAD__ )
    #define QTLS_PERF_ALLOCATION_COUNTER_SANITIZED 1
#elif defined( __has_feature )
    #if __has_feature( address_sanitizer ) || __has_feature( thread_sanitizer )
        #define QTLS_PERF_ALLOCATION_COUNTER_SANITIZED 1
    #endif
#endif
#if !defined( QTLS_PERF_ALLOCATION_COUNTER_SANITIZED )
    #define QTLS_PERF_ALLOCATION_COUNTER_SANITIZED 0
#endif

#if !QTLS_PERF_ALLOCATION_COUNTER_SANITIZED

namespace
{
    //! Per-thread, so only the measuring thread's allocations are counted.
    thread_local long gCount = 0;
    thread_local bool gCounting = false;
}

void* operator new
    (
    std::size_t aSize
    )
{
    if( gCounting )
    {
        ++gCount;
    }
    void* p = std::malloc( aSize != 0 ? aSize : 1 );
    if( !p )
    {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[]
    (
    std::size_t aSize
    )
{
    return operator new( aSize );
}

void operator delete
    (
    void* aPtr
    ) noexcept
{
    std::free( aPtr );
}

void operator delete[]
    (
    void* aPtr
    ) noexcept
{
    std::free( aPtr );
}

void operator delete
    (
    void* aPtr,
    std::size_t
    ) noexcept
{
    std::free( aPtr );
}

void operator delete[]
    (
    void* aPtr,
    std::size_t
    ) noexcept
{
    std::free( aPtr );
}

namespace PerfHarness
{
    namespace Allocations
    {
        //! True: this file being linked is what makes counting work.
        bool available()
        {
            return true;
        }

        void start()
        {
            gCount    = 0;
            gCounting = true;
        }

        long stop()
        {
            gCounting = false;
            return gCount;
        }
    }
}

#else // QTLS_PERF_ALLOCATION_COUNTER_SANITIZED

namespace PerfHarness
{
    namespace Allocations
    {
        //! False: the sanitizer runtime owns operator new/delete here, so nothing counts.
        bool available()
        {
            return false;
        }

        void start()
        {
        }

        long stop()
        {
            return 0;
        }
    }
}

#endif // QTLS_PERF_ALLOCATION_COUNTER_SANITIZED
