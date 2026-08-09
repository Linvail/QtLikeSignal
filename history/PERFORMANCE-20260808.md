# Performance findings — snapshot 2026-08-08

Costs measured in `src/`, kept separate from `OPEN-RISKS-20260808.md` because none of these are
defects: the code is correct, it is just paying more than it needs to. Items are `P<n>` so they do
not collide with the `R<n>` risk numbering. P2 is the same subject as risk R25, recorded here with
the numbers it was missing.

Nothing here has been fixed. Nothing here should be fixed on the strength of these numbers alone —
see *Before acting on any of this* at the end.

## How these were measured

**Not with the test build.** `waf` configures `-O0` plus a sanitizer, which is right for correctness
work and useless for timing — ThreadSanitizer alone distorts by an order of magnitude. Every figure
below comes from a standalone benchmark compiled directly:

```
clang++ -std=c++17 -O2 -Isrc -Isubmodules/external/boost bench.cpp src/*.cpp -lpthread
```

Loops whose result is otherwise unused are pinned with an `asm volatile` barrier. Without it the
optimiser deletes them outright — the first attempt at the atomic-load baseline reported 0.00 ns for
exactly that reason.

Absolute numbers are one machine, one run of a microbenchmark with no work between iterations.
Ratios and scaling curves are the durable part; the nanosecond values are not.

## Summary

| ID | Finding | Impact | Confirmed by |
|----|---------|--------|--------------|
| P1 | `~Object()` scans the whole process-wide `callLater` registry | **High** | Measured — 130x with 2000 pending |
| P2 | `Object::thread()` takes a mutex where Qt takes an atomic load (= R25) | Medium | Measured — 32x under 8-thread contention |
| P3 | Every emit builds a `std::function` on the heap, even for a direct call | Medium | Measured — +38 ns over raw boost |
| P4 | One dispatcher mutex serialises every object on a thread | Unknown | Inspection only |
| P5 | Timer list is scanned linearly twice per dispatch pass | Low | Inspection only |
| P6 | Dispatch is 2–3.5x Qt 6's; same-thread **fixed**, queued improved 17% but still 2x QtMimic | Medium | Measured — `test_QtLikeSignal_Performance.cpp` |

---

## P1 — `~Object()` scans the whole process-wide `callLater` registry

**Impact: High. Measured.**

Every `Object` destruction takes `CallLaterRegistry::sMutex` — a single process-wide lock — and walks
the *entire* map of pending `callLater` entries looking for its own:

```cpp
std::lock_guard<std::mutex> lock( CallLaterRegistry::sMutex );
auto& pending = CallLaterRegistry::sPending;
for( auto it = pending.begin(); it != pending.end(); )
{
    if( it->first.mContext == this ) { it = pending.erase( it ); }
    else                             { ++it; }
}
```

This runs unconditionally, including for the overwhelming majority of objects that never called
`callLater()` at all. Cost of one construct-and-destruct against a growing registry:

| pending `callLater`s | ns per ctor+dtor | per pending entry |
|---|---|---|
| 0 | ~100 | — |
| 500 | 1 868 | 3.74 ns |
| 1 000 | 5 978 | 5.98 ns |
| 2 000 | 12 478 | 6.24 ns |
| 4 000 | 27 276 | 6.82 ns |

Linear, at roughly 6 ns per entry scanned. **2000 pending entries make destroying an unrelated
`Object` about 130x more expensive** (~100 ns → ~12.9 µs), and 2000 is not a large backlog for a busy
application.

There are two separate costs and the second is the worse one. The scan is *O(all pending callLaters
in the process)*, and it happens while holding a lock shared by every thread — so object destruction
is serialised process-wide, and gets slower as unrelated work queues up elsewhere.

Directions, cheapest first:
- Skip the scan entirely unless this object has ever scheduled a `callLater`. One `bool` set in
  `scheduleCallLater()` removes the cost for every object that does not use the feature, which is
  most of them. Does not help an object that does.
- Add a secondary index keyed by `Object*`, so removal is a lookup rather than a full walk. Removes
  the *O(n)*, leaves the global lock.
- Move the pending set into the `Object` itself, which removes both, at the cost of reworking how
  deduplication keys are stored.

## P2 — `Object::thread()` takes a mutex where Qt takes an atomic load

**Impact: Medium. Measured. Same subject as risk R25.**

Qt reads affinity with two atomic loads and no lock:

```cpp
QThread *QObject::thread() const
{
    return d_func()->threadData.loadRelaxed()->thread.loadAcquire();
}
```

`QObjectPrivate::threadData` is a `QAtomicPointer<QThreadData>`, and `QObjectPrivate` holds one
counted reference for the object's whole lifetime, so a read never needs a temporary one. Ours goes
through `Affinity::data()`, which locks a mutex and copies a `shared_ptr`.

Uncontended:

```
Object::thread()     =  5.1 – 6.2 ns/call
  bare atomic load   =  0.24 ns          <- what Qt pays
  uncontended mutex  =  2.7 – 3.5 ns     <- the bulk of the difference
  shared_ptr copy    =  0.3 ns
direct emit          =  ~86 ns           (affinity read ~6% of it)
```

Under contention — and the mutex is per-`Object`, so this is precisely the "several threads emit to
one receiver" shape:

| threads reading the same object | ns/call |
|---|---|
| 1 | 23 |
| 2 | 136 |
| 4 | 292 |
| 8 | 744 |

About 32x degradation at 8 threads. An atomic load would stay flat: the cache line stays shared and
nothing invalidates it.

**The mutex is buying something Qt does not offer, which is why this is not simply a mistake.**
`Affinity::data()` returns a strong reference that is guaranteed alive for the duration of the call.
Qt's raw `QAtomicPointer` does not: a reader on another thread can in principle load a `QThreadData`
that a concurrent `moveToThread()` then drops to zero. Qt tolerates that because `moveToThread()` is
thread-confined, the posting path re-checks under a retry loop
(`QCoreApplicationPrivate::lockThreadPostEventList`), and each connection caches its own ref-counted
`receiverThreadData` so `doActivate()` never reads through the receiver at all.

The Qt-shaped change that would keep our guarantee is to split the two uses, which are genuinely
different: `thread()` and the `AutoConnection` same-thread comparison only ever *compare*, and could
read an `std::atomic<ThreadData*>` cached alongside the `shared_ptr`; only the queued path needs a
strong reference, and it is about to allocate an event and take the dispatcher's mutex anyway. The
residual hazard is the fast path holding a raw pointer across a concurrent `moveToThread()`, and
closing that properly needs retired-pointer retention — which is where the change stops being small.

## P3 — every emit builds a `std::function` on the heap

**Impact: Medium. Measured in aggregate; the allocation itself is inferred.**

```
boost::signals2 emit, bare slot   =  50.9 ns
QtLikeSignal direct emit          =  88.8 ns   (+38.0 ns, ~75% overhead)
```

Per emit, the `connect()` wrapper locks the life token, constructs `boundSlot` capturing the weak
token, receiver, slot pointer and a copy of every argument, and hands it to `dispatchMetaCall()` as a
`std::function<void()>`. That closure exceeds the small-object buffer, so it heap-allocates — **on
every emit, including a `DirectConnection`, and including emits whose metacall is subsequently
dropped**.

The allocation is inferred rather than counted directly, but the evidence is solid: 800k emits filled
AddressSanitizer's freed-block quarantine to ~37 MB, and re-running with
`ASAN_OPTIONS=quarantine_size_mb=1` made that vanish. Only per-emit allocate/free traffic produces
that pattern.

The direct path could plausibly avoid it: when the resolved type is `DirectConnection`, the slot can
be invoked in place rather than packaged into a `std::function` first.

## P4 — one dispatcher mutex serialises every object on a thread

**Impact: Unknown — inspection only, not measured.**

`EventDispatcherDefault::mMutex` guards the event queue and the timer list, and is taken by
`postEvent()`, `registerTimer()`, `unregisterTimer()` and every `processEvents()` pass. It is shared
by *all* objects living on that thread, so it is a strictly wider bottleneck than P2's per-object
lock.

Worth stating because it changes what fixing P2 would achieve: for cross-thread queued emits, both
locks are taken, so removing P2's alone would likely relocate the contention rather than remove it.
Measure this one first if queued-emit throughput ever matters.

## P5 — timer list is scanned linearly twice per dispatch pass

**Impact: Low — inspection only, not measured.**

`processEvents()` walks `mTimers` once to collect expired timers, then again to find the earliest
next deadline for the wait timeout. Two *O(n)* passes per wake, where Qt keeps timers in deadline
order so the next one is the head of the list. Irrelevant for a handful of timers; it would matter
for hundreds on one thread.

---

## P6 — dispatch costs 2–5x Qt 6's, driven by allocations per emit

**Impact: High. Measured, with the cause isolated.**

`src/tests/test_QtLikeSignal_Performance.cpp` runs the same four scenarios against QtLikeSignal,
QtMimic and Qt 6 in one process. Median of three runs, `-O2`, no sanitizer:

| scenario | Qt 6 | QtLikeSignal | QtMimic | ours vs Qt 6 | ours vs QtMimic |
|---|---|---|---|---|---|
| `connect()` | 122 ns | 537 ns | 680 ns | 4.4x | 0.79x |
| emit → receive, direct | 29 ns | 76 ns | 64 ns | 2.6x | 1.18x |
| emit → receive, auto same-thread | 28 ns | 98 ns | 99 ns | 3.5x | 0.99x |
| emit → receive, queued cross-thread | 539 ns | 1022 ns | 496 ns | **1.9x** | **2.1x** |

> All rows are **after the 2026-08-09 fixes**. Before them: direct 150 ns, auto 148 ns, queued
> 1240 ns. The queued row is measured from queued-only runs, which are noticeably less noisy than
> reading it out of a full run — it is a producer/consumer race, and the numbers spread by ±10%.

Qt is faster on every row, and by a wider margin than the earlier QtMimic-only comparison suggested.
Qt's direct emit (30 ns) even beats a bare `boost::signals2` emit (51 ns, see P3): its
`QMetaObject::activate()` walks a preallocated connection list and calls the slot without allocating
anything, where our path heap-allocates several times per emit. `connect()` is the one row where we
beat QtMimic, and it is still 4.5x Qt's.

### Same-thread paths: fixed (2026-08-09)

We allocated on *every* emit, including a `DirectConnection` that just calls the slot and returns:

```
                            before   after
QtLikeSignal direct           1.00    0.00   allocations per emit
QtLikeSignal auto same-thread 1.00    0.00
QtMimic (either)              0.00    0.00
```

The wrapper built its `boundSlot` closure and passed it *by value* as `std::function<void()>` into
`dispatchMetaCall()`, and only inside that call was the connection discovered to be direct. The
closure exceeds the small-object buffer, so that was a malloc and free per emit, plus a redundant
second `weakLife.lock()` (the inner closure re-checked what the wrapper had already checked) and an
indirect call.

QtMimic never had the problem because it decides first and acts second:

```cpp
if( aType == ConnectionType::Direct ) { slot( args... ); return; }   // no closure, no affinity read
```

Fixed by splitting the decision out of the dispatch: `decideDispatch()` answers CallInline / Queue /
Drop, and `invokeOrQueue()` takes the invocation as a *template parameter* so the inline path never
materialises a `std::function`. Only the queued branch wraps it, because only that branch stores it.
An explicit `DirectConnection` now never reads the affinity at all, which also skips the `Affinity`
mutex measured in P2.

It had to be a helper in the .cpp rather than inline logic in the wrapper because `Object.h` only
forward-declares `Thread` — which is exactly why the original code deferred everything to
`dispatchMetaCall()` in the first place.

### The queued path: partly fixed, and the rest is structural

`new MetaCallEvent( aSlot )` copied a by-value parameter that was dead on the next line, at both
call sites. Moving instead took allocations per queued emit from **3.93 to 2.78** (QtMimic: 2.42) and
the time from ~1240 ns to ~1022 ns, about 17%.

Allocation count is now close to parity, so it is no longer what separates us. The remaining
difference is **one extra lock per emit**, and it is architectural rather than an oversight:

| | mutexes taken per queued emit |
|---|---|
| QtLikeSignal | 3 — `Affinity::data()`, `ThreadData::dispatcher()`, `EventDispatcherDefault::postEvent()` |
| QtMimic | 2 — `Affinity::data()`, `ThreadData::post()` |

Our `ThreadData` holds a *pointer to a dispatcher object* that owns the queue, so finding the queue
costs a guarded lookup and a `shared_ptr` copy before the queue's own lock is even taken. QtMimic's
`ThreadData` owns its mailbox directly, so there is nothing to look up. Qt does the same as QtMimic —
`QThreadData` owns `postEventList`.

Closing it means one of:
- making the dispatcher pointer atomically readable, which carries the same lifetime trade-off
  discussed in P2 (the guarded `shared_ptr` is what makes R12's "dispatcher cannot die mid-call"
  guarantee hold); or
- moving the event queue into `ThreadData`, which is what Qt and QtMimic both do, and is the larger
  change.

Neither is worth doing on microbenchmark evidence alone. Also unmeasured: the consumer copies the
queue into a fresh `std::vector` on every dispatch pass, where QtMimic swaps its deque, which may
account for part of the residual 0.36 allocations per emit.

**A syscall hypothesis was checked and rejected.** The obvious suspect for the queued row was
`EventDispatcherLinux::wakeWaiter()` writing to its eventfd on every post, which QtMimic (condvar
only) does not do. `strace -c` disproved it: 200k emits produced **725** writes, because the
`mWakePending` collapsing flag does its job, and total syscall time was comparable (~40 ms each).

> ### Correction: the first version of this table was biased
>
> The numbers first published here reported direct emit at 82 ns for QtLikeSignal against 74 ns for
> QtMimic (1.11x), and same-thread `Auto` at 82 vs 107 ns — i.e. that we were *faster* on `Auto`.
> Both were artefacts of test ordering. The real figures are 2.3x and 1.5x **slower**.
>
> glibc keeps a lock-free fast path for `malloc` while a process is single-threaded and abandons it
> permanently once a second thread has existed. gtest runs tests in registration order, so
> QtLikeSignal's direct and auto scenarios ran *before* any benchmark had started a thread, on the
> fast path, while QtMimic's ran *after* `QtLikeSignal_QueuedEmitCrossThread` had created one. The
> comparison was measuring allocator state as much as dispatch.
>
> Isolated, the effect is unambiguous: QtMimic's direct emit measured 40.3 ns alone, 63.2 ns after
> QtLikeSignal's queued test, and 64.3 ns after Qt 6's — the same penalty whichever library spawned
> the thread. Qt 6's own direct emit was unaffected (30.3 → 29.7 ns) because it does not allocate per
> emit, and the penalty scaled with each library's allocations per emit, which is itself corroborating
> evidence for the allocation finding above.
>
> `PerfHarness::settleAllocatorState()` now spawns and joins one thread before anything is timed, so
> every library is measured in the same state — which is also the state any threaded application is
> in. After the fix the ordering sensitivity is gone: QtMimic's direct emit reads 70.0 ns alone and
> 65.6 ns after Qt 6's queued test.

Caveat on `connect()`: that scenario connects 20 000 slots to a single signal, so it also measures
insertion into a growing slot list. It is the same shape for all three libraries, so the comparison
holds, but the absolute number is not the cost of one connection to an empty signal.

**Nothing here has been profiled against a real workload.** These are microbenchmarks with no work
between iterations, which is the condition most favourable to making lock and allocation overhead
look decisive. In a program that does anything between emits, all of these shrink as a proportion.

If a real profile does point here, the order is P1, then P4, then P2/P3. P1 is first because it is
the only one that degrades with unrelated activity elsewhere in the process, and because the cheapest
fix for it is a single flag.

## Measurement traps hit while producing this

Recorded because each cost real time and each would recur:

- **Do not benchmark the test build.** It is `-O0` with a sanitizer.
- **`std::clock()` is not CPU time on MSVC.** It returns wall-clock time since CRT init, so a
  cpu/wall ratio computed from it is ~1.0 on Windows no matter what the code does. Use
  `TestSupport::processCpuSeconds()` (`src/tests/TestCpuTime.h`).
- **AddressSanitizer's quarantine inflates RSS.** Freed blocks are retained, so a memory measurement
  under ASan reports growth for perfectly healthy code. Cross-check with
  `ASAN_OPTIONS=quarantine_size_mb=1`.
- **A single RSS sample cannot distinguish a leak from allocator arena high-water.** Sample per round
  over several rounds; correct code grows once and then flattens.
- **An unused result gets optimised away.** Pin benchmark loops with a barrier, and be suspicious of
  any result reporting 0.00 ns.
- **glibc's allocator gets permanently slower once a second thread has existed.** In one process,
  tests that run before any thread is spawned are therefore measured on a faster `malloc` than the
  ones after, which silently biased the first comparison table by ~2x in one library's favour. Settle
  the process into its multi-threaded state before timing anything
  (`PerfHarness::settleAllocatorState()`), and distrust any in-process A/B where one side runs first.
