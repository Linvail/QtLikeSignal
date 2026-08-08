# Open risks — snapshot 2026-08-08

Review of `src/` after the `Affinity` port, the cross-thread-destruction diagnostic, and the
`deleteLater()` de-bounce (all in the same working tree this document is committed with).

Numbering continues from `OPEN-RISKS-20260802.md` (R1–R16). Items R1/R6/R9 from that document are
still open and are re-stated here only where this pass added new evidence.

Each item says how it was confirmed. **Probe** means a throwaway test was compiled into the suite,
run, and then removed — the measured numbers are quoted. **Inspection** means the code was read but
no runtime probe was written.

## Status at a glance

| ID  | Risk | Severity | Confirmed by |
|-----|------|----------|--------------|
| R17 | Destroyed receivers are never disconnected — unbounded dead-slot growth | **High** | Probe (672 MB, 327 ms emit) — **Fixed 2026-08-08** |
| R18 | `CoreApplication::exec()` after `quit()` burns 100% CPU (was R1) | **High** | Probe (cpu/wall = 100%) — **Fixed 2026-08-08** |
| R19 | Objects with no affinity get cross-thread *direct* calls; Qt drops them | Medium | Probe |
| R20 | `~Object()`'s new warning dereferences a `Thread*` that may dangle | Low-Med | Inspection — *self-inflicted* — **Fixed 2026-08-08** |
| R21 | `CoreApplication` has no test coverage at all | Medium | Inspection — **Fixed 2026-08-08** (13 tests) |
| R22 | Platform dispatchers are still empty shells (was R6) | Medium | Inspection |
| R23 | `moveToThread(nullptr)` can leave stale `ThreadData` behind | Low | Inspection |
| R24 | Timer ids never recycle and can wrap onto the `-1` sentinel | Low | Inspection |
| R25 | `Object::thread()` now costs a mutex on every call | Low | Inspection |
| R26 | Repeating timers drift; interval is measured from dispatch, not deadline | Low | Inspection |
| R27 | `Thread::create()` returns an owning raw pointer with no ownership doc | Low | Inspection |

---

## R17 — Destroyed receivers are never disconnected from the signals they were connected to *(fixed 2026-08-08)*

**Severity: High. Confirmed by probe. This was the most serious finding in this pass.**

> **Resolution (2026-08-08).** Ported QtMimic's two-sided teardown. `Object` gained
> `mIncoming` (+ its mutex) recording every connection where it is the receiver, and `~Object()`
> swaps that vector out and disconnects each handle — after `mLife.reset()`, so the `Cleanup`
> destructors it triggers bail out instead of re-entering `mIncoming`. Each connection's closure
> also carries a `shared_ptr<Object::Cleanup>` whose destructor prunes that object's own
> `mIncoming` entry when the connection ends any other way (manual `disconnect()`, sender
> destroyed), which is the half that stops `mIncoming` growing on a receiver that outlives its
> connections. The disconnect loop deliberately does **not** hold `mIncomingMutex`, to avoid
> nesting it inside boost's signal mutex — the reverse of the order `~Cleanup` takes them in.
>
> Re-running the same probe: **RSS growth 672036 kB → 32 kB, emit 326844 us → 30 us.**
>
> Covered by `ObjectDefectTest.DestroyedReceiverIsDisconnectedFromItsSender`, which asserts through
> a new read-only `Signal::receivers()` accessor (mirroring `QObject::receivers()` and QtMimic's
> `Signal::receivers()`) rather than by measuring memory. Verified to catch the regression:
> disabling the disconnect loop makes it report 501 accumulated dead slots.

`Object::connect()` ends with `return aSignal.connect( wrapper );` and nothing anywhere removes
that slot again. `~Object()` resets the life token, runs cleanup callbacks, clears the callLater
registry, and strips pending dispatcher events — but it never disconnects connections where this
object was the *receiver*. The wrapper's `weakLife.lock()` check makes a dead slot **inert**, but
inert is not the same as **gone**: the slot stays in the signal's slot list, holding its captured
state, and is still walked on every subsequent emit.

So for the very common Qt pattern of one long-lived signal and many short-lived receivers, both
memory and emit cost grow without bound.

Probe: 200 000 short-lived `Object`s each connected to one long-lived `Signal<int>`, then destroyed.

```
rss_before=25592kB  rss_after=697628kB  growth=672036kB
emit=326844us       live_calls=1
```

**672 MB retained after every receiver was destroyed, and a single `emit()` taking 327 ms to
deliver to the one slot that was still alive.** Roughly 3.4 KB of retained state per dead
connection. Neither figure recovers — the growth is permanent for the lifetime of the signal.

This is a genuine regression relative to `external/QtMimic`, which solves it explicitly:
`Object::mIncoming` records every connection where the object is the receiver, and `~Object()`
walks it calling `handle.disconnect()`; a `Cleanup` token captured by each slot prunes the entry
from the other direction when the connection ends first. Qt does the equivalent in `~QObject()` by
walking `cd->senders` and removing each connection. QtLikeSignal is the only one of the three with
no teardown path at all.

Fixing it means porting QtMimic's `mIncoming` + `Cleanup` pair (the two halves are both needed —
`mIncoming` alone leaks entries for connections disconnected manually before the receiver dies).
Note this interacts with R20 and with the boost re-entrancy that motivated the `Affinity` port:
`disconnect()` from `~Object()` does not wait for an in-flight emit, which is exactly why the
affinity read had to be moved out of the receiver in the first place.

## R18 — `CoreApplication::exec()` after a `quit()` burns 100% CPU (was R1) *(fixed 2026-08-08)*

**Severity: High. Confirmed by probe. Previously reported as R1 "by inspection"; reproduced with
numbers, then fixed.**

> **Resolution (2026-08-08).** `processEvents()` now *consumes* the interrupt
> (`mInterrupt.exchange(false)`) at both the entry check and the post-wait check, instead of only
> testing a flag nothing ever cleared. This is exactly what Qt does, at exactly the same point:
> `const bool wasInterrupted = d->interrupt.fetchAndStoreRelaxed(false);` at the top of
> `QEventDispatcherWin32::processEvents()`.
>
> Investigating the fix turned up a **worse half of the same defect than the CPU burn**: because the
> latched flag made `processEvents()` return before it reached the queue, a second `exec()` was
> silently *inert* — timers never fired and queued slots never ran, while the loop looked healthy.
> Two regression tests cover the two halves:
> `CoreApplicationTest.ReExecAfterQuitBlocksInsteadOfSpinning` (CPU, via portable `std::clock()`)
> and `CoreApplicationTest.LoopStillDispatchesAfterAQuitExecCycle` (dispatch still works).
>
> Both are stopped by a watchdog thread rather than a `Timer`, deliberately: with the defect present
> a quitting `Timer` never fires, so a timer-driven test **hangs instead of failing**. Verified by
> reverting the fix — both then fail with numbers attached (`0.300126s of CPU over 0.300091s of
> wall`) rather than hanging.

`EventDispatcherDefault::mInterrupt` is latched true by `interrupt()` and **never cleared
anywhere** — `grep -n mInterrupt` finds the declaration, three reads, and exactly one write
(`interrupt()`, line 415). `processEvents()` opens with `if( mInterrupt ) return false;`.

`CoreApplication::quit()` calls `mDispatcher->interrupt()`. `CoreApplication::exec()` then resets
only its own `mExiting` flag and loops `while( !mExiting ) mDispatcher->processEvents();`. Because
`mInterrupt` is still latched, every iteration returns instantly and the loop spins as fast as the
CPU allows until some other thread calls `quit()` again.

Probe (re-`exec()` after a `quit()`, stopped by another thread after 300 ms):

```
wall=0.300s  cpu=0.300s  (cpu/wall=100%)
```

Worker `Thread`s dodge this by accident, not by design: `Thread::threadBody()` drops the dispatcher
it created (`setDispatcher(nullptr)`) when the run ends, so a restart builds a fresh one with
`mInterrupt == false`. That protection disappears for any thread whose dispatcher it did *not*
create — `createdDispatcher` stays false and the interrupted dispatcher is reused. The main thread
is precisely that case.

Minimal fix is to clear `mInterrupt` at the start of `processEvents()` (or on entry to `exec()`),
matching Qt, where `QEventLoop::exec()` resets its own interruption state per run.

## R19 — An object with no thread affinity receives cross-thread *direct* calls

**Severity: Medium. Confirmed by probe.**

`dispatchMetaCall()` resolves `AutoConnection` as `currentThread == targetThread ? Direct :
Queued`. When an object has no affinity **and** the emitting thread is not a `QtLikeSignal::Thread`,
both sides are `nullptr`, compare equal, and the slot is invoked **directly on the emitting
thread**.

Probe: an unaffiliated `Object` (`thread() == nullptr`), connected `AutoConnection`, emitted from a
plain `std::thread`:

```
ran=1  ranOnEmitterThread=1   (Qt would DROP this)
```

Qt is explicit that this should not happen — *"If a QObject has no thread affinity (that is, if
`thread()` returns zero) ... then it cannot receive queued signals or posted events"*
(`qobject.cpp`, "Thread Affinity"). QtMimic implements exactly that, dropping the call rather than
falling back to a direct one, and calls the fallback out as deliberate:

```cpp
if( ctxData == nullptr || ctxData->thread() == nullptr ) return;  // deliberately NOT a direct call
```

The structural reason QtLikeSignal ends up here is that it does **not** auto-adopt native threads.
`Thread::currentThread()` returns `nullptr` outside a `Thread`, whereas QtMimic's
`Thread::current()` creates a dummy adopted `Thread` so every object always has affinity.

This is load-bearing in the current test suite, which is why it cannot simply be changed: the test
binary has no `CoreApplication`, so main-thread objects have no affinity and the whole
direct-connection suite depends on `Auto` resolving to `Direct`. Changing the rule without
auto-adoption would break those tests; adding auto-adoption is the larger, Qt-faithful fix. Until
then, the hazard is a silent unsynchronised cross-thread slot invocation with no diagnostic.

## R20 — `~Object()`'s new cross-thread warning dereferences a `Thread*` that may dangle *(fixed 2026-08-08)*

**Severity: Low-Medium. Inspection. Introduced by this pass — flagged against my own change.**

> **Resolution (2026-08-08).** The running flag moved out of `Thread` and into `ThreadData` as
> `mThreadRunning`, with `Thread::isRunning()` reading through to it — one source of truth, not a
> mirror that could drift. The destructor now asks the `ThreadData` it already holds alive
> (`ownerData->isThreadRunning()`) and only ever *compares* the `Thread*`
> (`ownerData->thread() != Thread::currentThread()`), never dereferences it. `grep` over `src/*.cpp`
> confirms no remaining dereference of a `thread()` result anywhere.
>
> Behaviour is unchanged: the suite still emits exactly the same two warnings, 66 tests pass, zero
> TSan reports, deterministic across four runs.

The diagnostic added to `~Object()` reads:

```cpp
if( Thread* owner = thread() )
{
    if( owner != Thread::currentThread() && owner->isRunning() )
```

`thread()` is safe (it resolves through `Affinity` → `ThreadData`, which outlives its `Thread` and
reports `nullptr` once that `Thread` is gone). But the returned raw pointer is then **dereferenced**
for `isRunning()`, and nothing keeps the `Thread` alive across that gap — a concurrent `~Thread()`
in the window between the load and the call makes it a use-after-free.

What makes this worth recording rather than shrugging at: this is the **only** site in `src/` that
dereferences the result of `Object::thread()`. Every other caller — `startTimer()`, `killTimer()`,
`moveToThread()`, both `dispatchMetaCall()` overloads — only ever *compares* it. The `Affinity` port
was specifically about not reading through the receiver at moments like this, and the diagnostic
quietly reintroduces one instance of the pattern it was meant to remove.

Mitigating: it is a diagnostic-only path that changes no behavior; it only runs when the object is
already being destroyed from a foreign thread; and the window is a few instructions. Qt has the
same hazard in principle, since `QObject::thread()` also hands out a raw `QThread*`.

Clean fix: mirror the running flag into `ThreadData` (already held alive by a `shared_ptr` at this
point) and test that instead of dereferencing the `Thread`.

## R21 — `CoreApplication` has no tests *(fixed 2026-08-08)*

**Severity: Medium. Inspection.**

> **Resolution (2026-08-08).** Added `src/tests/test_gcoreapplication.cpp` — 13 tests covering
> construction/adoption and release of the calling thread, `instance()` lifetime, both constructors,
> the derived-class `init()/exec()/deInit()` usage from the mission, exit codes, the two R18
> regressions, `exec()` rejection off the main thread, nested-`exec()` rejection, cross-thread queued
> delivery onto the main loop, `deleteLater()` during and at shutdown, and main-thread timers.
>
> Because constructing a `CoreApplication` adopts the calling thread by setting the process-global
> `Thread::sCurrentThread`, and gtest runs every test on that thread, each test destroys its
> application before returning. Verified non-contaminating: the full suite passes at 79 tests under
> three different `--gtest_shuffle` seeds as well as in declaration order.

The suite has `ObjectTest`, `ObjectDefectTest`, `ThreadTest`, `ThreadDefectTest`, `ThreadPriority`,
`TimerTest`, `TimerDefectTest` and `EventDispatcherDefaultDefectTest` — and no
`CoreApplicationTest`. `grep -rn CoreApplication src/tests/` returns one `#include` and two comments
noting its *absence* is what makes some other test deterministic.

So `CoreApplication`'s constructor (dispatcher creation, main-thread self-adoption, the
`moveToThread(mMainThread.get())` dance), `exec()`, `quit()`, and destructor ordering are all
entirely unexercised. R18 above is a direct consequence — a 100% CPU spin in `exec()` survived
because nothing ever calls it. R9 (unguarded singleton) is still open for the same reason.

This is the largest coverage gap in the project, and it is where the two High findings live.

## R22 — Platform dispatchers are still empty shells (was R6)

**Severity: Medium. Inspection. Unchanged since 2026-08-02; restated because mission stage 5
depends on it.**

`EventDispatcherLinux` and `EventDispatcherWin32` are both a constructor and a destructor, both
`= default`, deriving from `EventDispatcherDefault` and overriding nothing. No `epoll`, no
`GetMessage`, no OS event source. `mission.txt` stage 5 ("Support QAbstractEventDispatcher, and
concrete dispatcher for Windows and Linux. I want to receive OS/platform's messages") is therefore
not started, despite the files existing and being wired into `Thread::threadBody()`.

## R23 — `moveToThread(nullptr)` can leave stale `ThreadData` behind

**Severity: Low. Inspection.**

`moveToThread()` short-circuits on `currentAffinity == aThread`. Once an object's `Thread` has been
destroyed, `thread()` reports `nullptr`, so `moveToThread(nullptr)` compares equal and returns
`true` **without clearing the `Affinity` box** — which still holds a `shared_ptr` to the dead
thread's `ThreadData`.

The object is then in a mildly inconsistent state: `thread() == nullptr` but `threadData() !=
nullptr`. Consequences are benign today (the dead thread's dispatcher has already been released, so
`deleteLater()` falls back to a synchronous delete and `startTimer()` reports "no event
dispatcher"), and the stale `ThreadData` is kept alive by this reference. Worth tightening if
`moveToThread()` grows any behavior that depends on `threadData()` being in step with `thread()`.

## R24 — Timer ids never recycle and can wrap onto the `-1` sentinel

**Severity: Low. Inspection.**

`Object::sNextTimerId` is a monotonically increasing `std::atomic<int>` consumed by
`fetch_add(1)`; ids are never returned to a pool, unlike Qt, which explicitly releases them
(`QAbstractEventDispatcherPrivate::releaseTimerId`). After 2^31 `startTimer()` calls the counter
wraps (well-defined for atomics, two's complement), and will eventually hand out `-1` — the exact
value `startTimer()` returns to signal failure and that `Timer::stop()` tests against. It can also
collide with a still-live timer's id.

Not reachable in any realistic run; recorded so the sentinel collision is a known property rather
than a surprise.

## R25 — `Object::thread()` now costs a mutex acquisition on every call

**Severity: Low (performance). Inspection. Introduced by this pass.**

Before the `Affinity` port, `thread()` was a single relaxed atomic load of `mThread`. It is now
`mAffinity->data()` — which locks `Affinity::mMutex`, copies a `shared_ptr` (atomic refcount
increment), unlocks, then does an acquire load and drops the refcount again.

That is on the hot path: `dispatchMetaCall()` calls it on every queued emit, and `startTimer()` /
`killTimer()` / `moveToThread()` / `~Object()` all use it. The correctness win is worth it, but if
emit throughput ever matters, the obvious cheaper design is Qt's: hold the affinity in a single
`atomic<ThreadData*>` with the data refcounted separately, so the common read is one atomic load.
No measurement was taken — flagged as a known cost, not a demonstrated problem.

## R26 — Repeating timers drift

**Severity: Low. Inspection.**

`EventDispatcherDefault::processEvents()` re-arms an expired timer with
`t.mNextFire = now + interval`, where `now` is when the dispatcher got around to collecting it —
not the deadline that just passed. Every late wakeup is therefore folded permanently into the
schedule, so a 10 ms timer in a busy loop runs slower than 100 Hz and never catches up. Qt's
default `Qt::CoarseTimer` tolerates drift too, but computes from the deadline rather than from
dispatch time, so the error does not accumulate.

## R27 — `Thread::create()` returns an owning raw pointer with no ownership documentation

**Severity: Low. Inspection.**

`Thread::create()` heap-allocates a `FuncThread`, starts it, and returns `Thread*`. The caller owns
it and must `delete` it (the tests do, after `wait()`), but neither the declaration nor the doxygen
comment says so — the comment is just *"Creates and starts a Thread executing the specified
function."* Easy to leak. Returning `std::unique_ptr<Thread>` would make the contract
self-enforcing.

---

## Suggested order

1. ~~**R17**~~ — done 2026-08-08.
2. ~~**R18**~~ — done 2026-08-08.
3. ~~**R21**~~ — done 2026-08-08. R9 (unguarded singleton) is now warned about, not asserted.
4. ~~**R20**~~ — done 2026-08-08.
5. **R19** — needs the auto-adoption design decision first; do not change the rule piecemeal.

## Not re-verified this pass

R2–R5, R7, R8, R10, R12, R13 (all fixed 2026-08-02) were not re-tested beyond the suite passing.
R14 (GCC `libtsan` false positives) and R16 (phantom lock-order-inversion) were not revisited; the
current run is clang-based and reports zero warnings.
