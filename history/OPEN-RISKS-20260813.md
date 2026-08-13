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
> `linux64-clang`, debug, no sanitizer). No existing test caught R28.
>
> After the R28 fix and its three regression tests: **167 tests, 0 failures**, in declaration order
> and under `--gtest_shuffle`.

> **`src/tests/QtLikeSignal-test-known-defects.cpp` is still empty.** R28 went straight to
> `QtLikeSignal-test-defect-regressions.cpp` because it was fixed in the same pass that found it;
> its three tests were checked against a deliberately broken build rather than left red first. The
> file stays for the convention: a defect found without a fix in hand still belongs there.

| ID  | Risk | Severity | Confirmed by |
|-----|------|----------|--------------|
| R28 | An object destroyed during a dispatch pass still receives the rest of that batch | **High** | Probe — two segfaults — **Fixed 2026-08-13** |
| R29 | `connectImpl()` publishes the `Connection` handle outside `mIncomingMutex` | Low | Inspection |
| R30 | `unregisterEventSource()` does not stop a callback that is already in flight | Low-Med | Inspection |
| R31 | Three dispatcher paths run the wake callback with `mMutex` held; `postEvent()` does not | Low | Inspection |

Still open from earlier passes, restated at the end: R9, R15, R22 (Windows residual), R25.

---

## R28 — an object destroyed during a dispatch pass still receives the rest of that batch *(fixed 2026-08-13)*

**Severity: High. Confirmed by probe. This is the most serious finding in this pass.**

> **Resolution (2026-08-13).** A dispatch pass now publishes the batches it is working through, and
> `removeEventsForReceiver()` cancels the destroyed receiver's entries in them. This is the mechanism
> `unregisterTimer()` has used on the timer batch since R24, generalised to all three batches and
> wired to the other canceller as well.
>
> **Published as a chain, not as one frame.** `mDispatchFrames` links every pass currently running on
> the dispatcher, innermost first ([EventDispatcherDefault.h](src/EventDispatcherDefault.h)). Passes
> nest — a handler running its own `processEvents()` is ordinary in Qt-shaped code — and the outer
> pass is *suspended, not finished*: it still holds a batch it will go on dispatching when the nested
> one returns. A single published frame would let the inner pass hide the outer one and then clear
> the publication on the way out, which is worse than not publishing at all, because it looks safe.
> A frame is unlinked by searching for it rather than by popping the head, so two threads driving one
> dispatcher cannot corrupt the chain.
>
> All three dispatch loops now take each entry out of their batch under the lock, clearing the slot
> as they go, exactly as the timer loop already did. That is what makes cancellation safe rather than
> a new race: whoever clears an entry owns its event, and the other side sees `nullptr` and skips.
> The cost is one uncontended mutex acquire per dispatched event, against the several hundred
> nanoseconds a queued metacall already costs.
>
> `processDeferredDeletes()` got the same treatment. It has the identical shape — collect into a
> local vector, dispatch with the lock released — and destroying one object there can destroy another
> that is also in that batch.
>
> `deletedReceivers` is kept. It is now redundant in the common case but not in all of them:
> `~Object()` cancels through the dispatcher its *current* affinity names, so an object whose
> affinity changed after its events were posted cancels somewhere else and leaves ours behind.
>
> Three regression tests, each pinning a different property, and each verified against a
> deliberately broken build rather than assumed:
>
> | test | what it pins | with that part removed |
> |---|---|---|
> | `EventDispatcherDefaultDefectTest.ObjectDeletedDuringTimerDispatchIsNotThenSentItsOwnTimer` | the timer batch is cancellable | **segfault** (exit 139) |
> | `ObjectDefectTest.DeferredDeleteInTheSameBatchDoesNotDeleteAnAlreadyDeletedObject` | the event batch is cancellable | **segfault** (exit 139) |
> | `EventDispatcherDefaultDefectTest.DeletionInANestedPassCancelsTheOuterPassEntriesToo` | the chain, not just the innermost frame | **segfault** (exit 139) |
>
> The third was written after the first two were already green, because the first version of this fix
> published one frame per dispatcher and would have passed both of them while still crashing under
> nesting. Restricting the cancellation walk to the innermost frame leaves the first two tests
> passing and crashes only the third, which is what makes them three tests rather than one.
>
> Suite: **167 tests, 0 failures**, in declaration order and under `--gtest_shuffle`. Standalone ASan
> runs of both crash scenarios report no error and no leak, which is the check that matters for a fix
> that changes who deletes each event.

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

### The queued-event half — a second crash, and one claim withdrawn

Four queued metacalls to one receiver; the first deletes the receiver. Tracing the dispatch loop:

```
TRACE batch size=4
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040 -> hit 1, delete this
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040
TRACE entry recv=0x6e2307de0040 ...   TRACE calling event() on 0x6e2307de0040
```

`event()` runs three times on the destroyed object.

> **Correction.** The first draft of this entry called that "a use-after-free that happens to do no
> damage, because the freed block still holds a usable vtable". That is wrong, and the reason is
> worth keeping. `Object::event()` is **not virtual** — only `timerEvent()` is
> ([Object.h:582](src/Object.h#L582), [Object.h:89](src/Object.h#L89)) — and its `MetaCall` branch
> reads only the event, never `this` ([Object.cpp:539-541](src/Object.cpp#L539-L541)). So those
> three calls dereference no byte of the freed object at all. They are undefined behaviour (a member
> call on a destroyed object) that no sanitizer can see, which is exactly what the first probe
> found: ASan stayed silent while the timer probe it was compared against reported cleanly. The
> draft asserted a dereference that does not occur, on evidence that showed the opposite.

The queued path does crash, but through a narrower door. Put a `deleteLater()` *after* a queued
emission, so the batch is `[MetaCall, DeferredDelete]` for one object, and let the metacall delete
the object:

```
onCall, deleting self
~SelfDeleter 0x5bf2415f41c0
Segmentation fault (core dumped)
```

`Event::DeferredDelete` runs `delete this` ([Object.cpp:535-537](src/Object.cpp#L535-L537)), so the
second entry deletes an object that the first one already destroyed. That is a double free, and it
is the queued half's real severity. The `deletedReceivers` guard does not cover it: that set records
a receiver only once a `DeferredDeleteEvent` has destroyed it, and here the destruction came from an
ordinary metacall.

So both halves crash. The genuinely silent case — two plain metacalls — is undefined behaviour with
no observable today, and it stops being harmless the moment `event()` becomes virtual or its
`MetaCall` branch touches a member.

### Why this was not caught earlier

The timer batch already has a publication mechanism —
`mDispatchingTimerBatch` ([EventDispatcherDefault.cpp:154](src/EventDispatcherDefault.cpp#L154)) —
added by R24 so that `unregisterTimer()` can cancel entries in a batch that is being dispatched.
The mechanism is right; it is only wired to `unregisterTimer()`. `~Object()` never calls
`unregisterTimer()` for its running timers — it calls `removeEventsForReceiver()` and then hands
the ids back to the pool ([Object.cpp:214-225](src/Object.cpp#L214-L225)) — so object destruction
walks past the very guard that would have covered it.

### Fix

Make `removeEventsForReceiver()` reach the in-flight snapshots, the way `unregisterTimer()` already
reaches one. See the resolution block at the top of this entry for what was done.

Qt does not need any of this because it never snapshots: `QCoreApplication::sendPostedEvents()`
walks `QThreadData::postEventList` in place under the list mutex, and `removePostedEvents()` blanks
entries in that same list. Our snapshot is what buys the lock-free dispatch, so the batches have to
be reachable for cancellation instead.

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

1. ~~**R28**~~ — done 2026-08-13.
2. **R29** — small, and it is a data race, so it will surface under TSan sooner or later.
3. **R31** — a comment or three unlock calls.
4. **R30** — documentation is enough unless a real event source is added.

## Follow-up the R28 fix exposed but did not close

`~Object()` cancels pending work through the dispatcher its **current** affinity names. An object
whose affinity changed after events were posted for it therefore cancels on the new thread and
leaves its events sitting in the old thread's queue. `moveToThread()` migrates active timers
([Object.cpp:399-445](src/Object.cpp#L399-L445)) but not posted events; Qt migrates both. Not
probed, and not part of R28 — recorded here so the next pass has the thread to pull.
