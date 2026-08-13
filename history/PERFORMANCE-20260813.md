# Performance findings — snapshot 2026-08-13

Costs measured in `src/` after boost::signals2 was replaced by our own `Signal`/`Connection`
(`40fd910`, `52ef2ff`). Kept separate from `OPEN-RISKS-20260813.md` because none of these is a
defect: the code is correct, it just pays more than it needs to.

Items are `P<n>`, continuing from `PERFORMANCE-20260808.md` (P1–P6).

**The 2026-08-08 numbers are now partly obsolete.** Every figure in that document was taken against
the boost-backed `Signal`. P1, P2, P4 and P5 are unchanged code and its analysis still applies; P3
is fixed; P6's whole comparison table needs re-running before anyone acts on it. Details at the end.

## How these were measured

Not with the test build — `waf` configures `-O0` plus a sanitizer. Every figure below comes from a
standalone benchmark compiled directly, the same way the 2026-08-08 document did it:

```
clang++ -std=c++17 -O2 -Isrc bench.cpp \
    src/AbstractEventDispatcher.cpp src/CoreApplication.cpp src/EventDispatcherDefault.cpp \
    src/Object.cpp src/Thread.cpp src/ThreadData.cpp src/Timer.cpp \
    src/EventDispatcherLinux.cpp src/ThreadPosix.cpp -lpthread
```

QtMimic was built the same way from `external/QtMimic/src` with `-Isubmodules/external/boost`, and
run in the same process shape, so the two columns are comparable to each other. Absolute numbers are
one machine and one run; the **scaling curves** are the durable part.

## Summary

| ID | Finding | Impact | Confirmed by |
|----|---------|--------|--------------|
| P7 | `disconnect()` is O(slots on the signal), so tearing down N receivers of one signal is O(N²) | **High** | Measured — 245x QtMimic at N=16000, and diverging |
| P8 | One connect/disconnect makes the next emit rebuild the whole slot list | Medium | Measured — 24 µs per cycle at 4000 resident slots |
| P1 | `~Object()` scans the whole process-wide `callLater` registry *(unchanged since 2026-08-08)* | **High** | Re-measured — 324x with 4000 pending |

---

## P7 — `disconnect()` is O(slots on the signal)

**Impact: High. Measured. A regression introduced by the in-house `Signal`.**

`Connection::disconnect()` clears its own flag and then calls `removeDisconnected()` on the signal
([Connection.h:78-93](src/Connection.h#L78-L93)). That function takes the signal's mutex and
`stable_partition`s the **entire** working slot list to find the one entry that just died
([Signal.h:337-365](src/Signal.h#L337-L365)).

So each disconnect costs O(slots currently on that signal). `~Object()` disconnects every incoming
connection it holds ([Object.cpp:184-187](src/Object.cpp#L184-L187)), which makes destroying the N
receivers of one long-lived signal O(N²).

Destroying N receivers previously connected to one signal, `-O2`:

| N | QtLikeSignal | per object | QtMimic (boost) | per object |
|---|---|---|---|---|
| 1 000 | 1.4 ms | 1.36 µs | 0.16 ms | 0.159 µs |
| 2 000 | 4.6 ms | 2.31 µs | 0.33 ms | 0.166 µs |
| 4 000 | 20.0 ms | 5.01 µs | 0.66 ms | 0.164 µs |
| 8 000 | 114.5 ms | 14.31 µs | 1.32 ms | 0.165 µs |
| 16 000 | **671.2 ms** | 41.95 µs | 2.74 ms | 0.171 µs |

QtMimic is flat — 0.16 µs per object at every size, because boost::signals2 removes a slot from an
intrusive list. Ours grows linearly per object, which is quadratic in total: **16x the receivers
costs 490x the teardown time**, and at 16 000 receivers we are 245x slower than the library we
replaced. Neither figure levels off.

This is not a microbenchmark artefact. One long-lived signal with many short-lived receivers is the
Qt pattern R17 was fixed to support, and this is the same shape of cost R17 removed, moved from emit
to teardown.

**Fix.** The slot list needs removal that does not scan. Options, cheapest first:

- Give `ConnectionState` the slot's index or an iterator into a `std::list`, so `removeDisconnected()`
  becomes a single erase. A `std::list` costs one indirection per slot on the emit walk, which the
  snapshot already pays for.
- Keep the vector and defer compaction: mark dead, count dead entries, and compact only when the
  dead fraction crosses a threshold. Amortised O(1) per disconnect, one scan per rebuild. This keeps
  the emit path's contiguous walk, which is where the current design wins (see P8's `emit_only`
  column).

Either way the invariant that must survive is the one `discardSnapshot()` exists for
([Signal.h:397-401](src/Signal.h#L397-L401)): a disconnected slot has to be *destroyed* promptly,
because its destructor runs the `Cleanup` token that prunes `Object::mIncoming`. Deferring
compaction means deferring that destruction, so a threshold scheme must still drop the slot's
`shared_ptr` at disconnect time even if the vector slot itself is compacted later.

**Second-order, same family:** `~Cleanup` erases from `Object::mIncoming` with a linear
`std::remove` ([Object.cpp:244-246](src/Object.cpp#L244-L246)), so an object holding K incoming
connections that are disconnected one by one pays O(K²). K is normally small, and `~Object()` swaps
the vector out first so the destructor path skips it entirely. Not worth fixing on its own; worth
fixing in the same pass if the handle bookkeeping is touched.

## P8 — one connect/disconnect makes the next emit rebuild the whole slot list

**Impact: Medium. Measured. Same root as P7, different victim.**

Readers walk an immutable snapshot, rebuilt whenever the working list changed
([Signal.h:375-384](src/Signal.h#L375-L384)). The design note is right that a steady emit loop
rebuilds nothing and a burst of connects pays one copy. What it does not say is what *alternating*
costs: every connect or disconnect calls `discardSnapshot()`, so the next emit copies all N
`shared_ptr`s again.

Cycle = construct one receiver, connect it, emit once, destroy it. Measured against a fixed number
of resident slots, with the emit-only cost of the same fan-out subtracted:

| resident slots | emit only | emit + churn | churn cost | QtMimic churn cost |
|---|---|---|---|---|
| 0 | 0.005 µs | 0.258 µs | 0.25 µs | 0.53 µs |
| 500 | 1.37 µs | 3.06 µs | 1.69 µs | 1.60 µs |
| 1 000 | 2.89 µs | 6.15 µs | 3.27 µs | 0.79 µs |
| 2 000 | 6.11 µs | 14.90 µs | 8.79 µs | 6.84 µs |
| 4 000 | 39.02 µs | 63.40 µs | **24.38 µs** | 3.22 µs |

Ours grows with the resident count; QtMimic's is flat within noise (its column is noisy — it is a
small difference between two large numbers, since boost's emit is itself 2–7x slower here).

Two costs are stacked in that column, and they should be attacked together: the O(N) disconnect of
P7, and one full snapshot rebuild per emit. Fixing P7 with deferred compaction removes both, because
a marked-dead slot does not need the snapshot discarded at all — the emit loop already re-checks each
slot's flag before calling it ([Signal.h:294-303](src/Signal.h#L294-L303)).

**The emit path itself is a clear win and should not be disturbed.** The `emit only` column above is
the same measurement against QtMimic at 2.9 µs vs 20.4 µs (1 000 slots) and 39.0 µs vs 95.1 µs
(4 000). The new `Signal` bought real speed on the hot path; the regression is confined to
connection lifecycle.

## P1 — `~Object()` scans the whole process-wide `callLater` registry *(unchanged)*

**Impact: High. Re-measured 2026-08-13. Code unchanged since the 2026-08-08 finding
([Object.cpp:189-203](src/Object.cpp#L189-L203)).**

Every `Object` destruction takes the process-wide `CallLaterRegistry::sMutex` and walks the whole
pending map looking for its own entries — including for the overwhelming majority of objects that
never called `callLater()`.

| pending `callLater`s | ns per ctor+dtor | vs empty |
|---|---|---|
| 0 | 74.5 | — |
| 500 | 1 301 | 17x |
| 1 000 | 5 067 | 68x |
| 2 000 | 11 299 | 152x |
| 4 000 | 24 139 | **324x** |

This reproduces 2026-08-08 (which measured ~100 ns → 12.5 µs at 2 000) closely enough to confirm
nothing has changed. It remains the item with the worst ratio of cost to fix difficulty: one `bool`
set in `scheduleCallLater()`, tested before the scan, removes it for every object that does not use
the feature. See the 2026-08-08 entry for the two deeper options.

---

## Status of the 2026-08-08 items

| ID | Then | Now |
|----|------|-----|
| P1 | `~Object()` scans the callLater registry | **Open, unchanged** — re-measured above |
| P2 | `Object::thread()` takes a mutex (= R25) | **Open, unchanged** — [ThreadData.hpp:127-131](src/ThreadData.hpp#L127-L131) still locks |
| P3 | Every emit builds a `std::function` on the heap | **Fixed** — see below |
| P4 | One dispatcher mutex serialises every object on a thread | **Open, still unmeasured** |
| P5 | Timer list scanned linearly twice per dispatch pass | **Open, unchanged** — [EventDispatcherDefault.cpp:54-90](src/EventDispatcherDefault.cpp#L54-L90) |
| P6 | Dispatch costs 2–3.5x Qt 6's | **Numbers obsolete** — measured against boost |

**P3 is fixed.** The wrapper is type-erased into a `std::function` once, at connect time, and
`connectImpl()` keeps the slot's concrete type all the way into the wrapper
([Object.h:664-740](src/Object.h#L664-L740)). The direct and same-thread branches now call the slot
in place; only the queued branch builds a closure, and it holds its argument tuple inline rather
than behind a second `make_shared`.

**P6 has to be re-run before it is quoted again.** Every row of that table was measured with
boost::signals2 underneath QtLikeSignal. The emit rows are certainly better now (see P8's
`emit only` column); the `connect()` row is a different code path entirely; the queued row's
three-mutex analysis still holds structurally, because `ThreadData` still owns a *pointer to a
dispatcher* that owns the queue ([ThreadData.hpp:61-77](src/ThreadData.hpp#L61-L77)), where QtMimic
and Qt both put the queue in the thread data directly. Re-run
`src/tests/test_QtLikeSignal_Performance.cpp` and replace the table.

## If a real profile points here, the order is

1. **P7 + P8 together** — one change to the slot list fixes both, and P7 is the only item that grows
   without bound.
2. **P1** — the cheapest fix in the document, and the only one that degrades with unrelated activity
   elsewhere in the process.
3. **P6** — re-measure before deciding anything.
4. **P4**, then **P2/P5**.

**Nothing here has been profiled against a real workload.** These are microbenchmarks with no work
between iterations, which is the condition most favourable to making lock and allocation overhead
look decisive. P7 is the exception worth stating plainly: a quadratic cost does not shrink as a
proportion when the program does more, it only takes longer to become visible.

## Measurement traps

All six recorded on 2026-08-08 still apply and are not repeated here. One to add:

- **Subtract the fan-out.** A first attempt at P8 timed "connect + emit" against a growing resident
  slot count and reported a clean linear curve — which was mostly the emit itself, since an emit to
  N slots is O(N) by definition. The churn cost is only visible as the *difference* between the same
  fan-out with and without the connect/disconnect. Measure the baseline in the same loop shape.
