# Performance findings — snapshot 2026-08-17

boost::signals2 is back in the comparison table, as a fourth column measured in the same process as
the other three. Items are `P<n>`, continuing the numbering from `PERFORMANCE-20260813.md`.

It adds a **measurement**, corrects a figure that `PERFORMANCE-20260813.md` states more favourably
than the evidence supports, fixes two defects in the benchmark harness — one of which had been
quietly distorting the `connect()` row for as long as that row has existed — and then acts on what
the corrected measurement showed, taking three costs out of `Object` that bought nothing.

Written 2026-08-17 and extended 2026-08-18 with the P11 work.

How every figure below was produced is at the end, under
[How these were measured](#how-these-were-measured). Read it before quoting a number.

## Status

| ID | Status | Finding | Impact | Latest measurement |
|----|--------|---------|--------|--------------------|
| P11 | **In progress** | Teardown against *raw* boost::signals2 is ~3x, not the 1.35x recorded on 2026-08-13 | Medium | Teardown 258.6 → **246.4 ns**; bare `~Object()` 110.6 → **86.8 ns**; Qt 6 gap 2.02x → **1.54x**. Object construction 84 → **62 ns**, one allocation fewer |
| P12 | **Fixed** | The summary table's ratio baseline was whichever library linked first, so adding a fourth benchmark file silently inverted every ratio | High (in the tool) | Baseline is now named, not inferred |
| P13 | **Fixed** | The first allocation-heavy test in a process paid for heap growth, and the `connect()` row charged it to whichever library ran first | High (in the tool) | boost's connect **358.3 → 190.7 ns**; our advantage over boost on that row was overstated by 1.7x |

Two things are worth knowing without reading further:

- **The direct-emit and queued results from 2026-08-13 hold up.** We are slightly ahead of Qt 6 on
  direct emit (0.90x), level on queued cross-thread (1.09x), and 2.7x faster than boost on emit —
  against the 2.7x recorded then, from an independent run four days later on a rebuilt harness. That
  part of P6 was solid.
- **Two claims did not hold up.** Teardown was measured against QtMimic-when-it-used-boost rather
  than against boost (P11), and the `connect()` row was measuring run position as much as it was
  measuring libraries (P13). Both are corrected below, and the corrections move in opposite
  directions from each other.

---

# Details

## The table

`linux64-clang`, `--mode=release`, no sanitizer. boost 1.74.0, Qt 6.11.1. Each cell is the **minimum
of three consecutive runs**, for the reason `PerfHarness::bestOf` already documents: the minimum
estimates how fast the code can go, and everything above it is the scheduler.

```
scenario                        QtLikeSignal      boost       Qt6    QtMimic   vs boost  vs Qt6  vs QtMimic
-----------------------------------------------------------------------------------------------------------
connect()                          112.0 ns    190.7 ns   83.7 ns   121.6 ns     0.59x   1.34x     0.92x
emit->receive, direct               24.4 ns     65.9 ns   27.1 ns    25.6 ns     0.37x   0.90x     0.95x
destroy N receivers                258.6 ns     86.6 ns  131.7 ns   285.2 ns     2.99x   1.96x     0.91x
emit->receive, auto same-thread     45.2 ns          -    26.5 ns    47.3 ns         -   1.71x     0.96x
emit->receive, queued x-thread     486.6 ns          -   447.1 ns   498.6 ns         -   1.09x     0.98x
```

Ratios are QtLikeSignal against each other library; above 1 means we are slower.

Run-to-run spread over those three runs, so the ratios are read with the right precision:

| row | QtLikeSignal | boost | Qt6 | QtMimic |
|---|---|---|---|---|
| `connect()` | 112.0–137.5 | 190.7–226.5 | 83.7–92.9 | 121.6–139.5 |
| `direct` | 24.4–26.3 | 65.9–76.7 | 27.1–30.3 | 25.6–27.3 |
| `destroy N` | 258.6–276.2 | 86.6–107.5 | 131.7–145.0 | 285.2–298.8 |
| `auto same-thread` | 45.2–47.4 | – | 26.5–28.7 | 47.3–50.8 |
| `queued x-thread` | 486.6–529.5 | – | 447.1–519.6 | 498.6–526.6 |

`connect()` remains the noisiest row by a wide margin — the ratio against Qt 6 ranged 1.21x to 1.64x
across those three runs — which is what `PERFORMANCE-20260813.md` already said about it, and why the
allocation-count guard is the sharper instrument for that path.

The two blank boost cells are not omissions. signals2 is a signal library, not an object and
threading framework: it has no thread affinity and no event loop, so those scenarios have no boost
equivalent. An "auto" row would re-measure the direct row, and a "queued" row would have to borrow
our event loop and would then be measuring our queue. `test_Boost_Performance.cpp` says the same at
greater length.

## P11 — teardown against raw boost is ~3x, not 1.35x *(In progress)*

`PERFORMANCE-20260813.md` (P7) records, after the quadratic teardown was fixed:

> We are 1.35x slower than boost on teardown alone and 2.4x faster on connect.

Measured against boost::signals2 directly, the teardown figure is **2.99x** — 258.6 ns per receiver
against 86.6 ns, at 16 000 resident, and 2.57x–3.01x across the three runs. Qt 6 sits between the
two at 131.7 ns, so it is 1.96x ahead of us on the same row.

**The two numbers are not in conflict; they are different comparisons.** The 2026-08-13 column
labelled `QtMimic (boost)` was QtMimic's own receiver bookkeeping with a boost signal underneath, so
it carried overhead that raw signals2 does not. That made it the right column for the question being
asked at the time — *did replacing boost inside our own design make teardown worse?* — and the wrong
column for the question it is now quoted as answering, which is what the underlying primitive costs.
~3x is the honest number for the second question.

Nothing about P7's fix is invalidated: teardown is still flat rather than quadratic and still 169x
better than before. The composite of connect + emit + destroy still favours us over boost, because
we win emit by 2.7x — though by less than the record implies, since the connect advantage is 1.7x
rather than the 2.4x stated, for the reason P13 gives.

What changes is that **teardown is a wider gap than the record said**, and it is the only row where
both reference libraries beat us.

### Where the cost is

Decomposed by destroying N objects that hold no connection at all, against N that hold one:

| | before | after |
|---|---|---|
| `~Object()`, no connections | 110.6 ns | **86.8 ns** |
| `~Object()`, one connection, shared signal | 278.6 ns | **252.3 ns** |
| `~Object()`, one connection, own signal | – | 237.5 ns |

Two things follow. **The bare destructor was already more expensive than boost's entire teardown**
(110.6 against 86.6), before a single connection is involved — so this was never mainly a
disconnect-path problem. And the shared-signal bookkeeping (tombstones, compaction, one mutex for
16 000 receivers) accounts for only ~32 ns of the ~166 ns a connection adds; the rest is per-object
work that no amount of signal-side tuning would reach.

### What was fixed, and what it cost in features

**Nothing.** Both changes remove work that bought nothing:

- **`mUsedTimers`**, a third set-once flag beside `mUsedCallLater` and `mMayHaveQueuedWork`. The
  destructor took `mRunningTimerIdsMutex` unconditionally to swap a vector that is empty for every
  object that never started a timer. Same guard, same precedent, same honest trade — an object that
  has owned a timer keeps taking the lock.
- **`Affinity::namesOtherRunningThread()`**. The cross-thread-destruction diagnostic called
  `Affinity::data()`, which copies a `shared_ptr` out from under the mutex. Two atomic
  read-modify-writes to keep alive, for the length of two atomic loads, something the mutex already
  held. The question is now asked in one call, answered inside the mutex. The diagnostic is
  unchanged, and the `Thread*` is still only ever compared, never dereferenced.

Together: bare `~Object()` **110.6 → 86.8 ns (−22%)**, the shipped teardown row **258.6 → 246.4 ns**,
and the Qt 6 gap on that row **2.02x → 1.54x**. QtMimic, untouched, still measures ~305 ns, which is
a clean control: the same benchmark, the same process, the same source minus these two changes.

### A change that was tried and reverted

Replacing the destructor's `std::vector` of incoming connections with an eight-slot stack buffer.
It did exactly what it was meant to — allocations per destroy went from **1.004 to 0.004** — and the
teardown row got **30 ns slower**, reproducibly, across every inline capacity tried:

| inline capacity | `~Object()`, one connection |
|---|---|
| vector (original) | 278.6 ns |
| 1 | 282.0 ns |
| 2 | 292.7 ns |
| 4 | 295.6 ns |
| 8 | 289.9 ns |

The allocation it removed was a hot tcache hit costing less than the zeroing and destruction of the
buffer that replaced it. **Reverted.** Recorded here because "remove the allocation" is the obvious
next idea and it is wrong: on this path the allocation count and the time do not move together, and
the allocation guards next door would have called this a win.

### The life token merged into the affinity box

Every `Object` carried **two** separately allocated boxes — `mLife`, a `shared_ptr<int>` life token,
and `mAffinity` — where Qt allocates one `QObjectPrivate`. The two had identical lifetime
requirements: both had to outlive the Object, and both were captured by the same closures. Keeping
them apart cost an allocation, a free, and a second sixteen-byte capture in every connection, all to
carry one bit.

Done 2026-08-18. The flag now lives in the affinity box as `Affinity::isObjectAlive()` /
`markObjectDead()`, a plain atomic load where the old check was `weak_ptr::expired()`.

| | before | after |
|---|---|---|
| allocations per `Object` construction | 3.003 | **2.002** |
| `Object` construction | 81.2–88.4 ns | **58.7–66.6 ns** |
| teardown per receiver | ~250 ns | ~250 ns, unchanged |

**The saving is at construction, not teardown**, which is worth stating plainly because teardown is
the row that motivated the work. The allocation happens when the Object is built; the matching free
is a tcache push cheap enough to vanish under everything else the destructor does. This is the third
time in this document that removing an allocation did not move the time it was expected to
(see P13, and the reverted stack buffer above) — on these paths the allocation count and the clock
are close to independent, and each has to be measured on its own.

Measured against QtMimic as an in-process control, before it was ported, interleaved run by run so
neither library got the warmer half: 22 ns apart, same direction every run, well outside the ±10%
these two normally differ by.

**No feature was given up.** `Object::objectLife()` is public and had a test, so it keeps working: it
returns an `ObjectLife` token instead of a `std::weak_ptr<int>`. The one operation callers ever used,
`expired()`, is unchanged, and the token still outlives the Object. Only the type's name changed,
which is an improvement in itself — the old signature leaked `shared_ptr<int>`, an implementation
detail, into the public interface.

**Verified harder than anything else in this document**, because it rewrites the mechanism every
queued connection depends on for its use-after-free safety: ThreadSanitizer and AddressSanitizer,
both libraries, all from clean builds.

### A build trap this change sets

Removing `mLife` changes `sizeof(Object)` and the offset of every member after it. An **incremental**
build that leaves one translation unit compiled against the old layout produces a binary that
crashes with an access violation — order-dependent, and disappearing under a sanitizer, which is the
most misleading combination a bug can have. That happened here on win64-msvc, reproducibly across
three runs, and vanished entirely on `waf clean` followed by a full rebuild.

Not a defect in the change, but recorded because the symptom is alarming and the cause is not
obvious: anyone pulling this wants a clean build first, and anyone bisecting across it should not
trust an incremental one.

### Left open

The remaining gap to Qt 6 on the teardown row is ~1.5x, and nothing identified accounts for it in one
piece. What is left is spread across a mutex per disconnect, a `weak_ptr` upgrade to reach the
signal, and the frees of the connection node and the slot — each of which buys something, and none
of which is obviously the next thing to attack.

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

| run | direct emit | ratio | destroy N receivers | ratio |
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

**Iteration counts.** 20 000 connects, 1 000 000 direct emits, 200 000 queued emits, 16 000 resident
receivers for teardown. The teardown count matches the size P7 was measured at, so the row is
comparable with `PERFORMANCE-20260813.md`.

**Process warm-up.** `settleAllocatorState()` spawns and joins a thread, because glibc drops its
single-threaded malloc fast path permanently once a second thread has existed. `settleHeap()` grows
and faults in the arena, per P13. Both run in `main()` before any measurement, because the state
they settle is process-wide and one-way. **Numbers taken before 2026-08-17 have the second of these
missing**, so any `connect()` figure from an earlier snapshot understates whichever library ran
first — including, in `PERFORMANCE-20260813.md`, our own advantage over boost on that row.

**Teardown timing** covers only the destruction. Connecting is setup. Each teardown test emits once
afterwards and asserts nothing is received, which proves the destructors really disconnected and the
row is not timing the destruction of N objects that were attached to nothing.

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
