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
   `/home/evan/Projects/qtbase-everywhere-src-6.11.1`

3. Append the generated files to source list.

---

## PLAN (written 2026-08-12, no implementation done yet)

This doc is for AI. Read all of it, especially section 2, before writing a generator.

### SUMMARY, so nobody has to read to the end to get the point

The survey was done and the benchmarks were run. The conclusion is:

> **Do not build the code generator for speed. Our signal is already as fast as Qt's.**

Direct emit, measured on one machine, `-O2`, no sanitizer, same harness shape:

| library | direct emit | |
|---|---|---|
| Qt6 | ~29 ns | |
| QtLikeSignal (own signal) | ~32 ns | ← already at parity |
| QtLikeSignal (boost) | ~74 ns | ← what we replaced |

The remaining gap is one heap allocation per emit, measured at exactly 1.000 allocations/emit
against Qt's 0.000. That is the snapshot vector in `Signal::Impl::emit()`. Removing it is an
afternoon of ordinary work on `Signal.h` — see section 3 — and a code generator would not remove it,
because the allocation has nothing to do with type erasure or with how signals are declared.

Where we ARE slow is somewhere moc does not help either:

| scenario | Qt6 | ours | |
|---|---|---|---|
| auto connection, same thread | ~29 ns | ~65 ns | 2.2x slower |
| `connect()` | ~109 ns | ~343 ns | 3.1x slower |

The first is `Object`'s affinity resolution running on every emit, which is the long-standing P2/R25
item in `history/PERFORMANCE-20260808.md`, not a signal problem at all.

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

All figures below: `-O2`, no sanitizer, one process per measurement, allocator settled with a
throwaway thread first, order rotated, best-of-several. The benchmark sources are in the scratch
directory of the session that produced this document; recreate them from the shapes described here
rather than trusting a stale copy.

| scenario | Qt6 | ours (own) | ours (boost) |
|---|---|---|---|
| direct emit | ~29 ns | ~32 ns | ~74 ns |
| auto connection, same thread | ~29 ns | ~65 ns | ~106 ns |
| `connect()` | ~109 ns | ~343 ns | ~625 ns |
| allocations per emit | 0.000 | 1.000 | (not measured) |

```
raw Signal::emit(), no Object::connect wrapper:  ~30 ns
```

That last line is the one that decides this whole question. Of the 32 ns a direct emit costs, about
30 is the signal machinery itself and about 2 is `Object`'s wrapper. So there is no fat in the
`Object` layer to cut on the direct path, and the signal is within ~10% of Qt already.

And the 30 ns is dominated by one thing: `Signal::Impl::emit()` copies the connection list into a
local vector under the lock, which heap-allocates. Once per emit, regardless of how many slots are
connected — measured, not inferred.

**A trap, recorded so the next person does not fall into it.** `waf --mode=release` is `-O2` *with
ThreadSanitizer*. Running the existing performance suite there reports QtLikeSignal at 577 ns
against Qt6 at 38 ns, because Qt6 is a prebuilt uninstrumented library and our code is instrumented.
Those published ratios are meaningless. Either build the perf suite with no sanitizer, or do not
compare against Qt at all. `history/PERFORMANCE-20260808.md` already warns about benchmarking
sanitizer builds; this is the same trap wearing a different hat, because "release" sounds like it
should be safe.

### 3. What to do instead, if the goal is speed

In priority order. None of these needs a code generator.

**3a. Remove the per-emit allocation.** Expected to take direct emit to or below Qt's ~29 ns.
Copy-on-write: have `Impl` hold `std::shared_ptr<const std::vector<std::shared_ptr<Slot>>>`.
`emit()` copies the `shared_ptr` — one atomic increment, no allocation — and iterates it with no
lock held. `connect()`/`disconnect()` build a new vector and swap the pointer under the lock.
Writers pay an allocation instead of readers, which is the right way round: emits vastly outnumber
connects.
Keep every guarantee in `ForAI/mission-signal.md` section 2 intact, and keep
`QtLikeSignal-test-signal.cpp` green — it was written to pin exactly these.

**3b. The auto-connection path**, ~65 ns against Qt's ~29. This is `Object::connectImpl()`'s wrapper
resolving affinity on every emit: `Affinity::data()` takes a mutex and returns a `shared_ptr` copy,
then compares against the current thread. It is item P2/R25 in `history/PERFORMANCE-20260808.md` and
it has been open a while. An atomic load in place of the mutex is the obvious move; the reason it
has not been done is that `moveToThread()` has to publish safely against it.

**3c. `connect()` at ~343 ns against Qt's ~109.** Three heap allocations per connect (`Slot`,
`ConnectionState`, and the vector growth), plus `Object::connectImpl` building a `Cleanup` token and
taking `mIncomingMutex`. Worth a look only after 3a and 3b; connect is not a hot path.

### 4. If the generator is built anyway, build it for these reasons

There are real, non-performance arguments for it, and they should be the stated goal:

**4a. Memory per object.** Every `Signal<Args...>` member today is a `shared_ptr` plus a
`SignalView` plus an `Impl` holding a mutex and a vector. A class with ten signals pays ten of
those, whether or not anything is connected. Qt pays one pointer per object no matter how many
signals it declares. For a library aimed at embedded-ish use this is a stronger argument than speed.

**4b. Ergonomics.** `QT_MIMIC_SIGNAL void valueChanged(int)` reads like Qt, and it would let
`connect(&Sender::valueChanged, ...)` take a member-function pointer the way Qt's does, instead of
requiring the caller to name a member object, `sender.valueChanged`.

**4c. Compile-time signature checking** at the connect site, which today is partly deferred to the
`std::function` conversion.

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

Do 3a. It is small, it is measured, and it very likely puts us at or ahead of Qt on the path that
matters most.

Then decide about the generator on the strength of 4a and 4b alone, with the understanding that it
will not make emitting faster than 3a already does.
