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
| P6 | Cross-thread queued dispatch is ~2.4x QtMimic's, from ~1.5 extra allocations per emit | **High** | Measured — `test_QtLikeSignal_Performance.cpp` |

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

## P6 — cross-thread queued dispatch costs ~2.4x QtMimic's, from redundant allocations

**Impact: High. Measured, with the cause isolated.**

`src/tests/test_QtLikeSignal_Performance.cpp` runs the same four scenarios against both libraries in
one process. Median of three runs, clang `-O2`, no sanitizer:

| scenario | QtLikeSignal | QtMimic | ratio |
|---|---|---|---|
| `connect()` | 466 ns | 716 ns | **0.65x** |
| emit → receive, direct | 82 ns | 74 ns | 1.11x |
| emit → receive, auto same-thread | 82 ns | 107 ns | **0.79x** |
| emit → receive, queued cross-thread | 1287 ns | 550 ns | **2.34x** |

Three of four favour us or are close. `connect()` is meaningfully cheaper, and the same-thread `Auto`
path is ~20% cheaper — QtMimic resolves `Auto` by comparing `shared_ptr<ThreadData>` values, which
costs it two refcount round-trips, where we compare raw `Thread*`.

The cross-thread queued path is the outlier, and the cause is allocation count, not syscalls:

```
QtLikeSignal queued: 3.85 allocations per emit
QtMimic      queued: 2.33 allocations per emit
```

**The syscall hypothesis was wrong and worth recording as such.** The obvious suspect was
`EventDispatcherLinux::wakeWaiter()` writing to its eventfd on every post, which QtMimic (condvar
only) does not do. `strace -c` disproved it: 200k emits produced **725** writes, because the
`mWakePending` collapsing flag is doing its job, and total syscall time was comparable between the
two (~40 ms each).

The extra allocations come from a `std::function` being copied along the queued path rather than
moved. Each queued emit currently: builds `boundSlot`, converts it to `std::function<void()>` when
passing it *by value* into `dispatchMetaCallTo()`, copies it *again* into the `MetaCallEvent`
constructor's by-value parameter, and heap-allocates the `MetaCallEvent` itself.

At least one of those copies is free to remove — `new MetaCallEvent( aSlot )` can be
`new MetaCallEvent( std::move( aSlot ) )`, since `aSlot` is a by-value parameter that is dead
afterwards — and the wrapper can `std::move( boundSlot )` into the call. That is a small, contained
change with a directly measurable target: get allocations per emit from 3.85 toward QtMimic's 2.33.

Caveat on `connect()`: that scenario connects 20 000 slots to a single signal, so it also measures
insertion into a growing slot list. It is the same shape for both libraries, so the comparison holds,
but the absolute number is not the cost of one connection to an empty signal.

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
