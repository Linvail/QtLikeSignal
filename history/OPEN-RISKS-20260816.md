# Open risks — snapshot 2026-08-16

Filed while wiring AddressSanitizer/ThreadSanitizer into the `waf` build phase
(`tools/sanitizer.py`, `.agents/AGENTS.md`). Numbering continues from `OPEN-RISKS-20260813.md`
(R32).

## Status

| ID | Status | Risk | Severity | Evidence and outcome |
|----|--------|------|----------|-----------------------|
| R33 | **Fixed** | Test/benchmark helper code has unsynchronized cross-thread reads and writes | Low | One real race, fixed: TSan on the suite now reports **0, exit 0** (was 2, exit 66). The benchmark half was a **false positive** — an uninstrumented Qt whose futex-based `QMutex` TSan cannot see; suppressed, 19 → 0 |
| R34 | **Fixed** | A foreign message loop wedges `EventDispatcherWin32`'s wake flag permanently | **High** | Probe — after a modal-style drain, no post ever woke the loop again and `quit()` stopped working. Found by reading Qt 6.11.1. Fixed with a real window procedure; one regression test |

---

# Details

## R33 — test/benchmark helper code has unsynchronized cross-thread reads and writes *(Fixed)*

**Severity: Low — confined to test and benchmark code, not the `QtLikeSignal` library. Confirmed
by probe (ThreadSanitizer, `linux64-clang`).**

*The text down to "Fixed, 2026-08-16" below is the original filing, kept as written. The entry was
filed as two items and they had different answers: the first was a real race and is fixed, the
second was a false positive and is suppressed with its cause established. Both sections follow.*

Running the full Linux build matrix under `--enable-tsan=yes` found two real data races, both in
test-only helper code, not in `src/`. gtest still reports every test **PASSED**, because gtest
checks assertions only, not thread-safety: the process still exits with code 66 (TSan's failure
code) on a run with zero failed assertions.

**1. `ThreadRecordingReceiver::onCall()`,
[QtLikeSignal-test-defect-regressions.cpp:1847](../src/tests/QtLikeSignal-test-defect-regressions.cpp#L1847).**

```cpp
void onCall( int aValue )
{
    if( mRanOn )
    {
        *mRanOn = Thread::currentThread();   // written from the worker thread
    }
}
```

`mRanOn` points at a plain `Thread*` local owned by the test. `waitFor` polls it from the main
thread with no lock and no atomic (line 1881: `return ranOn != nullptr;`). TSan reports a write
on the worker thread racing a read on the main thread, with no happens-before edge between them.
Confirmed twice under `linux64-clang`, in
`ObjectDefectTest.MoveToThreadCarriesAlreadyPostedEventsToTheNewThread` and
`ObjectDefectTest.EventsMovedToAnUnstartedThreadAreDeliveredWhenItStarts`.

**2. The Qt 6 benchmark harness**, `test_Qt6_Performance.cpp:159`, in
`Performance_Qt6_QueuedEmitCrossThread`. Same shape: an unsynchronized counter read and written
across the emitting and receiving threads, plus one race TSan attributes to `operator delete`
(likely the same counter's storage being freed while still visible to the other thread).

**Not a defect in the library.** Every race is in test/benchmark code — a spin-wait on a raw
pointer or counter with no atomic and no lock — not in `QtLikeSignal::Object`, `Thread`, or the
event dispatchers. Nothing here contradicts R28/R30/R32's fixes in `OPEN-RISKS-20260813.md`, each
confirmed independently by a regression test verified against a deliberately broken build. What
this affects is only whether the *test binaries themselves* can be trusted to exit 0 under TSan.

**GCC makes this worse, and is now excluded.** `linux64-gcc` reports the same two races plus many
more (20 vs. 2 under `linux64-clang` for the same binary) and hung outright running
`QtLikeSignal-Performance-Tests` under TSan (`setarch -R`, 2-minute timeout, no output). This
matches the precedent in `OPEN-RISKS-20260802.md` R14, where GCC 11.4's `libtsan` was proven —
via a `double lock of a mutex` report against a single, uncontested `lock_guard` — to fabricate
reports outright. `linux64-gcc` + `--enable-tsan` is excluded from this project's routine build
matrix in `.agents/AGENTS.md` for that reason, pending a newer GCC on this machine.

### Fixed, 2026-08-16 — the `Thread**` half

`ThreadRecordingReceiver::mRanOn` is now `std::atomic<Thread*>*`, and both tests' `ranOn` locals are
`std::atomic<Thread*>`. That is the whole change; the reads in `waitFor` and the final `EXPECT_EQ`
go through `.load()`.

Proven rather than asserted, since the only symptom was a sanitizer report:

| | races | exit code | tests |
|---|---|---|---|
| before | 2 | 66 | 180 passed |
| after | **0** | **0** | 180 passed |

`linux64-clang`, `--enable-tsan=yes`. The exit code is the part that matters: a TSan run of the
suite is now a signal that means something, instead of one that is always red for a known reason
and therefore ignored.

**The same defect was in QtMimic, and the entry did not mention it.** R33 named
`src/tests/QtLikeSignal-test-defect-regressions.cpp`, but
`external/QtMimic/tests/QtMimic-test-defect-regressions.cpp` carries a character-for-character copy
of the same helper, and `QtMimic-test` was reporting the same 2 races from its line 1844. Found by
running that binary rather than by reading — it was never in the original report, so nothing would
have pointed at it. Both copies now carry the atomic. `QtMimic-test`: **0 races, exit 0, 180
passed**, from 2.

No regression anywhere else — `linux64-clang` and `linux64-gcc` at 180 passed, `win64-msvc` with
ASan at 184 tests, 183 passed, 1 skipped.

### The benchmark half is a false positive — withdrawn, with a suppression instead

**Not a defect in this project's code. Reproduced in full, then explained.**

A first pass at this entry claimed the benchmarks could not be built here because there was no Qt 6.
That was wrong, and wrong for an avoidable reason: `_find_qt6()` in `src/tests/wscript` returns
`None` for any non-Linux target, so a **Windows** build always prints
`Qt 6 performance benchmarks skipped`, whatever is installed. Qt 6.11.1 is present in WSL at
`/usr/local/Qt-6.11.1`, `moc` runs, and the benchmarks are built and run there like anything else.
Reading a Windows log to decide a Linux fact is what produced the error.

Built and run properly, `linux64-clang --enable-tsan=yes` reports **19 ThreadSanitizer findings** in
`QtLikeSignal-Performance-Tests`, all inside `Performance_Qt6_QueuedEmitCrossThread`. So the entry
was right that something is reported, and wrong about what.

**What is actually racing is not our variable.** The report names
`test_Qt6_Performance.cpp:159` and `:169`, which are `loopReady.store( true )` and
`received.fetch_add( ... )` — both `std::atomic` operations, which cannot themselves race. The
memory TSan is complaining about is named in the report's own location line:

```
Location is heap block of size 24 at 0x720800000580 allocated by main thread:
  #0 operator new(unsigned long)
  #1 QMetaObject::invokeMethodCallableHelper<...>(...)  qobjectdefs.h:645
```

That is the heap block Qt allocates to hold the queued lambda. The main thread constructs it, the
worker thread reads it to make the call, and Qt frees it — which is also why three of the findings
land in `operator delete`. The reported line and column are where the lambda reads its own capture
out of that block, not where it touches `loopReady`.

**Why TSan sees no ordering.** Two facts, both checked rather than assumed:

1. The installed Qt is not instrumented. `nm -D /usr/local/Qt-6.11.1/lib/libQt6Core.so.6 | grep -c
   __tsan` gives **0**; the same count against our own binary gives **644**.
2. Qt's `QBasicMutex` does not use `pthread_mutex` on Linux. It synchronises with raw `futex(2)`
   syscalls — `qmutex.cpp` includes `qfutex_p.h` and takes the `futexAvailable()` path. TSan
   intercepts the pthread primitives, so it can see edges made with those even inside an
   uninstrumented library, but it does not model raw futex syscalls.

So the happens-before edge Qt genuinely establishes when it hands a queued call across threads is
invisible, and every object Qt passes that way looks unsynchronised. The control is in the same
binary: QtLikeSignal's own queued cross-thread benchmark has the same shape, is instrumented end to
end, and reports nothing.

**Suppressed, not fixed**, because there is nothing here to fix in our code.
`src/tests/tsan-suppressions.txt` carries two narrowly-scoped entries and the reasoning above:

```
race:Performance_Qt6_QueuedEmitCrossThread
race:test_Qt6_Performance.cpp
```

```
TSAN_OPTIONS=suppressions=src/tests/tsan-suppressions.txt \
    ./out/linux/Tests/linux64-clang/debug/src/tests/QtLikeSignal-Performance-Tests
```

takes the binary from 19 findings to **0, exit 0**. A broader `called_from_lib:libQt6Core.so.6` was
tried first and rejected: it silenced only 8 of the 19 *and* was wider than the two lines that
work, which is the worst of both. The suppressions deliberately do not apply to `QtLikeSignal-Tests`
or `QtMimic-test` — those are clean with no suppressions at all, and must stay that way.

The real fix, if this signal is ever wanted rather than merely quieted, is a Qt built with
`-fsanitize=thread`.

## R34 — a foreign message loop wedges `EventDispatcherWin32`'s wake flag permanently *(Fixed)*

**Severity: High. Confirmed by probe. Found by reading Qt 6.11.1's `qeventdispatcher_win.cpp`
against ours, which is what the exercise was for.**

`wakeWaiter()` collapses a burst of wakeups into one posted message:

```cpp
if( mWakePending.exchange( true ) )
{
    return;                       // a wakeup is already pending; don't post another
}
PostMessageA( mMessageWindow, kWakeUpMessage, 0, 0 );
```

The flag is cleared in exactly **one** place: `processPlatformEvents()`, when its own `PeekMessage`
drain takes that message off the queue. So the invariant the collapse depends on is "only our drain
ever removes our wakeup message" — and on Windows that is not true.

### Why it is not true

Our message-only window's procedure is `DefWindowProcA`. Any other message loop running on the same
thread — `MessageBox()`, a modal dialog, a menu or drag loop, a COM/OLE modal loop, any third-party
`GetMessage` loop — will `PeekMessage` our wakeup off the queue and `DispatchMessage` it to
`DefWindowProcA`, which discards it. The message is gone; the flag stays set; and **every later
`wakeWaiter()` returns early without posting anything.**

A loop blocked in `MsgWaitForMultipleObjectsEx` then never wakes again.

### The probe

Post a task, let a foreign loop drain the queue, run one dispatcher pass, then post again:

```
1. posted a task; a wakeup message is now queued
2. a foreign message loop drained 1 message(s), including ours
3. dispatcher pass ran; first task ran = yes
4. entering exec(); a healthy loop wakes on the second post

second task ran   : NO
watchdog had to fire: YES

RESULT: WEDGED -- the wake flag stayed set, so no wakeup was ever posted again.
```

**The first version of this probe hung outright**, and that is the sharpest thing it found:
`CoreApplication::quit()` wakes the loop through the very `wakeWaiter()` that is wedged, so on a
wedged dispatcher `quit()` sets the exit flag and the loop never notices. The probe only reports at
all because the watchdog also posts a raw `PostThreadMessage( WM_NULL )`. So the failure is not
merely "posted events stop arriving" — **the application can no longer be shut down through its own
API.** Nothing is logged, and nothing crashes.

### What Qt does instead

Qt has the same collapse — `wakeUps.testAndSetRelaxed( 0, 1 )` guarding a
`PostMessage( WM_QT_SENDPOSTEDEVENTS )` — and clears it in **two** places. The second is its window
procedure ([qeventdispatcher_win.cpp:199-211](H:/Projects-2026/qtbase-everywhere-src-6.11.1/src/corelib/kernel/qeventdispatcher_win.cpp)),
reached exactly when something other than Qt's own loop dispatches the message. Its comment names
this case outright:

```cpp
case WM_QT_SENDPOSTEDEVENTS:
    // We send posted events manually, if the window procedure was invoked
    // by the foreign event loop (e.g. from the native modal dialog).
    // Skip sending, if the message queue is not empty.
    // sendPostedEventsTimer will deliver posted events later.
    static const UINT mask = QS_ALLEVENTS;
    if (HIWORD(GetQueueStatus(mask)) == 0)
        q->sendPostedEvents();
    else
        d->startPostedEventsTimer();
    return 0;
```

and `startPostedEventsTimer()` begins with `wakeUps.storeRelaxed(0)` under the comment "we received
WM_QT_SENDPOSTEDEVENTS, so allow posting it again". Qt cannot wedge because the clear is attached to
the *message being dispatched*, not to the drain that happened to dispatch it.

### The fix

The message-only window now has a real window procedure instead of `DefWindowProcA`, and it clears
`mWakePending` on `kWakeUpMessage`. Whichever loop dispatches the message — ours or a foreign one —
re-arms the collapse. The existing clear in `processPlatformEvents()` stays as the fast path, so
that loop's `continue` still does not have to dispatch.

**What is stored in `GWLP_USERDATA` is the flag, not the dispatcher.** That keeps the whole change
inside the `.cpp`: the window procedure is a plain function in the anonymous namespace, it needs no
access to `EventDispatcherWin32`'s privates, and no Windows type reaches the header — which the
header's own comment says is the point of `mMessageWindow` being `void*`. It also tolerates a null
`GWLP_USERDATA`, because `CreateWindowEx` sends `WM_NCCREATE`/`WM_CREATE` through the procedure
before it has returned the handle to set it on.

**The window class name is now unique per copy of the library**, `QtLikeSignal_EventDispatcher_%p`
with the address of the window procedure, exactly as Qt appends `quintptr(qt_internal_proc)` to
"make sure that multiple Qt's can coexist in the same process". This was listed below as a harmless
observation when the entry was filed, and the fix is what made it stop being harmless: with a fixed
name, two copies of this library in one process collide on `RegisterClass`, and the loser creates
its windows against the winner's class — which now means the winner's *code* runs, deciding which
copy's flag gets cleared. Fixing the wedge without this would have traded one silent failure for a
rarer one.

**Not copied from Qt, deliberately.** Qt's window procedure also *delivers posted events*, so a
modal dialog does not freeze queued work while it is up, and it refuses to deliver when
`GetQueueStatus` shows other input pending, falling back to a `USER_TIMER_MINIMUM` timer. Both are
about responsiveness under a foreign loop rather than about the wedge, and they are a larger
behavioural change. The wedge is fixed; that is a separate question.

### The test

`EventDispatcherWin32Test.WakeSurvivesAForeignMessageLoopDrainingTheQueue`, which drives the probe's
sequence: cause a wakeup, drain the queue with a plain `PeekMessage`/`DispatchMessage` loop, run one
pass, then post again and require it to arrive.

Verified against a deliberately broken build — `lpfnWndProc` put back to `DefWindowProcA` — where it
**fails in 6.7 s with both reasons attached**, and the other six tests in the file still pass. It
does not hang, and that took a deliberate change to the shared `Watchdog`: it now posts a raw
`PostThreadMessage( WM_NULL )` as well as calling `quit()`, because on a wedged dispatcher `quit()`
wakes through the very `wakeWaiter()` that is stuck. The first version of the probe for this entry
hung for exactly that reason, which is the sharpest single fact in it.

One correction worth recording: the first run of the finished test failed on a *test* bug, not a
product one — the task it posted set its flag but never called `quit()`, so `exec()` ran on to the
watchdog with the wake having worked perfectly. The assertion that caught it was the watchdog one,
which is the assertion that exists to catch a hang. Worth noting because it is the failure mode a
watchdog assertion is most likely to produce falsely.

Suite after the fix: **185 tests, 184 passed, 1 skipped, 0 failures** on `win64-msvc` with ASan, in
declaration order and under `--gtest_shuffle`.

### One smaller observation from the same reading, still open

**Qt checks `interrupt` on every message; we check it once per pass.** Qt's drain loop is
`while (!d->interrupt.loadRelaxed())`, so an interrupt raised by a dispatched message stops that
drain immediately. Ours reads `mInterrupt` only at the top of `processEvents()` — the same
structural fact that made R22's `WM_QUIT` branch stop the *following* pass rather than the current
one. Left alone: R22 pinned the current behaviour with two tests deliberately, and changing when an
interrupt takes effect is a behavioural decision rather than a defect.

*(The second observation filed here — that our window class name was a fixed string where Qt's is
uniquified per loaded copy — was folded into the fix above, because the fix is what would have made
it bite.)*

### Confirming Qt 6 for R22

Incidentally settled while reading: Qt 6.11.1's `WM_QUIT` branch is character-for-character what Qt
5.15.19 had — `if (QCoreApplication::instance()) QCoreApplication::instance()->quit(); return false;`
([qeventdispatcher_win.cpp:545-549](H:/Projects-2026/qtbase-everywhere-src-6.11.1/src/corelib/kernel/qeventdispatcher_win.cpp)).
The R22 write-up in `OPEN-RISKS-20260813.md` cites 5.15.19; the claim holds unchanged against
6.11.1, which is the version `.agents/AGENTS.md` now names as the reference.
