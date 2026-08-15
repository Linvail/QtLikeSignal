// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

//! Entry point for running GoogleTest unit test suite. Returns 0 if all tests pass, non-zero
//! otherwise.
int main
    (
    int aArgc,     //!< Command line argument count.
    char** aArgv   //!< Command line argument vector.
    )
{
    ::testing::InitGoogleTest( &aArgc, aArgv );
    return RUN_ALL_TESTS();
}
