# Performance findings — snapshot 2026-08-13

Costs measured in `src/`. Kept separate from `OPEN-RISKS-20260813.md` because none of these is a
defect: the code is correct, it just pays more than it needs to. Items are `P<n>`, continuing the
numbering from `PERFORMANCE-20260808.md`.

How every figure below was produced is at the end of this document, under
[How these were measured](#how-these-were-measured). Read it before quoting a number.

## Status

| ID | Status | Finding | Impact | Latest measurement |
|----|--------|---------|--------|--------------------|
| P1 | **Fixed** | `~Object()` scans the whole process-wide `callLater` registry, and the dispatcher's whole event queue | High | 24 139 ns → **51.6 ns** per ctor+dtor at 4 000 pending, and flat |
| P2 | **By Design** | `Object::thread()` takes a mutex where Qt takes an atomic load (= risk R25) | Medium | 5–6 ns per call, 32x degradation at 8 threads |
| P3 | **Fixed** | Every emit builds a `std::function` on the heap, even for a direct call | Medium | 1.00 → **0.00** allocations per direct emit |
| P4 | **By Design** | One dispatcher mutex serialises every thread posting to one thread | Medium | Measured — a post costs 47 ns alone, **1.19 µs** with 8 posters; Qt serialises identically |
| P5 | **By Design** | Timer list is scanned linearly twice per dispatch pass | Low | Measured — **0.78 ns per timer** per idle pass; 130 ns at 100 timers |
| P6 | **Fixed** | Dispatch cost against Qt 6 — "2–3.5x slower on every row" | Medium | Re-measured — direct emit **beats Qt 6**, queued is parity, and we beat boost on every row |
| P7 | **Fixed** | `disconnect()` is O(slots on the signal), so tearing down N receivers of one signal is O(N²) | High | 671.2 ms → **3.98 ms** for 16 000 receivers, and flat |
| P8 | **In progress** | A connect makes the next emit rebuild the whole slot list | Medium | Churn 16.8 → **13.1 µs** per cycle; the rebuild itself remains |
| P9 | **By Design** | The R28 correctness fix costs one mutex per dispatched event | Low | **+3.3 ns** per event |

**Everything here is now measured, and nothing is queued.** Four items are fixed, four are accepted
costs, and one is partly done and deliberately parked.

Two things are worth knowing without reading further:

- **We are faster than Qt 6 on direct emit and level with it on queued cross-thread** (P6), and
  faster than the boost::signals2 we replaced on every row. Nothing degrades with scale on one
  thread any more: P1 and P7 were the two that did.
- **P4 degrades with *concurrency*, not with data.** Posting to a thread from eight others costs 25x
  what posting from one does. That is Qt's architecture as much as ours, and the entry says what
  changing it would take.

---

# Details

## P1 — `~Object()` scans two process-wide backlogs *(Fixed)*

Every `Object` destruction took the process-wide `CallLaterRegistry::sMutex` and walked the whole
pending map looking for its own entries — including for the overwhelming majority of objects that
never called `callLater()` at all. Cost of one construct-and-destruct against a growing backlog:

| pending | before | after |
|---|---|---|
| 0 | 74.5 ns | 51.3 ns |
| 500 | 1 301 ns | 57.0 ns |
| 1 000 | 5 067 ns | 54.6 ns |
| 2 000 | 11 299 ns | 51.2 ns |
| 4 000 | 24 139 ns | **51.6 ns** |

**468x at 4 000 pending, and flat** — the growth is gone, not reduced.

The fix is the flag the 2026-08-08 entry proposed: `mUsedCallLater`, set by `scheduleCallLater()`
and tested by `~Object()`.

**Measuring it exposed a second scan of the same shape, and the larger one.** `~Object()` also
called `removeEventsForReceiver()` unconditionally, which walks the dispatcher's whole event queue
*and* its whole timer list under that dispatcher's lock. With only the registry guard in place the
benchmark still grew, 51 ns to 1 600 ns, because the pending `callLater`s had also left 4 000
undispatched events in the queue. Guarded the same way, with `mMayHaveQueuedWork` plus "does this
object own any timer". Qt guards the identical call identically:
`if (d->postedEvents) QCoreApplication::removePostedEvents(this, 0);` in `~QObject()`.

Both flags are set-once and never cleared, so an object that used either feature keeps paying its
scan for life. Qt keeps an exact count instead, which needs the dispatch side to decrement; that is
more machinery than the difference is worth.

## P2 — `Object::thread()` takes a mutex where Qt takes an atomic load *(By Design)*

Qt reads affinity with two atomic loads and no lock. Ours goes through `Affinity::data()`, which
locks a mutex and copies a `shared_ptr`
([ThreadData.hpp:127-131](src/ThreadData.hpp#L127-L131)). Uncontended that is 5–6 ns against Qt's
0.24 ns; with eight threads reading one object's affinity it degrades about 32x. Full numbers and
Qt's implementation are in `PERFORMANCE-20260808.md`.

**Accepted, and not recommended as a standalone change.** Three reasons:

- The mutex buys a guarantee Qt does not offer: `data()` returns a strong reference guaranteed alive
  for the duration of the call. Removing it needs retired-pointer retention, which is where the
  change stops being small.
- The hot path no longer pays it. An explicit `DirectConnection` returns before it reads the affinity
  box at all ([Object.h](src/Object.h#L689-L695)), so only the auto and queued paths are affected.
- It is a constant factor, and for a cross-thread emit both this lock and the dispatcher's (P4) are
  taken. P4 is now measured at up to 1.19 µs per post under contention, against this one's 5–6 ns,
  so removing this lock alone would move the queue by a rounding error.

What this lock still costs is visible in P6's auto same-thread row: ~46 ns against Qt 6's ~28 ns, on
a path that reads the affinity on every emit. That row is the whole of P2's remaining impact, since
a `DirectConnection` never reads the box at all.

If it is ever done, fold it in with moving the event queue into `ThreadData` (see P4). Both touch the
same lifetime question, and doing them separately means solving it twice.

## P3 — every emit builds a `std::function` on the heap *(Fixed)*

Fixed 2026-08-09. The wrapper is type-erased into a `std::function` once, at connect time, and
`connectImpl()` keeps the slot's concrete type all the way into the wrapper
([Object.h:664-740](src/Object.h#L664-L740)). The direct and same-thread branches now call the slot
in place; only the queued branch builds a closure, and it holds its argument tuple inline rather
than behind a second `make_shared`.

| | before | after |
|---|---|---|
| allocations per direct emit | 1.00 | **0.00** |
| allocations per same-thread auto emit | 1.00 | **0.00** |

## P4 — one dispatcher mutex serialises every thread posting to one thread *(By Design)*

`EventDispatcherDefault::mMutex` guards the event queue and the timer list, and is taken by
`postEvent()`, `registerTimer()`, `unregisterTimer()` and every `processEvents()` pass. It is shared
by *all* objects living on that thread, so it is a strictly wider bottleneck than P2's per-object
lock.

**Measured 2026-08-13, and it is severe.** T threads posting to one dispatcher, against the same T
threads each posting to their own — identical work and identical allocations, so the difference is
the shared lock and nothing else:

| posting threads | shared dispatcher | separate dispatchers | ratio |
|---|---|---|---|
| 1 | 48.7 ns | 46.8 ns | 1.04 |
| 2 | 113.7 ns | 24.0 ns | 4.7 |
| 4 | 118.7 ns | 12.7 ns | 9.3 |
| 8 | 149.1 ns | 9.5 ns | **15.8** |

Those are per-post figures across all threads, so the separate column falling is throughput scaling
correctly. **The shared column rising is throughput going backwards**: 20.5 M posts/s on one thread,
6.7 M posts/s on eight. Per posting thread the latency of a single `postEvent()` goes from **47 ns
alone to 1.19 µs under eight-way contention, 25x**.

**The condvar is not the cause, which was worth checking.** `wakeWaiter()` runs `notify_all()` on
every post, on a cache line every poster touches. Removing it entirely changes nothing — 145.3 ns
against 140.4 ns at eight threads, inside the noise. It is the mutex.

**Accepted, for three reasons.**

- **Qt does exactly the same thing.** `QCoreApplication::postEvent()` takes
  `QMutexLocker locker(&data->postEventList.mutex)` — one mutex per target thread's event list, held
  by every poster. We are not deviating from the model we are copying.
- **The critical section is already minimal**: one `bool` test and a `push_back`, with the wake
  outside the lock. There is nothing left to shave.
- **The real fix is a different data structure**, not a smaller critical section. A lock-free MPSC
  queue is the textbook fit — many producers, one consumer, wait-free push — and it would remove the
  contention rather than reduce it.

**What blocks the MPSC queue, recorded so the next person does not rediscover it:**
`removeEventsForReceiver()` deletes entries *from the middle* of the queue, and
`processDeferredDeletes()` filters it by event type. An MPSC queue supports neither. Both would have
to become consumer-side operations on a drained batch — which is close to what R28's dispatch frames
already do, so it is not unthinkable, but it is a redesign of event ownership and not a local change.

**The measurement is a ceiling, not a forecast.** Eight threads posting in a tight loop with no work
between posts is the worst case that can be constructed. Real code posts occasionally.

## P5 — the timer list is scanned linearly twice per dispatch pass *(By Design)*

`processEvents()` walks `mTimers` once to collect expired timers, and again — on an otherwise idle
pass — to find the earliest deadline for the wait timeout
([EventDispatcherDefault.cpp:54-90](src/EventDispatcherDefault.cpp#L54-L90)). Qt keeps its timers in
deadline order, so its equivalent of the second scan is reading the head of the list.

**Measured 2026-08-13**, with the blocking wait overridden away so a pass costs only what it
computes:

| timers on the thread | idle pass (both scans) | busy pass (collect only) |
|---|---|---|
| 0 | 58.9 ns | 87.1 ns |
| 10 | 56.7 ns | 82.6 ns |
| 100 | 130.1 ns | 117.4 ns |
| 1 000 | 840.2 ns | 359.7 ns |
| 10 000 | 7 834 ns | 2 611 ns |

Linear, as expected: **0.78 ns per timer on an idle pass, 0.25 ns per timer on a busy one.**

**Accepted, because the absolute numbers do not justify the machinery.** A hundred timers on one
thread — already a lot — costs 130 ns per pass. The expensive scan is also the one that runs
*immediately before the loop blocks*, where a microsecond spent choosing a sleep deadline is not
worth optimising.

The shape where it would matter is many timers firing often, since the pass count then rises with
the timer count too: 10 000 timers each firing once a second is 7 834 ns × 10 000 = 7.8% of a core.
That is the point at which to act, and nothing near it exists today.

**If it is ever done**, the shape is Qt's: a deadline-ordered structure so the collect loop stops at
the first entry that is not due and the wait deadline is the head. The cost is that
`unregisterTimer()` looks up by *id*, and a re-armed repeating timer has to be repositioned, so it
needs an id index beside the ordering — which is why a sorted vector alone does not do it.

## P6 — dispatch cost against Qt 6 *(Fixed)*

The 2026-08-08 finding was "dispatch costs 2–3.5x Qt 6's" and its table was measured with
boost::signals2 underneath us. **Re-measured 2026-08-13**, `-O2`, no sanitizer, via
`src/tests/test_QtLikeSignal_Performance.cpp`, which runs all three libraries in one process:

| scenario | Qt 6 | QtLikeSignal | QtMimic (boost) | then (2026-08-08) | now |
|---|---|---|---|---|---|
| `connect()` | ~110 ns | ~320 ns | ~760 ns | 4.4x slower | **2.9x slower** |
| emit → receive, direct | ~29 ns | **~24 ns** | ~63 ns | 2.3x slower | **1.2x faster** |
| emit → receive, auto same-thread | ~28 ns | ~46 ns | ~96 ns | 2.9x slower | **1.6x slower** |
| emit → receive, queued cross-thread | ~490 ns | ~510 ns | ~550 ns | 1.9x slower | **parity** |

**We are now faster than Qt 6 on direct emit and level on queued cross-thread**, and faster than the
boost we replaced on every row. The two rows still behind Qt have known causes, below.

Numbers are the range over three runs in declaration order plus two under `--gtest_shuffle`; the emit
rows agreed to within a nanosecond or two across all five, so the ordering bias that invalidated the
first version of this table is not present. `connect()` is the noisy one — Qt 6 measured anywhere
from 76 to 119 ns depending on order — so treat its ratio as approximate.

### What the re-measurement changed in the code

The auto row was ~57 ns when this was first re-run, and is ~46 ns now. `Object::isCurrentThread()`
was written as:

```cpp
return aData == Thread::currentThread()->threadData();     // shared_ptr copy, to compare pointers
```

`Thread::threadData()` returns by value, so every `Auto` emit paid an atomic increment and decrement
to answer a pointer comparison. It now compares bare pointers through a new `threadDataPtr()`, which
is safe precisely because it is not an ownership handle: `mData` is assigned once in the constructor
and never reassigned, and the only caller asks it of `Thread::currentThread()` — this thread, which
cannot be destroyed underneath its own comparison.

**11 ns, about 19% of the auto path**, for four lines. Direct emit is unchanged at 23.3–23.5 ns,
confirming the saving is on the path it was aimed at.

### What is still behind Qt, and why it stays

- **Auto same-thread, ~46 ns against ~28 ns.** The remainder is P2: the affinity read locks a mutex
  where Qt does an atomic load, and it is accepted for the lifetime guarantee it buys. Direct
  connections skip it entirely.
- **`connect()`, ~320 ns against ~110 ns.** Three heap allocations per connection — `Slot`,
  `ConnectionState`, and the growth of the working list — plus `connectImpl()` building a `Cleanup`
  token and taking `mIncomingMutex`. Qt walks a preallocated connection list. `connect()` is not a
  hot path, and the benchmark connects 20 000 slots to one signal so it also measures list growth.
  Not worth chasing.
- **Queued cross-thread is at parity**, so the three-mutex analysis in the 2026-08-08 entry no longer
  describes a gap worth closing. It still describes the structure — `ThreadData` owns a *pointer to a
  dispatcher* that owns the queue ([ThreadData.hpp:61-77](src/ThreadData.hpp#L61-L77)), where Qt puts
  the queue in the thread data directly — and that is where P4's contention lives, but on a single
  posting thread it costs nothing measurable.

## P7 — `disconnect()` is O(slots on the signal) *(Fixed)*

`Connection::disconnect()` used to `stable_partition` the **entire** working slot list to find the
one entry that had just died. So each disconnect cost O(slots on that signal), and `~Object()`
disconnects every incoming connection it holds — which made destroying the N receivers of one
long-lived signal O(N²), with no ceiling.

This was the one place our own `Signal` was worse than the boost it replaced, and it was not close.

| N = 16 000 | before | after | QtMimic (boost) |
|---|---|---|---|
| destroy all receivers | 671.2 ms | **3.98 ms** | 2.94 ms |
| per receiver | 41.95 µs, growing | **0.23 µs, flat** | 0.18 µs, flat |
| connect all | 3.62 ms | 4.13 ms | 9.73 ms |

**169x faster, and the curve is flat rather than quadratic** — which is the part that matters. We
are 1.35x slower than boost on teardown alone and 2.4x faster on connect; the composite of connect +
emit + destroy is comfortably ahead.

**The design.** Each slot records its own index in the working list, reached through a type-erased
back-pointer in the `ConnectionState` the handle already holds. Removing a connection nulls that
element where it stands; nothing is searched for, no other slot's index moves, so emission order is
untouched. The nulls are compacted away in bulk once they outnumber the live entries — amortised
O(1) per removal, since each compaction at least halves the list and so the passes are geometrically
rare.

**A `std::list` was tried first and rejected.** It gives O(1) removal with no tombstones and measured
*better* on teardown (2.15 ms), but **66% worse on churn** — 36.3 µs against 21.9 — because every
snapshot rebuild then chases pointers instead of copying a contiguous block. The
index-plus-tombstone form keeps the writers' side a vector and wins on both.

**Second-order, same family, not fixed:** `~Cleanup` erases from `Object::mIncoming` with a linear
`std::remove` ([Object.cpp](src/Object.cpp#L244-L246)), so an object holding K incoming connections
disconnected one by one pays O(K²). K is normally small, and `~Object()` swaps the vector out first
so the destructor path skips it entirely. Worth fixing only if the handle bookkeeping is touched
anyway.

## P8 — a connect makes the next emit rebuild the whole slot list *(In progress)*

Readers walk an immutable snapshot, rebuilt whenever the working list changed. A steady emit loop
rebuilds nothing and a burst of connects pays one copy — but *alternating* connects and emits pays a
full rebuild every time.

**Done:** the disconnect half went with P7. Removal is now O(1), so a churn cycle no longer pays a
scan on top of the rebuild.

| churn cost per cycle, 4 000 resident slots | |
|---|---|
| before | 16.8 µs |
| after | **13.1 µs** |
| QtMimic (boost) | 5.3 µs |

**Left:** the rebuild itself, and it is the *connect* that forces it — a new slot must appear in the
next emission, whereas a removal alone would not need a rebuild at all. Closing that needs a two-tier
published list: a stable snapshot plus a small overflow the emit walks afterwards, merged in bulk.

**Not scheduled**, because we are already ahead end to end. The whole cycle costs 50 µs against
boost's 104 µs, since our emit is 2.7x faster. Only worth revisiting if a real profile shows
connection churn mattering.

Emit itself is unchanged by all of this, which was the thing to protect: 36.9 µs against 38.7 µs
before, at 4 000 receivers.

## P9 — the R28 fix costs one mutex per dispatched event *(By Design)*

Closing risk R28 meant a dispatch loop can no longer read an entry of its own batch unguarded: the
whole point is that `removeEventsForReceiver()` may cancel that entry from inside a handler. So all
three dispatch loops now take `mMutex` around taking each entry, as the timer loop already did.

Draining 800 000 posted events, minimum of ten runs:

| | ns per dispatched event |
|---|---|
| with the per-event lock (current) | 14.8 |
| without it | 11.5 |

**+3.3 ns per event, about 29% of the bare drain loop.** That matches the uncontended-mutex figure
measured independently on 2026-08-08 (P2: 2.7–3.5 ns), which is the corroboration that the number is
real and not an artefact.

In proportion it is nothing: a queued metacall costs roughly 850 ns end to end, so this is under half
a percent of the operation it sits inside. The alternative was a use-after-free that segfaults, so
the trade is not close. **The only way to give this cost back is to give the guarantee back**, which
is why it is By Design rather than queued.

The R30 fix adds a second lock of the same kind and is not worth measuring: one acquire per *ready
descriptor per poll round*, not per event, on a path that has just returned from a syscall.

---

# How these were measured

**Not with the test build.** `waf` configures `-O0` plus a sanitizer, which is right for correctness
work and useless for timing. Every figure above comes from a standalone benchmark compiled directly:

```
clang++ -std=c++17 -O2 -Isrc bench.cpp \
    src/AbstractEventDispatcher.cpp src/CoreApplication.cpp src/EventDispatcherDefault.cpp \
    src/Object.cpp src/Thread.cpp src/ThreadData.cpp src/Timer.cpp \
    src/EventDispatcherLinux.cpp src/ThreadPosix.cpp -lpthread
```

QtMimic was built the same way from `external/QtMimic/src` with `-Isubmodules/external/boost`, and
run in the same process shape, so the two columns are comparable to each other. Absolute numbers are
one machine; the **scaling curves and the ratios** are the durable part.

## What each benchmark actually does

**Teardown (P7).** Connect N receivers to one signal, then destroy them all and time that. Reported
per receiver so the shape of the curve is visible: flat means O(1) per removal, rising means O(N)
per removal and O(N²) in total.

**Churn (P8).** With a fixed number of *resident* receivers connected to one signal and kept alive
for the whole measurement, time 2 000 iterations of:

```cpp
Receiver tmp;                                    // 1. construct a receiver
Object::connect( sig, &tmp, &Receiver::slot );   // 2. connect it
sig.emit( 1 );                                   // 3. emit to all of them
                                                 // 4. tmp leaves scope -> destroyed -> disconnected
```

So one **cycle** is connect + emit + destroy-and-disconnect, and *resident* is the fan-out every emit
pays. Three numbers come out of it, and they must not be confused:

| column | meaning | at 4 000 resident, ours vs boost |
|---|---|---|
| `emit_only` | the same emit with nothing changing | 36.9 vs 98.5 µs |
| `with_churn` | the whole cycle above | 50.0 vs 103.8 µs |
| `churn_cost` | `with_churn − emit_only`, what the connect and disconnect add | 13.1 vs 5.3 µs |

The emit dominates the cycle, so we win the total while losing the isolated component. Note also
that the cycle includes constructing and destroying an `Object`, not only the `Signal` operations,
so `churn_cost` is not purely `Signal` work. And connecting and disconnecting on every emit is not a
realistic pattern — it exists to make connection changes visible against a fixed fan-out.

**Backlog (P1).** Build a backlog of pending `callLater` entries that nothing will dispatch, then
time construct-and-destruct of an *unrelated* `Object` against it. The point is what an object that
never touched the feature pays for other objects' pending work.

**Dispatch drain (P9).** Post N events to an idle dispatcher, then time one `processEvents()` drain.
Nothing else runs, so the loop cost is all that is measured.

**Post contention (P4).** T threads post the same number of events each, in two configurations: all
to **one** dispatcher, and each to its **own**. Same total work, same allocations, same thread count
— so the difference between the two columns is the shared mutex and nothing else. Reported per post
across all threads, which is why the uncontended column *falls* as threads are added: that is
throughput scaling as it should. Nothing drains the queues; this measures the producer side.

**Timer scan (P5).** Register N timers far enough in the future that none is ever due, then time
`processEvents()` passes. The blocking wait is overridden to return immediately in a subclass, so a
pass costs only what it computes rather than what it sleeps. Two configurations: an **idle** pass,
which runs both scans, and a **busy** pass with one event queued, which skips the wait branch and so
runs only the collect scan.

## Traps hit while producing this

The six recorded on 2026-08-08 all still apply and are not repeated here. Three to add:

- **Subtract the fan-out.** A first attempt at P8 timed "connect + emit" against a growing resident
  count and reported a clean linear curve — which was mostly the emit itself, since an emit to N
  slots is O(N) by definition. The churn cost is only visible as the *difference* between the same
  fan-out with and without the connect/disconnect.
- **Use the minimum, not the median, for a small constant difference.** P9's 3 ns effect sits inside
  a 5 ns run-to-run spread, so medians over a handful of runs showed the two builds overlapping and
  would have supported "no measurable difference". Minimum of ten separated them cleanly. The minimum
  estimates how fast the code can go; the median mostly reports what else the machine was doing.
- **Measure the composite as well as the component.** P7's first fix attempt (`std::list`) improved
  the number it targeted and made a different one 66% worse. Neither the teardown benchmark nor the
  churn benchmark would have caught that alone.
- **Check that a probe left the baseline intact.** Pricing P8's snapshot rebuild by suppressing the
  dirty flag on connect looked like it made churn 4x *worse*. It had not: without that flag the
  resident slots were never published either, so `emit_only` fell to 0.005 µs and the subtraction
  had nothing left to subtract. A probe that disables one thing usually disables two. Print the
  baseline column, not just the derived one — the invalid result is obvious there and invisible in
  the difference.

## Standing caveat

**Nothing here has been profiled against a real workload.** These are microbenchmarks with no work
between iterations, which is the condition most favourable to making lock and allocation overhead
look decisive. In a program that does anything between emits, every constant factor above shrinks as
a proportion. P7 was the exception worth acting on regardless: a quadratic cost does not shrink when
the program does more work, it only takes longer to become visible.
