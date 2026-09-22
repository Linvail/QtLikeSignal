# QtLikeSignal

A C++17 library that mimics part of QtCore: `Object`, `Signal`, `Thread`, `Timer`, `Event`,
`CoreApplication` and the event dispatchers behind them. Signals and slots connect across threads,
delivery is decided at emit time the way Qt decides it, and an object's lifetime is tracked so a
queued call to a destroyed receiver is dropped rather than delivered.

No moc, no code generation, no Qt headers, no Boost. `Signal` began as a wrapper over
`boost::signals2` and is now its own implementation; nothing under `src/` includes a Boost header,
and the submodule that used to bootstrap them is gone.

`QtLikeSignalGui` sits on top of it: windows and input, with a backend per window system.

## Repository layout

| path | what it is |
|---|---|
| `src/QtLikeSignal/` | the library -- event loop, threads, timers, signals, logging, properties, animation |
| `src/QtLikeSignalGui/` | windows and input over it, one backend per window system |
| `src/QtLikeSignalDebug/` | an optional library for what QtLikeSignal itself cannot link: a thread's call stack |
| `src/tests/` | the correctness suite (GoogleTest), one binary covering both libraries |
| `src/demo/` | demo programs, one per window system |
| `src/perf/` | the benchmarks and the regression guards |
| `tools/` | the waf tools: toolchains, sanitizers, the Wayland scanner rule |
| `submodules/external/` | waf and googletest, as submodules |

Each part under `src/` has its own wscript beside its own sources; `src/wscript` recurses into all
six.

The GUI tests live in the same binary as the rest rather than in one of their own, because most of
what they cover -- the input types, platform selection, the null-window guards -- is portable and
would otherwise need a second test program that existed only on Windows.

## Prerequisites

- **Python 3** -- runs the `waf` build tool.
- **A C++17 compiler** -- MSVC on Windows, clang or gcc on Linux.
- **The submodules** -- `git submodule update --init`, which fetches waf and googletest.

Everything below is optional and only widens what gets built. Each is detected at configure time
through `pkg-config` and skipped with a message if absent, so a bare machine still builds the
library and the whole test suite -- it just cannot open a window.

| optional (Debian/Ubuntu) | enables |
|---|---|
| `libx11-dev` | the X11 backend |
| `libinput-dev`, `libudev-dev` | the DRM/libinput backend |
| `libwayland-dev`, `libwayland-bin`, `wayland-protocols`, `libdecor-0-dev` | the Wayland backend |

`src/QtLikeSignalDebug/` needs no package. On Windows it loads `dbghelp.dll` when it runs, and on
Linux it links `libdl`. The build makes it for Windows and Linux only.

The build reports what it got:

```
QtLikeSignalGui backends                 : x11, drm, wayland
```

## Building

```bash
./waf configure
./waf install --project=Tests
```

On Windows use `waf` (or `python waf`) in place of `./waf`; from Windows against WSL, prefix with
`wsl python3`.

`Tests` is the only project. The default mode is `debug`; add `--mode=release` for an optimised
build, and `--toolchain` to pick a specific one (`--toolchain=?` lists them):

```bash
./waf install --project=Tests --mode=release --toolchain=linux64-gcc
```

### What you get

Everything lands in `install/Tests/<toolchain>/<mode>/usr/bin/`. The test suite is always built:

```
install/Tests/linux64-clang/debug/usr/bin/QtLikeSignal-test
install/Tests/win64-msvc/debug/usr/bin/QtLikeSignal-test.exe
```

The benchmarks come with it, as `QtLikeSignal-Performance-Tests`. Its own numbers always build; the
Qt 6 and boost::signals2 comparisons printed beside them are added only where those libraries are
found, and skipped with a message naming where the build looked when they are not.

The demo programs are built for whatever window system the target has -- `QtLikeSignal-Demo-X11`,
`QtLikeSignalGui-Demo-X11` and `QtLikeSignalGui-Demo-Wayland` on Linux, `QtLikeSignal-Demo-Windows`
and `QtLikeSignalGui-Demo` on Windows -- and skipped with a message where the headers are missing.
The `QtLikeSignalGui-Demo-*` ones are written against the library and own no event loop; the
`QtLikeSignal-Demo-*` ones own their window and their window system and hand the connection over,
which is the position a program with an existing toolkit is in.

### Build release as well as debug before trusting a result

The two are not interchangeable for a threaded library. A defect that dropped every queued call
posted during thread startup passed the whole debug suite on both platforms and only ever failed in
release, because debug was slow enough to close the window it needed. If you run one configuration,
run release too.

## A first program

```cpp
#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"

#include <cstdio>

//! Emits on whatever thread ticks it.
class Producer : public QtLikeSignal::Object
{
public:
    QtLikeSignal::Signal<int> valueChanged;
};

//! Receives on the thread it lives in, whichever thread emitted.
class Consumer : public QtLikeSignal::Object
{
public:
    void onValue( int aValue )
    {
        std::printf( "got %d\n", aValue );
        QtLikeSignal::CoreApplication::quit();
    }
};

int main()
{
    QtLikeSignal::CoreApplication app;

    Producer producer;
    Consumer consumer;

    QtLikeSignal::Thread worker;
    consumer.moveToThread( &worker );
    worker.start();

    // Auto: same thread calls directly, another thread queues to the receiver's loop.
    QtLikeSignal::Object::connect( producer.valueChanged, &consumer, &Consumer::onValue );

    QtLikeSignal::Timer timer;
    timer.setInterval( 10 );
    QtLikeSignal::Object::connect( timer.getTimeout(), &producer,
        [&producer] { producer.valueChanged.emit( 42 ); } );
    timer.start();

    const int result = app.exec();

    worker.quit();
    worker.wait();
    return result;
}
```

`connect()` returns a `Connection` you can keep and `disconnect()`, and takes a `ConnectionType` --
`Auto` (the default), `Direct` or `Queued`. `Auto` is Qt's rule: direct when the receiver lives in
the emitting thread, queued otherwise. A signal a class hands out publicly should be a `SignalView`,
as `Timer::getTimeout()` is, so a caller can subscribe but cannot emit.

## Parent-child ownership

An `Object` may own other `Object`s. Give one a parent and the parent destroys it; destroy the
parent and the whole subtree goes with it. This is Qt's design and it is the same trade: no
`unique_ptr` threaded through every signature, and no help from the type system either.

```cpp
QtLikeSignal::Object window;
auto* button = QtLikeSignal::Object::createChild<Button>( &window, "OK" );
auto* label  = QtLikeSignal::Object::createChild<Label>( &window );
// window's destructor destroys button and label.
```

Every rule below is pinned by a test in
[`src/tests/QtLikeSignal-test-parent-child.cpp`](src/tests/QtLikeSignal-test-parent-child.cpp). If
one of them ever stops being true, that suite fails.

### The one rule that matters

**A child must have exactly one owner, and if it has a parent, that owner is the parent.**

Everything else follows from it.

### Do

**Attach at construction.** `Object` takes an optional parent, exactly as `QObject` does:

```cpp
QtLikeSignal::Object window;
Button okButton( &window );        // owned by window
```

**For a heap child, prefer `createChild<T>()`.** It allocates, forwards constructor arguments and
attaches in one step, so the storage duration a parent requires is right by construction rather than
by care:

```cpp
auto* child = QtLikeSignal::Object::createChild<Child>( parent, args... );
```

**`Object` does not take a `Thread*`.** Passing one is a compile error rather than silently meaning
"make that Thread my parent" -- `Thread` derives from `Object`, so the mistake would otherwise
compile. To choose where an object lives, build it and push it:

```cpp
QtLikeSignal::Object object;
object.moveToThread( &worker );
```

**Delete a child directly, if you want to.** This is safe, and it is worth saying plainly because
"the parent owns its children" suggests otherwise:

```cpp
delete child;          // fine: the child unlinks itself from the parent first
child->deleteLater();  // also fine
```

`~Object()` detaches from the parent before it does anything else, so by the time the parent's own
teardown runs, the child is simply not in its list. There is no double free. It works in the other
order too: if the parent is destroyed while a `deleteLater()` on the child is still queued, that
pending delete is stripped rather than left to fire at freed memory.

**Re-home with `setParent()`,** including `setParent( nullptr )` to detach. A detached object is
yours again and you must delete it yourself.

**Check the return value of `setParent()`.** It returns `false` and *changes nothing* when the link
is refused -- a cycle, a parent that is being destroyed, or a parent in another thread. Qt refuses
the same links but leaves the object with no parent at all; this keeps the parent it had, which is
the entire reason the function returns a `bool`.

### Do not

**Do not put a parented object in a smart pointer.** This is the trap:

```cpp
auto child = std::make_unique<Child>();
child->setParent( parent );      // WRONG: now two things intend to delete it
```

Whether it crashes depends on destruction order, which is exactly what makes it worth avoiding. If
the `unique_ptr` goes first, the child unlinks itself and the parent never sees it -- no crash, and
the bug stays hidden. If the parent goes first, the child is destroyed, and the `unique_ptr` then
frees the same memory again. Same code, and the failure is decided by which scope ends first.

If you want smart-pointer ownership, do not give the object a parent. Pick one.

**Do not give a parent to an object that is not on the heap** -- unless it is certain to die first:

```cpp
Object parent;
Object child;
child.setParent( &parent );   // works *only* because child is destroyed first here
```

The constructor form is safer here than `setParent()`, and not by style: `Object child( &parent )`
requires the parent to already exist, so within a block the parent is declared first and therefore
destroyed *last*, and the child unlinks itself on the way out. `setParent()` lets the declarations
go the other way round, and then the parent calls `delete` on automatic storage. Nothing can check
this for you: standard C++ cannot ask whether a pointer names automatic or dynamic storage. Qt
documents the same hazard and warns rather than preventing it. `createChild<T>()` exists so the safe
path is also the short one.

**Do not move a child to another thread on its own.** `moveToThread()` refuses it:

```cpp
child->moveToThread( &worker );   // returns false, changes nothing
parent->moveToThread( &worker );  // moves the whole subtree, which is what you want
```

A child lives in its parent's thread, and the tree's pointers are deliberately unguarded because of
that invariant. If you really want to move one node, `setParent( nullptr )` first.

**Do not attach a parent from another thread.** `setParent()` refuses that too, and keeps the
existing parent.

### Thread rules, in one place

- A child lives in its parent's thread. Always.
- Move the parent; the subtree follows in one call.
- The tree is not locked. It is thread-confined, which is stronger and free: only the thread the
  subtree lives in may read or change any of it. Qt guards `parent`/`children` the same way, with an
  invariant rather than a mutex, which is why neither library carries a per-object lock for it.
- `parent()`, `firstChild()`, `nextSibling()`, `lastChild()`, `previousSibling()`,
  `childCount()`, `stackBefore()`, `stackAfter()`, `findChild()`, `findChildren()` and
  `setParent()` are all subject to that: call them from the thread the objects live in.

### The order of the children

The children keep the order in which they were attached, as `QObject::children()` does. A parent
also destroys its children in this order. `firstChild()` and `nextSibling()` walk the children
from the first to the last. `lastChild()` and `previousSibling()` walk them from the last to the
first.

`stackBefore()` and `stackAfter()` move a child to a different position in this order. Thus a
widget toolkit can use the order as its stacking order, as `QWidget` does. The toolkit calls
`setWidgetType()` on each widget. Then `isWidgetType()` tells which children are widgets, so the
toolkit can skip the other children, for example a `Timer`, without a `dynamic_cast`.

### Finding things

```cpp
auto* ok    = window.findChild<Button>( "ok" );   // first match, whole subtree, depth-first
auto  every = window.findChildren<Button>();      // all matches
window.dumpObjectTree();                          // the subtree, to stderr, for debugging
```

An empty name matches any name, as `QObject::findChild()` treats a null `QString`. Both searches are
O(subtree) with a `dynamic_cast` per node and no index anywhere -- the same plain recursive scan Qt
does. Fine for a dialog, wrong for a large model: treat them as wiring and diagnostic conveniences,
not something to call in a loop.

The child list is intrusive, so attaching costs no allocation and both attaching and detaching are
O(1) -- where Qt's `QList<QObject*>` makes removing one child O(siblings), and destroying N children
individually quadratic. The tree's pointers live in a lazily-allocated box that an object outside
any tree never allocates, so joining a tree is free for an object that has already been named or has
already run a timer.

## Reading a thread's call stack

`QtLikeSignalDebug` is a library of its own, which a program opts into by linking it. It has one
class, `CallStack`, which reads the call stack of a thread -- the calling thread, or another thread
that is stuck, which is what a watchdog wants.

The thread that you watch makes a `CallStack::Target` for itself, one time. Whoever watches it
reads the stack when it decides the thread is stuck:

```cpp
// On the thread that you watch, for example in the first task that you post to it:
QtLikeSignal::CallStack::Target target = QtLikeSignal::CallStack::Target::currentThread();

// On the watching thread:
const QtLikeSignal::CallStack stack = QtLikeSignal::CallStack::capture( target );
qCCritical( gLogApp ) << stack.toString();
```

`toString()` writes one line for each frame. Where symbols are available, each line has the
function name, and on Windows also the source line.

On Linux a capture sends the signal `SIGRTMIN + 5` to the thread, so do not use that signal for a
different purpose, and link the program with `-rdynamic`, or the stack does not show the names of
the program's own functions. `src/QtLikeSignalDebug/CallStack.hpp` says what each platform does to
a thread while it reads the stack.

## Compiler configuration

The build promotes **one** compiler diagnostic to an error: discarding the return value of a
`[[nodiscard]]` function.

| toolchain | flag |
|---|---|
| MSVC | `/we4834` |
| clang / gcc / mingw | `-Werror=unused-result` |

Set in [`tools/toolchain-windows.py`](tools/toolchain-windows.py) and
[`tools/toolchain-linux.py`](tools/toolchain-linux.py). If you build this project outside `waf`,
carry these across.

**Why this one, and why not `/WX` or `-Werror`.** A warning-free build is not a goal here, and
making every warning fatal lets an unrelated one block work that has nothing to do with it. This
diagnostic is different in kind. `[[nodiscard]]` is written only where discarding the result loses
something the caller needed: `Thread::post()` returning `false` means the task **will never run**.
A call that drops that answer is a defect at the call site, and the compiler is the only thing that
sees every call site.

If you genuinely do not care whether a task ran, say so explicitly:

```cpp
(void)QtLikeSignal::CoreApplication::post( [] { ... } );   // and a comment saying why
```

The point is that the decision appears in the source rather than being implied by its absence.

## Sanitizer builds

Add `--enable-asan=yes` or `--enable-tsan=yes` to a `waf install` command:

```bash
./waf install --project=Tests --enable-asan=yes
./waf install --project=Tests --enable-tsan=yes
```

The two are mutually exclusive. If both are given, AddressSanitizer wins and ThreadSanitizer is
dropped. `--enable-tsan` is not supported on Windows with MSVC. The suite must stay at zero findings
under either, and needs no suppression file.

**Known issue:** on this project's WSL2 setup, `linux64-gcc` combined with `--enable-tsan` is
unreliable -- it can hang or report spurious data races. Use `linux64-clang` for ThreadSanitizer
testing until this is resolved.

## Running the tests

Run the binary directly. GoogleTest flags work as usual.

```bash
./install/Tests/linux64-clang/release/usr/bin/QtLikeSignal-test
```

```powershell
.\install\Tests\win64-msvc\release\usr\bin\QtLikeSignal-test.exe
```

Filtering:

```bash
# every test in one suite
./install/Tests/linux64-clang/release/usr/bin/QtLikeSignal-test --gtest_filter=ThreadTest.*

# the regression suites for previously fixed defects
./install/Tests/linux64-clang/release/usr/bin/QtLikeSignal-test --gtest_filter=*DefectTest.*

# repeat, for flushing out races
./install/Tests/linux64-clang/release/usr/bin/QtLikeSignal-test --gtest_repeat=10
```

The suites are `ObjectTest`, `SignalTest`, `ThreadTest`, `TimerTest`, `CoreApplicationTest`,
`ParentChildTest`, `ThreadAdoptionTest`, `ThreadPriority`, `ObjectTimerTest`, `TimerSingleShotTest`,
`ThreadTimerTest`, `ObjectArgumentCopyingTest`, the `Gui*Test` suites, the `EventDispatcher*Test`
ones, and the `*DefectTest` regression suites.

Which of them exist depends on the platform and on the backends the build found:
`EventDispatcherWin32Test` and `GuiWindowTest` are Windows-only, `EventDispatcherLinuxTest`,
`GuiX11Test`, `GuiWaylandTest` and `GuiDrmTest` are Linux-only, and a test whose backend is missing
reports `SKIPPED` rather than failing.

## Licence

MIT. See [LICENSE](LICENSE).
