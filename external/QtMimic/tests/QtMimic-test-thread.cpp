//! @file
//!
//! GoogleTest suite for QtMimic::Thread's event loop and lifecycle signals.
//!
//! Deliberately parallel to QtLikeSignal's counterpart file -- same tests, same order,
//! same names -- so the two can be diffed against each other. See
//! history/TEST-UNIFICATION-PLAN-20260810.md.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic-test-types.hpp"

#include "gtest/gtest.h"
#include <chrono>
#include <future>

namespace
{

    using namespace std::chrono_literals;
    using namespace QtMimic;

    /*----------------------------------------------------------
       Verify Thread lifecycle signals are emitted for both
       startup and shutdown transitions.
       ----------------------------------------------------------*/
    TEST( ThreadTest, StartedAndFinishedSignalsAreEmitted )
    {
        Thread worker( "thread-lifecycle-worker" );
        Thread localThread;
        Object local( &localThread ); // context for the signal connections

        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool finished = false;

        localThread.start();

        // Connect BEFORE start(): the started signal is emitted from inside the
        // worker's loop the moment it begins, so a connection made afterwards could
        // miss it.
        auto startedConnection = Object::connect( worker.getStarted(), &local, [&]()
            {
                std::lock_guard<std::mutex> locker( mutex );
                started = true;
                cv.notify_all();
            } );

        auto finishedConnection = Object::connect( worker.getFinished(), &local, [&]()
            {
                std::lock_guard<std::mutex> locker( mutex );
                finished = true;
                cv.notify_all();
            } );

        worker.start();

        {
            std::unique_lock<std::mutex> locker( mutex );
            EXPECT_TRUE( cv.wait_for( locker, 2s, [&]
                {
                    return started;
                } ) );
        }

        worker.quit();
        worker.wait();

        {
            std::unique_lock<std::mutex> locker( mutex );
            EXPECT_TRUE( cv.wait_for( locker, 2s, [&]
                {
                    return finished;
                } ) );
        }

        startedConnection.disconnect();
        finishedConnection.disconnect();

        localThread.quit();
        localThread.wait();
    }

} // namespace
