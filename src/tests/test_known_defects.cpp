//! @file
//!
//! Tests for defects that are known, reproduced, and **not yet fixed**.
//!
//! Currently empty: R23, R24 and R26 were all fixed on 2026-08-08 and their tests moved to
//! test_defect_regressions.cpp. The file is kept because the convention is worth having in place
//! when the next defect turns up.
//!
//! **How to use it.** Prove a defect is real here first, as a test named `KnownDefect.*` that
//! fails. Leave it red. When the defect is fixed the test turns green and *moves out* to
//! test_defect_regressions.cpp, renamed to describe the correct behaviour rather than the bug.
//! Moving it is not tidying: `--gtest_filter=-KnownDefect.*` is the green baseline, so a passing
//! test left in here would be silently excluded from it. Never make a test here pass by weakening
//! it -- that is the one change that destroys its only purpose.
//!
//! **What does not belong here.** A test that encodes a requirement nobody has actually stated. Two
//! recorded risks are deliberately absent for that reason:
//!
//!   * **R25** (`Object::thread()` costs a mutex per call) is a documented trade-off, not a defect.
//!     No requirement says how fast it must be, so a failing test would be inventing one, and any
//!     wall-clock threshold would be machine-dependent.
//!   * **R27** (`Thread::create()` returns an owning raw pointer with no ownership documentation) is
//!     an API-shape issue with no runtime signature: the caller either deletes the pointer or leaks
//!     it. Asserting the return type were `unique_ptr` would fail to *compile* rather than fail as a
//!     test, breaking the build for everyone instead of reporting one defect.
//!
//! R26 is the cautionary example. Its first test here asserted that a repeating timer realigns its
//! cadence after a long stall -- which Qt does not do either, since it resynchronises past one
//! interval. That test would have failed against Qt itself, so it was an invented requirement in
//! exactly the sense above, and it was replaced rather than moved when the real (narrower) defect
//! was fixed.
//!
//! **Sanitizers.** Address and thread sanitizers are mutually exclusive in tools/toolchain-linux.py,
//! so only one runs at a time; switch with `./waf configure --enable-address-sanitizer-on-Linux`.
//! Run both in turn -- they catch disjoint classes, and neither would have caught R23, whose growth
//! was not a leak at all but reachable memory that LeakSanitizer is designed to ignore.
