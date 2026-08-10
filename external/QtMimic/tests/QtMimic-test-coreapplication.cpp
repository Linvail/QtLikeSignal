//! @file
//!
//! GoogleTest suite for QtMimic::CoreApplication.
//!
//! Deliberately parallel to QtLikeSignal's counterpart file -- same tests, same order,
//! same names -- so the two can be diffed against each other. See
//! history/TEST-UNIFICATION-PLAN-20260810.md.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic-test-types.hpp"

#include "CoreApplication.hpp"

#include "gtest/gtest.h"
#include <chrono>
#include <future>

namespace
{

    using namespace std::chrono_literals;
    using namespace QtMimic;

    /*----------------------------------------------------------
       Verify CoreApplication executes posted work on the main
       thread and exits with code 0 when quit is posted.
       ----------------------------------------------------------*/
    TEST( CoreApplicationTest, PostAndExec )
    {
        int argc = 1;
        char arg0[] = "core-application-test";
        char* argv[] = { arg0, nullptr };

        CoreApplication app( argc, argv );
        EXPECT_EQ( CoreApplication::instance(), &app );

        std::thread::id mainId = std::this_thread::get_id();
        std::thread::id taskId;

        CoreApplication::post( [&]()
            {
                taskId = std::this_thread::get_id();
                CoreApplication::quit();
            } );

        int rc = app.exec();
        EXPECT_EQ( rc, 0 );
        EXPECT_EQ( taskId, mainId );
    }

} // namespace
