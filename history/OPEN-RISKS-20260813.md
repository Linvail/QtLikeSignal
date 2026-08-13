# Open risks — snapshot 2026-08-13

Review of `src/` after the two changes that the 2026-08-08 documents predate: boost::signals2 was
replaced by our own `Signal`/`Connection` (`40fd910`, `52ef2ff`), and the connect() bodies were
merged into one `connectImpl()` (`7896ee3`).

Numbering continues from `OPEN-RISKS-20260808.md` (R1–R27). Performance items stay in
`PERFORMANCE-20260813.md` as `P<n>`.

Each item says how it was confirmed. **Probe** means a throwaway program was compiled against
`src/` and run, and its output is quoted. **Inspection** means the code was read but no runtime
probe was written.

## Status at a glance

> **Baseline.** `./waf build` succeeds and the suite passes: **164 tests, 0 failures** (2026-08-13,
> `linux64-clang`, debug). So R28 below is *not* caught by any existing test.

> **`src/tests/QtLikeSignal-test-known-defects.cpp` is still empty.** R28 is the first defect since
> 2026-08-08 that belongs there. Prove it there, leave it red, and move it to
> `QtLikeSignal-test-defect-regressions.cpp` once the fix is in — that is what keeps
> `--gtest_filter=-KnownDefect.*` a trustworthy baseline.

| ID  | Risk | Severity | Confirmed by |
|-----|------|----------|--------------|
| R28 | An object destroyed during a dispatch pass still receives the rest of that batch | **High** | Probe — segfault, and ASan use-after-free |
| R29 | `connectImpl()` publishes the `Connection` handle outside `mIncomingMutex` | Low | Inspection |
| R30 | `unregisterEventSource()` does not stop a callback that is already in flight | Low-Med | Inspection |
| R31 | Three dispatcher paths run the wake callback with `mMutex` held; `postEvent()` does not | Low | Inspection |

Still open from earlier passes, restated at the end: R9, R15, R22 (Windows residual), R25.

---

## R28 — an object destroyed during a dispatch pass still receives the rest of that batch

**Severity: High. Confirmed by probe. This is the most serious finding in this pass.**

`EventDispatcherDefault::processEvents()` takes two snapshots before it dispatches anything: it
swaps the whole event queue into a local `eventsToProcess`
([EventDispatcherDefault.cpp:149](src/EventDispatcherDefault.cpp#L149)), and it collects expired
timers into a local `timerEventsToProcess`
([EventDispatcherDefault.cpp:54-76](src/EventDispatcherDefault.cpp#L54-L76)). It then walks both
lists with `mMutex` released, calling `ep.mReceiver->event( ep.mEvent )` on each entry.

`~Object()` calls `removeEventsForReceiver()`
([Object.cpp:205-212](src/Object.cpp#L205-L212)), and that function strips the object's entries
from `mEventQueue` and `mTimers` only
([EventDispatcherDefault.cpp:457-490](src/EventDispatcherDefault.cpp#L457-L490)). **Neither
snapshot is reachable from it.** So an object destroyed by any handler in a pass keeps its
remaining entries in that pass, and each one is a call through a freed pointer.

The one guard that exists, `deletedReceivers`
([EventDispatcherDefault.cpp:183](src/EventDispatcherDefault.cpp#L183)), covers only receivers
destroyed *through a `DeferredDeleteEvent` in this same batch*. A plain `delete` from inside a slot
or a timer handler is not covered, and neither is a `deleteLater()` that was already dispatched.

### The timer half — a hard crash

Two objects with timers of the same interval, so both `TimerEvent`s land in one batch. A's handler
deletes B:

```
timerEvent on A (0x605f77e29110) dead=0
deleting the sibling
~Ticker B (0x605f77e29a60)
Segmentation fault (core dumped)
```

Under AddressSanitizer, with the receiver named:

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x79650ebe0180
READ of size 8 at 0x79650ebe0180 thread T0
    #0 QtLikeSignal::Object::event(QtLikeSignal::Event*)  src/Object.cpp:532
    #1 QtLikeSignal::EventDispatcherDefault::processEvents()  src/EventDispatcherDefault.cpp:234
    #2 QtLikeSignal::Thread::processEvents()  src/Thread.cpp:448
freed by thread T0 here:
    #1 Ticker::~Ticker()
```

`src/EventDispatcherDefault.cpp:234` is the `ep.mReceiver->event( ep.mEvent )` of the timer loop.
This is a segfault in a plain build, not a latent hazard.

### The queued-event half — the same hole, quieter

Four queued metacalls to one receiver; the first deletes the receiver. Tracing the dispatch loop:

```
TRACE batch size=4
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040 -> hit 1, delete this
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040
```

`event()` runs three times on the destroyed object. It happens to do no visible damage here,
because `Object::event()` reads a vtable and an event type that the freed block still holds, and
the metacall closure's own `weakLife.expired()` check then makes the call a no-op. That is luck,
not a guard: the dereference of the freed receiver happens *before* the life token is ever
consulted. This half is the more dangerous one precisely because it does not crash.

### Why this was not caught earlier

The timer batch already has a publication mechanism —
`mDispatchingTimerBatch` ([EventDispatcherDefault.cpp:154](src/EventDispatcherDefault.cpp#L154)) —
added by R24 so that `unregisterTimer()` can cancel entries in a batch that is being dispatched.
The mechanism is right; it is only wired to `unregisterTimer()`. `~Object()` never calls
`unregisterTimer()` for its running timers — it calls `removeEventsForReceiver()` and then hands
the ids back to the pool ([Object.cpp:214-225](src/Object.cpp#L214-L225)) — so object destruction
walks past the very guard that would have covered it.

### Fix

Make `removeEventsForReceiver()` reach both in-flight snapshots, the way `unregisterTimer()`
already reaches one:

1. Publish the event batch under `mMutex` as well (a second pointer beside
   `mDispatchingTimerBatch`, retracted by the same `BatchRetractor`).
2. In `removeEventsForReceiver()`, null the `mEvent` of every entry in both published batches whose
   `mReceiver` matches, and delete those events. Both dispatch loops already skip a null `mEvent`,
   so nothing else has to change in them.
3. Keep `deletedReceivers`; it stays the cheaper guard for the `DeferredDelete` case.

Qt does not need any of this because it never snapshots: `QCoreApplication::sendPostedEvents()`
walks `QThreadData::postEventList` in place under the list mutex, and
`removePostedEvents()` blanks entries in that same list. Our snapshot is what buys the lock-free
dispatch, so the batches have to be reachable for cancellation instead.

**Recommended test** (`KnownDefect.*` first): the timer version is deterministic and crashes
without a sanitizer, so it is the better regression test of the two. Add the queued version beside
it, asserted through a counter rather than a crash.

## R29 — `connectImpl()` publishes the `Connection` handle outside `mIncomingMutex`

**Severity: Low. Inspection. Introduced with the connect() unification (`7896ee3`).**

[Object.h:742-757](src/Object.h#L742-L757) runs in this order:

```cpp
Connection handle = aSignal.connect( wrapper );   // the slot is live from here on
...
cleanup->mHandle = handle;                        // unsynchronised write
std::lock_guard<std::mutex> lock( receiver->mIncomingMutex );
receiver->mIncoming.push_back( handle );
```

The slot — and the `Cleanup` token it owns — is reachable the instant `connect()` returns. If
another thread ends that connection in the window before the push (`Signal::disconnectAll()`, or
the sender `Signal` being destroyed), `~Cleanup` ([Object.cpp:234-247](src/Object.cpp#L234-L247))
*reads* `mHandle` under `mIncomingMutex` while this thread *writes* it without holding anything.
That is a data race on two `shared_ptr`/`weak_ptr` members.

Two consequences, both minor:
- The race itself, which ThreadSanitizer would report if a test exercised it. Nothing currently does.
- `~Cleanup` finds nothing to erase (the push has not happened yet), and this function then pushes a
  handle for an already-dead connection. `mIncoming` keeps a stale entry until the receiver dies.

The comment above the assignment says publishing the handle first is what lets the destructor "match
on" it. That is true for ordering *within* this thread, but it does not make the write visible
safely to a concurrent reader.

**Fix:** move `cleanup->mHandle = handle;` inside the `mIncomingMutex` scope, next to the push, and
have `~Cleanup` treat a default-constructed `mHandle` as "not registered yet, nothing to prune".

## R30 — `unregisterEventSource()` does not stop a callback that is already in flight

**Severity: Low-Medium. Inspection.**

`EventDispatcherLinux::waitForEvents()` snapshots the descriptor set *and copies the callbacks out*
under `mMutex` ([EventDispatcherLinux.cpp:129-143](src/EventDispatcherLinux.cpp#L129-L143)), then
unlocks and invokes them ([EventDispatcherLinux.cpp:159-167](src/EventDispatcherLinux.cpp#L159-L167)).
`unregisterEventSource()` ([EventDispatcherLinux.cpp:86-112](src/EventDispatcherLinux.cpp#L86-L112))
removes the source and wakes the loop, then returns `true`.

So a callback copied into the snapshot still runs after `unregisterEventSource()` has returned. Its
doxygen invites exactly the use that breaks: it is marked thread-safe, and the comment says the
caller "is entitled to close it once we return". A caller that unregisters and then destroys what
the callback captured has a use-after-free; a caller that unregisters and closes the descriptor gets
one final callback carrying `POLLNVAL`.

Qt has the same shape but not the same exposure — `QSocketNotifier` is thread-confined to its own
loop, so unregistration cannot overlap a dispatch. Ours is documented as callable from anywhere.

**Fix, cheapest first:** state in the doxygen that unregistration is only synchronous when called
from the dispatcher's own thread, and that a callback may still run once otherwise. If a real
guarantee is wanted, give each source a generation counter checked under `mMutex` immediately before
the callback is invoked.

## R31 — three dispatcher paths run the wake callback with `mMutex` held

**Severity: Low. Inspection. A consistency defect rather than a demonstrated failure.**

`wakeWaiter()` may invoke `mWakeCallback`, which is user code
([EventDispatcherDefault.cpp:270-295](src/EventDispatcherDefault.cpp#L270-L295)). Its own comment
says the callback "must not be run while holding a lock we might not even own". Four callers, and
they do not agree:

| caller | `mMutex` when `wakeWaiter()` runs |
|---|---|
| `postEvent()` | released first ([EventDispatcherDefault.cpp:431-445](src/EventDispatcherDefault.cpp#L431-L445)) |
| `wakeUp()`, `interrupt()` | released first |
| `registerTimer()` | **held** ([EventDispatcherDefault.cpp:330-350](src/EventDispatcherDefault.cpp#L330-L350)) |
| `unregisterTimer()` | **held** ([EventDispatcherDefault.cpp:360-412](src/EventDispatcherDefault.cpp#L360-L412)) |
| `takeTimersForReceiver()` | **held** ([EventDispatcherDefault.cpp:506-524](src/EventDispatcherDefault.cpp#L506-L524)) |

`Thread::setWakeCallback()` documents the strict contract — "called with the dispatcher's internals
locked, so it must not block or re-enter the dispatcher"
([Thread.cpp:458-459](src/Thread.cpp#L458-L459)) — so a conforming callback is safe today. The
problem is that `postEvent()`, the path a callback author will actually test against, is the
permissive one. A callback that posts back into the same dispatcher works in every test and
self-deadlocks the first time a timer is started, because the mutex is not recursive.

**Fix:** pick one. Releasing the lock before `wakeWaiter()` in the three timer paths matches
`postEvent()` and lets the documented contract be relaxed; keeping them and documenting the strict
contract on `AbstractEventDispatcher` as well is the smaller change.

---

## Still open from earlier passes

Re-checked against the current tree, not re-probed.

- **R9 — `CoreApplication`'s singleton is unguarded.** Now a warning rather than silence
  ([CoreApplication.cpp:48-59](src/CoreApplication.cpp#L48-L59)): a second instance is refused
  registration and says so on stderr. Qt asserts. Accepted as-is; recorded so it is not re-filed.
- **R15 — "Thread-safe" doc claims not fully audited against Qt.** Unchanged. The new `Signal` and
  `Connection` headers add a fresh set of such claims that no audit has covered.
- **R22 residual — Windows OS-message dispatch is still untested.** Nothing in the suite creates a
  real window or posts a real `WM_` message, so `TranslateMessage`/`DispatchMessage` and the
  `WM_QUIT` branch of `EventDispatcherWin32` have still never executed. Mission stage 5's "I want to
  receive OS/platform's messages" is proven on Linux only.
- **R25 — `Object::thread()` costs a mutex.** Unchanged; `Affinity::data()` still locks
  ([ThreadData.hpp:127-131](src/ThreadData.hpp#L127-L131)). Numbers live in `PERFORMANCE-20260808.md`
  (P2), and the reasoning there still holds: the mutex buys a lifetime guarantee Qt does not offer.

## Suggested order

1. **R28** — the only item here that crashes.
2. **R29** — small, and it is a data race, so it will surface under TSan sooner or later.
3. **R31** — a comment or three unlock calls.
4. **R30** — documentation is enough unless a real event source is added.
