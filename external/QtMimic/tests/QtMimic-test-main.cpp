//! @file
//!
//! GoogleTest suite for the QtMimic framework (Object affinity/connections,
//! Thread event loops, and CoreApplication).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "gtest/gtest.h"

int main
    (
    int argc,
    char** argv
    )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
