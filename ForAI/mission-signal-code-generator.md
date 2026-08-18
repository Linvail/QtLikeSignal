# Mission: a Qt-style signal code generator

After we decided to get rid of boost::signal2, an interest idea came to my mind —
can we mimic Qt6's code generator to make even faster signal?

Because we have powerful waf build system, we can use Python to do many complex things,
including generating code.

The idea is:

1. Make some preprocessors.

   ```cpp
   #define QT_MIMIC_SIGNAL
   ```

   In a class, write

   ```cpp
   QT_MIMIC_SIGNAL void valueChanged(int aValue);
   ```

2. Create a waf feature, send a list of file to that feature for code generation.

   Search for `QT_MIMIC_SIGNAL` text.
   Try to generates more code for:

   ```cpp
   QT_MIMIC_SIGNAL void valueChanged(int aValue);
   ```

   But, how to write those code? We need to survey how Qt does.
   `/home/evan/Projects/qtbase-everywhere-src-6.11.1`, or `H:\Projects-2026\qtbase-everywhere-src-6.11.1`
   when working on Windows. Both are listed in `.agents/AGENTS.md`.

3. Append the generated files to source list.

---

## PLAN (written 2026-08-12; re-measured and corrected 2026-08-16)

This doc is for AI. Read all of it, especially section 2, before writing a generator.

> **UPDATE 2026-08-16.** The conclusion below has not changed, but the reason for it has, and every
> number in the original has. **Section 3a — remove the per-emit allocation — was done on
> 2026-08-13**, one day after this was written, as part of P7/P8 in
> `history/PERFORMANCE-20260813.md`. `Signal::Impl::emit()` no longer copies anything: it takes a
> `shared_ptr` to an immutable published snapshot. So the "remaining gap" this document was built
> around no longer exists, and **direct emit now beats Qt 6** rather than merely matching it.
> The tables below are the 2026-08-16 re-measurement. Section 5 records what is left to decide.

### SUMMARY, so nobody has to read to the end to get the point

The survey was done and the benchmarks were run. The conclusion is:

> **Do not build the code generator for speed. Our signal is already faster than Qt's on the path
> that matters.**

Measured 2026-08-16, `linux64-clang`, `--mode=release` (`-O2`, no sanitizer), Qt 6.11.1 from
`/usr/local/Qt-6.11.1`, all three libraries in one process. Ranges over four runs in declaration
order and two under `--gtest_shuffle`:

| scenario | Qt 6 | QtLikeSignal | QtMimic | |
|---|---|---|---|---|
| emit → receive, direct | 27.8–31.6 ns | **24.2–27.0 ns** | 23.1–25.7 ns | ← **we are faster** |
| emit → receive, auto same-thread | 27.5–33.1 ns | 47.8–52.4 ns | 46.8–55.6 ns | ← ~1.7x slower |
| emit → receive, queued cross-thread | 494–522 ns | 462–516 ns | 460–521 ns | ← parity |
| `connect()` | 106–181 ns | 129–172 ns | 124–165 ns | ← now within noise |
| allocations per direct emit | 0.000 | **0.000** | 0.000 | |

**There is no per-emit allocation left to remove.** The original version of this document was built
around one — "exactly 1.000 allocations/emit … the snapshot vector in `Signal::Impl::emit()`" — and
it is gone twice over: P3 removed a `std::function` built on the heap per emit, and P7/P8 replaced
the snapshot copy with a published immutable list that `emit()` merely takes a reference count on.
`PerformanceRegression.DirectEmitAllocatesNothing` pins it, and passes.

A code generator would not have removed either, because neither had anything to do with type
erasure or with how signals are declared. That argument survives its own numbers.

Where we are still slower is somewhere moc does not help either:

| scenario | Qt 6 | ours | |
|---|---|---|---|
| auto connection, same thread | ~29 ns | ~49 ns | ~1.7x slower |

That is `Object`'s affinity resolution running on every emit — item P2 in
`history/PERFORMANCE-20260813.md`, accepted **By Design** as risk R25 in
`history/OPEN-RISKS-20260813.md`. Not a signal problem at all.

**`connect()` is no longer the outlier this document reported.** It was ~343 ns against Qt's ~109
when this was written; P10 took one connection from five heap blocks to Qt's own two, and the two
now overlap so heavily across runs — Qt 106–181 ns, ours 129–172 — that neither can be called
faster on this machine. Treat that row as "comparable, and noisy", not as a ratio.

The generator is still worth building for *other* reasons — ergonomics and per-object memory,
section 4 — but that is a different project with a different justification, and it should not be
sold as a performance change.

### 1. Survey: what moc actually generates

Done by running the real thing rather than reading about it:

```
/usr/local/Qt-6.11.1/libexec/moc probe.h -o moc_probe.cpp
```

for a class with `signals: void valueChanged(int); void twoArgs(int, double);`. Three pieces come
out, and the third is the interesting one.

**1a. The signal body.** A signal is an ordinary member function that moc *defines* for you:

```cpp
void Probe::valueChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}
```

That `0` is the signal's index within the class. This is the whole reason a Qt signal can be
declared as a member function and still work: you declare it, moc writes the body.

**1b. A per-class static dispatcher**, which is how a *slot* gets called without a `std::function`:

```cpp
void Probe::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<Probe *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->valueChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->twoArgs(...); break;
        case 2: _t->onValue(...); break;
        }
    }
    ...
}
```

Arguments travel as an untyped `void**` array and are cast back at the call site. Nothing is copied
on the way through; the slot's own parameter list decides what gets copied, exactly once, when it is
finally called.

**1c. A PMF-to-index lookup**, `QtMocHelpers::indexOfMethod`, which is what makes
`connect(&Sender::sig, ...)` resolve to an integer at connect time.

Connections then live in `QObjectPrivate::ConnectionData`, keyed by signal index. An object with no
connections carries one pointer.

**One correction to a natural assumption.** It is tempting to conclude "Qt is fast because the
switch avoids type erasure". That is only half true. For the modern PMF `connect()` — the only form
we support — Qt wraps the slot in a `QSlotObjectBase` and calls it through a *virtual* function.
That is the same order of indirection as our `std::function`. Qt's speed on the emit path comes from
allocating nothing and from touching very little memory, not from the switch.

### 2. Measurements, and why they say what they say

**The benchmarks are in the tree now**, which they were not when this was written:
`src/tests/test_QtLikeSignal_Performance.cpp` and `test_Qt6_Performance.cpp` run all three libraries
in one process, and the Qt 6 half is compiled only when a built Qt exists at `/usr/local/Qt-6.11.1`.
The original advice to recreate them from the shapes described here is superseded — run these.

```
wsl ./waf install --project=Tests --mode=release
./out/linux/Tests/linux64-clang/release/src/tests/QtLikeSignal-Performance-Tests
```

The original 2026-08-12 figures are kept here for the record, because the argument the document
makes was built on them:

| scenario | Qt6 | ours (own) | ours (boost) |
|---|---|---|---|
| direct emit | ~29 ns | ~32 ns | ~74 ns |
| auto connection, same thread | ~29 ns | ~65 ns | ~106 ns |
| `connect()` | ~109 ns | ~343 ns | ~625 ns |
| allocations per emit | 0.000 | 1.000 | (not measured) |

**Superseded 2026-08-16** — see the table in the summary. Every row moved, three of them because
work landed: P3 and P7/P8 on the emit path, P10 on `connect()`, and part of the auto path via P6's
`threadDataPtr()` change. The "ours (boost)" column no longer describes anything in the tree:
QtLikeSignal replaced boost before this was written, and **QtMimic has since dropped it too**
(`grep -rl boost external/QtMimic/src/` finds nothing), so the third column is now a second
hand-written implementation rather than a boost control.

The reasoning that made the original table decisive is worth keeping, because it still holds in
shape even though its numbers changed:

```
raw Signal::emit(), no Object::connect wrapper:  ~30 ns   (2026-08-12)
```

Of the direct-emit cost, nearly all is the signal machinery and about 2 ns is `Object`'s wrapper.
There was never fat in the `Object` layer to cut on the direct path — which is why the direct path
was fixed by changing `Signal`, not by generating code.

**A trap, and it is not the one this document originally recorded.**

The original warning said `waf --mode=release` is `-O2` *with ThreadSanitizer*, so its Qt ratios were
meaningless. **That is no longer true.** Sanitizers moved to build-time opt-in — `--enable-asan=yes`
/ `--enable-tsan=yes`, defaulting to `no` — in `tools/sanitizer.py`, and the configure-time flags
were removed from `tools/toolchain-linux.py`. A release build now carries no `-fsanitize=` at all,
verified against its `compile_commands.json`. **Release is the correct build to benchmark in.**

The real trap is one layer down, and it survives: **do not benchmark, or sanitize, against Qt 6 in a
sanitizer build.** The installed Qt is prebuilt and uninstrumented — `nm -D libQt6Core.so.6 |
grep -c __tsan` is 0 against 644 for our own binary — so a TSan run compares instrumented code
against uninstrumented code and also reports false races on Qt's queued-call machinery, because
`QMutex` synchronises with raw `futex(2)` that TSan does not model. That is R33 in
`history/OPEN-RISKS-20260816.md`, with `src/tests/tsan-suppressions.txt`.

### 3. What to do instead, if the goal is speed

In priority order. None of these needs a code generator.

**3a. Remove the per-emit allocation. — DONE, 2026-08-13.** The prescription below was written on
2026-08-12 and implemented the next day, in almost exactly this form:

> Copy-on-write: have `Impl` hold `std::shared_ptr<const std::vector<std::shared_ptr<Slot>>>`.
> `emit()` copies the `shared_ptr` — one atomic increment, no allocation — and iterates it with no
> lock held. `connect()`/`disconnect()` build a new vector and swap the pointer under the lock.
> Writers pay an allocation instead of readers, which is the right way round: emits vastly
> outnumber connects.

`Signal.hpp` now has `using PublishedListPtr = std::shared_ptr<const PublishedList>`, and
`emit()` opens with `const PublishedListPtr slots = publishedSlots();`. The rebuild is deferred
behind an `mDirty` flag rather than done on every write, which is the one refinement over the
prescription — see P8, where the remaining cost is that a *connect* forces the next emit to rebuild.

The prediction was right: direct emit went to **below** Qt's ~29 ns, and is 24–27 ns today. Every
guarantee in `ForAI/mission-signal.md` section 2 held, and `QtLikeSignal-test-signal.cpp` stayed
green.

**3b. The auto-connection path**, now ~49 ns against Qt's ~29, was ~65 ns when this was written.
This is `Object::connectImpl()`'s wrapper resolving affinity on every emit: `Affinity::data()` takes
a mutex and returns a `shared_ptr` copy, then compares against the current thread.

Partly addressed already: P6 found that `isCurrentThread()` was copying a `shared_ptr` purely to
compare pointers, and replacing that with `threadDataPtr()` removed 11 ns — about 19% of the path.
What remains is the mutex, which is item **P2** in `history/PERFORMANCE-20260813.md` and risk
**R25** in `history/OPEN-RISKS-20260813.md`, where it is accepted **By Design**: the mutex buys a
strong reference guaranteed alive for the call, which Qt's raw `QAtomicPointer` does not offer, and
removing it needs retired-pointer retention. An atomic load is still the obvious move; the reason it
has not been made is that `moveToThread()` has to publish safely against it.

**3c. `connect()` — no longer clearly behind.** Ranges now overlap Qt's: ours 129–172 ns against Qt
106–181 across six runs. It was ~343 ns when this was written, and five heap blocks against Qt's
two; P10 in `history/PERFORMANCE-20260813.md` took it to two blocks in stage 1 and made ending one
receiver's connections O(K) instead of O(K²) in stage 2.

The finding P10 recorded still stands and is the useful part: removing three of the five allocations
bought only about a tenth of the time, so **most of what is left in `connect()` is not allocation**,
and nobody has found what it is. That is where a look would pay — but connect is not a hot path, and
it is now inside the noise band of the thing it was being compared against, so the case for looking
is weaker than it was.

### 4. If the generator is built anyway, build it for these reasons

There are real, non-performance arguments for it, and they should be the stated goal:

**4a. Memory per object.** Every `Signal<Args...>` member today is a `shared_ptr` plus a
`SignalView` plus an `Impl` holding a mutex and a vector. A class with ten signals pays ten of
those, whether or not anything is connected. Qt pays one pointer per object no matter how many
signals it declares. For a library aimed at embedded-ish use this is a stronger argument than speed.

**4b. Ergonomics.** `QT_MIMIC_SIGNAL void valueChanged(int)` reads like Qt, and it would let
`connect(&Sender::valueChanged, ...)` take a member-function pointer the way Qt's does, instead of
requiring the caller to name a member object, `sender.valueChanged`.

**4c. Compile-time signature checking** at the connect site. **Weaker than when this was written.**
The original argument was that checking is "partly deferred to the `std::function` conversion" —
but P10 removed `std::function` from the slot path entirely. `Signal::connect()` is a template now
and stores the callable by its own type, so the conversion this item complained about no longer
happens. Whatever is left to gain here needs restating against the current code before it is used
as an argument for anything.

And the costs, which are not small:

- A build-time tool that every consumer of the library must run, and a waf feature to drive it.
  Anyone using the library with CMake or plain make now needs an equivalent.
- Generated code that clangd and the debugger have to be taught about.
- A parser. `QT_MIMIC_SIGNAL void f(const std::map<int, std::string>& x = {});` will find every
  weakness in a regex-based scan. moc is a real C++ parser for a reason. Restricting the accepted
  grammar aggressively — and *failing loudly* on anything outside it, never silently skipping — is
  the only version of this that is safe.
- It is the largest architectural change proposed for this project so far, and it touches every
  class that declares a signal.

Suggested shape if it goes ahead:

1. Decide the grammar first, in writing, and make the tool reject everything else with a clear error
   and a non-zero exit. Start with: void return only, no default arguments, no templates, no
   function-pointer parameters. Grow it only when a real case demands it.
2. Keep the runtime dumb. The generator should emit calls into a small hand-written runtime
   (`emitSignal(index, args...)`) that lives in normal source and is normally testable. Do not put
   logic in the generated text; put it in the runtime the generated text calls.
3. Signals-as-member-functions is the point of the exercise. If the generator produces something
   that still requires a `Signal<>` member, it has bought the costs above and none of 4a or 4b.
4. Do it in QtLikeSignal only, and only after 3a, so the two are not confounded. Keep QtMimic on the
   plain `Signal<>` as the control: having one library of each kind is worth more than having two of
   the same.

### 5. Recommendation

**Original, 2026-08-12:** do 3a, then decide about the generator on the strength of 4a and 4b alone.

**3a is done, and it did what it promised.** Direct emit is now *ahead* of Qt 6, queued cross-thread
is at parity, and there is no allocation left on the emit path to blame. So the performance case for
a code generator is not merely unproven now — it is closed. Nothing moc does would move any row in
the summary table.

What is left to decide is unchanged in substance, and should be decided on its own merits:

- **4a, memory per object**, is the strongest surviving argument, and it got slightly stronger:
  `Impl` now also carries a published-list pointer, a tombstone count and a dirty flag on top of the
  mutex and vector. Ten signals on a class still cost ten of those against Qt's one pointer.
- **4b, ergonomics** — `connect(&Sender::valueChanged, ...)` taking a member-function pointer —
  is unchanged.
- **4c has weakened** and should not be counted until it is restated.

Against the costs in section 4, which have not shrunk. The two conditions in section 4's suggested
shape still apply: decide the grammar first and fail loudly outside it, and keep QtMimic on the
plain `Signal<>` as the control. The "only after 3a" condition is now satisfied.

**One thing to re-check before acting on any of this.** These numbers came from a WSL2 session on a
machine doing other work, and `connect()` in particular swung 106–181 ns for Qt 6 alone across six
runs. The emit rows were stable to a nanosecond or two and can be trusted; treat `connect()` as
"comparable" and nothing finer.
