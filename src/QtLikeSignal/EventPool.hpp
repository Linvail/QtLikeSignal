// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A pool of fixed-size blocks for the events a queued emit allocates.

#ifndef QT_LIKE_SIGNAL_EVENTPOOL_HPP
#define QT_LIKE_SIGNAL_EVENTPOOL_HPP

#include <cstddef>

namespace QtLikeSignal
{
    //! A pool of fixed-size blocks, so a queued emit need not reach the heap.
    //!
    //! **What this is for.** Every cross-thread signal allocates one block to carry the call, and
    //! frees it on the receiving thread. That is the case a general allocator handles least well:
    //! glibc returns the block to the freeing thread's arena, so a steady producer/consumer pair
    //! migrates memory from one arena to another and both keep asking the OS for more. It is also
    //! an unbounded operation on a path a frame budget has to absorb -- malloc takes a lock, can
    //! fall into mmap, and has a tail nobody can reproduce on a desk.
    //!
    //! **How it works.** One block size, kBlockSize, chosen to fit the overwhelming majority of
    //! queued calls; anything larger goes to the heap and is counted. Each thread keeps its own
    //! free list, which the common path touches with no atomics at all, refilled from and spilled
    //! to a central pool in batches under a mutex. Blocks are interchangeable, so one allocated on
    //! the emitting thread and freed on the receiving thread simply joins the receiver's list; the
    //! central pool is what balances a pure producer against a pure consumer.
    //!
    //! A thread-local free list rather than a lock-free stack, deliberately. The obvious design is
    //! a Treiber stack, and it carries the ABA problem in its worst form: a popper that has read
    //! head and head->next can be overtaken by a thread that pops both and pushes the first back,
    //! after which its compare-exchange succeeds and publishes a block that is in use. Avoiding
    //! that needs a tagged pointer and a double-width compare-exchange, which is not portable to
    //! every target here. Batching under a mutex is correct without any lock-free reasoning, and
    //! touches the mutex once per kBatchSize events rather than once per event.
    //!
    //! **Using it.** Call reserve() during start-up with the worst case the application expects.
    //! After that the steady state never reaches the allocator. A pool that is empty still works --
    //! it allocates from the heap and counts a miss -- because turning a mis-sized pool into
    //! dropped signals would be a far worse failure than a slow one.
    //!
    //! @code
    //!   int main()
    //!   {
    //!       QtLikeSignal::CoreApplication app;
    //!       QtLikeSignal::EventPool::reserve( 4096 );   // 1 MB, held for the life of the process
    //!       ...
    //!   }
    //! @endcode
    //!
    //! Every function here is thread-safe.
    namespace EventPool
    {
        //! One pooled block, in bytes, including the header this pool puts in front of it.
        //!
        //! A MetaCallEvent header is 32 bytes, and the closure behind it captures a receiver
        //! pointer, a member-function pointer (16 bytes on MSVC), a connection type, a shared_ptr
        //! and a copy of every emitted argument -- so a typical queued call is well under 128.
        //! 256 leaves room for a signal carrying real payloads before anything reaches the heap.
        constexpr std::size_t kBlockSize = 256;

        //! Blocks moved between a thread's own list and the central pool in one go.
        //!
        //! The whole point of the thread-local list: the central mutex is taken once per this many
        //! events rather than once per event. Larger batches mean less contention and more memory
        //! parked in a thread that may not need it.
        constexpr std::size_t kBatchSize = 32;

        //! Pre-allocates @p aBlocks blocks, so the steady state never reaches the allocator.
        //!
        //! Call it once during start-up, sized for the worst case: the deepest the queues get
        //! multiplied by however many threads are emitting. Calling it again adds more.
        //!
        //! Allocated in chunks rather than one block at a time, and never returned to the OS --
        //! see the namespace comment. The cost is @p aBlocks * kBlockSize, held until exit.
        void reserve
            (
            std::size_t aBlocks  //!< Blocks to add to the pool.
            );

        //! @return a block of at least @p aBytes usable bytes; never null, it throws as
        //! ::operator new does.
        //!
        //! Comes from the pool when @p aBytes fits and a block is free. Otherwise from the heap,
        //! and missCount() records it. Release it with release() and nothing else -- the block
        //! carries a header the caller cannot see.
        void* allocate
            (
            std::size_t aBytes  //!< Usable bytes needed.
            );

        //! Returns a block from allocate() to this thread's free list, or to the heap.
        //!
        //! Which one is decided by the block's own header, so a block may be released on a
        //! different thread from the one that allocated it -- which is the normal case here, since
        //! a queued call is allocated by the emitter and freed by the receiver.
        void release
            (
            void* aBlock  //!< A block from allocate(); null is ignored.
            );

        //! @return how many blocks the pool owns, handed out or not.
        std::size_t blocksReserved();

        //! @return how many blocks are free right now, in the central pool and every thread cache.
        //!
        //! A sample rather than a promise: another thread can take one before the caller reads the
        //! answer. For a health report, not for deciding whether the next allocate() will hit.
        std::size_t blocksAvailable();

        //! @return how many allocations have reached the heap rather than the pool.
        //!
        //! Non-zero means the pool is too small, or the blocks asked for are bigger than
        //! kBlockSize. Never resets, so two readings subtract to a rate. A tuning knob whose miss
        //! rate cannot be read is a knob nobody can set.
        unsigned long long missCount();

    }
}

#endif // QT_LIKE_SIGNAL_EVENTPOOL_HPP
