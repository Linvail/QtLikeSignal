# Plan: unify the QtLikeSignal and QtMimic test content — 2026-08-10

Maintaining two independently-written suites for two libraries with the same core API costs twice
and catches less: a case one side thought of is simply absent on the other, and nothing tells you
so. This is the plan to make the *content* the same while the *files* stay separate.

**Target shape.** Each suite keeps its own files, named in parallel, and the bodies match:

```
src/tests/QtLikeSignal-test-timer.cpp        external/QtMimic/tests/QtMimic-test-timer.cpp
```

Not one shared file compiled twice. Separate files that a `diff` shows to be near-identical.

## Where things stand

Measured 2026-08-10, after the naming unification, the timer-test port and the SignalView port.

| | QtLikeSignal | QtMimic |
|---|---|---|
| Tests | 118 | 74 |
| Present only on this side | 84 | 40 |
| Library line coverage | 88.3% | 82.0% |

Test names present in **both** suites: **34**.

34 shared names is up from 12 this morning. All 34 came from two deliberate ports — the
`ThreadPriority` suite and the timer suite — which is the evidence that this approach works: nobody
had to invent a framework, they were transformed and they compiled.

### The timer files are the proof of concept

`test_gtimer.cpp` was produced by transforming `QtMimic-test-timer.cpp`. Normalising only the
library name, the two ~1285-line files differ by **111 lines, 8.6%**. Every one of those
differences falls into six buckets:

| Difference | Lines | Cause |
|---|---|---|
| File header prose | 7 | describes each library's own plumbing |
| Include block | 5 | `.h` vs `.hpp`, `Event.h` vs `TimerEvent.hpp` |
| Helper-class constructors | ~24 | `Object( Thread* )` vs `moveToThread()` |
| `Thread worker( "name" )` | ~20 | QtLikeSignal's Thread has no name |
| `new Timer( &worker )` | ~5 | same constructor difference |
| `Object context( &worker )` | ~2 | same constructor difference |

Four of the six are **one API difference** — `Object`'s and `Thread`'s constructors — showing up in
four syntactic places. That is the shape of the whole problem: a handful of API deltas, each
sprayed across hundreds of lines.

## The design: push every delta into a per-library support header

Each suite gets one support header. Everything that differs between the libraries lives there and
nowhere else, so the `.cpp` bodies become identical text.

```
src/tests/QtLikeSignal-test-support.h    external/QtMimic/tests/QtMimic-test-support.hpp
```

Each provides the same vocabulary:

| Provided | QtLikeSignal | QtMimic |
|---|---|---|
| `using namespace ...` | `QtLikeSignal` | `QtMimic` |
| `makeThread( name )` | ignores the name | passes it to the constructor |
| `TestObject` / `TestReceiver` base | ctor takes `Thread*`, calls `moveToThread()` | ctor forwards to `Object( Thread* )` |
| `waitUntilRunning( thread )` | identical body | identical body |
| `runOnThread( thread, body )` | identical body | identical body |
| `drainQueuedTasks( thread )` | identical body | identical body |
| `waitFor( predicate )`, `kPatience` | identical | identical |
| Feature macros | see below | see below |

The test bodies then say `TestReceiver receiver( &worker );` and `auto worker = makeThread( "x" );`
on both sides, with nothing to reconcile. Applied to the timer files, this removes roughly 90 of
the 111 differing lines; the rest is the header prose and the include block, which should differ.

Deriving the test helpers from a `TestObject` base rather than from `Object` directly is the single
highest-leverage item here — it absorbs the constructor difference that accounts for four of the
six buckets.

### One-sided APIs

Three categories, handled three different ways. Choosing the right one per case is what keeps this
from turning into a mess of `#if`.

**A. Same concept, different spelling → rename one side.** Cheap, permanent, no conditional code.
Already done: `join()`→`wait()`, `current()`→`currentThread()`, `AutoConnection`→`Auto`,
`timeout`→`getTimeout()`. Still outstanding:

| QtLikeSignal | QtMimic | Suggested |
|---|---|---|
| `Object()` + `moveToThread()` | `Object( Thread* = nullptr )` | give QtLikeSignal the ctor |
| `Thread()` | `Thread( const std::string& = {} )` | give QtLikeSignal the name |
| `wait( unsigned long )` → `bool` | `wait()` → `void` | give QtMimic the timeout+bool |
| `Signal::disconnect/receivers` | `disconnectAll/receivers/empty` | align on QtLikeSignal's |
| `SignalView::connect` | `SignalView::connectReflective` | keep both, they differ in kind |

**B. Feature genuinely absent on one side → feature macro, test stays in both files.** The test
body is written once and compiled on the side that has the feature. Absence is then *visible* in
both files rather than silently missing from one:

```cpp
#if LIB_HAS_CALL_LATER
TEST( ObjectTest, CallLaterCoalescesDuplicates ) { ... }
#endif
```

Macros needed: `LIB_HAS_CALL_LATER`, `LIB_HAS_EVENT_DISPATCHER`, `LIB_HAS_OBJECT_NAME`,
`LIB_HAS_CLEANUP_CALLBACKS`, `LIB_HAS_THREAD_CREATE`, `LIB_HAS_ADOPTION`,
`LIB_HAS_INCOMING_CONNECTION_COUNT`, `LIB_HAS_EXTERNAL_DISPATCHER` (QtMimic's
`setDispatcher`/`setWaiter`).

**C. Different architecture → accept divergence, in a marked section.** Some tests cannot exist on
the other side because the thing they test does not. These go at the *end* of the file under a
banner comment saying so, so a diff shows one clean block rather than scattered noise:

- QtLikeSignal: `EventDispatcherLinuxTest` (4), `EventDispatcherDefaultDefectTest` (7) — QtMimic has
  no dispatcher object at all.
- QtMimic: nothing currently in this category.

## File map

| Concern | QtLikeSignal | QtMimic | State |
|---|---|---|---|
| main | `main.cpp` → `QtLikeSignal-test-main.cpp` | `QtMimic-test-main.cpp` | rename |
| support header | *new* | *new* | write both |
| object | `test_gobject.cpp` → `-test-object.cpp` | split out of `QtMimic-tests.cpp` (20) | rename + split |
| thread | `test_gthread.cpp` → `-test-thread.cpp` | split out of `QtMimic-tests.cpp` (1) | rename + split |
| coreapplication | `test_gcoreapplication.cpp` → `-test-coreapplication.cpp` | split out of `QtMimic-tests.cpp` (1) | rename + split |
| thread priority | `test_gthread_priority.cpp` → `-test-thread-priority.cpp` | `QtMimic-test-thread-priority.cpp` | ~~content already aligned~~ **names only — see correction** |
| timer | `test_gtimer.cpp` → `-test-timer.cpp` | `QtMimic-test-timer.cpp` | **content aligned** |
| defect regressions | `test_defect_regressions.cpp` → `-test-defect-regressions.cpp` | `QtMimic-test-defect-regressions.cpp` | rename, content diverges |
| argument copying | `test_gobject_argument_copying.cpp` → `-test-argument-copying.cpp` | 2 tests in `QtMimic-tests.cpp` | rename + split |
| stress | *missing* | `QtMimic-test-stress.cpp` (9 tests, 743 lines) | **port to QtLikeSignal** |
| adoption | `test_gthread_adoption.cpp` → `-test-thread-adoption.cpp` | *missing* | port to QtMimic |
| linux dispatcher | `test_geventdispatcher_linux.cpp` | — | category C, stays one-sided |
| known defects | `test_known_defects.cpp` | — | category C, stays one-sided |

Two real gaps fall out of this table, and both are worth more than the tidying:

- **QtLikeSignal has no stress suite.** QtMimic's 743-line `QtMimic-test-stress.cpp` — concurrent
  connect/disconnect/emit, massive fan-in, a slot that disconnects itself mid-emission,
  disconnect-all during emission — has no QtLikeSignal counterpart at all.
- **QtMimic has no thread-adoption suite**, where QtLikeSignal has six tests.

## Work order

Each phase builds, passes both suites, and commits on its own. Ordered by payoff per unit of risk.

| # | Phase | Why here | Rough size |
|---|---|---|---|
| 1 | Write both support headers; retrofit the two already-aligned suites (timer, thread-priority) onto them | Proves the shim design on files already known to be close; drops the timer diff from 111 lines to ~15 | half a day |
| 2 | Port `QtMimic-test-stress.cpp` to QtLikeSignal | Biggest genuine coverage gain in the whole plan; no renaming needed to do it | half a day |
| 3 | Category-A renames from the table above | Every later phase gets cheaper; touches library headers, so do it while the test churn is still small | half a day |
| 4 | Split `QtMimic-tests.cpp` into object / thread / coreapplication | Prerequisite for aligning those three; mechanical | 2 hours |
| 5 | Rename all files to the parallel scheme | Pure rename, no content change, so it stays reviewable | 1 hour |
| 6 | Align object / thread / coreapplication content, introducing feature macros as needed | The long tail: 84 + 40 one-sided tests to reconcile one at a time | 2–3 days |
| 7 | Port the adoption suite to QtMimic | Small, and closes the last structural gap | 2 hours |
| 8 | Add the drift check below to the build | Only worth it once the files are actually close | 1 hour |

Phases 1 and 2 are worth doing even if the rest is never finished. Phase 6 is the bulk and can be
done incrementally, one suite at a time, indefinitely.

## Drift control

Once the files are close, keep them close mechanically. A script that normalises the known-legal
differences and diffs the rest:

```sh
tools/compare-test-suites.sh          # prints the per-file divergence, non-zero exit on regression
```

It should normalise `QtLikeSignal`↔`QtMimic`, the include block, and the file header, then report
the differing-line count per file pair against a checked-in budget:

```
QtLikeSignal-test-timer.cpp  <->  QtMimic-test-timer.cpp     15 lines   (budget 20)  OK
QtLikeSignal-test-object.cpp <->  QtMimic-test-object.cpp    240 lines  (budget 60)  FAIL
```

A budget rather than zero: category-B and category-C blocks legitimately differ, and the number
should be allowed to shrink over time without the check becoming a nuisance. Ratchet the budget
down as phases land; never up without a comment saying why.

## Non-goals

- **One shared file compiled twice.** Rejected: it forces a lowest-common-denominator API and makes
  every library-specific test an `#if`. The libraries are allowed to differ; the tests should make
  the differences visible, not hide them.
- **Identical test *names* everywhere.** Where a library has no counterpart the name should simply
  be absent, not stubbed.
- **Converging the two libraries' architectures.** QtLikeSignal has an event-dispatcher hierarchy
  and an `Event` type set; QtMimic's mailbox carries callables. That difference is the point of
  having both, and the tests should reflect it rather than paper over it.


---

## Progress log

### 2026-08-10 — phases 2, 3, 4, 5 and 8 landed

Worked in a different order than planned. Phase 3 (the category-A renames) moved to the front,
because two of them *removed* most of what phase 1's shim layer was going to paper over — giving
QtLikeSignal `Object( Thread* )`, `Timer( Thread* )` and `Thread( name )` is strictly cheaper than
writing shims that translate between the two spellings forever. Phase 1's support headers are
consequently much smaller than budgeted, and only the shared-fixture half
(`QtLikeSignal-test-types.h`) has been needed so far.

| Phase | State |
|---|---|
| 3 — category-A renames | done (constructors, `ConnectionHandle`→`Connection`, `Signal::disconnectAll/empty`) |
| 2 — port the stress suite | done, 9 tests |
| 4 — split `QtMimic-tests.cpp` | done, into object / thread / coreapplication |
| 5 — rename to the parallel scheme | done, both sides |
| 8 — drift check | done, `tools/compare-test-suites.sh` |
| 1 — support headers | partial: shared fixtures done, helper vocabulary not yet extracted |
| 1b — thread-priority alignment | **not started, and larger than planned — see correction** |
| 6 — object/thread/coreapplication content | not started |
| 7 — adoption suite to QtMimic | not started |

**Correction to this document.** The file map above claimed the thread-priority suites were
"content already aligned". That was wrong, and it was inferred from the wrong evidence: the two
share 11 of 12 test *names*, so a name-overlap count made them look ported. The bodies were written
independently and differ by 814 lines. Only the timer pair is genuinely aligned. Aligning
thread-priority is real work, comparable to the timer port, and is now tracked as its own phase.

**What porting found.** Twice now, porting a test suite has surfaced a genuine difference rather
than just moving text:

- QtLikeSignal's `Timer` was missing four Qt-conformance behaviours QtMimic had: negative-interval
  clamping, restart-on-`setInterval`, a guaranteed-fresh timer id across a restart, and
  cancellation of a `singleShot` whose context is destroyed.
- QtLikeSignal's `Signal::emit()` took its arguments **by value**, costing one copy of every
  argument per emit before boost saw them. Caught by the ported copy-counting stress test.

That is the argument for the whole plan in miniature: each of those was present on one side and
absent on the other, and nothing said so.

### Current drift

```
timer                            13         25   ok
stress                            8         20   ok
object                         1264       1300   ok
thread                          408        450   ok
coreapplication                 407        450   ok
thread-priority                 814        850   ok
defect-regressions             1874       1900   ok
```

Budgets are ratchets set just above today's numbers, so the check is green now and goes red on
regression. Lower them as phases 1b, 6 and 7 land. `timer` and `stress` show the far end: all that
remains there is the file header and the include block.


### 2026-08-11 — all eight phases complete

| Pair | Differing lines | Was |
|---|---|---|
| thread | 0 | 408 |
| coreapplication | 0 | 407 |
| thread-adoption | 0 | one-sided |
| object | 2 | 1264 |
| thread-priority | 8 | 814 |
| stress | 8 | one-sided |
| timer | 9 | 111 |

153 QtLikeSignal tests and 111 QtMimic, from 118 and 74. `defect-regressions` is
deliberately outside the comparison (category C).

**What porting found.** Eight defects and gaps, every one of them present on one side and absent on
the other with nothing to say so — which is the argument this document opened with:

| Library | Found |
|---|---|
| QtLikeSignal | `Timer` missing four Qt behaviours: negative-interval clamping, restart on `setInterval()`, a fresh id across a restart, cancelling a `singleShot` whose context died |
| QtLikeSignal | `Signal::emit()` took arguments by value — one copy of every argument per emit before boost saw them |
| QtLikeSignal | `applyPriority()` was a no-op on POSIX, so `setPriority()` on a running thread changed nothing the scheduler could see |
| QtMimic | `started`/`finished` emitted inside `loop()`, so a subclass overriding `run()` emitted neither |
| QtMimic | `tCurrentThread` registered inside `loop()`, so such a subclass could not resolve its own affinity |
| QtMimic | `mExitCode` raced between `exit()` and `exec()` |
| QtMimic | `exec()` did not reset the mailbox, so a second `exec()` returned instantly with no loop |
| QtMimic | `loop()` un-adopts the caller on exit, so after `exec()` the main thread is not its own `Thread` |

**Open decisions**, all recorded as feature macros so they are greppable rather than forgotten:

- ~~`LIB_HAS_EXEC_GUARDS`~~ — **resolved 2026-08-11.** QtMimic's `processEvents()` and
  `CoreApplication::exec()` now refuse the wrong thread, and `exec()` refuses re-entry.
- ~~`LIB_HAS_ADOPTION_SURVIVES_EXEC`~~ — **resolved 2026-08-11.** `loop()` no longer clears the
  per-thread registration; `~Thread()` does, if it still points there. Registration belongs to the
  thread, not the loop. Fixing it also forced a second fix: `loop()` clears `mAccepting` as it
  stops, which is right for a worker whose OS thread is ending and wrong for a thread that merely
  finished an `exec()` cycle, so `exec()` now restores it. The un-adopt had been *masking* that
  one, by handing out a fresh dummy with a fresh mailbox on every call.
- ~~`LIB_HAS_NULL_CONTEXT_REJECTED`~~ — **resolved 2026-08-11.** QtMimic now refuses a null
  context and returns a dead handle, matching QtLikeSignal and Qt. The macro is gone and the test
  asserts it unconditionally on both sides.
- ~~`LIB_HAS_SHUTDOWN_DEFERRED_DELETE`~~ — **resolved 2026-08-11.** QtMimic keeps deferred deletes
  in their own list, separable from ordinary queued work, and drains them from the loop, from
  processEvents(), from ~Thread() and from ~CoreApplication().
- `LIB_HAS_CALL_LATER`, `LIB_HAS_OBJECT_NAME`,
  `LIB_HAS_CLEANUP_CALLBACKS`, `LIB_HAS_THREAD_CREATE`, `LIB_HAS_EVENT_DISPATCHER`,
  `LIB_HAS_OBJECT_LIFE`, `LIB_HAS_STATIC_DISCONNECT`, `LIB_HAS_WAIT_TIMEOUT`,
  `LIB_HAS_THREAD_IS_RUNNING`, `LIB_HAS_THREAD_IS_ADOPTED`,
  `LIB_HAS_POST_REJECTED_BEFORE_START` — features QtMimic does not have and may never want.

**Left for a later pass.** The object suites were merged as the union, so several pairs now cover
the same behaviour twice under different names (`DirectConnectionSameThread` and
`DirectSignalSlotConnection`, for instance). Pruning them means reading bodies rather than names,
and deleting a test is a decision that should not be made by a script.

**One trap worth remembering.** A feature macro fails open in the wrong direction. When
QtLikeSignal's coreapplication file was missing its types-header include, the macros were
undefined, `#if UNDEFINED` evaluated to 0, and four tests silently vanished from the suite that was
supposed to have them. Only the test count dropping showed it. The drift script does not catch
this; the count does.
