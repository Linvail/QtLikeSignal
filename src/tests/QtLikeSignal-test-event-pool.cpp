// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for EventPool -- reserving, handing out and taking back blocks, the heap
//! fallback, and the cross-thread free that a queued emit actually performs.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignal/EventPool.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace QtLikeSignal;
using namespace std::chrono_literals;

namespace
{
    //! Blocks reserved by the fixture below, enough for every case here with room to spare.
    //!
    //! Reserving is one-way -- the pool never gives memory back -- so every test in this binary
    //! shares whatever the first one asked for. That is why nothing here asserts on an absolute
    //! miss count or an absolute number of free blocks: only on differences it caused itself.
    //!
    //! **It must exceed the deepest burst any test here puts in flight at once, not the number
    //! that is usually outstanding.** It was 1024 against the 2 064 events
    //! `AQueuedEmitDoesNotMissAReservedPool` emits, which made that test pass only while its
    //! receiving worker kept up: an emitter that gets ahead by more than the reserve empties the
    //! pool, and emptying the pool is the one thing that test asserts does not happen. On an idle
    //! machine the worker does keep up and it passed; with every core busy it does not, and the
    //! test failed about one run in six. 4 096 blocks is 1 MB, held for the life of the test
    //! binary, and leaves the worker free to be as late as the scheduler makes it.
    constexpr std::size_t kReserved = 4096;

    //! Reserves once for the whole suite, on first use.
    //!
    //! A function rather than a fixture SetUp, because the pool is process-wide and reserving it
    //! per test would grow it per test.
    void ensureReserved()
    {
        static bool sDone = false;
        if( !sDone )
        {
            EventPool::reserve( kReserved );
            sDone = true;
        }
    }

    //! Receives the queued signal the cross-thread cases emit.
    class PoolReceiver : public Object
    {
    public:
        void onValue
            (
            int aValue  //!< Emitted value.
            )
        {
            mSum.fetch_add( aValue, std::memory_order_relaxed );
        }

        std::atomic<int> mSum { 0 };  //!< Sum of everything received.
    };
}

//! Tests that a pool nobody reserved gives itself one, rather than missing forever.
//!
//! **The measurement behind this:** an unreserved pool was about 25 % slower on a saturated queue
//! than the plain allocator it replaced, because every queued emit paid a miss -- the counter, the
//! fallback, and a header the old path did not have. Being slower than not having the feature is
//! not a defensible default, so the first miss grows the pool once.
//!
//! **Skipped rather than passed when the pool is already warm**, which it is in any full run: the
//! pool is process-wide and one-way, and the suites that run before this one emit cross-thread
//! signals, which grows it. An earlier version of this case wrapped the check in `if( reserved == 0
//! )` and quietly passed having tested nothing -- a green test that pinned no behaviour at all. A
//! skip says so out loud.
//!
//! What still runs it: `--gtest_filter=EventPoolTest.AnUnreservedPoolGrowsItselfOnce` on its own.
TEST( EventPoolTest, AnUnreservedPoolGrowsItselfOnce )
{
    if( EventPool::blocksReserved() != 0 )
    {
        GTEST_SKIP() << "the pool was already grown by an earlier suite; run this case alone to "
            "exercise the first miss.";
    }

    void* first = EventPool::allocate( 64 );
    ASSERT_NE( first, nullptr );
    EXPECT_GT( EventPool::blocksReserved(), 0u )
        << "the first miss did not grow an unreserved pool.";
    EventPool::release( first );

    // And having grown, it stops missing -- which is the whole point of growing.
    const unsigned long long before = EventPool::missCount();
    void* block = EventPool::allocate( 64 );
    ASSERT_NE( block, nullptr );
    EventPool::release( block );

    EXPECT_EQ( EventPool::missCount(), before )
        << "a pool that owns blocks is still reaching the heap.";
}

//! Tests that reserve() adds the blocks it was asked for.
TEST( EventPoolTest, ReserveAddsBlocks )
{
    ensureReserved();

    const std::size_t before = EventPool::blocksReserved();
    EventPool::reserve( 16 );
    EXPECT_EQ( EventPool::blocksReserved(), before + 16 );

    // Reserving nothing is a no-op rather than an error, so a configuration value of zero does not
    // need a special case at the call site.
    EventPool::reserve( 0 );
    EXPECT_EQ( EventPool::blocksReserved(), before + 16 );
}

//! Tests that a block that fits comes from the pool and goes back to it.
TEST( EventPoolTest, ABlockThatFitsIsPooled )
{
    ensureReserved();

    const unsigned long long missesBefore = EventPool::missCount();

    void* block = EventPool::allocate( 64 );
    ASSERT_NE( block, nullptr );

    // Writing the whole request proves the block really is that big, which a test that only holds
    // the pointer would not. ASan is what turns a wrong answer here into a failure rather than
    // silent corruption of the next block.
    std::memset( block, 0xAB, 64 );

    EventPool::release( block );

    EXPECT_EQ( EventPool::missCount(), missesBefore )
        << "a block that fits should not have reached the heap.";
}

//! Tests that a block too large for the pool still succeeds, and is counted as a miss.
//!
//! Refusing would turn an over-large connection into a dropped signal. Falling back keeps every
//! program working and makes the cost visible instead.
TEST( EventPoolTest, AnOversizedBlockFallsBackToTheHeap )
{
    ensureReserved();

    const unsigned long long missesBefore = EventPool::missCount();

    const std::size_t oversized = EventPool::kBlockSize * 4;
    void* block = EventPool::allocate( oversized );
    ASSERT_NE( block, nullptr );

    std::memset( block, 0xCD, oversized );
    EventPool::release( block );

    EXPECT_EQ( EventPool::missCount(), missesBefore + 1 )
        << "an oversized allocation was not counted as a miss.";
}

//! Tests that taking every block and giving it back leaves the pool as it was.
//!
//! The property that matters most: a pool that loses a block per cycle is a leak with extra steps,
//! and it would take a long-running program rather than a test to notice.
TEST( EventPoolTest, EveryBlockComesBack )
{
    ensureReserved();

    const std::size_t before = EventPool::blocksAvailable();
    ASSERT_GT( before, 0u );

    std::vector<void*> blocks;
    blocks.reserve( 256 );
    for( int i = 0; i < 256; ++i )
    {
        void* block = EventPool::allocate( 64 );
        ASSERT_NE( block, nullptr );
        blocks.push_back( block );
    }

    for( void* block : blocks )
    {
        EventPool::release( block );
    }

    EXPECT_EQ( EventPool::blocksAvailable(), before )
        << "the pool did not end with the blocks it started with.";
}

//! Tests that releasing null is ignored rather than a crash.
TEST( EventPoolTest, ReleasingNullIsHarmless )
{
    ensureReserved();

    const std::size_t before = EventPool::blocksAvailable();
    EventPool::release( nullptr );
    EXPECT_EQ( EventPool::blocksAvailable(), before );
}

//! Tests that a block allocated on one thread can be freed on another.
//!
//! Not an edge case -- it is what every queued emit does. The event is allocated by the emitting
//! thread and freed by the receiving one, so a pool that could only free on the allocating thread
//! would be useless here.
TEST( EventPoolTest, ABlockCrossesThreadsToBeFreed )
{
    ensureReserved();

    constexpr int kBlocks = 128;

    std::vector<void*> blocks;
    blocks.reserve( kBlocks );
    for( int i = 0; i < kBlocks; ++i )
    {
        void* block = EventPool::allocate( 64 );
        ASSERT_NE( block, nullptr );
        std::memset( block, 0xEF, 64 );
        blocks.push_back( block );
    }

    // Freed on a thread that never allocated one of them.
    std::thread freer( [&blocks]()
        {
            for( void* block : blocks )
            {
                EventPool::release( block );
            }
        } );
    freer.join();

    // The freeing thread has ended, so its cache has spilled everything back and the blocks are
    // reachable again from this thread.
    void* reused = EventPool::allocate( 64 );
    ASSERT_NE( reused, nullptr );
    EventPool::release( reused );
}

//! Tests that a producer and a consumer running for a long time settle rather than drain the pool.
//!
//! The arrangement the thread caches have to survive: one thread only ever allocates and the other
//! only ever frees, so blocks travel one way and the central pool is the only thing that can carry
//! them back. A design that only spilled at thread exit would starve the producer here.
TEST( EventPoolTest, AProducerAndConsumerPairSettles )
{
    ensureReserved();

    constexpr int kRounds = 4000;

    std::atomic<bool> stop { false };
    std::vector<void*> handoff;
    std::mutex handoffMutex;

    std::thread consumer( [&stop, &handoff, &handoffMutex]()
        {
            while( true )
            {
                std::vector<void*> mine;
                {
                    std::lock_guard<std::mutex> lock( handoffMutex );
                    mine.swap( handoff );
                }

                for( void* block : mine )
                {
                    EventPool::release( block );
                }

                if( mine.empty() )
                {
                    if( stop.load( std::memory_order_acquire ) )
                    {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        } );

    for( int i = 0; i < kRounds; ++i )
    {
        void* block = EventPool::allocate( 64 );
        ASSERT_NE( block, nullptr );

        std::lock_guard<std::mutex> lock( handoffMutex );
        handoff.push_back( block );
    }

    stop.store( true, std::memory_order_release );
    consumer.join();

    // Anything the consumer had not taken yet is this thread's to release.
    {
        std::lock_guard<std::mutex> lock( handoffMutex );
        for( void* block : handoff )
        {
            EventPool::release( block );
        }
        handoff.clear();
    }

    EXPECT_GT( EventPool::blocksAvailable(), 0u )
        << "a one-way producer/consumer pair drained the pool.";
}

//! Tests that a thread ending returns the blocks its cache was holding.
//!
//! Without the thread-cache destructor, a program that starts and stops threads would lose up to a
//! batch of blocks to each one -- a leak that only shows on a long-running system.
TEST( EventPoolTest, AThreadEndingReturnsItsCache )
{
    ensureReserved();

    const std::size_t before = EventPool::blocksAvailable();

    std::thread worker( []()
        {
            // More than a batch, so the cache is holding some when the thread ends.
            std::vector<void*> blocks;
            for( int i = 0; i < 64; ++i )
            {
                blocks.push_back( EventPool::allocate( 64 ) );
            }
            for( void* block : blocks )
            {
                EventPool::release( block );
            }
        } );
    worker.join();

    EXPECT_EQ( EventPool::blocksAvailable(), before )
        << "a thread that ended kept blocks the pool can no longer reach.";
}

//! Tests that a queued emit uses the pool -- the only caller that matters.
//!
//! Every other case here exercises the pool directly. This one goes through the machinery that
//! actually allocates in production, and checks that a run of cross-thread emits against a reserved
//! pool never reaches the heap.
TEST( EventPoolTest, AQueuedEmitDoesNotMissAReservedPool )
{
    ensureReserved();

    PoolReceiver receiver;

    {
        Thread worker;
        worker.start();
        ASSERT_TRUE( waitFor( [&worker]()
            {
                return worker.eventDispatcher() != nullptr;
            } ) );

        ASSERT_TRUE( receiver.moveToThread( &worker ) );

        Signal<int> signal;
        Object::connect( signal, &receiver, &PoolReceiver::onValue, ConnectionType::Queued );

        // Warmed first, so the emitting thread's cache is filled and the receiving thread has seen
        // a block. Measuring from cold would count the two refills as misses, which they are not.
        for( int i = 0; i < 64; ++i )
        {
            signal.emit( 1 );
        }
        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.mSum.load( std::memory_order_relaxed ) >= 64;
            } ) );

        const unsigned long long missesBefore = EventPool::missCount();

        // Nothing paces this loop, so the worker is free to fall arbitrarily far behind and every
        // emit can be in flight at once. That is deliberate -- it is the saturated case the pool
        // exists for -- but it means the reserve has to cover the whole burst rather than the depth
        // the queue usually reaches. kReserved says the same thing from the other end; the check
        // below keeps the two from drifting apart, because if they do this test starts failing on
        // a busy machine and passing on an idle one.
        constexpr int kEmits = 2000;
        static_assert( kReserved >= 64 + kEmits,
            "the pool must be able to hold every event this test can have in flight at once." );

        for( int i = 0; i < kEmits; ++i )
        {
            signal.emit( 1 );
        }

        EXPECT_TRUE( waitFor( [&receiver]()
            {
                return receiver.mSum.load( std::memory_order_relaxed ) >= 64 + kEmits;
            } ) ) << "the worker did not receive everything that was emitted.";

        EXPECT_EQ( EventPool::missCount(), missesBefore )
            <<
            "a queued emit reached the heap even though the pool had blocks. Either the closure is "
            "bigger than EventPool::kBlockSize, or the pool ran dry.";

        drainQueuedTasks( worker );
        worker.quit();
        worker.wait();
    }

    // Only after ~Thread() has nulled the back-pointer; one thread may not re-home another live
    // thread's object.
    ASSERT_TRUE( receiver.moveToThread( Thread::currentThread() ) );
}
