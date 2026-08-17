// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Dispatch-overhead benchmarks for boost::signals2, run alongside the QtLikeSignal, QtMimic and
//! Qt 6 ones so every library appears in a single comparison table.
//!
//! boost::signals2 is where this project started. QtLikeSignal took its signal from it, QtMimic
//! followed, and both replaced it with an in-house Signal in August 2026. It belongs in the table as
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
//! Note that this is **not** the `QtMimic (boost)` column in history/PERFORMANCE-20260813.md. That
//! was QtMimic's own machinery with a boost signal underneath, which is why it had all four rows.
//! This is raw signals2, and it is a narrower claim.
//!
//! Built only where boost's headers are installed; see src/tests/wscript. signals2 is header-only,
//! so nothing here is linked -- on Debian and Ubuntu `sudo apt install libboost-dev` is the whole
//! dependency, and no boost submodule or bootstrap comes back with it.
//!
//! Its own translation unit for the reason every library here has one: it keeps the boost headers
//! out of the builds that do not have them, which is what lets waf add and drop this file wholesale.

#include "PerfHarness.hpp"

#include <gtest/gtest.h>

#include <boost/signals2/connection.hpp>
#include <boost/signals2/signal.hpp>

#include <chrono>
#include <memory>
#include <vector>

using PerfHarness::keep;
using PerfHarness::kConnectOps;
using PerfHarness::kDirectOps;
using PerfHarness::kTeardownResident;
using PerfHarness::record;
using PerfHarness::timeLoop;

namespace
{
    //! The signal every benchmark below drives. One `int`, matching the other libraries.
    using BoostSignal = boost::signals2::signal<void ( int )>;

    //! Holds one connection and ends it on destruction.
    //!
    //! The counterpart of a QtLikeSignal `Object` or a `QObject` receiver, and the thing whose
    //! destructor the teardown scenario times. signals2 has no receiver concept of its own, so a
    //! `scoped_connection` is the closest equivalent -- and it is what our receivers held internally
    //! back when they were built on boost, so the comparison is like for like.
    class BoostPerfReceiver
    {
    public:
        //! Connects @p aSlot to @p aSignal, holding the connection for as long as this object lives.
        template <typename Callable>
        BoostPerfReceiver
            (
            BoostSignal& aSignal,   //!< Signal to connect to.
            Callable aSlot          //!< Slot to invoke on emission.
            )
            : mConnection( aSignal.connect( aSlot ) )
        {
        }

    private:
        boost::signals2::scoped_connection mConnection;
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

//! Measures destroying N receivers that are all connected to one long-lived signal.
//!
//! The row boost exists in this table for. P7 was a quadratic teardown -- 16 000 receivers took
//! 671 ms against boost's 2.94 ms -- and after the fix we were still 1.35x behind on this one
//! operation while ahead on every other. That is a live comparison rather than a historical one, so
//! it is worth measuring in the same process as everything else rather than trusting a number
//! recorded in a document last August.
//!
//! Only the teardown is timed. Connecting is setup, and timing it too would blur the thing being
//! measured.
TEST( Performance, Boost_TeardownAtScale )
{
    BoostSignal sig;
    long long received = 0;

    std::vector<std::unique_ptr<BoostPerfReceiver> > receivers;
    receivers.reserve( kTeardownResident );
    for( int i = 0; i < kTeardownResident; ++i )
    {
        receivers.push_back( std::unique_ptr<BoostPerfReceiver>(
            new BoostPerfReceiver( sig, [&received]( int aValue )
            {
                received += aValue;
            } ) ) );
    }

    const auto start = std::chrono::steady_clock::now();
    receivers.clear();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    record( "destroy N receivers", "boost",
        std::chrono::duration<double, std::nano>( elapsed ).count() / kTeardownResident );

    // Proves the destructors really did disconnect, so the row above is not the time to destroy
    // N objects that were never attached to anything. Checked by emitting rather than by reading a
    // slot count, so the same check can be written for every library in this table.
    sig( 1 );
    EXPECT_EQ( received, 0 );
}
