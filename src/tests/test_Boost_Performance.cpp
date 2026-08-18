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
using PerfHarness::kDisconnectOps;
using PerfHarness::record;
using PerfHarness::timeLoop;

namespace
{
    //! The signal every benchmark below drives. One `int`, matching the other libraries.
    using BoostSignal = boost::signals2::signal<void ( int )>;

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
