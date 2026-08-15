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

#include "PerfHarness.hpp"

#include <cstdlib>
#include <new>

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
