//! @file
//!
//! GoogleTest suite for Signal, SignalView and Connection themselves.
//!
//! The rest of the suite exercises signals through Object::connect(), which is the right level for
//! testing Object but the wrong one for testing the signal: it can only reach the guarantees
//! indirectly, and a signal that broke one of them would show up as a puzzling failure three
//! layers away. These tests aim at the guarantees directly.
//!
//! Four of them matter, and they are the four ForAI/mission-signal.md identified as the reason
//! replacing boost::signals2 was delicate. Each one fails silently rather than at compile time.


#include "QtLikeSignal-test-types.h"

#include "gtest/gtest.h"
#include "Object.h"
#include "Signal.h"
#include "Thread.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>

using namespace QtLikeSignal;

//! Counts its own copies and moves, for asserting exactly what an emit() costs.
//!
//! Movable on purpose. Declaring only a copy constructor would suppress the implicit move one, and
//! every move below would silently degrade into a copy -- which says more about the probe than
//! about the signal. Real payloads are movable; HeavyPayload in the stress suite is.
struct CopyCounter
{
    CopyCounter() = default;

    CopyCounter
        (
        const CopyCounter&
        )
    {
        ++sCopies;
    }

    CopyCounter
        (
        CopyCounter&&
        ) noexcept
    {
        ++sMoves;
    }

    CopyCounter& operator=
        (
        const CopyCounter&
        ) = delete;

    //! Zeroes both counters.
    static void reset()
    {
        sCopies = 0;
        sMoves = 0;
    }

    static int sCopies;
    static int sMoves;
};

int CopyCounter::sCopies = 0;
int CopyCounter::sMoves = 0;

//================================================================
// The basics
//================================================================

//! A connected slot is called with the emitted arguments; a disconnected one is not.
TEST( SignalTest, ConnectEmitDisconnect )
{
    Signal<int> sig;
    int seen = 0;

    Connection handle = sig.connect( [&seen]( int aValue )
        {
            seen += aValue;
        } );
    EXPECT_TRUE( handle.connected() );
    EXPECT_EQ( sig.receivers(), 1U );
    EXPECT_FALSE( sig.empty() );

    sig.emit( 5 );
    EXPECT_EQ( seen, 5 );

    handle.disconnect();
    EXPECT_FALSE( handle.connected() );
    EXPECT_EQ( sig.receivers(), 0U );
    EXPECT_TRUE( sig.empty() );

    sig.emit( 7 );
    EXPECT_EQ( seen, 5 ) << "a disconnected slot was called";
}

//! Copies of a handle refer to the same connection, which is what lets ~Cleanup find its entry in
//! Object::mIncoming by comparing handles.
TEST( SignalTest, HandleCopiesCompareEqualAndShareState )
{
    Signal<> sig;
    Connection first = sig.connect( []()
        {
        } );
    Connection copy = first;
    Connection other = sig.connect( []()
        {
        } );

    EXPECT_TRUE( first == copy );
    EXPECT_FALSE( first == other );
    EXPECT_TRUE( first != other );

    copy.disconnect();
    EXPECT_FALSE( first.connected() ) << "disconnecting a copy must disconnect the connection";
    EXPECT_TRUE( other.connected() );

    EXPECT_TRUE( Connection() == Connection() ) << "two null handles refer to the same nothing";
    EXPECT_FALSE( Connection().connected() );
}

//! A handle may outlive its signal, which Object::mIncoming does routinely.
TEST( SignalTest, HandleOutlivingItsSignalIsSafe )
{
    Connection handle;
    {
        Signal<> sig;
        handle = sig.connect( []()
            {
            } );
        EXPECT_TRUE( handle.connected() );
    }

    EXPECT_FALSE( handle.connected() ) << "the signal is gone, so the connection is too";
    handle.disconnect();  // must be a harmless no-op rather than a use-after-free
}

//================================================================
// The four guarantees
//================================================================

//! Guarantee 1: a slot stays alive for its whole invocation, even if it disconnects itself.
//!
//! The captured probe is destroyed with the slot. If disconnecting freed the slot immediately, the
//! rest of this slot body would be running inside a destroyed closure.
TEST( SignalTest, SlotSurvivesDisconnectingItselfMidCall )
{
    Signal<> sig;
    auto alive = std::make_shared<std::atomic<bool> >( true );
    std::weak_ptr<std::atomic<bool> > watch = alive;

    Connection handle;
    bool stillAliveAfterDisconnect = false;
    handle = sig.connect( [&handle, &stillAliveAfterDisconnect, alive]()
        {
            handle.disconnect();
            // Still inside the closure that owns `alive`; it must not have been destroyed.
            stillAliveAfterDisconnect = alive->load();
        } );
    alive.reset();

    sig.emit();
    EXPECT_TRUE( stillAliveAfterDisconnect );
    EXPECT_TRUE( watch.expired() ) << "the slot should be destroyed once the call returns";
}

//! Guarantee 2: no lock is held while a slot runs, so a slot may re-enter the signal freely.
TEST( SignalTest, SlotMayReenterTheSignal )
{
    Signal<int> sig;
    int depth = 0;
    int maxDepth = 0;

    sig.connect( [&sig, &depth, &maxDepth]( int aValue )
        {
            ++depth;
            maxDepth = std::max( maxDepth, depth );
            if( aValue > 0 )
            {
                sig.emit( aValue - 1 );   // nested emit, from inside a slot
            }
            --depth;
        } );

    sig.emit( 3 );
    EXPECT_EQ( maxDepth, 4 ) << "a nested emit deadlocked or was refused";
}

//! Guarantee 2, continued: connecting and disconnecting from inside a slot must not deadlock.
TEST( SignalTest, SlotMayConnectAndDisconnectFromInsideACall )
{
    Signal<> sig;
    int added = 0;

    sig.connect( [&sig, &added]()
        {
            Connection inner = sig.connect( [&added]()
                {
                    ++added;
                } );
            inner.disconnect();
        } );

    sig.emit();
    SUCCEED() << "reaching here at all is the assertion; a held lock would have deadlocked";
}

//! Guarantee 3: a connection made during an emission does not run in that emission.
TEST( SignalTest, ConnectionMadeDuringEmissionRunsOnlyNextTime )
{
    Signal<> sig;
    int lateCalls = 0;
    bool added = false;

    sig.connect( [&sig, &lateCalls, &added]()
        {
            if( added )
            {
                return;
            }
            added = true;
            sig.connect( [&lateCalls]()
            {
                ++lateCalls;
            } );
        } );

    sig.emit();
    EXPECT_EQ( lateCalls, 0 ) << "a slot connected mid-emission ran in that same emission";

    sig.emit();
    EXPECT_EQ( lateCalls, 1 );
}

//! Guarantee 4: a slot disconnected during an emission is not called in that emission.
TEST( SignalTest, SlotDisconnectedDuringEmissionIsSkipped )
{
    Signal<> sig;
    int firstCalls = 0;
    int secondCalls = 0;
    Connection second;

    sig.connect( [&firstCalls, &second]()
        {
            ++firstCalls;
            second.disconnect();   // the slot behind us in this very emission
        } );
    second = sig.connect( [&secondCalls]()
        {
            ++secondCalls;
        } );

    sig.emit();
    EXPECT_EQ( firstCalls, 1 );
    EXPECT_EQ( secondCalls, 0 ) << "a slot disconnected earlier in this emission was still called";
}

//! Guarantee 4, the blunt form: disconnectAll() from inside a slot aborts the rest.
TEST( SignalTest, DisconnectAllDuringEmissionAbortsTheRest )
{
    Signal<> sig;
    int calls = 0;

    sig.connect( [&sig, &calls]()
        {
            ++calls;
            sig.disconnectAll();
        } );
    for( int i = 0; i < 10; ++i )
    {
        sig.connect( [&calls]()
            {
                ++calls;
            } );
    }

    sig.emit();
    EXPECT_EQ( calls, 1 ) << "slots after the disconnectAll() still ran";
    EXPECT_TRUE( sig.empty() );
}

//================================================================
// Arguments and views
//================================================================

//! Emission copies nothing of its own: the only copy is the one each by-value slot parameter needs.
TEST( SignalTest, EmitCopiesOncePerByValueSlotAndNoMore )
{
    Signal<CopyCounter> byValue;
    byValue.connect( []( CopyCounter )
        {
        } );
    byValue.connect( []( CopyCounter )
        {
        } );

    CopyCounter payload;
    CopyCounter::reset();
    byValue.emit( payload );

    // One copy per slot and no more. Emission itself copies nothing: it holds the caller's object
    // by reference and hands it to each slot in turn, and the copy is the one the slot's own
    // by-value parameter needs.
    EXPECT_EQ( CopyCounter::sCopies, 2 ) << "emission copied more than the slot parameters needed";

    // The moves are std::function's internal hop from its own parameter to the target's, which is
    // a move for any movable type. Asserted rather than ignored, so that a change turning these
    // back into copies -- as it silently would for a type with no move constructor -- is visible.
    EXPECT_EQ( CopyCounter::sMoves, 2 );

    Signal<const CopyCounter&> byRef;
    byRef.connect( []( const CopyCounter& )
        {
        } );
    CopyCounter::reset();
    byRef.emit( payload );
    EXPECT_EQ( CopyCounter::sCopies, 0 ) << "a by-reference signal must copy nothing at all";
    EXPECT_EQ( CopyCounter::sMoves, 0 );
}

//! A view connects to the signal it views, and cannot emit it -- the latter asserted at compile
//! time in QtLikeSignal-test-object.cpp, which checks SignalView has no emit().
TEST( SignalTest, ViewConnectsToTheViewedSignal )
{
    Signal<int> sig;
    Object context;
    int seen = 0;

    Object::connect( sig.view(), &context, [&seen]( int aValue )
        {
            seen = aValue;
        } );

    sig.emit( 9 );
    EXPECT_EQ( seen, 9 );
}

//================================================================
// Concurrency
//================================================================

//! Connect, disconnect and emit concurrently from several threads without corruption.
//!
//! There is no assertion on counts -- the interleaving decides those. What is being tested is that
//! this is safe at all, which the sanitizer builds are the real judge of.
TEST( SignalTest, ConcurrentConnectDisconnectAndEmitIsSafe )
{
    constexpr int kThreads = 4;
    constexpr int kRounds = 500;

    Signal<int> sig;
    std::atomic<int> calls { 0 };
    std::atomic<bool> stop { false };

    std::thread emitter( [&sig, &stop]()
        {
            while( !stop.load() )
            {
                sig.emit( 1 );
            }
        } );

    std::vector<std::thread> churners;
    for( int t = 0; t < kThreads; ++t )
    {
        churners.emplace_back( [&sig, &calls]()
            {
                for( int i = 0; i < kRounds; ++i )
                {
                    Connection handle = sig.connect( [&calls]( int aValue )
                        {
                            calls.fetch_add( aValue, std::memory_order_relaxed );
                        } );
                    handle.disconnect();
                }
            } );
    }

    for( auto& churner : churners )
    {
        churner.join();
    }
    stop.store( true );
    emitter.join();

    EXPECT_TRUE( sig.empty() ) << "every connection was disconnected, so none should remain";
}
