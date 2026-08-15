# Mission: replace boost::signals2 with our own Signal

We want to get rid of boost::signal2.
We want to implement our own.

The current set of public functions of `Signal<>` is enough.

---

## PLAN (written 2026-08-12, no implementation done yet)

This doc is for AI. Read it before touching `Signal.hpp` / `Signal.hpp`.

### 0. Why this is smaller than it looks, and where the danger actually is

The surface we use from boost is tiny. The whole dependency is:

| boost entity | what we use |
|---|---|
| `boost::signals2::signal<void(Args...)>` | `connect()`, `operator()()`, `disconnect_all_slots()`, `num_slots()`, `empty()` |
| `boost::signals2::connection` | `disconnect()`, `connected()`, `operator==`, default-construct, copy |
| `connect_extended()` | QtMimic's `connectReflective()`: the slot also receives its own connection |

That is it. Six functions and one handle type, in exactly two files — `src/QtLikeSignal/Signal.hpp` and
`external/QtMimic/src/QtMimic/Signal.hpp` — plus the `using Connection = boost::signals2::connection` in
`src/QtLikeSignal/Global.hpp` and QtMimic's `Signal.hpp`.

So the typing is not the problem. The problem is that boost is currently supplying four
*guarantees* that the rest of both libraries quietly lean on, and three of them are not obvious
from the API. Getting those wrong will not fail to compile. It will produce a use-after-free under
load, months later. Read section 2 before writing any code.

### 1. What has to be preserved

Public API, which the mission statement says is already enough. Keep every one of these, with the
same names and semantics:

```
Signal<Args...>
    connect( callable )                       -> Connection   template, not std::function -- see 1c
    connectReflective( callable )             -> Connection   (QtMimic only, see below)
    emit( args... )                           perfect-forwarding, NOT by value -- see 1a
    operator()( args... )
    disconnect( const Connection& )           (QtLikeSignal only; a forwarder)
    disconnectAll()
    receivers()                               -> size_t
    empty()                                   -> bool
    view()                                    -> SignalView<Args...>&

SignalView<Args...>                           subscription-only window onto a Signal.
    Private connect(), friend Object. Copy-constructible. Holds a reference, not a copy.

Connection
    disconnect(), connected(), operator==, default-construct, copy.
    Copies must refer to the same connection, and a handle is one pointer: the node it
    names carries both the live flag and the Signal, and is itself the receiver's
    list element. See 1d.
```

**1a.** `emit()` must forward, never take `Args...` by value. Taking by value cost one copy of every
argument per emit before boost had even seen them; it was fixed on 2026-08-11 and
`ObjectTest.DeepArgumentCopying_QueuedEventsMinimizeCopies` is what caught it. Do not regress it.

**1b.** `connectReflective()` exists only in QtMimic, where `Object::connectImpl()` uses it. Check
whether it is still needed before reimplementing it — QtLikeSignal does the same job without it. If
it is not needed, delete it and let the two Signal implementations converge, which is worth more
than the feature.

**1c.** `connect()` takes the callable as a template parameter, not as a
`std::function<void(Args...)>`. Keeping its concrete type lets the slot hold it by value inside its
own allocation, which is one heap block rather than two — the emit-time wrapper `Object::connect()`
builds is far past any small-object buffer. If you type-erase at this boundary you put that block
back. See P10 in `history/PERFORMANCE-20260813.md`.

**1d.** A `Connection` is a single `shared_ptr` to the connection node, and the node carries the
`weak_ptr` to the Signal rather than the handle carrying it. That is not tidiness: the receiver's
list of incoming connections is threaded through the nodes themselves, so `~Object()` walks nodes,
not handles, and has to reach each one's Signal from the node. Two connections cost two heap blocks
in total — one node and one slot each — with nothing allocated for the receiver's list. A design
that keeps the receiver's side in a container instead pays a third block per connection and makes
ending K connections into one receiver O(K²).

### 2. The four guarantees boost is silently providing

These are the reason to be careful. Each one has a comment in the tree explaining it; go and read
the comment before deciding your implementation satisfies it.

**2a. A slot stays alive for the whole of its invocation, even if it is disconnected mid-call.**
boost does this with `connection_body`'s `m_slot_refcount`. We do it by holding the emit snapshot's
`shared_ptr` to the slot across the call. It matters because the slot owns the captured
`shared_ptr<Affinity>` that the queued path dereferences on every emit.
Test: `ObjectTest.KamikazeSlot_DisconnectsItselfDuringEmissionWithoutCrashing`.

**This is why the slot cannot live in the same allocation as the connection node.** The node is what
a `Connection` handle holds, so it outlives the slot; the slot must be released the moment the
connection ends, or a caller keeping a handle in order to disconnect later pins everything the slot
captured. Two lifetimes, two allocations — Qt splits them the same way, into
`QObjectPrivate::Connection` and `QSlotObjectBase`.
Test: `SignalTest.SlotSurvivesDisconnectingItselfMidCall`.

**2b. Emission does NOT hold the signal's lock while calling slots.**
boost copies the connection list and releases the signal mutex before invoking anything.
Everything downstream assumes this: a slot may connect, disconnect, emit the same signal again,
post to another thread, or destroy an object — all of which reach back into the signal or into
`Object`'s own mutexes. Hold a lock across invocation and you will deadlock immediately.
Read the long comment on QtMimic's `Affinity` class in `ThreadData.hpp`; it documents this property
and the ASan-confirmed use-after-free that motivated the current design.

**2c. The consequence of 2b, which is the dangerous one: `disconnect()` does NOT wait for an
in-flight emit.** `~Object()` → `disconnect()` returns immediately while another thread is still
inside the slot. This is *why* `Object` captures a `shared_ptr<Affinity>` and a weak life token
instead of touching the receiver. If your implementation makes `disconnect()` block until in-flight
calls finish, you will have changed a documented invariant — possibly for the better, but every
comment about why the `Affinity` box exists becomes wrong, and you must update them rather than
leave them lying.

**2d. A connection made during an emission does not run in that emission, and `disconnectAll()`
during an emission aborts the slots not yet reached.**
Tests: `ObjectTest.ChainReaction_NewConnectionDoesNotRunInCurrentEmission`,
`ObjectTest.TheNuke_DisconnectAllDuringEmissionAbortsRemaining`.
A snapshot-then-invoke design gives 2d's first half for free and needs explicit work for the
second: iterating a snapshot must still re-check each connection's connected flag before calling it.

### 3. Suggested design

Nothing exotic is needed. The shape that satisfies section 2:

```cpp
struct ConnectionState                      // one per connection, heap, shared_ptr
{
    std::function<void(Args...)> mSlot;
    std::atomic<bool>            mConnected;
};

// Signal holds:      std::mutex, std::vector<std::shared_ptr<ConnectionState>>
// Connection holds:  std::weak_ptr<ConnectionState>
```

| operation | behaviour |
|---|---|
| `connect()` | lock, `push_back`, return a `Connection` wrapping a `weak_ptr` |
| `disconnect()` | lock the `weak_ptr`; if it is still there, clear `mConnected` and drop it from the vector. Does not wait for in-flight calls — see 2c |
| `connected()` | lock the `weak_ptr` and read `mConnected` |
| `operator==` | compare the `weak_ptr`s with `owner_before` both ways, or store a `shared_ptr` in the `Connection` instead if that is simpler to get right |
| `emit()` | copy the vector under the lock, release the lock, then for each entry re-check `mConnected` before invoking. The `shared_ptr` copy is what keeps the slot alive through the call (2a); the re-check is what makes 2d work |

The per-emit vector copy is the obvious cost. Do not optimise it away before measuring — see
section 5. If it does show up, the usual answer is a small-vector or a copy-on-write list, not
holding the lock longer.

Watch the destruction order: `~Signal()` must mark every state disconnected, and it must be safe for
a `Connection` to outlive its `Signal` — `Object::mIncoming` routinely holds handles to signals that
have already gone away.

### 4. Order of work

Do it in QtLikeSignal first, all the way to green, and only then port to QtMimic. The two Signal
files are near-identical and the second one takes an hour once the first is proven.

1. Write the new `Signal<>`/`SignalView<>`/`Connection` behind the existing names, in a new header,
   not yet wired in.
2. Unit-test it directly against section 2's four guarantees, before any integration. These are the
   tests that will actually find the bugs; the existing suite exercises them only indirectly.
3. Swap `src/QtLikeSignal/Signal.hpp` and `src/QtLikeSignal/Global.hpp` over. Build. Expect the failures to be concentrated in the
   stress suite.
4. Fix. Then run the whole suite under BOTH sanitizers — `./waf configure
   --enable-thread-sanitizer-on-Linux` and the default AddressSanitizer build. A signal
   implementation is exactly the kind of code where one sanitizer sees nothing and the other
   screams.
5. Port to QtMimic. `tools/compare-test-suites.sh` must still pass; the two Signal headers should
   end up as close to identical as the rest of the tree already is.
6. Delete the boost submodule reference from the build if nothing else uses it. Check first — grep
   the wscripts.

### 5. Two things that will change, and must be handled honestly

**5a. The copy-count tests will change and that is expected.**
`ObjectTest.DeepArgumentCopying_QueuedEventsMinimizeCopies` asserts
`copyCount == numReceivers + 2`, where the `+ 2` is boost's own internal copies. With our own
implementation that constant will be different — very likely `numReceivers + 0`.
Do NOT simply relax the assertion to a range. Work out what the correct number is for the new
implementation, assert exactly that, and update the comment to explain where each copy comes from.
The whole value of that test is that it is exact.

**5b. Performance must be measured, not assumed.**
`src/tests/test_QtLikeSignal_Performance.cpp` compares against Qt 6 and against QtMimic. Take a
baseline BEFORE starting and compare after. History says the direct-emit path is the one that
matters and that it is easy to regress by accident — see `history/PERFORMANCE-20260808.md`, and note
the measurement traps recorded there: never benchmark the `-O0` sanitizer build, and settle the
allocator state first.
A plausible outcome is that our own implementation is *faster* than boost on the direct path,
because boost pays for features we do not use (combiners, slot groups, extended slots,
`shared_connection_block`). If it is slower, find out why before accepting it.

### 6. What NOT to do

- Do not implement combiners, slot groups, ordering/priority, `scoped_connection`, or
  `shared_connection_block`. Nothing in either library uses them. The mission statement is explicit
  that the current public API is enough.
- Do not make `Signal<>` non-copyable-but-movable or otherwise change its value semantics; it is a
  member of user classes and of `Thread`/`Timer`.
- Do not change `emit()` to take arguments by value (see 1a).
- Do not hold the signal lock across slot invocation (see 2b). This is the single most likely way to
  get this wrong.
