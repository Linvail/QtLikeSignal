# Open risks — snapshot 2026-08-13

Review of `src/` after the two changes that the 2026-08-08 documents predate: boost::signals2 was
replaced by our own `Signal`/`Connection` (`40fd910`, `52ef2ff`), and the connect() bodies were
merged into one `connectImpl()` (`7896ee3`). Numbering continues from `OPEN-RISKS-20260808.md`
(R1–R27). Performance items live in `PERFORMANCE-20260813.md` as `P<n>`.

How each item was confirmed, and the test baseline, are at the end under
[How these were confirmed](#how-these-were-confirmed).

## Status

| ID | Status | Risk | Severity | Evidence and outcome |
|----|--------|------|----------|----------------------|
| R9 | **Fixed** | `CoreApplication`'s singleton is unguarded | Low → **Med** | The filed half is accepted; the half nobody filed was a data race on `sInstance`, now atomic |
| R15 | **Fixed** | "Thread-safe" doc claims not audited against Qt | Medium | Two false claims corrected, and the term is now defined once in `Global.h` |
| R22 | **Fixed** | Windows OS-message dispatch is untested | Medium → **Low** | Closed on Windows, 2026-08-16. Six regression tests; the risk was a third the size it was filed at, and one comment was wrong |
| R25 | **By Design** | `Object::thread()` costs a mutex (= P2) | Low | Accepted — the mutex buys a lifetime guarantee Qt does not offer |
| R28 | **Fixed** | An object destroyed during a dispatch pass still receives the rest of that batch | **High** | Probe — two segfaults, one of them a double free. Three regression tests |
| R29 | **Withdrawn** | ~~`connectImpl()` publishes the `Connection` handle outside `mIncomingMutex`~~ | — | **Not a defect.** TSan probe, 200 000 racing connects against the pre-"fix" code: silent |
| R30 | **Fixed** | `unregisterEventSource()` does not stop a callback already in flight | Low-Med | Inspection. Unregistration is now synchronous on the loop's own thread |
| R31 | **Withdrawn** | Three dispatcher paths run the wake callback with `mMutex` held | — | **Not a defect** — the constraint was documented. Contract widened anyway, to remove a trap |
| R32 | **Fixed** | `moveToThread()` does not migrate already-posted events | **Medium** | Probe — a queued slot ran on the thread the object *left*. Three regression tests |

**Statuses.** *Fixed* — code changed and the defect is gone. *By Design* — the current state is
accepted and is not considered a bug. *Withdrawn* — investigated, and there was no defect to fix.
*Queue* — not started. Nothing is In progress.

Everything filed in this pass is closed, and so are the two carried items that could be closed here.
**No confirmed defect is open.** One item remains: **R25**, a deliberate trade. R22 was closed on
2026-08-16, on Windows; see its entry for what that turned out to involve.

---

# Details

## R9 — `CoreApplication`'s singleton is unguarded *(Fixed)*

Two halves, and the one that was filed was the less important one.

**Filed: a second application object is not rejected.** No change, deliberately. It is a warning
rather than an abort ([CoreApplication.cpp](src/CoreApplication.cpp#L48-L59)); the first instance
stays the one `instance()` reports, so the damage is contained and visible. Qt asserts. A diagnostic
is more useful than killing the process, and constructing two is misuse either way.

**Not filed, and the real problem: `sInstance` was a plain pointer.** `instance()`, `exit()`,
`quit()` and `post()` are all callable from any thread and all read it, while the constructor and
destructor write it from the main thread. Every one of those was a data race, under a "Thread-safe"
that promised otherwise. Now `std::atomic<CoreApplication*>`, with a compare-exchange at both writes
so "the first one wins" holds even under the misuse the warning exists to report.

Qt 6 has the identical plain pointer and knows it: `QCoreApplication` keeps a second, atomic
`g_self` for internal use and documents `instanceExists()` as "a Qt 6 thread-safe (no data races)
version of `instance() != nullptr`". Qt 7 makes the pointer itself atomic behind a `#warning`.

What is left is documented, not fixed: a caller may load the pointer and then dereference it while
the main thread runs `~CoreApplication()`. All four now carry the caveat in Qt's own words — Qt has
the same hole on `quit()` and states it the same way. That makes it the caller's, which under this
project's rule ends it.

## R15 — "Thread-safe" doc claims not audited against Qt *(Fixed)*

Sized properly, this was never 76 claims. It was **two false ones and a definition**, and both false
ones are corrected.

- **`Object::objectName()` / `setObjectName()`** — said "Thread-safe"; they read and write a plain
  `std::string` with no lock. The member's own comment in [Object.h](src/Object.h#L792-L799) said the
  opposite ("deliberately unguarded … naming it from another is the same **misuse**"), so the header
  blamed the caller and the implementation invited them in. Now "Not thread-safe: must be called from
  this object's own thread", on the declarations as well. Comment change only. Qt agrees: its
  `objectName()` has no locking, and its cross-thread branch is commented `// Unsafe code path`.
- **The four `CoreApplication` statics** — see R9 above.

**The definition is now written**, at the top of [Global.h](src/Global.h): "Thread-safe" means
callable concurrently without a data race, and nothing more — not that the answer is still true when
it reaches you, and not that two such calls are atomic. "Not thread-safe" always names the thread
that may call. The four claims that promise less than a reader assumes — `Signal::empty()`,
`Signal::receivers()`, `Thread::isRunning()`, `Thread::isFinished()` — now say so at the function as
well. Qt does exactly this: `\threadsafe` on `QThread::isRunning()` plus a separate note that the
thread may still be running afterwards.

One process lesson worth keeping: both false claims lived in a `.cpp` while the truth lived in the
`.h`. They were never read side by side. **A thread-safety claim belongs on the declaration.**

## R22 — Windows OS-message dispatch is untested *(Fixed)*

**Closed 2026-08-16, on Windows 11 with MSVC 2022.** The blocker was never the code; it was that the
machine this was written on could only reach Windows through WSL. Run natively, the whole item
changes shape.

### It was a third the size it was filed at

Filed as "Windows OS-message dispatch is untested". That was too broad. `CoreApplication` and
`Thread` construct `EventDispatcherWin32` unconditionally on Windows, so the first native run of the
existing suite — **178 tests, 177 passed, 1 skipped, 0 failures**, under AddressSanitizer — had
already executed the constructor, the destructor, the `MsgWaitForMultipleObjectsEx` wait, the
`wakeWaiter()` post, the `mWakePending` collapse and the `PeekMessage` drain, several hundred times
over. None of that was untested. It was unobserved, which is not the same thing.

What had genuinely never executed was the part of `processPlatformEvents()` that handles a message
the dispatcher did not send itself:

- `TranslateMessage` / `DispatchMessageA`
- the `WM_QUIT` branch
- the three `mMessageWindow == nullptr` fallbacks and the failed-`PostMessage` rollback

The first two are now covered. The third is not, and is recorded below rather than quietly counted.

### Three things stood between the repository and a green Windows run

None of them was R22, and all three had to go first, because "the suite passes on Windows" is the
precondition for everything else here.

1. **`asm volatile` in `test_QtLikeSignal_Regression.cpp`.** GCC syntax, which MSVC will not parse,
   and the one thing in the tree that stopped a Windows build. It is an optimiser barrier around a
   construct/destroy benchmark; `PerfHarness::keep()` already existed with an `_MSC_VER` arm doing
   exactly that job, so the line now goes through it instead of holding a second, less portable copy
   of the same idea.
2. **`QtMimic-test` never linked `user32`.** `src/tests/wscript` had always linked it;
   `external/QtMimic/wscript` had not, so the one Windows target that was not part of the original
   library was also the one that could not link. Nine unresolved externals, all of them the Win32
   calls in `EventDispatcherWin32.cpp`.
3. **Every Windows binary died at startup with `0xC0000135`.** ASan is on by default on Windows
   here, and Windows resolves a DLL from the executable's own directory before it consults `PATH`,
   which nothing puts the MSVC bin directory on. The suite looked like it crashed when in fact it
   could not start. `add_AddressSanitizer_on_Windows()` now copies `clang_rt.asan_dynamic-*.dll`
   next to every program it builds, so an ASan build runs as built.

A fourth was observed once and did not survive scrutiny: a parallel build failed with
`C1041: cannot open program database vc140.pdb` on two files. `/FS` is already set for both modes in
`tools/toolchain-windows.py`, and the failure has not reproduced across two full clean parallel
rebuilds since. **Not fixed, because there is nothing confirmed to fix** — inventing a second PDB
mechanism for a failure that reproduces zero times out of two is the mistake R29 is in this document
to remember. Recorded here so that if it returns, it returns to a note rather than to a surprise.

### The defect it found: a comment that described the wrong pass

Probed, not inferred. `processPlatformEvents()` is called from
[EventDispatcherDefault.cpp:187](src/QtLikeSignal/EventDispatcherDefault.cpp#L187) — *after* both of
`processEvents()`'s `mInterrupt.exchange( false )` checks ([:41](src/QtLikeSignal/EventDispatcherDefault.cpp#L41),
[:139](src/QtLikeSignal/EventDispatcherDefault.cpp#L139)) and after the event batch has been taken.
So the `interrupt()` raised by the `WM_QUIT` branch cannot affect the pass that saw the message. The
comment on that branch said it could: *"interrupt() ends the current pass"*.

A probe, posting a task and then `PostQuitMessage(0)`:

```
posted task A, then WM_QUIT
    [task A ran]
pass 0 -> true
posted task B
pass 1 -> false
    [task B ran]
pass 2 -> true
```

The pass that saw `WM_QUIT` ran its whole batch and reported success. The pass *after* it returned
false and ran nothing — including task B, which had been queued in between and which waited one
further pass. And since `Thread::exec()` loops on its own `mExiting` flag without ever reading what
`processEvents()` returned, the entire effect of `WM_QUIT` on a running loop is **one idle pass**.

The comment is corrected to say that, and both halves are now pinned by tests. The behaviour itself
is left alone: it is a deliberate divergence from Qt, which quits the application here
(`qeventdispatcher_win.cpp` calls `QCoreApplication::instance()->quit()` and returns false —
verified against the local Qt 5.15.19 source, not from memory). Whether a worker's queue seeing
`WM_QUIT` *should* end the process is a design question, not a defect, and it is not this item's to
answer.

### One hypothesis probed and withdrawn

`DestroyWindow()` fails when called from a thread other than the window's creator, and
`~EventDispatcherWin32()` ignores its return value. Since the dispatcher is held through a
`shared_ptr` on `ThreadData`, a last reference dropped on the joining thread looked like a silent
window leak.

It is not one. Counting live message-only windows of the dispatcher's class from outside the
library, while deliberately holding the worker's dispatcher alive past the worker's exit:

```
worker running:                             2 window(s)
worker joined, dispatcher still referenced:  1 window(s)
still holding the dispatcher:                1 window(s)
dispatcher released on the main thread:      1 window(s)
```

The window is already gone at the join, while the dispatcher object is still alive. Windows destroys
a thread's windows when the thread exits, so the OS is the backstop and the late `DestroyWindow()`
is a harmless call on a stale handle. **No defect** — filed here so the same reading is not filed
again.

### Tests

Six, in `src/tests/QtLikeSignal-test-eventdispatcher-win32.cpp`, whole file inside
`#if defined( _WIN32 )` so the cross-build stays clean. Each was verified against a deliberately
broken build, and **each is killed by a different mutation** — which is what makes them six tests
rather than one:

| test | what it pins | mutation that fails it |
|---|---|---|
| `PostedWindowMessageIsDispatchedToItsWindowProc` | `DispatchMessageA` — stage 5 on Windows | remove `DispatchMessageA` |
| `KeyDownIsTranslatedIntoACharacterMessage` | `TranslateMessage`, separately from dispatch | remove `TranslateMessage` |
| `WmQuitEndsTheFollowingPassAndNotTheProcess` | *which* pass `WM_QUIT` stops | remove `interrupt()` |
| `WmQuitDoesNotStopAnExecLoop` | the deliberate divergence from Qt | make the branch call `CoreApplication::quit()` |
| `AWorkerThreadDispatchesTheMessagesOfItsOwnWindow` | a worker services its own message queue | give workers `EventDispatcherDefault` |
| `IdleLoopWithAWindowDoesNotSpin` | stage 5's "no 100% cpu-spin" | pass `0` as the wait timeout |

The first two matter as a pair. `TranslateMessage` and `DispatchMessage` sit on consecutive lines,
and removing either leaves the other looking correct; the mutation runs confirm the split, with
exactly one test failing each time.

`IdleLoopWithAWindowDoesNotSpin` is not a copy of its Linux sibling. `MsgWaitForMultipleObjectsEx`
is asked for `QS_ALLINPUT` with `MWMO_INPUTAVAILABLE`, which returns for input *already* in the
queue — so any message the drain fails to remove makes the wait return immediately, forever, at full
CPU. Under the mutation it burned 0.89 s of CPU over 0.92 s of wall time, a ratio of 0.96 against a
threshold of 0.1.

**One test had to be rewritten because a mutation exposed a flaw in the test rather than in the
code.** With `interrupt()` removed, `WmQuitEndsTheFollowingPassAndNotTheProcess` did not fail — it
*hung*, because a pass that wrongly returned true had already drained the queue, leaving the next
`processEvents()` to block in `MsgWaitForMultipleObjectsEx` with no deadline and nothing scheduled.
The two checks that guard that call are now `ASSERT` rather than `EXPECT`, so a broken build stops
there and reports which expectation broke, in 2 ms. This is the R31 lesson applied to a test that
was written after it: **a test that hangs reports nothing.**

Suite on Windows: **184 tests, 183 passed, 1 skipped, 0 failures**, in declaration order and under
`--gtest_shuffle`, with AddressSanitizer on. Up from 178 before this pass.

### What it did not close

- **The `mMessageWindow == nullptr` fallbacks remain uncovered.** Reaching them means making
  `CreateWindowEx` fail, which needs a seam in the class — a test-only constructor flag or a virtual
  window-creation hook. That is production code reshaped for testability, and it is a decision
  rather than an oversight; three lines did not seem worth it. Recorded as a known-uncovered branch.
- **`external/QtMimic` has the same dispatcher and no such tests.** Its copy is byte-identical apart
  from namespace, include path and licence header. Its build was fixed here — it now links — but the
  tests were not ported, because R22 as filed names `src/`.
- **Only the `win64-msvc` debug variant was run.** Release and `win32-msvc` are untried.

## R25 — `Object::thread()` costs a mutex *(By Design)*

`Affinity::data()` still locks ([ThreadData.hpp:127-131](src/ThreadData.hpp#L127-L131)). This is the
same item as P2 in `PERFORMANCE-20260813.md`, where the numbers and the full reasoning live.

Accepted: the mutex buys a strong reference guaranteed alive for the duration of the call, which
Qt's raw `QAtomicPointer` does not offer, and removing it needs retired-pointer retention. A
`DirectConnection` no longer reads the affinity box at all, so what remains is a constant factor on
the auto and queued paths.

## R28 — an object destroyed during a dispatch pass still receives the rest of that batch *(Fixed)*

**Severity: High. Confirmed by probe. The most serious finding in this pass.**

`EventDispatcherDefault::processEvents()` takes two snapshots before it dispatches anything: it
swaps the whole event queue into a local `eventsToProcess`, and it collects expired timers into a
local `timerEventsToProcess`. It then walks both lists with `mMutex` released, calling
`ep.mReceiver->event( ep.mEvent )` on each entry.

`~Object()` calls `removeEventsForReceiver()`, and that function stripped the object's entries from
`mEventQueue` and `mTimers` only. **Neither snapshot was reachable from it.** So an object destroyed
by any handler in a pass kept its remaining entries in that pass, and each one was a call through a
freed pointer.

The one guard that existed, `deletedReceivers`, covers only receivers destroyed *through a
`DeferredDeleteEvent` in this same batch*. A plain `delete` from inside a slot or a timer handler is
not covered, and neither is a `deleteLater()` that was already dispatched.

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

That line is the `ep.mReceiver->event( ep.mEvent )` of the timer loop. A segfault in a plain build,
not a latent hazard.

### The queued half — a second crash, and one claim withdrawn

Four queued metacalls to one receiver; the first deletes the receiver. `event()` runs three times on
the destroyed object.

> **Correction.** The first draft of this entry called that "a use-after-free that happens to do no
> damage, because the freed block still holds a usable vtable". That is wrong, and the reason is
> worth keeping. `Object::event()` is **not virtual** — only `timerEvent()` is — and its `MetaCall`
> branch reads only the event, never `this`. So those three calls dereference no byte of the freed
> object at all. They are undefined behaviour that no sanitizer can see, which is exactly what the
> probe found: ASan stayed silent while the timer probe it was compared against reported cleanly.
> The draft asserted a dereference that does not occur, on evidence that showed the opposite.

The queued path does crash, but through a narrower door. Put a `deleteLater()` *after* a queued
emission, so the batch is `[MetaCall, DeferredDelete]` for one object, and let the metacall delete
the object:

```
onCall, deleting self
~SelfDeleter 0x5bf2415f41c0
Segmentation fault (core dumped)
```

`Event::DeferredDelete` runs `delete this`, so the second entry deletes an object the first one
already destroyed. That double free is the queued half's real severity, and `deletedReceivers` does
not cover it: that set records a receiver only once a `DeferredDeleteEvent` has destroyed it, and
here the destruction came from an ordinary metacall.

So both halves crash. The genuinely silent case — two plain metacalls — is undefined behaviour with
no observable today, and it stops being harmless the moment `event()` becomes virtual or its
`MetaCall` branch touches a member.

### Why it was not caught earlier

The timer batch already had a publication mechanism, `mDispatchingTimerBatch`, added by R24 so that
`unregisterTimer()` could cancel entries in a batch being dispatched. The mechanism was right; it was
only wired to `unregisterTimer()`. `~Object()` never calls `unregisterTimer()` for its running timers
— it calls `removeEventsForReceiver()` and then hands the ids back to the pool — so object
destruction walked past the very guard that would have covered it.

### The fix

A dispatch pass now publishes the batches it is working through, and `removeEventsForReceiver()`
cancels the destroyed receiver's entries in them — the R24 mechanism generalised to all three
batches and wired to both cancellers.

**Published as a chain, not as one frame.** `mDispatchFrames` links every pass currently running on
the dispatcher, innermost first ([EventDispatcherDefault.h](src/EventDispatcherDefault.h)). Passes
nest — a handler running its own `processEvents()` is ordinary in Qt-shaped code — and the outer pass
is *suspended, not finished*: it still holds a batch it will go on dispatching when the nested one
returns. A single published frame would let the inner pass hide the outer one and then clear the
publication on the way out, which is worse than not publishing at all, because it looks safe. A frame
is unlinked by searching for it rather than by popping the head, so two threads driving one dispatcher
cannot corrupt the chain.

All three dispatch loops now take each entry out of their batch under the lock, clearing the slot as
they go, exactly as the timer loop already did. That is what makes cancellation safe rather than a
new race: whoever clears an entry owns its event, and the other side sees `nullptr` and skips. The
cost is one uncontended mutex acquire per dispatched event — measured at +3.3 ns, recorded as P9.

`processDeferredDeletes()` got the same treatment: identical shape, and destroying one object there
can destroy another that is also in that batch.

`deletedReceivers` is kept. It is now redundant in the common case but not in all of them: `~Object()`
cancels through the dispatcher its *current* affinity names, so an object whose affinity changed after
its events were posted cancels somewhere else and leaves ours behind. That gap is R32.

Qt needs none of this because it never snapshots: `QCoreApplication::sendPostedEvents()` walks
`QThreadData::postEventList` in place under the list mutex, and `removePostedEvents()` blanks entries
in that same list. Our snapshot is what buys the lock-free dispatch, so the batches have to be
reachable for cancellation instead.

### Tests

Three, each pinning a different property, each verified against a deliberately broken build:

| test | what it pins | with that part removed |
|---|---|---|
| `EventDispatcherDefaultDefectTest.ObjectDeletedDuringTimerDispatchIsNotThenSentItsOwnTimer` | the timer batch is cancellable | **segfault** (exit 139) |
| `ObjectDefectTest.DeferredDeleteInTheSameBatchDoesNotDeleteAnAlreadyDeletedObject` | the event batch is cancellable | **segfault** (exit 139) |
| `EventDispatcherDefaultDefectTest.DeletionInANestedPassCancelsTheOuterPassEntriesToo` | the chain, not just the innermost frame | **segfault** (exit 139) |

The third was written after the first two were green, because the first version of this fix published
one frame per dispatcher and would have passed both while still crashing under nesting. Restricting
the cancellation walk to the innermost frame leaves the first two passing and crashes only the third,
which is what makes them three tests rather than one.

Standalone ASan runs of both crash scenarios report no error and no leak — the check that matters for
a fix that changes who deletes each event.

## R29 — ~~`connectImpl()` publishes the `Connection` handle outside `mIncomingMutex`~~ *(Withdrawn)*

**This entry was wrong. It is kept, rather than deleted, so the same reading is not filed again.**

The claim was that `connectImpl()` races `~Cleanup()`:

```cpp
Connection handle = aSignal.connect( wrapper );   // the slot is live from here on
...
cleanup->mHandle = handle;                        // "unsynchronised write"
std::lock_guard<std::mutex> lock( receiver->mIncomingMutex );
receiver->mIncoming.push_back( handle );
```

The slot really is reachable — and disconnectable — the instant `connect()` returns, and `~Cleanup()`
really does read `mHandle`. What the entry missed is **who holds the last reference**. `cleanup` is a
local `shared_ptr` in `connectImpl()`, and the wrapper captures a *copy*. So a concurrent disconnect
drops the slot's reference, not ours, and the refcount cannot reach zero until this function returns.
`~Cleanup()` therefore cannot start until after both the write and the push have completed, and it
runs on this thread, not the disconnecting one. There is no concurrent reader to race, and no window
in which the push is missing.

Confirmed rather than argued: a ThreadSanitizer probe raced 200 000 `Object::connect()` calls against
two threads spinning on `Signal::disconnectAll()`, **against the pre-"fix" code**, three times. Zero
reports each run. The same probe catches a deliberate two-thread race on a plain `int`, so TSan was
working.

The change made in response to this entry has been reverted. What survives is a comment at the
assignment stating the invariant, because the code does look racy at a glance — the original comment
there ("publish the handle before registering it, so the destructor has something to match on")
described an ordering that does not matter and did not mention the one that does.

**Superseded 2026-08-15.** P10 stage 1 removed the `Cleanup` token, so none of the code quoted above
still exists. The same hazard does, in a different shape: `connectImpl()` still returns from
`connect()` before it registers the handle, so a concurrent `disconnectAll()` can prune a receiver
entry that has not been pushed yet. It is closed explicitly now rather than by a refcount argument —
`ConnectionNode::pruneReceiver()` sets `mPruned` under the receiver's `mIncomingMutex`, and
`registerWithReceiver()` takes the same mutex and does not push when it finds the flag set. The two
orders both give a receiver with no entry for a connection that has ended.

## R30 — `unregisterEventSource()` does not stop a callback already in flight *(Fixed)*

**Severity: Low-Medium. Inspection.**

`EventDispatcherLinux::waitForEvents()` snapshotted the descriptor set *and copied the callbacks out*
under `mMutex`, then unlocked and invoked them. `unregisterEventSource()` removed the source, woke the
loop, and returned `true`.

So a callback copied into the snapshot still ran after `unregisterEventSource()` had returned. Its
doxygen invited exactly the use that breaks: marked thread-safe, and saying the caller "is entitled to
close it once we return". A caller that unregisters and then destroys what the callback captured has a
use-after-free; one that unregisters and closes the descriptor gets a final callback carrying
`POLLNVAL`.

Qt has the same shape but not the same exposure — `QSocketNotifier` is thread-confined to its own
loop, so unregistration cannot overlap a dispatch. Ours was documented as callable from anywhere.

**The fix.** `waitForEvents()` no longer copies the callbacks out. It snapshots only each source's
*identity* — descriptor plus a generation stamp — and looks the callback up again under `mMutex`
immediately before invoking it. Same rule as R28's dispatch batches: re-check under the lock, act
outside it.

The generation stamp is what makes the descriptor an identity. A descriptor number is reused after
close, so an fd alone cannot tell "still registered" from "unregistered, closed, and reopened by
something else". Every `registerEventSource()` takes a fresh generation, replacements included.

This makes unregistration **synchronous when called from the dispatcher's own thread**, including from
inside another callback, which is the case a caller can actually rely on. From another thread it still
is not, and cannot be without blocking on arbitrary user code; the doxygen now says so plainly instead
of implying the opposite.

Covered by `EventDispatcherLinuxTest.SourceUnregisteredFromACallbackIsNotCalledInThatRound`: two
descriptors ready in one `poll()` round, the first callback unregistering the second. Verified to
catch the regression — restoring the up-front callback copy makes it fail with the sibling's call
count at 1.

## R31 — three dispatcher paths run the wake callback with `mMutex` held *(Withdrawn)*

**Not a defect. Recorded as one; that was the wrong label.**

The constraint was documented: `Thread::setWakeCallback()` said the callback "must not block or
re-enter the dispatcher". A callback that re-entered was therefore misuse, and the deadlock that
followed was the caller's, not ours. This project does not treat the consequences of ignoring a stated
precondition as defects, and this entry should not have been an exception. An earlier draft called it
"a reachable deadlock", which asserts the opposite.

What *was* wrong with it is why the change was still made: the constraint was **inconsistent and
untestable**. `wakeWaiter()` may invoke the callback, and the callers did not agree:

| caller | `mMutex` when `wakeWaiter()` ran |
|---|---|
| `postEvent()`, `wakeUp()`, `interrupt()` | released first |
| `registerTimer()`, `unregisterTimer()`, `takeTimersForReceiver()` | **held** |

So a violating callback passed every test anyone would write — `postEvent()` is the path you reach
for — and deadlocked the first time a timer happened to wake the loop. A precondition that only bites
on a path you did not exercise is a trap, not a contract.

**The change.** The three timer paths now scope their lock and call `wakeWaiter()` after releasing it,
matching `postEvent()`. Only `unregisterTimer()` needed restructuring, and its body moved into a
`takeTimerLocked()` helper so the early returns stay readable. The permissive contract is now the
documented one in all three places that stated the strict one:
`AbstractEventDispatcher::setWakeCallback()`, `Thread::setWakeCallback()` and
`EventDispatcherDefault::wakeWaiter()`. A wake callback may call back into the dispatcher; it should
still not block, because it runs on the poster's thread on the critical path of every post.

`EventDispatcherDefaultDefectTest.WakeCallbackMayReEnterTheDispatcherFromEveryPath` pins the new
contract rather than proving an old defect. With the wake put back inside `registerTimer()`'s lock it
deadlocks, and the test **fails in 5 s with the reason attached** rather than hanging: the work runs
on a worker thread behind a future, and the failure path detaches that thread onto a deliberately
leaked dispatcher rather than letting it hold a pointer into a dead stack frame.

Keeping the test matters more than it would for a bug fix. A widened contract is only worth the paper
it is written on if something fails when it narrows again by accident.

## R32 — `moveToThread()` does not migrate already-posted events *(Fixed)*

**Severity: Medium. Confirmed by probe, against Qt 6 run side by side. Filed as an unprobed
hypothesis on 2026-08-13, probed and fixed on 2026-08-14; the hypothesis was right, and the
consequence is not the one it guessed.**

`moveToThread()` migrates active timers ([Object.cpp:399-445](src/Object.cpp#L399-L445)) but nothing
migrates events already sitting in the old thread's queue. Qt migrates both, in
`QObjectPrivate::setThreadData_helper()`:

```cpp
// move posted events
qsizetype eventsMoved = 0;
for (qsizetype i = 0; i < currentData->postEventList.size(); ++i) {
    const QPostEvent &pe = currentData->postEventList.at(i);
    if (!pe.event) continue;
    if (pe.receiver == q) {
        targetData->postEventList.addEvent(pe);          // move it across
        const_cast<QPostEvent &>(pe).event = nullptr;    // blank the old entry
        ++eventsMoved;
    }
}
if (eventsMoved > 0 && targetData->hasEventDispatcher()) {
    targetData->canWait = false;
    targetData->eventDispatcher.loadRelaxed()->wakeUp();  // and wake the new thread
}
```

### The divergence, measured

The same program in both libraries: post a queued call while the receiver lives on the main thread,
move the receiver to a worker, then let both loops run. The move is confirmed to have taken effect
(`thread() == worker`) before anything is dispatched.

| | slot ran on |
|---|---|
| **QtLikeSignal** | **MAIN — the thread the object left** |
| **Qt 6** | **WORKER — the thread it now lives on** |

**This is the guarantee a queued connection exists to provide.** The slot runs on a thread the object
no longer belongs to, with no warning and no crash, which is the failure mode this library's thread
affinity is meant to prevent. Nothing tells the caller it happened.

### `deleteLater()` is the sharper case

`deleteLater()` posts its `DeferredDeleteEvent` into the current thread's queue, so a move afterwards
strands it too:

```
deleteLater() on main, then moved to the worker
  destructor ran on: MAIN (the thread it left)
Object::~Object: object destroyed from a thread other than the one it lives in while that
thread's event loop is still running; this is not safe. Use deleteLater() to destroy an object
from another thread.
```

**Our own `deleteLater()` trips our own cross-thread-destruction warning**, and the warning's advice
is to do the thing that caused it. Qt runs the destructor on the worker, as it should.

### What it is *not*

The hypothesis this entry was filed on — that a stranded event outlives the object and becomes a
use-after-free — is **not confirmed**. Post a queued call, move the object, destroy it on its new
thread, then drain the old thread's queue: no crash, and AddressSanitizer is silent. That is the same
shape as R28's queued half and for the same reason: `Object::event()` is not virtual and its
`MetaCall` branch never touches `this`, so the freed object is never dereferenced. It is undefined
behaviour with no observable today, and it would become a crash the moment `event()` becomes virtual.

So the defect is wrong-thread execution, not memory corruption. Filed severity Unknown; measured
severity Medium, and for a reason the original note did not anticipate.

### The fix

`moveToThread()` now calls `Object::migratePostedEvents()`, which takes this object's events off the
outgoing dispatcher and posts them to the incoming one. All three event kinds travel: metacalls,
deferred deletes, and — as before — timers, which were already carried by
`takeTimersForReceiver()`. Both probes now report the destination thread, matching Qt.

**Three things had to be solved, none of them obvious up front.**

**The two dispatcher mutexes.** Qt needs `QOrderedMutexLocker` because it moves the events and the
affinity together under both locks. Swapping the affinity *first* means each queue is only ever
touched alone, so two moves in opposite directions cannot deadlock and no ordered locker is needed.
That is safe for a reason specific to this function: **`moveToThread()` runs on the object's own
thread**, so the old thread is inside the call and cannot be dispatching the events being taken.

**Events already in a dispatch batch.** The R28 frames mean an event can be in flight rather than in
the queue — which is exactly where it is when `moveToThread()` is called from inside a handler, the
normal place for an object to move itself. `takeEventsForReceiver()` walks the published frames as
well as the queue, taking entries out with the same clear-the-slot handover R28 introduced. Nothing
is left behind.

**A destination with no dispatcher at all.** The canonical idiom is `Thread w; o.moveToThread(&w);
w.start();` — and a `Thread` has no dispatcher until its run body creates one, so there was nowhere
to post. Dropping the events loses work silently; leaving them behind is the defect itself. They are
now parked on the destination's `ThreadData` and handed to the dispatcher the moment
`setDispatcher()` installs one. Qt has no equivalent problem because its queue lives in
`QThreadData` rather than in the dispatcher; this is the smallest version of that.

A late discovery worth recording: `Thread::isRunning()` is set by `start()` *before* the run body
creates the dispatcher, so "running" does not imply "can take events". The parking path is therefore
load-bearing for a started worker too, not only for an unstarted one — the first two tests below
failed without it until they were changed to wait on `eventDispatcher()` instead.

### Tests

Three, each verified against a deliberately broken build, and each isolating one mechanism:

| test | what it pins | migration off | parking off |
|---|---|---|---|
| `ObjectDefectTest.MoveToThreadCarriesAlreadyPostedEventsToTheNewThread` | metacalls travel | **fails** | passes |
| `ObjectDefectTest.MoveToThreadCarriesAPendingDeleteLaterToTheNewThread` | deferred deletes travel | **fails** | passes |
| `ObjectDefectTest.EventsMovedToAnUnstartedThreadAreDeliveredWhenItStarts` | parking, and the flush on `setDispatcher()` | **fails** | **fails** |

Suite: **172 tests, 0 failures**, in declaration order and under `--gtest_shuffle`.

### What it did not close

The R28 residual stands: `~Object()` still cancels through the dispatcher its *current* affinity
names, so `deletedReceivers` is still needed in `processEvents()`. Migration narrows that window
rather than removing it — an object can still be destroyed while an event for it is in flight in a
batch on the thread it left, which is the case R28's guard covers.

---

# How these were confirmed

**Probe** means a throwaway program was compiled against `src/` and run, and its output is quoted in
the entry. **Inspection** means the code was read but no runtime probe was written. The distinction
matters more than it looks — see below.

**Baseline.** `./waf build` succeeds and the suite passes: **164 tests, 0 failures** (2026-08-13,
`linux64-clang`, debug, no sanitizer). No existing test caught any of these. After the R28 and R30
fixes, the R31 contract change and their five tests: **169 tests, 0 failures**, in declaration order
and under `--gtest_shuffle`.

**Windows baseline, added 2026-08-16 by R22.** `.\waf.bat build` succeeds and the suite passes:
**184 tests, 183 passed, 1 skipped, 0 failures** (`win64-msvc`, debug, AddressSanitizer on), in
declaration order and under `--gtest_shuffle`. Before R22's six tests it was 178. This is the first
time any of these numbers came from Windows rather than from Linux; getting there needed three build
fixes that had nothing to do with any risk in this document, and they are itemised in the R22 entry.
The Linux numbers above have not been re-measured since, and R22's changes to shared files are a
guarded test file and one `PerfHarness::keep()` call, neither of which a Linux build should notice.

**Every regression test here was verified against a deliberately broken build**, not merely observed
to pass. A test that has never failed has not been shown to test anything. Where a fix had several
independent parts, each part was disabled separately, which is what established that R28 needed three
tests rather than one.

**`src/tests/QtLikeSignal-test-known-defects.cpp` is still empty.** R28 went straight to
`QtLikeSignal-test-defect-regressions.cpp` because it was fixed in the same pass that found it. The
file stays for the convention: a defect found *without* a fix in hand still belongs there, red, until
one exists.

## The lesson this pass earned

**Two of the four items filed were not defects, and both were filed from inspection alone.**

- **R29 was simply wrong.** The reasoning was local to one function and the disproof was one ownership
  fact three lines above it.
- **R31 was real but mislabelled.** The constraint it violated was documented, so breaking it was
  misuse, and this project does not count the consequences of misuse as defects.

The two that were defects, R28 and R30, were both confirmed by running something.

So: **an inspection finding is a hypothesis.** Probe it before filing it. And before filing it, check
whether the behaviour it complains about is already written down as a precondition — a documented
precondition moves the fault to the caller. What that does *not* excuse is a precondition that is
inconsistent or untestable, which is what R31 turned out to be.

R32 was filed under that rule — listed so it would not be lost, labelled unprobed so it would not be
mistaken for a finding — then probed and fixed a day later. The rule earned its keep twice over: the
hypothesis was right that something was wrong, and wrong about what. It guessed a use-after-free; the
actual defect was wrong-thread execution, which is worse in practice because nothing reports it.

That is also why it was worth probing before fixing. A fix aimed at the guessed defect would have
been aimed at the wrong thing.
