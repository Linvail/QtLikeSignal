# Performance findings — snapshot 2026-08-17

boost::signals2 is back in the comparison table, as a fourth column measured in the same process as
the other three. Items are `P<n>`, continuing the numbering from `PERFORMANCE-20260813.md`.

It adds a **measurement**, fixes two defects in the benchmark harness — one of which had been quietly
distorting the `connect()` row for as long as that row has existed — withdraws a comparison that was
measuring the wrong thing, and takes three costs out of `Object` that bought nothing.

Written 2026-08-17; extended and partly retracted 2026-08-18.

How every figure below was produced is at the end, under
[How these were measured](#how-these-were-measured). Read it before quoting a number.

## Status

| ID | Status | Finding | Impact | Latest measurement |
|----|--------|---------|--------|--------------------|
| P11 | **Withdrawn** | "Teardown is ~3x boost" — the scenario compared `~Object()` against a 16-byte connection handle, and measured the object model rather than the disconnect | Medium | Replaced by a `disconnect()` row: **79.7 ns** against boost's 81.1 and Qt 6's 70.7. The three `Object` savings it turned up are kept |
| P12 | **Fixed** | The summary table's ratio baseline was whichever library linked first, so adding a fourth benchmark file silently inverted every ratio | High (in the tool) | Baseline is now named, not inferred |
| P13 | **Fixed** | The first allocation-heavy test in a process paid for heap growth, and the `connect()` row charged it to whichever library ran first | High (in the tool) | boost's connect **358.3 → 190.7 ns**; our advantage over boost on that row was overstated by 1.7x |

Two things are worth knowing without reading further:

- **The direct-emit and queued results from 2026-08-13 hold up.** We are slightly ahead of Qt 6 on
  direct emit (0.90x), level on queued cross-thread (1.09x), and 2.7x faster than boost on emit —
  against the 2.7x recorded then, from an independent run four days later on a rebuilt harness. That
  part of P6 was solid.
- **Two rows were measuring something other than what they claimed.** The teardown row compared our
  `Object` destructor against a sixteen-byte boost handle and reported the difference as a disconnect
  cost (P11, withdrawn — replaced by a `disconnect()` row where we are at parity with boost). The
  `connect()` row was measuring run position as much as it was measuring libraries (P13, fixed).

---

# Details

## The table

`linux64-clang`, `--mode=release`, no sanitizer. boost 1.74.0, Qt 6.11.1. Each cell is the **minimum
of three consecutive runs**, for the reason `PerfHarness::bestOf` already documents: the minimum
estimates how fast the code can go, and everything above it is the scheduler.

```
scenario                        QtLikeSignal      boost       Qt6    QtMimic   vs boost  vs Qt6  vs QtMimic
-----------------------------------------------------------------------------------------------------------
connect()                          104.0 ns    188.6 ns   82.9 ns   111.1 ns     0.55x   1.25x     0.94x
emit->receive, direct               24.7 ns     67.0 ns   29.1 ns    25.7 ns     0.37x   0.85x     0.96x
disconnect()                        79.7 ns     81.1 ns   70.7 ns    74.5 ns     0.98x   1.13x     1.07x
emit->receive, auto same-thread     49.3 ns          -    28.5 ns    47.5 ns         -   1.73x     1.04x
emit->receive, queued x-thread     477.2 ns          -   437.1 ns   441.6 ns         -   1.09x     1.08x
```

Ratios are QtLikeSignal against each other library; above 1 means we are slower.

Run-to-run spread over those three runs, so the ratios are read with the right precision:

| row | QtLikeSignal | boost | Qt6 | QtMimic |
|---|---|---|---|---|
| `connect()` | 104.0–109.3 | 188.6–242.4 | 82.9–120.3 | 111.1–121.0 |
| `direct` | 24.7–27.3 | 67.0–69.3 | 29.1–29.9 | 25.7–28.7 |
| `disconnect()` | 79.7–89.3 | 81.1–96.1 | 70.7–78.6 | 74.5–100.0 |
| `auto same-thread` | 49.3–51.2 | – | 28.5–29.9 | 47.5–49.4 |
| `queued x-thread` | 477.2–540.5 | – | 437.1–472.2 | 441.6–488.4 |

`connect()` remains the noisiest row by a wide margin — the ratio against Qt 6 ranged 1.21x to 1.64x
across those three runs — which is what `PERFORMANCE-20260813.md` already said about it, and why the
allocation-count guard is the sharper instrument for that path.

The two blank boost cells are not omissions. signals2 is a signal library, not an object and
threading framework: it has no thread affinity and no event loop, so those scenarios have no boost
equivalent. An "auto" row would re-measure the direct row, and a "queued" row would have to borrow
our event loop and would then be measuring our queue. `test_Boost_Performance.cpp` says the same at
greater length.

## P11 — the teardown comparison was measuring the wrong thing *(Withdrawn)*

This entry originally read "teardown against raw boost::signals2 is ~3x, not the 1.35x recorded on
2026-08-13", against `PERFORMANCE-20260813.md` (P7):

> We are 1.35x slower than boost on teardown alone and 2.4x faster on connect.

**The ~3x was real and the conclusion drawn from it was not.** The scenario destroyed N receivers
and timed it. On our side a receiver is an `Object` — thread affinity, a life flag, an incoming
connection list, a queued-event strip, a timer-id return. On boost's side it is a
`scoped_connection`: sixteen bytes, no identity, no thread, no event loop. The row was comparing an
object model against a handle and reporting the difference as a disconnect cost.

The decomposition below made that unmistakable and was not read carefully enough at the time: **the
bare `~Object()` with no connections at all already cost more than boost's entire teardown**, 110.6
against 86.6. Nothing about the disconnect path was needed to produce the ~3x.

### Replaced by a `disconnect()` row

The row is gone. In its place the table measures what both libraries genuinely have in common:
`Connection::disconnect()` against `boost::signals2::connection::disconnect()`. One signal, N slots
connected through `Signal::connect()` so no receiver exists at all, N handle disconnects timed. No
`~Object()` anywhere in the timed region, on any side.

| `disconnect()`, min of 3 runs | ns |
|---|---|
| QtLikeSignal | **79.7** |
| QtMimic | 74.5 |
| Qt 6 | 70.7 |
| boost | 81.1 |

**We are at parity with boost** — 0.84x, 0.93x and 1.01x over three runs, changing sign, which by
this document's own standard is not a gap at all — and about 1.1x Qt 6. There is nothing left here
to fix.

One caveat belongs on boost's number, recorded so it is not mistaken for an artefact to correct.
`connection::disconnect()` flips a flag and drops the slot's refcount; it does **not** unlink the
entry from the signal's list, which signals2 sweeps later from `connect()` or from an emit. The
benchmark does neither afterwards, so boost's list maintenance falls outside the timed region while
ours is inside it. That is a real design difference — lazy against eager — and ours buys bounded
memory and an emit that never walks dead entries, which is what P7 was about. It does mean boost's
figure is a lower bound.

### What the investigation produced anyway

The wrong scenario still pointed at real costs, all in `Object` rather than in disconnect, and all
kept:

| | before | after |
|---|---|---|
| bare `~Object()` | 110.6 ns | **86.8 ns** |
| `Object` construction | 81.2–88.4 ns | **58.7–66.6 ns** |
| allocations per `Object` construction | 3.003 | **2.002** |

Three changes, none of which gave up a feature:

- **`mUsedTimers`**, a third set-once flag beside `mUsedCallLater` and `mMayHaveQueuedWork`. The
  destructor took `mRunningTimerIdsMutex` unconditionally to swap a vector that is empty for every
  object that never started a timer. Same guard and same trade as its two siblings.
- **`Affinity::namesOtherRunningThread()`**. The cross-thread-destruction diagnostic called
  `Affinity::data()`, which copies a `shared_ptr` out from under the mutex — two atomic
  read-modify-writes to keep alive, for the length of two atomic loads, something the mutex already
  held. Asked as one question now, answered inside the mutex. The diagnostic is unchanged and the
  `Thread*` is still only ever compared, never dereferenced.
- **The life token folded into the affinity box.** Every `Object` carried two separately allocated
  boxes — `mLife`, a `shared_ptr<int>`, and `mAffinity` — where Qt allocates one `QObjectPrivate`.
  Both had to outlive the Object and both were captured by the same closures, so keeping them apart
  cost an allocation, a free, and a second sixteen-byte capture per connection to carry one bit. The
  flag is now `Affinity::isObjectAlive()`, a plain atomic load.

  `Object::objectLife()` is public and had a test, so it still works: it returns an `ObjectLife`
  token rather than a `std::weak_ptr<int>`. `expired()` is unchanged and the token still outlives
  the Object. Only the type's name changed, which is an improvement on its own — the old signature
  leaked `shared_ptr<int>` into the public interface.

The saving from the merge is at **construction**, not teardown. That is the third time in this
document that removing an allocation did not move the time it was expected to — see P13, and the
reverted stack buffer below. On these paths the allocation count and the clock are close to
independent, and each has to be measured on its own.

### A change that was tried and reverted

Replacing the destructor's `std::vector` of incoming connections with an eight-slot stack buffer. It
did exactly what it was meant to — allocations per destroy went from **1.004 to 0.004** — and the
scenario got **30 ns slower**, reproducibly, at every inline capacity tried:

| inline capacity | `~Object()`, one connection |
|---|---|
| vector (original) | 278.6 ns |
| 1 | 282.0 ns |
| 2 | 292.7 ns |
| 4 | 295.6 ns |
| 8 | 289.9 ns |

The allocation it removed was a hot tcache hit costing less than the zeroing and destruction of the
buffer that replaced it. **Reverted.** Recorded because "remove the allocation" is the obvious next
idea and the allocation guards next door would have scored it a win.

### A build trap the merge sets

Removing `mLife` changes `sizeof(Object)` and the offset of every member after it. An **incremental**
build that leaves one translation unit compiled against the old layout produces a binary that
crashes with an access violation — order-dependent, and disappearing under a sanitizer, which is the
most misleading combination a bug can have. That happened on win64-msvc, reproducibly across three
runs, and vanished on `waf clean` followed by a full rebuild.

Not a defect in the change, but anyone pulling it wants a clean build first, and anyone bisecting
across it should not trust an incremental one.

### The lesson worth keeping

A benchmark row is a claim about what two libraries have in common. This one asserted that
destroying a receiver is the same operation in both, and it is not. The number was correct and
reproducible for eight days, and it was still measuring the wrong thing — which no amount of
run-to-run care would have caught, because the error was in the scenario and not in the timing.

## P12 — the summary table's ratio baseline followed link order *(Fixed)*

`PerfHarness::printSummary()` made the ratio baseline **whichever library recorded a result first**,
documented as "column order follows first appearance".

First appearance is registration order, registration order is static-initialisation order, and that
follows link order across translation units. Adding `test_Boost_Performance.cpp` as a fourth
benchmark file put boost at the front. Every ratio in the table inverted — the same measurements,
now meaning the opposite thing, with only the footer line changing to say so:

```
Ratios are boost against each other library; > 1 means boost is slower.
```

A reader comparing this snapshot against the last one would have read `0.34x` where the previous
document said `2.9x` and concluded something had improved by 8x.

**Fixed** by naming the baseline (`PerfHarness::baselineLibrary()`, `QtLikeSignal`) and rotating it
to the front of the column list, so the ratios read the same way regardless of link order, of
`--gtest_filter`, or of `--gtest_shuffle`. A filtered run that excludes the baseline falls back to
the first column, and the footer still names whatever that is.

A defect in the instrument rather than in the library, which is why it earns a numbered entry: a
benchmark that can silently change direction is worse than no benchmark.

## P13 — the first allocating test in a process paid for heap growth *(Fixed)*

`connect()` takes two heap blocks per connection and the row runs 20 000 of them. Whichever
library's `connect()` test ran **first in the process** therefore paid for the arena growing to hold
40 000 blocks, and charged it to that library.

Same binary, same test, same machine:

| `Performance.QtLikeSignal_Connect` | ns/op |
|---|---|
| with other tests ahead of it | 122.7 |
| first test in the process (3 runs) | 232.4, 203.7, 314.5 |

A **1.9x artefact**, larger than any real difference in the row it sits in.

**How far this reached.** After P12 moved boost to the front of the link order, boost's `connect()`
became the first allocating test in the process — so boost paid the growth cost for all four
libraries, and the first table produced for this snapshot recorded boost at **358.3 ns** and a
`0.34x` ratio in our favour. That number was wrong, and it was wrong in the direction most likely to
be quoted approvingly. The same mechanism explains an apparent 1.8x gap between QtLikeSignal and
QtMimic that survived five consecutive runs and looked entirely systematic — see
[Why QtLikeSignal and QtMimic differ](#why-qtlikesignal-and-qtmimic-differ).

**Running each test in its own process is not the fix**, which is worth stating because it is the
obvious first idea. It makes every library cold instead of one of them, and the cold penalty is not
uniform — ours is about 1.7x, Qt 6's about 1.1x — so the ratios move anyway:

| `connect()`, isolated, 4 runs each | ns/op |
|---|---|
| QtLikeSignal | 207.3, 214.4, 295.8, 253.6 |
| QtMimic | 198.6, 216.4, 268.8, 249.9 |
| Qt6 | 137.0, 152.2, 137.4, 126.6 |
| boost | 297.3, 259.9, 330.8, 272.6 |

**Fixed** by `PerfHarness::settleHeap()`, called from `main()` beside the existing
`settleAllocatorState()` and before any measurement. It allocates `2 * kConnectOps` blocks, writes
to each — reserving address space is cheap, it is the page fault on first touch that costs — and
frees them, leaving the arena grown and faulted in for whichever library runs first. The same run,
isolated, after the fix:

| `connect()`, isolated **and warmed**, 4 runs each | ns/op |
|---|---|
| QtLikeSignal | 162.4, 149.6, 179.5, 174.4 |
| QtMimic | 163.3, 192.8, 162.6, 167.6 |
| Qt6 | 86.5, 107.8, 93.7, 104.2 |
| boost | 187.5, 238.3, 214.8, 235.3 |

boost's in-process figure moves from 358.3 ns to 190.7 ns, and the systematic QtLikeSignal-vs-QtMimic
gap disappears entirely.

**A residual remains, and is not claimed as fixed.** Isolated-and-warmed, QtLikeSignal's `connect()`
still measures 149.6 ns against 112.0 ns in a full run, so warming the heap removes most of the
first-mover cost but not all of it. What is left is something other than the allocator — CPU
frequency ramp, instruction cache, gtest fixture setup — and it is small enough that the row's own
run-to-run spread covers it. Recorded here so that nobody re-derives it from scratch.

**The other four rows were never affected.** They allocate little or nothing per operation, which is
also why the allocation-count guards in `test_QtLikeSignal_Regression.cpp` caught none of this: a
count does not care how warm the heap is. That is a point in their favour, not against them — but it
does mean the timing rows needed a fix the counting rows did not.

---

# Why QtLikeSignal and QtMimic differ

They do not. The question is worth answering in full because the table looked as though they did,
and because chasing it is what uncovered P13.

**The sources are identical.** Comparing all 24 header and implementation files, after normalising
the library name and header-guard prefix, the only differences are:

- the SPDX lines, which QtLikeSignal carries and QtMimic does not;
- the `Copyright 2026 by Garmin Ltd. or its subsidiaries.` line, which QtMimic carries and
  QtLikeSignal does not;
- explanatory comments present in QtLikeSignal and absent in QtMimic — the `PERFORMANCE-...` and
  `ObjectTest....` back-references, the note on why `Args&...` cannot be used, the ThreadSanitizer
  story behind the two-list design;
- one blank line, in `Connection.hpp`.

No executable statement differs. The compile flags are identical (`/std:c++17 /permissive- /utf-8
/W4 …`, differing only in include paths), neither library is built as a shared object, and both are
linked into the benchmark binary as plain object files.

**So the differences in the table are measurement noise**, and the evidence is that the sign of the
difference is not stable. Five consecutive runs, QtLikeSignal against QtMimic:

| run | direct emit | ratio | teardown (row since withdrawn) | ratio |
|---|---|---|---|---|
| 1 | 28.2 vs 25.9 ns | 1.09x | 328.7 vs 285.2 ns | 1.15x |
| 2 | 27.8 vs 28.5 ns | **0.98x** | 277.5 vs 338.4 ns | **0.82x** |
| 3 | 31.4 vs 26.8 ns | 1.17x | 357.6 vs 302.0 ns | 1.18x |
| 4 | 27.7 vs 27.1 ns | 1.02x | 306.5 vs 300.6 ns | 1.02x |
| 5 | 28.0 vs 28.7 ns | **0.98x** | 278.1 vs 267.9 ns | 1.04x |

Each library wins some runs and loses others, with a spread of roughly ±10% on emit and ±18% on
teardown. Identical machine-code sequences at different addresses routinely differ by that much from
instruction-cache and branch-predictor aliasing, loop alignment, and page placement. **A gap that
changes sign between runs is not a gap.**

**The `connect()` row was the interesting one**, because it did *not* change sign. QtLikeSignal
measured 1.58x to 2.72x slower than QtMimic across all five runs, which is exactly what a real
regression looks like:

```
connect()   QtLikeSignal 263.6 ns   QtMimic 124.8 ns   2.11x
connect()   QtLikeSignal 217.0 ns   QtMimic 120.3 ns   1.80x
connect()   QtLikeSignal 362.8 ns   QtMimic 133.6 ns   2.72x
connect()   QtLikeSignal 218.1 ns   QtMimic 137.8 ns   1.58x
connect()   QtLikeSignal 219.2 ns   QtMimic 125.8 ns   1.74x
```

It was not a regression. QtLikeSignal's `connect()` test registers before QtMimic's, so it ran
first, and it paid the heap-growth cost for both — that is P13, discovered by refusing to accept the
gap. With `settleHeap()` in place the row behaves like every other, at 0.92x, 1.01x and 0.92x over
three runs: still bouncing either side of parity, as two identical implementations should.

**The one place a real difference would be legitimate** is the queued cross-thread row, because the
benchmarks are genuinely not identical there: QtLikeSignal's receiver is created and then
`moveToThread()`d, while QtMimic's binds affinity at construction. That difference is in the setup,
not the timed region, so it does not reach the measurement — and the row bears that out at 0.98x.

**A caveat on the residual.** Across the fifteen post-fix QtLikeSignal-vs-QtMimic cells, twelve sit
just below 1.0 — a systematic tilt of a few percent that is smaller than the run-to-run spread but
larger than nothing. That is consistent with code layout, and it is not worth chasing; it is noted
so a future reader does not mistake it for a discovery.

---

# How these were measured

Built and run as:

```sh
./waf configure                                    # no --enable-*-sanitizer-on-Linux
./waf install --project=Tests --mode=release
./install/Tests/linux64-clang/release/usr/bin/QtLikeSignal-Performance-Tests
```

A `-O0` build, or one with a sanitizer, inflates everything by roughly an order of magnitude and
does not inflate the libraries equally. No number here came from a debug build.

**The benchmarks.** `src/tests/test_QtLikeSignal_Performance.cpp` (QtLikeSignal and QtMimic),
`test_Qt6_Performance.cpp`, and `test_Boost_Performance.cpp`, sharing scenarios, iteration counts
and timing code through `PerfHarness.hpp`. All four libraries are measured in one process, on one
machine, interleaved in time, with the same slot bodies, so the only difference between rows is the
dispatch machinery.

**Iteration counts.** 20 000 connects, 1 000 000 direct emits, 200 000 queued emits, and 20 000
disconnects — the last deliberately equal to the connect count, so the two rows describe the two
halves of the same connection's life and can be read against each other.

**Process warm-up.** `settleAllocatorState()` spawns and joins a thread, because glibc drops its
single-threaded malloc fast path permanently once a second thread has existed. `settleHeap()` grows
and faults in the arena, per P13. Both run in `main()` before any measurement, because the state
they settle is process-wide and one-way. **Numbers taken before 2026-08-17 have the second of these
missing**, so any `connect()` figure from an earlier snapshot understates whichever library ran
first — including, in `PERFORMANCE-20260813.md`, our own advantage over boost on that row.

**Disconnect timing** covers only the disconnects. Connecting is setup. Each disconnect test emits
once afterwards and asserts nothing is received, which proves the handles really ended their
connections and the row is not timing a no-op. The check is written as an emit rather than as a slot
count so that the identical assertion can be made for all four libraries.

**boost.** 1.74.0 from Ubuntu 22.04's `libboost-dev`, header-only, nothing linked. The benchmark
compiles only where the headers are found; `src/tests/wscript` probes for them and skips with a
message otherwise, the same shape as the existing Qt 6 probe. Both probes are native-Linux only, so
a Windows build reports the two columns it can. Note that 1.74 is from 2020 and older than the boost
submodule removed in `bee83fe`; signals2 has been effectively frozen for years, but the version is
recorded here so a future re-measure can be compared honestly.

**The P11 decomposition** used a scratch benchmark that destroyed N objects with and without a
connection, and with the receivers sharing one signal or holding one each. It is not committed: it
measures internals rather than the public shape, and it exists in this document as numbers rather
than as a test somebody has to keep passing. `bestOf(5)` throughout.

**The P11 fixes were verified under ThreadSanitizer**, not only in release — both touch threading
(a new atomic flag, and a diagnostic moved inside the affinity mutex), so a release-only pass would
not have been evidence. 180/180 clean, no race reports.

**What this run did not do.** No threshold in
`test_QtLikeSignal_Regression.cpp` or the Qt 6 timing guards was changed — all of them still pass
with the corrected numbers, the loosest of them (`ConnectKeepsUpWithQt6`, barred at 6x) now sitting
at about 1.3x. All 28 tests in the performance binary pass, as do `QtLikeSignal-Tests` (180/180) and
`QtMimic-test` (180/180) on linux64-clang, and `QtLikeSignal-Tests` on win64-msvc (184 passed, 1
skipped) — the same counts as `bee83fe`.
