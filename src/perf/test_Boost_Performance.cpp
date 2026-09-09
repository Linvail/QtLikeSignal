// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Dispatch-overhead benchmarks for boost::signals2, run alongside the QtLikeSignal and
//! Qt 6 ones so every library appears in a single comparison table.
//!
//! boost::signals2 is where this project started. QtLikeSignal took its signal from it, but later
//! replaced it with an in-house Signal in August 2026. It belongs in the table as
//! the second reference point: Qt 6 says what the thing being imitated costs, boost says what the
//! thing that was replaced cost, and our own rows have to be read against both.
//!
//! **This column is narrower than the others, deliberately.** signals2 is a signal library, not an
//! object and threading framework -- it has no thread affinity and no event loop. The
//! `auto same-thread` and `queued x-thread` scenarios therefore have no boost equivalent, and are
//! left blank rather than filled by redefining what is being measured. An "auto" row would just
//! re-measure the direct row, and a "queued" row would have to borrow our event loop and would then
//! be measuring our queue, not boost's.
//!
//! Built only where boost's headers are installed; see src/tests/wscript. signals2 is header-only,
//! so nothing here is linked -- on Debian and Ubuntu `sudo apt install libboost-dev` is the whole
//! dependency, and no boost submodule or bootstrap comes back with it.
//!
//! Its own translation unit for the reason every library here has one: it keeps the boost headers
//! out of the builds that do not have them, which is what lets waf add and drop this file
//! wholesale.

#include "PerfHarness.hpp"

#include <gtest/gtest.h>

#include <boost/bind/bind.hpp>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/signals2/signal.hpp>
#include <boost/weak_ptr.hpp>

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using PerfHarness::keep;
using PerfHarness::kConnectOps;
using PerfHarness::kDeferredCallAllocOps;
using PerfHarness::kDeferredCallOps;
using PerfHarness::kDirectOps;
using PerfHarness::kDisconnectOps;
using PerfHarness::record;
using PerfHarness::timeLoop;

namespace
{
    //! The signal every benchmark below drives. One `int`, matching the other libraries.
    using BoostSignal = boost::signals2::signal<void ( int )>;

    //! Base of the events the deferred-call rows post, mirroring what a GUI framework built on
    //! boost does: a heap event with a virtual entry point, owned by a shared_ptr.
    class MessageEvent
    {
    public:
        virtual ~MessageEvent()
        {
        }

        //! Runs whatever this event carries, on the thread that drained it.
        virtual void emit() = 0;

    };

    //! An event carrying a callback, which is boost's answer to "call this later, on that thread".
    //!
    //! Reproduced here rather than referenced, because the shape is what is being measured and it
    //! is small: a `boost::function` member, set at construction, cleared by disconnect(), and
    //! called by emit(). The double type erasure is the point -- the event is virtual and the
    //! function it holds is virtual too, so a call arrives through two indirections.
    class FunctionCallbackEvent : public MessageEvent
    {
    public:
        //! The callback an event of this kind carries.
        typedef boost::function<void ( )> CallbackFunctionType;

        //! Constructs an event that will run @p aFunction when it is drained.
        explicit FunctionCallbackEvent
            (
            const CallbackFunctionType& aFunction  //!< Callback to run on the consumer thread.
            )
            : mFunction( aFunction )
        {
        }

        //! Runs the callback, if one is still set.
        virtual void emit() override
        {
            if( mFunction )
            {
                mFunction();
            }
        }

        //! Clears the callback, so an event already queued runs nothing.
        //!
        //! This is how the idiom cancels: the owner keeps the shared_ptr and clears it from its own
        //! destructor. Nothing in the library does it, which is the difference the
        //! `deferred call, x-thread` row exists to price.
        void disconnect()
        {
            mFunction.clear();
        }

    private:
        CallbackFunctionType mFunction;  //!< The callback, or empty once disconnected.
    };

    //! The consumer the deferred-call rows post to, built from plain parts only.
    //!
    //! **Nothing of ours is in here, deliberately.** A boost row that borrowed QtLikeSignal's loop
    //! would report our queue with a boost payload, which is not what anyone would read it as. A
    //! deque, a mutex and a condition variable are what an application writes when its framework
    //! does not supply a loop, so that is what this is.
    class BoostEventQueue
    {
    public:
        //! Queues @p aEvent for the consumer thread and wakes it. Thread-safe.
        void post
            (
            const boost::shared_ptr<MessageEvent>& aEvent  //!< Event to run on the consumer.
            )
        {
            {
                std::lock_guard<std::mutex> lock( mMutex );
                mQueue.push_back( aEvent );
            }
            mCv.notify_one();
        }

        //! Drains events until stop() is called and the queue is empty. Runs on the consumer
        //! thread.
        void run()
        {
            for( ;; )
            {
                boost::shared_ptr<MessageEvent> event;
                {
                    std::unique_lock<std::mutex> lock( mMutex );
                    mCv.wait( lock, [this]()
                        {
                            return mStopping || !mQueue.empty();
                        } );
                    if( mQueue.empty() )
                    {
                        return;
                    }
                    event = mQueue.front();
                    mQueue.pop_front();
                }

                // Run and release with the lock dropped, as the framework this mimics does: a
                // handler is arbitrary code that may post, and the event's destructor is arbitrary
                // code too. Holding the mutex across either is what that code was careful to avoid.
                event->emit();
                event.reset();
            }
        }

        //! Asks run() to return once it has drained what is queued. Thread-safe.
        void stop()
        {
            {
                std::lock_guard<std::mutex> lock( mMutex );
                mStopping = true;
            }
            mCv.notify_one();
        }

    private:
        std::deque<boost::shared_ptr<MessageEvent> > mQueue;  //!< Events waiting to run.

        std::mutex mMutex;             //!< Guards mQueue and mStopping.

        std::condition_variable mCv;   //!< Wakes the consumer thread.

        bool mStopping { false };      //!< True once stop() has been called.
    };

    //! What the deferred calls arrive at.
    class DeferredCallReceiver
    {
    public:
        //! The call every deferred-call row makes.
        void onCall()
        {
            mCalls.fetch_add( 1, std::memory_order_release );
        }

        //! Calls delivered so far, read by the posting thread to pace itself against the consumer.
        std::atomic<int> mCalls { 0 };
    };

}

//! Measures establishing a connection.
TEST( Performance, Boost_Connect )
{
    BoostSignal sig;
    const double ns = timeLoop( kConnectOps, [&]( int )
        {
            sig.connect( []( int )
            {
            } );
        } );
    record( "connect()", "boost", ns );
}

//! Measures emit -> receive on one thread.
//!
//! signals2 has one delivery mode, so this single row is the counterpart of both the `direct` and
//! the `auto same-thread` rows the other libraries produce. It is recorded against `direct`, which
//! is the like-for-like comparison: neither path resolves a receiver's thread affinity.
TEST( Performance, Boost_DirectEmit )
{
    BoostSignal sig;
    long long received = 0;
    sig.connect( [&received]( int aValue )
        {
            received += aValue;
        } );

    sig( 1 );   // warm up
    const double ns = timeLoop( kDirectOps, [&]( int )
        {
            sig( 1 );
            keep( received );
        } );
    record( "emit->receive, direct", "boost", ns );
    EXPECT_GT( received, 0 );
}

//! Measures ending a connection through its handle.
//!
//! The row this column exists for. Every other scenario either has no boost equivalent or compares
//! machinery boost does not have; this one is a signal, a slot and a handle on both sides, with no
//! object model involved anywhere in the timed region.
//!
//! One caveat belongs on the number. `connection::disconnect()` flips a flag and drops the slot's
//! refcount; it does **not** unlink the entry from the signal's list, which signals2 sweeps later
//! from `connect()` or from an emit. This benchmark does neither afterwards, so boost's deferred
//! list maintenance falls outside the timed region while ours is inside it. That is a real
//! difference in design -- eager against lazy -- and not an artefact to correct for, but the number
//! is a lower bound on what a signals2 disconnect eventually costs.
//!
//! Only the disconnects are timed. Connecting is setup.
TEST( Performance, Boost_Disconnect )
{
    BoostSignal sig;
    long long received = 0;

    std::vector<boost::signals2::connection> handles;
    handles.reserve( kDisconnectOps );
    for( int i = 0; i < kDisconnectOps; ++i )
    {
        handles.push_back( sig.connect( [&received]( int aValue )
            {
                received += aValue;
            } ) );
    }

    const auto start = std::chrono::steady_clock::now();
    for( auto& handle : handles )
    {
        handle.disconnect();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "disconnect()", "boost",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kDisconnectOps );

    // Proves the handles really ended their connections, so the row above is not timing a no-op.
    sig( 1 );
    EXPECT_EQ( received, 0 );
}

//! Measures making one deferred call, lifetime-tracked, over boost's own queue.
//!
//! **This is the row the boost column was missing.** The scenario is not a signal: it is "call this
//! member function later, on that thread", which is what a queued metacall is, and which boost
//! answers with `function` + `bind` + `shared_ptr` + a queue of your own. Looking for it in
//! signals2 is what left the queued rows blank -- signals2 has no part in it.
//!
//! Lifetime-tracked, so it compares like with like. A queued metacall is cancelled when its
//! receiver dies; a bound raw pointer is not, and pricing a guarantee on one side only would
//! flatter whichever side skipped it. A `weak_ptr` the callback locks is the self-contained way to
//! buy it back. The other spelling -- the owner clearing the event from its own destructor -- buys
//! the same thing but needs a cooperating destructor, which would put one in the timed region.
//!
//! **Posted without waiting, deliberately.** What differs between this row and QtLikeSignal's is
//! the payload: two heap blocks and two indirections against a pooled block and one. A thread wake
//! is two orders of magnitude dearer than that, so pacing this round-trip would report the wake in
//! every column and bury the thing being compared.
TEST( Performance, Boost_DeferredCall )
{
    BoostEventQueue queue;
    std::thread consumer( [&queue]()
        {
            queue.run();
        } );

    const boost::shared_ptr<DeferredCallReceiver> receiver( new DeferredCallReceiver() );
    const boost::weak_ptr<DeferredCallReceiver> weak( receiver );

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < kDeferredCallOps; ++i )
    {
        queue.post( boost::shared_ptr<MessageEvent>( new FunctionCallbackEvent( [weak]()
            {
                const boost::shared_ptr<DeferredCallReceiver> alive = weak.lock();
                if( alive )
                {
                    alive->onCall();
                }
            } ) ) );
    }
    while( receiver->mCalls.load( std::memory_order_acquire ) < kDeferredCallOps )
    {
        std::this_thread::yield();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "deferred call, x-thread", "boost",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kDeferredCallOps );

    EXPECT_EQ( receiver->mCalls.load(), kDeferredCallOps );
    queue.stop();
    consumer.join();
}

//! Measures the same deferred call with nothing tracking the receiver's lifetime.
//!
//! The idiom exactly as application code writes it: `bind` over a raw `this`, which fires into
//! whatever is at that address whether or not the object is still there. The gap between this row
//! and the one above is what the guarantee costs, which is roughly one `weak_ptr::lock()`. If they
//! ever come out equal the guard has been optimised away and the tracked row measures nothing.
//!
//! Kept as its own row rather than replacing the tracked one because both are real: this is what
//! the code does, and the tracked row is what it would have to do to match a queued metacall.
TEST( Performance, Boost_DeferredCallUntracked )
{
    BoostEventQueue queue;
    std::thread consumer( [&queue]()
        {
            queue.run();
        } );

    DeferredCallReceiver receiver;

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < kDeferredCallOps; ++i )
    {
        queue.post( boost::shared_ptr<MessageEvent>( new FunctionCallbackEvent(
            boost::bind( &DeferredCallReceiver::onCall, &receiver ) ) ) );
    }
    while( receiver.mCalls.load( std::memory_order_acquire ) < kDeferredCallOps )
    {
        std::this_thread::yield();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "deferred call, untracked", "boost",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kDeferredCallOps );

    EXPECT_EQ( receiver.mCalls.load(), kDeferredCallOps );
    queue.stop();
    consumer.join();
}

//! Measures a boost callback event carried by **our** queue -- an application partway across.
//!
//! Recorded under QtLikeSignal because our loop is what runs it; the scenario name carries the
//! rest. This is what a codebase looks like when the transport has been migrated and the payload
//! has not: every deferred call still allocates its event and its control block and still arrives
//! through two indirections, and now a `std::function` closure carries the `shared_ptr` on top.
//!
//! It earns a row because it answers a question neither of the others does. `deferred call,
//! x-thread` says what boost's whole idiom costs against ours; this one says what is left to gain
//! from a migration already half made, which is the only number that helps someone in that
//! position decide whether to finish.
TEST( Performance, Boost_DeferredCallViaQtLikeSignalQueue )
{
    QtLikeSignal::Thread worker( "perf-deferred-half" );
    worker.start();

    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    ASSERT_TRUE( worker.post( [&ready]()
        {
            ready.set_value();
        } ) );
    ASSERT_EQ( readyFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    DeferredCallReceiver receiver;

    const auto start = std::chrono::steady_clock::now();
    for( int i = 0; i < kDeferredCallOps; ++i )
    {
        // Two objects and a capture, which is the shape such an application has: the event is built
        // and owned exactly as it always was, then handed to a closure that our queue carries.
        const boost::shared_ptr<MessageEvent> event( new FunctionCallbackEvent(
            boost::bind( &DeferredCallReceiver::onCall, &receiver ) ) );
        ( void )worker.post( [event]()
            {
                event->emit();
            } );
    }
    while( receiver.mCalls.load( std::memory_order_acquire ) < kDeferredCallOps )
    {
        std::this_thread::yield();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "deferred call, boost event via QtLikeSignal", "QtLikeSignal",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kDeferredCallOps );

    EXPECT_EQ( receiver.mCalls.load(), kDeferredCallOps );
    worker.quit();
    worker.wait();
}

//! Reports how many heap blocks one boost deferred call costs.
//!
//! **This is the number the nanosecond rows would otherwise be misread as.** Most of the gap
//! between a boost callback event and a queued metacall is the allocator rather than the dispatch,
//! so a timing row on its own reads as "QtLikeSignal dispatches faster" when what it says is
//! "QtLikeSignal dispatches without allocating". The two belong beside each other.
//!
//! Printed, never asserted. These shapes are boost's, and holding a library this repository does
//! not own to a value would produce a red build nobody here can fix. It is also legitimately
//! platform-dependent: whether `boost::function` reaches the heap for a `bind` expression depends
//! on how much of it fits the small-object buffer, and a member-function pointer is 8 bytes under
//! the Itanium ABI and up to 16 under MSVC. QtLikeSignal's own figure is asserted, in the
//! regression suite, where a number this repository controls belongs.
//!
//! Counted on the posting thread only. The counter is per-thread, so the consumer's frees are not
//! netted off -- and blocks allocated per call is the question, not blocks outstanding.
TEST( Performance, Boost_DeferredCallAllocations )
{
    if( !PerfHarness::Allocations::available() )
    {
        GTEST_SKIP() << "allocation counting is not linked in";
    }

    //! Runs @p aOps deferred calls through a fresh queue and returns heap blocks per call.
    //!
    //! The queue, the consumer thread, the receiver and one warm-up call all happen before counting
    //! starts, so thread start-up and the deque's first growth are not charged to the calls. Paced
    //! one at a time rather than saturated, which is what keeps the deque one deep and stops it
    //! growing again mid-count; the timed rows are saturated because they measure time, and this
    //! one is not because it measures blocks, which pacing does not change.
    auto blocksPerCall = []( int aOps, auto aMakeEvent )
        {
            BoostEventQueue queue;
            std::thread consumer( [&queue]()
                {
                    queue.run();
                } );

            // Owned here and handed to the factory, never created inside it. An owner built per
            // event would be the only strong reference, so it would die with the factory call and
            // leave the tracked callback holding an already-expired weak_ptr -- which locks to
            // null, calls nothing, and hangs the pacing loop below waiting for a call that cannot
            // arrive.
            const boost::shared_ptr<DeferredCallReceiver> receiver( new DeferredCallReceiver() );
            int done = 0;

            //! Posts one event and waits for it, so the queue never holds more than one.
            auto postOne = [&]()
                {
                    queue.post( aMakeEvent( receiver ) );
                    while( receiver->mCalls.load( std::memory_order_acquire ) <= done )
                    {
                        std::this_thread::yield();
                    }
                    ++done;
                };

            postOne();   // warm up

            PerfHarness::Allocations::start();
            for( int i = 0; i < aOps; ++i )
            {
                postOne();
            }
            const long blocks = PerfHarness::Allocations::stop();

            queue.stop();
            consumer.join();
            return static_cast<double>( blocks ) / aOps;
        };

    const double untracked = blocksPerCall( kDeferredCallAllocOps,
        []( const boost::shared_ptr<DeferredCallReceiver>& aReceiver )
        {
            return boost::shared_ptr<MessageEvent>( new FunctionCallbackEvent(
                boost::bind( &DeferredCallReceiver::onCall, aReceiver.get() ) ) );
        } );

    const double tracked = blocksPerCall( kDeferredCallAllocOps,
        []( const boost::shared_ptr<DeferredCallReceiver>& aReceiver )
        {
            const boost::weak_ptr<DeferredCallReceiver> weak( aReceiver );
            return boost::shared_ptr<MessageEvent>( new FunctionCallbackEvent( [weak]()
                {
                    const boost::shared_ptr<DeferredCallReceiver> alive = weak.lock();
                    if( alive )
                    {
                        alive->onCall();
                    }
                } ) );
        } );

    std::printf( "  %-34s %-13s %10.2f blocks/call\n", "deferred call, untracked", "boost",
        untracked );
    std::printf( "  %-34s %-13s %10.2f blocks/call\n", "deferred call, x-thread", "boost",
        tracked );
    std::fflush( stdout );

    // Not a threshold on boost, which is the point of the comment above. This only says the shape
    // allocates at all, so a counter that has silently stopped working shows up as a zero.
    EXPECT_GT( untracked, 0.0 );
    EXPECT_GT( tracked, 0.0 );
}
