# Open risks — snapshot 2026-08-16

Filed while wiring AddressSanitizer/ThreadSanitizer into the `waf` build phase
(`tools/sanitizer.py`, `.agents/AGENTS.md`). Numbering continues from `OPEN-RISKS-20260813.md`
(R32).

## Status

| ID | Status | Risk | Severity | Evidence and outcome |
|----|--------|------|----------|-----------------------|
| R33 | **Open** | Test/benchmark helper code has unsynchronized cross-thread reads and writes | Low | Probe — confirmed under `linux64-clang`, real races on a raw `Thread**` and a benchmark counter |

---

# Details

## R33 — test/benchmark helper code has unsynchronized cross-thread reads and writes *(Open)*

**Severity: Low — confined to test and benchmark code, not the `QtLikeSignal` library. Confirmed
by probe (ThreadSanitizer, `linux64-clang`).**

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

**Suggested fix, not yet done.** Change `mRanOn`/`ranOn` in the test helper to
`std::atomic<Thread*>` (or guard both sides with a mutex and condition variable), and the same for
the Qt6 benchmark's cross-thread counter. Low priority: it affects only the reliability of the
TSan signal on the test suite, not the shipped library, and was out of scope for the sanitizer
build-system work in progress when it was found.
