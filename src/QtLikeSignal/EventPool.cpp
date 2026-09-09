// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The block pool behind a queued emit. See EventPool.hpp for what it is for and why it is shaped
//! this way.

#include "QtLikeSignal/EventPool.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignal/LogCategories.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

namespace QtLikeSignal
{
    namespace EventPool
    {
        namespace
        {
            //! What sits immediately before every block allocate() hands out.
            //!
            //! release() is given a bare pointer and nothing else. It cannot ask the object it came
            //! from, because by then that object is destroyed, and it cannot use a sized
            //! deallocation, because the block is deliberately larger than the object -- which is
            //! the whole reason MetaCallEvent declares an unsized operator delete. So provenance
            //! travels in front of the block.
            struct BlockHeader
            {
                //! kPooledTag if this block belongs to the pool, kHeapTag if it came from the heap.
                //!
                //! A tag rather than a bool so that a block released twice, or a pointer that never
                //! came from allocate(), is something the debugger can recognise rather than a
                //! plausible-looking zero.
                std::size_t mTag;
            };

            //! Marks a block the pool owns and will take back.
            constexpr std::size_t kPooledTag = 0x516D506Fu;

            //! Marks a block that came straight from the heap and must go back to it.
            constexpr std::size_t kHeapTag = 0x516D486Du;

            //! Bytes reserved in front of every block for its BlockHeader.
            //!
            //! Rounded up to the strictest fundamental alignment, so what follows the header is as
            //! aligned as ::operator new would have made it. MetaCallEvent already refuses an
            //! over-aligned callable at compile time, so fundamental alignment is the whole
            //! requirement.
            constexpr std::size_t kHeaderSize
                = ( ( sizeof( BlockHeader ) + alignof( std::max_align_t ) - 1 )
                / alignof( std::max_align_t ) ) * alignof( std::max_align_t );

            //! Usable bytes in a pooled block, once its header is accounted for.
            constexpr std::size_t kUsableSize = kBlockSize - kHeaderSize;

            //! A free block, threaded through the memory of the block itself.
            //!
            //! A free list needs a next pointer per entry, and a block that is free has nothing
            //! else to store -- so the pointer lives in the block rather than in a node beside it.
            //! That is what makes the list itself allocation-free.
            struct FreeNode
            {
                FreeNode* mNext;
            };

            //! Everything the central pool owns, behind one mutex.
            //!
            //! A function-local static rather than a namespace-scope object, so its construction is
            //! ordered by first use rather than by link order -- a static Object somewhere else in
            //! the program may well emit before main() runs.
            struct Central
            {
                std::mutex mMutex;                    //!< Guards every member below.
                FreeNode* mFree { nullptr };          //!< Blocks nobody holds, as a list.
                std::size_t mFreeCount { 0 };         //!< Length of mFree.
                std::size_t mReserved { 0 };          //!< Blocks this pool owns in total.

                //! The chunks reserve() allocated, kept only so they are not leaked in the eyes of
                //! a leak checker. Never freed before exit: see the header's note on why the pool
                //! does not return memory to the OS.
                std::vector<void*> mChunks;
            };

            //! The one central pool.
            Central& central()
            {
                static Central sCentral;
                return sCentral;
            }

            //! Allocations that reached the heap rather than the pool.
            //!
            //! Outside the mutex because the miss path may not take it -- an over-large block never
            //! touches the central pool at all -- and because a counter read for a health report
            //! should not contend with the allocation path it is measuring.
            std::atomic<unsigned long long> gMisses { 0 };

            //! Blocks the pool gives itself on the first miss, when nothing was ever reserved.
            //!
            //! 512 blocks is 128 kB, which is enough for a queue far deeper than a healthy program
            //! runs and small enough that a program which never queues anything has not lost
            //! anything worth counting. An application that wants a different number, or wants to
            //! know the memory was taken during start-up rather than on the first emit, calls
            //! reserve() itself.
            constexpr std::size_t kDefaultBlocks = 512;

            //! False once a miss has been reported, so only the first of a burst is logged.
            //!
            //! A program that never calls reserve() misses on every emit, and a log line per emit
            //! would cost more than the allocation it is complaining about.
            std::atomic<bool> gMissReported { false };

            //! One thread's cache of free blocks, and the spill that empties it at thread exit.
            //!
            //! The common path -- take one, put one back -- touches only these two members, with no
            //! atomics and no lock. The central pool is reached once per kBatchSize events.
            struct ThreadCache
            {
                //! Returns everything this thread still holds, so a program that starts and stops
                //! threads does not lose blocks to the ones that ended.
                ~ThreadCache()
                {
                    if( !mFree )
                    {
                        return;
                    }

                    // Walks to the tail so the whole list can be spliced in one splice rather than
                    // pushed one block at a time under the lock.
                    FreeNode* tail = mFree;
                    while( tail->mNext )
                    {
                        tail = tail->mNext;
                    }

                    Central& c = central();
                    std::lock_guard<std::mutex> lock( c.mMutex );
                    tail->mNext = c.mFree;
                    c.mFree = mFree;
                    c.mFreeCount += mCount;

                    mFree = nullptr;
                    mCount = 0;
                }

                FreeNode* mFree { nullptr };  //!< This thread's free blocks.
                std::size_t mCount { 0 };     //!< Length of mFree.
            };

            //! This thread's cache.
            //!
            //! A function-local static inside a function rather than a bare thread_local variable,
            //! so it is constructed on first use on each thread and destroyed at that thread's
            //! exit, in an order the standard defines.
            ThreadCache& cache()
            {
                static thread_local ThreadCache sCache;
                return sCache;
            }

            //! Moves up to kBatchSize blocks from the central pool into @p aCache.
            //! @return true if the cache has at least one block afterwards.
            bool refill
                (
                ThreadCache& aCache  //!< The cache to fill.
                )
            {
                Central& c = central();
                std::lock_guard<std::mutex> lock( c.mMutex );

                std::size_t moved = 0;
                while( moved < kBatchSize && c.mFree )
                {
                    FreeNode* const node = c.mFree;
                    c.mFree = node->mNext;
                    --c.mFreeCount;

                    node->mNext = aCache.mFree;
                    aCache.mFree = node;
                    ++aCache.mCount;
                    ++moved;
                }

                return aCache.mFree != nullptr;
            }

            //! Moves half of @p aCache back to the central pool, so one thread cannot hoard.
            //!
            //! Half rather than all: a thread that has just spilled is very likely to allocate
            //! again, and emptying it completely would send it straight back to the mutex.
            void spill
                (
                ThreadCache& aCache  //!< The cache to drain.
                )
            {
                const std::size_t keep = kBatchSize;

                FreeNode* head = aCache.mFree;
                std::size_t kept = 0;
                while( head && kept + 1 < keep )
                {
                    head = head->mNext;
                    ++kept;
                }

                if( !head || !head->mNext )
                {
                    return;
                }

                FreeNode* const surplus = head->mNext;
                head->mNext = nullptr;

                FreeNode* tail = surplus;
                std::size_t moved = 1;
                while( tail->mNext )
                {
                    tail = tail->mNext;
                    ++moved;
                }

                Central& c = central();
                std::lock_guard<std::mutex> lock( c.mMutex );
                tail->mNext = c.mFree;
                c.mFree = surplus;
                c.mFreeCount += moved;

                aCache.mCount -= moved;
            }

            //! Records a miss, and logs the first of a burst.
            void noteMiss()
            {
                gMisses.fetch_add( 1, std::memory_order_relaxed );

                if( gMissReported.exchange( true, std::memory_order_relaxed ) )
                {
                    return;
                }

                qCWarning( gLogDispatcher )
                    << "event pool missed. It grows itself once if nothing was ever reserved;"
                    << "call EventPool::reserve() during start-up to size it deliberately and to"
                    << "keep the allocation out of the steady state. Blocks reserved:"
                    << static_cast<unsigned long long>( blocksReserved() );
            }
        }

        //! Pre-allocates blocks into the central pool. See the declaration.
        void reserve
            (
            std::size_t aBlocks  //!< Blocks to add to the pool.
            )
        {
            if( aBlocks == 0 )
            {
                return;
            }

            // One chunk for the whole request rather than one allocation per block: the point of
            // the pool is to stop asking the allocator, so asking it aBlocks times to build it
            // would be a strange way to start.
            void* const chunk = ::operator new( aBlocks * kBlockSize );

            Central& c = central();
            std::lock_guard<std::mutex> lock( c.mMutex );

            c.mChunks.push_back( chunk );
            c.mReserved += aBlocks;

            char* cursor = static_cast<char*>( chunk );
            for( std::size_t i = 0; i < aBlocks; ++i )
            {
                FreeNode* const node = reinterpret_cast<FreeNode*>( cursor );
                node->mNext = c.mFree;
                c.mFree = node;
                ++c.mFreeCount;
                cursor += kBlockSize;
            }
        }

        //! Hands out a block, from the pool if it can. See the declaration.
        void* allocate
            (
            std::size_t aBytes  //!< Usable bytes needed.
            )
        {
            if( aBytes <= kUsableSize )
            {
                ThreadCache& tc = cache();
                if( tc.mFree || refill( tc ) )
                {
                    FreeNode* const node = tc.mFree;
                    tc.mFree = node->mNext;
                    --tc.mCount;

                    // The node pointer and the header occupy the same bytes; the list is threaded
                    // through the blocks themselves, so writing the tag is what turns a free block
                    // back into a live one.
                    void* const raw = static_cast<void*>( node );
                    static_cast<BlockHeader*>( raw )->mTag = kPooledTag;
                    return static_cast<char*>( raw ) + kHeaderSize;
                }
            }

            // Either the block is bigger than the pool deals in, or the pool is empty. Both are a
            // miss, and both still succeed: refusing here would turn a mis-sized pool into dropped
            // signals, which is far worse than a slow one.
            noteMiss();

            // A pool that has never been reserved grows itself once, here, rather than missing on
            // every queued emit for the life of the process.
            //
            // Measured, and the reason this exists: an unreserved pool was about 25 % slower on a
            // saturated queue than the plain ::operator new it replaced, because every emit paid a
            // miss -- the counter, the fallback, and a header the old path did not have. Being
            // slower than not having the feature at all is not a defensible default.
            //
            // Only when nothing has been reserved. An application that sized its own pool and then
            // outgrew it has a number to fix, and quietly growing underneath it would hide exactly
            // what missCount() exists to show. The warning still fires either way.
            if( aBytes <= kUsableSize && blocksReserved() == 0 )
            {
                reserve( kDefaultBlocks );
                return allocate( aBytes );
            }

            void* const raw = ::operator new( kHeaderSize + aBytes );
            static_cast<BlockHeader*>( raw )->mTag = kHeapTag;
            return static_cast<char*>( raw ) + kHeaderSize;
        }

        //! Takes a block back. See the declaration.
        void release
            (
            void* aBlock  //!< A block from allocate(); null is ignored.
            )
        {
            if( !aBlock )
            {
                return;
            }

            void* const raw = static_cast<char*>( aBlock ) - kHeaderSize;
            const std::size_t tag = static_cast<BlockHeader*>( raw )->mTag;

            if( tag != kPooledTag )
            {
                // Anything that is not ours goes back where it came from. A tag that is neither of
                // the two means the block was already released or never came from allocate(), and
                // there is nothing safe to do with it but hand it to the heap -- which is what the
                // program would have done without this pool.
                ::operator delete( raw );
                return;
            }

            // Onto this thread's list, which may not be the thread that allocated it: a queued call
            // is allocated by the emitter and freed by the receiver, and blocks are interchangeable
            // so it does not matter. The central pool is what balances a pure producer against a
            // pure consumer.
            ThreadCache& tc = cache();
            FreeNode* const node = static_cast<FreeNode*>( raw );
            node->mNext = tc.mFree;
            tc.mFree = node;
            ++tc.mCount;

            if( tc.mCount > kBatchSize * 2 )
            {
                spill( tc );
            }
        }

        //! @return blocks the pool owns, handed out or not.
        std::size_t blocksReserved()
        {
            Central& c = central();
            std::lock_guard<std::mutex> lock( c.mMutex );
            return c.mReserved;
        }

        //! @return blocks that are free right now, centrally. See the declaration.
        //!
        //! Deliberately does not include the blocks parked in other threads' caches: there is no
        //! safe way to read another thread's cache, since the whole reason it is fast is that
        //! nothing synchronises it. So this under-reports, and says so rather than taking a lock
        //! that would make the allocation path slower to make a diagnostic tidier.
        std::size_t blocksAvailable()
        {
            Central& c = central();
            std::lock_guard<std::mutex> lock( c.mMutex );
            return c.mFreeCount + cache().mCount;
        }

        //! @return allocations that reached the heap rather than the pool.
        unsigned long long missCount()
        {
            return gMisses.load( std::memory_order_relaxed );
        }
    }
}
