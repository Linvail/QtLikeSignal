// Regression tests for the defects found in the code review of technology/G* (see CHANGES.md /
// the accompanying patch). Kept separate from test_gobject.cpp / test_gthread.cpp / test_gtimer.cpp
// since these specifically target crash/UAF/leak/race scenarios rather than day-to-day API
// behavior, and several of them are stress tests rather than single-shot deterministic checks --
// see each test's doc comment for what it actually proves and how to get the strongest signal
// out of it (most benefit from being run under AddressSanitizer and/or ThreadSanitizer; this
// project's default debug build already enables AddressSanitizer, see tools/toolchain-linux.py).
#include <gtest/gtest.h>
#include "Object.h"
#include "Thread.h"
#include "Timer.h"
#include "Signal.h"
#include "EventDispatcherDefault.h"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace QtLikeSignal;

// ---------------------------------------------------------------------------------------------
// Defect: Thread::start() could call std::terminate().
// ---------------------------------------------------------------------------------------------

//! Minimal Thread subclass whose run() returns immediately, used to reliably reach the
//! "finished but never waited on" state needed by RestartAfterFinishWithoutWaitDoesNotTerminate.
class DefectInstantFinishThread : public Thread
{
protected:
    //! Returns immediately so the thread reaches the finished state quickly.
    virtual void run() override
    {
    }

};

//! Regression test for Thread::start() overwriting the previous run's OS thread when restarted
//! after that run finished but wait() was never called.
//!
//! A finished thread is not a reaped one: its handle is still open (Windows) or still joinable
//! (pthreads) until someone waits on it. start() used to overwrite it regardless, which back when
//! the backing store was a std::thread meant destroying a joinable one -- std::terminate(),
//! aborting the whole test process rather than failing this test. Against the OS thread APIs the
//! same defect leaks the thread instead, quietly. Either way the fix is the same: start() reaps
//! the previous run first. Fully deterministic.
TEST( ThreadDefectTest, RestartAfterFinishWithoutWaitDoesNotTerminate )
{
    DefectInstantFinishThread thread;

    thread.start();

    // Poll isFinished() without ever calling wait(), so mThread is left holding a joinable
    // std::thread when we restart below.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
    while( !thread.isFinished() && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    ASSERT_TRUE( thread.isFinished() )
        << "thread did not finish in time to set up the regression scenario";

    // Before the fix, this line calls std::terminate() and aborts the whole test process rather
    // than failing gracefully -- that abort *is* the failure signal for this test.
    thread.start();

    EXPECT_TRUE( thread.wait( 2000 ) );
    EXPECT_TRUE( thread.isFinished() );
}

// ---------------------------------------------------------------------------------------------
// Defect: use-after-free in EventDispatcherDefault::processEvents()'s dispatch loop when a
// DeferredDeleteEvent and another event for the same receiver land in the same drained batch.
// ---------------------------------------------------------------------------------------------

//! Minimal receiver used only to give processEvents() a second event type to dispatch to
//! the same receiver that a DeferredDeleteEvent targets, in DeferredDeleteFollowedByQueuedEventInSameBatchDoesNotCrash.
class DefectUafTestReceiver : public Object
{
public:
    //! No-op slot; only its signature and being invoked (or not) on a live object matters.
    void onValue
        (
        int aVal  //!< Unused.
        )
    {
        ( void ) aVal;
    }

};

//! Regression test verifying EventDispatcherDefault::processEvents() does not call
//! through a receiver after it has already been deleted earlier in the same dispatched batch.
//!
//! eventsToProcess is a flat snapshot drained once per processEvents() call. If a receiver has a
//! DeferredDeleteEvent queued before another event also targeting it, and both are already
//! sitting in the queue by the time processEvents() drains it, dispatching the DeferredDelete
//! event first deletes the receiver; before the fix, the loop's next iteration then called a
//! virtual function through the now-dangling pointer. removeEventsForReceiver() (invoked from
//! ~Object()) cannot help here -- it only prunes the dispatcher's live queue, not the
//! already-copied local batch.
//!
//! This is made reliable (not a race) by blocking the worker thread's event loop inside a
//! queued slot while we post both events, guaranteeing they land in the same queue/batch before
//! the worker ever gets a chance to drain it -- the same technique the existing
//! ReceiverDestroyedBeforeQueuedEventHandled test in test_gobject.cpp uses. Reaching the end of
//! this test without crashing is the assertion; run under AddressSanitizer for a hard failure
//! (heap-use-after-free) if this regresses, rather than a possibly-silent one.
TEST( EventDispatcherDefaultDefectTest, DeferredDeleteFollowedByQueuedEventInSameBatchDoesNotCrash
    )
{
    Thread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    Object dummyContext;
    dummyContext.moveToThread( &workerThread );

    Signal<> blockSig;
    Object::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        ConnectionType::QueuedConnection );

    blockSig.emit();
    blockEnteredFuture.get();

    // Worker thread is now stuck inside the blocking slot above, so anything we post next
    // accumulates in the queue and gets drained into a single processEvents() batch once we
    // release it below.
    auto* victim = new DefectUafTestReceiver();
    victim->moveToThread( &workerThread );

    // First: a DeferredDelete event for `victim` ...
    victim->deleteLater();

    // ... then a second, unrelated queued event for the SAME (about-to-be-deleted) receiver.
    Signal<int> sig;
    Object::connect( sig, victim, &DefectUafTestReceiver::onValue, ConnectionType::QueuedConnection
                   );
    sig.emit( 123 );

    // Release the worker thread; it will now drain and dispatch BOTH events in one
    // processEvents() call, deleting `victim` on the first and (pre-fix) touching the dangling
    // pointer on the second.
    blockReleasePromise.set_value();

    // Sync point: this queued event, posted strictly after both above, is guaranteed to be
    // dispatched strictly after them (single dispatcher, FIFO queue, single consumer thread), so
    // by the time workerThread.wait() returns below, the scenario above has already run.
    Signal<> quitSig;
    Object::connect(
        quitSig, &dummyContext, [&workerThread]()
        {
            workerThread.quit();
        }, ConnectionType::QueuedConnection );
    quitSig.emit();

    workerThread.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: EventDispatcherDefault::processEvents()'s wait_for() predicate ignored timer changes,
// so a newly-registered shorter timer could be starved until the stale wait computed for an
// existing, longer-interval timer happened to elapse.
// ---------------------------------------------------------------------------------------------

//! Regression test verifying that registering a short timer while processEvents() is
//! already asleep waiting on a longer-interval timer causes it to wake and re-evaluate promptly,
//! instead of sleeping out the stale wait duration computed before the new timer existed.
//!
//! Deterministic with generous margins: a 3000ms timer is registered first (letting the worker
//! thread's exec() loop settle into its long wait_for() sleep), then a 50ms single-shot timer is
//! registered and we assert it fires within 1500ms -- more than enough headroom over its actual
//! ~50ms interval, but well under the unrelated long timer's 3000ms, so the two cases are not
//! confusable even accounting for CI scheduling jitter.
TEST( EventDispatcherDefaultDefectTest, NewShorterTimerWakesPromptly )
{
    Thread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    // Configure each timer *before* handing it to the worker: once it lives there, its members
    // belong to that thread and touching them from here would be a plain data race (Timer has no
    // locking, exactly like QTimer). Arming likewise happens on the worker, via callLater(),
    // because start() is thread-confined.
    Timer longTimer;
    longTimer.moveToThread( &workerThread );
    Object::callLater( &longTimer, &Timer::start, 3000 );

    // Give the worker thread's exec() loop a chance to notice the long timer and enter its
    // ~3s wait_for() sleep before we register the short timer below.
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    Object context;
    context.moveToThread( &workerThread );

    std::promise<std::chrono::steady_clock::time_point> firePromise;
    auto fireFuture = firePromise.get_future();

    Timer shortTimer;
    shortTimer.setSingleShot( true ); // configure before the object belongs to another thread
    shortTimer.moveToThread( &workerThread );
    Object::connect(
        shortTimer.timeout,
        &context,
        [&firePromise]()
        {
            firePromise.set_value( std::chrono::steady_clock::now() );
        },
        ConnectionType::DirectConnection );

    auto shortTimerStart = std::chrono::steady_clock::now();
    Object::callLater( &shortTimer, &Timer::start, 50 );

    auto status = fireFuture.wait_for( std::chrono::milliseconds( 1500 ) );
    ASSERT_EQ( status, std::future_status::ready )
        << "short timer did not fire within 1500ms of being registered while an unrelated "
        "3000ms timer was already pending -- the dispatcher likely slept through the stale "
        "wait_for timeout instead of waking to notice the new, shorter timer.";

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        fireFuture.get() - shortTimerStart )
        .count();
    EXPECT_LT( elapsedMs, 1500 ) << "short timer fired after " << elapsedMs
                                 << "ms; expected well under the unrelated 3000ms timer's "
        "interval.";

    // Stop the still-running long timer on the thread that owns it. Leaving it to ~Timer() would
    // call stop() from this thread, which killTimer() refuses -- correct behaviour (Qt warns the
    // same way), but this test should not be the thing committing the misuse.
    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    Signal<>          stopSignal;
    Object::connect(
        stopSignal,
        &context,
        [&longTimer, &stoppedPromise]()
        {
            longTimer.stop();
            stoppedPromise.set_value();
        },
        ConnectionType::QueuedConnection );
    stopSignal.emit();
    EXPECT_EQ( stoppedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    workerThread.quit();
    workerThread.wait();
}

// ---------------------------------------------------------------------------------------------
// Defect: leaked TimerEvent allocations if interrupt() lands between
// EventDispatcherDefault::processEvents() collecting already-expired timers and its subsequent
// mInterrupt check.
// ---------------------------------------------------------------------------------------------

//! Best-effort stress test targeting the narrow window in processEvents() between
//! collecting already-expired timers into a local batch and checking mInterrupt right after.
//!
//! Before the fix, any TimerEvent objects already allocated during collection were leaked if
//! interrupt() landed in that window, since processEvents() returned early without ever handing
//! them to the dispatch loop that would otherwise delete them.
//!
//! There is no portable, non-racy way to deterministically land inside that exact window from
//! outside the class, so this runs many trials with a large number of already-expired timers (to
//! widen the collection loop's duration) racing against a concurrent interrupt() caller. The real
//! assertion this test is written to support is "no leak reported at process exit" -- this
//! project's default debug build already enables AddressSanitizer, which includes
//! LeakSanitizer on Linux (see tools/toolchain-linux.py) -- so run this as part of a normal debug
//! build/test invocation to get that signal. The check below only confirms the scenario runs to
//! completion without crashing or hanging; it cannot by itself prove the leak window was hit.
//!
//! Exposes EventDispatcherDefault::registerTimer() for this whitebox stress test.
//!
//! registerTimer() is protected and friended to Object alone, so that only Object's internals
//! (startTimer()/killTimer()) can register a timer on another object's behalf. This test needs to
//! drive it directly -- it registers thousands of raw timer IDs on a standalone dispatcher that is
//! deliberately not attached to any thread, to widen the collection loop that the race targets.
//! A using-declaration re-widens access in this subclass without loosening the shipping class.
class DefectTestableDispatcher : public EventDispatcherDefault
{
public:
    using EventDispatcherDefault::postEvent;
    using EventDispatcherDefault::registerTimer;
};

TEST( EventDispatcherDefaultDefectTest, InterruptDuringTimerCollectionStress )
{
    constexpr int kTrials         = 30;
    constexpr int kTimersPerTrial = 8000;

    Object dummyReceiver;

    for( int trial = 0; trial < kTrials; ++trial )
    {
        DefectTestableDispatcher dispatcher;
        for( int i = 0; i < kTimersPerTrial; ++i )
        {
            // interval 0 => already due by the time processEvents() checks it.
            dispatcher.registerTimer( trial * kTimersPerTrial + i, 0, &dummyReceiver );
        }

        std::atomic<bool> go { false };
        std::thread racer(
            [&dispatcher, &go]()
            {
                while( !go.load( std::memory_order_acquire ) )
                {
                    std::this_thread::yield();
                }
                for( int i = 0; i < 500; ++i )
                {
                    dispatcher.interrupt();
                }
            } );

        go.store( true, std::memory_order_release );
        dispatcher.processEvents();
        racer.join();
    }

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: moveToThread() left the object's active timers registered with the thread it left, so
// timerEvent() kept being delivered on a thread the object no longer lived in.
// ---------------------------------------------------------------------------------------------

//! Records which thread its timerEvent() is delivered on.
class DefectTimerAffinityProbe : public Object
{
public:
    //! Gets the thread the most recent timer event arrived on, or nullptr if no event has
    //! arrived.
    Thread* firingThread() const
    {
        return mFiringThread.load();
    }

    //! Gets how many timer events have been delivered.
    int fireCount() const
    {
        return mFireCount.load();
    }

protected:
    //! Records the delivering thread.
    virtual void timerEvent
        (
        TimerEvent* event  //!< Unused.
        ) override
    {
        ( void ) event;
        mFiringThread.store( Thread::currentThread() );
        mFireCount.fetch_add( 1 );
    }

private:
    std::atomic<Thread*> mFiringThread { nullptr };
    std::atomic<int>      mFireCount { 0 };
};

//! Verifies moveToThread() carries active timers to the destination thread.
//!
//! Qt documents this: "all active timers for the object will be reset. The timers are first stopped
//! in the current thread and restarted (with the same interval) in the targetThread." Without it,
//! a moved object's timers keep firing on the thread it left -- delivering timerEvent() somewhere
//! the object no longer lives, which is precisely what the thread-confinement rules exist to
//! prevent.
//!
//! The timer is started on threadA (start is thread-confined, so via callLater), the object is then
//! moved to threadB by threadA (the only thread allowed to move it), and the test asserts the
//! events subsequently arrive on threadB.
TEST( ObjectDefectTest, MoveToThreadCarriesActiveTimersToTheNewThread )
{
    Thread threadA;
    threadA.start();
    while( !threadA.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    Thread threadB;
    threadB.start();
    while( !threadB.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    DefectTimerAffinityProbe probe;
    ASSERT_TRUE( probe.moveToThread( &threadA ) ); // legal: no affinity yet

    // Arm on threadA, then let it fire there at least once.
    Object::callLater( &probe, &Object::startTimer, 10 );

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( probe.fireCount() == 0 && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    ASSERT_GT( probe.fireCount(), 0 ) << "timer never fired on its original thread.";
    ASSERT_EQ( probe.firingThread(), &threadA ) <<
        "timer did not fire on the thread it was started on.";

    // Only threadA may move it, so ask threadA to do so.
    const int countBeforeMove = probe.fireCount();
    Object::callLater( &probe, &Object::moveToThread, &threadB );

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( probe.firingThread() != &threadB && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    EXPECT_EQ( probe.firingThread(), &threadB )
        << "after moveToThread(), the timer kept firing on the old thread -- active timers were "
        "not carried across.";
    EXPECT_GT( probe.fireCount(), countBeforeMove ) <<
        "the timer stopped firing entirely after the move.";

    // Stop it on its current owner before tearing down.
    Object::callLater( &probe, &Object::moveToThread, nullptr );
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

    threadA.quit();
    threadA.wait();
    threadB.quit();
    threadB.wait();
}

// ---------------------------------------------------------------------------------------------
// Defect: data race on Object::mThreadData (written by moveToThread(), read unsynchronized
// elsewhere).
// ---------------------------------------------------------------------------------------------

//! Thread that repeatedly re-homes its own object, to drive the mThreadData write side.
//!
//! moveToThread() is now thread-confined, so only the thread that owns an object may re-home it.
//! A thread *can* legally toggle its own object between itself and "no affinity": releasing it is
//! allowed because the caller is the current owner, and re-adopting it is allowed by the
//! unowned-object exception. That gives a tight, entirely legal write loop.
class DefectAffinityTogglingThread : public Thread
{
public:
    Object mSubject;  //!< The object whose affinity is toggled. Owned by this thread once run() starts.

    std::atomic<bool> mStopToggling { false };  //!< Set to stop the toggle loop.

protected:
    //! Toggles the subject's affinity between this thread and none until stopped.
    virtual void run() override
    {
        while( !mStopToggling.load( std::memory_order_acquire ) )
        {
            mSubject.moveToThread( this );    // adopt: legal, subject currently has no affinity
            mSubject.moveToThread( nullptr ); // release: legal, this thread is the current owner
        }
    }

};

//! Best-effort stress test targeting the data race on Object::mThreadData, previously
//! written by moveToThread() and read -- with no synchronization -- by threadData(),
//! startTimer(), killTimer(), deleteLater(), and dispatchMetaCall(), despite all of those being
//! documented thread-safe.
//!
//! Concurrent unsynchronized read/write of a std::shared_ptr is undefined behavior and, in
//! practice, can corrupt the control block (torn reference counts), which typically surfaces as
//! heap corruption / a double-free crash catchable by AddressSanitizer even without
//! ThreadSanitizer, given enough iterations. Best validated under one of those sanitizers; the
//! check below only confirms the scenario runs to completion without crashing.
//!
//! Restructured once moveToThread() became thread-confined. It used to bounce a shared object
//! between two threads from two *other* threads, which the confinement rule now (correctly)
//! refuses -- so it tested nothing but the rejection path. The write side is now driven by the
//! object's own owner, which is the only arrangement the rule permits, while the reads that the
//! mThreadData mutex actually protects continue to come from other threads.
TEST( ObjectDefectTest, ConcurrentMoveToThreadAndThreadDataAccessStress )
{
    DefectAffinityTogglingThread toggler;
    toggler.start();

    std::atomic<bool> stopReading { false };

    // Readers hammering the paths that read mThreadData from another thread -- exactly what the
    // mutex exists for. threadData() itself is no longer public (it is internal plumbing, as in
    // Qt), so the read is driven through a queued signal emission instead: dispatchMetaCall()
    // calls target->threadData() to find the dispatcher to post to, which takes the same lock.
    auto readerBody = [&toggler, &stopReading]()
        {
            Signal<> sig;
            Object::connect( sig, &toggler.mSubject, []()
                {
                }, ConnectionType::QueuedConnection );
            while( !stopReading.load( std::memory_order_acquire ) )
            {
                ( void ) toggler.mSubject.thread();
                sig.emit();
            }
        };
    std::thread reader1( readerBody );
    std::thread reader2( readerBody );

    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    stopReading.store( true, std::memory_order_release );
    reader1.join();
    reader2.join();

    toggler.mStopToggling.store( true, std::memory_order_release );
    toggler.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Formerly: ObjectDefectTest.ConcurrentEmitDuringDestructionStress.
//
// That test raced `delete victim` (called from the test's own thread) against a queued
// `sig.emit()` (from a third thread) while `victim` lived on a *fourth*, still-running
// workerThread. That is precisely the pattern Qt's own qobject.cpp documents as unsupported:
// "QObject: shared QObject was deleted directly. The program is malformed and may crash." Qt's
// safety for cross-thread teardown comes from deleteLater() deferring the actual delete onto the
// object's own thread (where it can never race that thread's own event loop, since it's the same
// thread) -- not from making a raw, foreign-thread delete safe. QObject::event()'s DeferredDelete/
// MetaCall cases dereference the receiver exactly the way Object::event() does; nothing about Qt's
// dispatch loop is intrinsically safer.
//
// We now make that same contract explicit (see the warning at the top of ~Object() in Object.cpp)
// rather than trying to engineer around a scenario Qt itself does not guarantee. The test above
// exercised exactly that out-of-contract pattern, so it was removed rather than "fixed" -- there is
// nothing to fix here that Qt itself does not also leave unfixed.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Defect (R17): a destroyed receiver's slot was never removed from the sender. The wrapper's life
// token made it inert, but it stayed in the signal's slot list holding its captured state and was
// still walked on every emit, so one long-lived signal feeding many short-lived receivers grew
// without bound. Measured before the fix, with 200k transient receivers on one signal: 672 MB of
// RSS retained after every receiver was destroyed, and a single emit() taking 327 ms to reach the
// one slot still alive. After: 32 kB and 30 us.
// ---------------------------------------------------------------------------------------------

//! Regression test for receiver-side connection teardown.
//!
//! ~Object() now disconnects every connection where it was the receiver (mIncoming), and each
//! connection carries a Cleanup token that prunes its own mIncoming entry when the connection ends
//! some other way. Both halves are needed and both are checked here: without the first, dead slots
//! accumulate on the sender; without the second, mIncoming accumulates stale handles on a receiver
//! that outlives connections made to it.
//!
//! Asserted through Signal::receivers() rather than by measuring memory, so the check is exact and
//! portable -- boost only counts still-connected slots, so an inert-but-connected slot is caught.
TEST( ObjectDefectTest, DestroyedReceiverIsDisconnectedFromItsSender )
{
    Signal<int> longLivedSignal;
    ASSERT_EQ( longLivedSignal.receivers(), 0u );

    {
        Object receiver;
        Object::connect( longLivedSignal, &receiver, []( int )
            {
            }, ConnectionType::DirectConnection );
        EXPECT_EQ( longLivedSignal.receivers(), 1u );
    }
    EXPECT_EQ( longLivedSignal.receivers(), 0u )
        << "the destroyed receiver's slot is still connected to the sender; it is inert (the life "
        "token stops it running) but still retained and still walked on every emit.";

    // Repeat in bulk: this is the shape that made the leak unbounded.
    for( int i = 0; i < 500; ++i )
    {
        Object receiver;
        Object::connect( longLivedSignal, &receiver, []( int )
            {
            }, ConnectionType::DirectConnection );
    }
    EXPECT_EQ( longLivedSignal.receivers(), 0u )
        << "dead slots accumulated on the sender across many short-lived receivers.";

    // The other direction: a connection ended while its receiver is still alive must prune its own
    // mIncoming entry, so the receiver is not left holding a stale handle to disconnect later.
    {
        Object receiver;
        ConnectionHandle handle = Object::connect( longLivedSignal, &receiver, []( int )
            {
            }, ConnectionType::DirectConnection );
        EXPECT_EQ( longLivedSignal.receivers(), 1u );

        Object::disconnect( handle );
        EXPECT_EQ( longLivedSignal.receivers(), 0u );

        // Reconnecting after the manual disconnect must still work, and destroying the receiver
        // must still clean up -- i.e. the pruning above did not corrupt mIncoming.
        Object::connect( longLivedSignal, &receiver, []( int )
            {
            }, ConnectionType::DirectConnection );
        EXPECT_EQ( longLivedSignal.receivers(), 1u );
    }
    EXPECT_EQ( longLivedSignal.receivers(), 0u );

    // A live emit must still reach a live receiver -- the teardown must not over-disconnect.
    Object liveReceiver;
    int calls = 0;
    Object::connect( longLivedSignal, &liveReceiver, [&calls]( int )
        {
            ++calls;
        }, ConnectionType::DirectConnection );
    longLivedSignal.emit( 1 );
    EXPECT_EQ( calls, 1 );
}

// ---------------------------------------------------------------------------------------------
// Defect: ~Object() invoked cleanup callbacks while still holding mCleanupMutex, so a callback
// that called addCleanupCallback() on the same object self-deadlocked on a non-recursive mutex.
// ---------------------------------------------------------------------------------------------

//! Regression test for the cleanup-callback deadlock in ~Object().
//!
//! ~Object() used to run the callbacks inside the mCleanupMutex lock_guard scope. A callback
//! that re-entered addCleanupCallback() on the same object then blocked forever trying to relock
//! a mutex its own call stack already held (locking a non-recursive std::mutex recursively is
//! undefined behavior; deadlock is the usual manifestation). The fix swaps the callback vector out
//! from under the lock and invokes the copies with the mutex released.
//!
//! Deterministic -- there is no timing window here; the old code failed on every run. How that
//! failure presents is platform-dependent, and both forms were confirmed by temporarily restoring
//! the old destructor body:
//!   - MSVC (verified): its std::mutex detects the recursive lock and aborts the test binary with
//!     exit code 3, before this test can reach any assertion. That abort *is* the failure signal,
//!     the same way ThreadDefectTest.RestartAfterFinishWithoutWaitDoesNotTerminate's
//!     std::terminate() is.
//!   - Implementations that simply block instead (the classic deadlock) are caught by the 5s
//!     timeout below, which is why the destruction runs on its own thread rather than inline --
//!     otherwise a regression would hang the whole binary with no output.
//!
//! On the timeout path the worker stays blocked forever and is detached, so the process may also
//! report a leak or hang at exit; the EXPECT_TRUE is the intended signal and the messy exit is a
//! side effect. Everything the worker touches is owned by it or held via shared_ptr, so nothing
//! dangles if the test body returns first.
TEST( ObjectDefectTest, CleanupCallbackRegisteringAnotherDoesNotDeadlock )
{
    auto reentrantCallSucceeded = std::make_shared<std::atomic<bool> >( false );
    auto* subject                = new Object();

    subject->addCleanupCallback(
        [subject, reentrantCallSucceeded]()
        {
            // Pre-fix, this call blocks forever: ~Object() is still holding mCleanupMutex.
            subject->addCleanupCallback( []()
            {
            } );
            reentrantCallSucceeded->store( true );
        } );

    std::promise<void> donePromise;
    auto doneFuture = donePromise.get_future();
    std::thread destroyer(
        [subject, p = std::move( donePromise )]() mutable
        {
            delete subject;
            p.set_value();
        } );

    const bool finished
        = doneFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;
    EXPECT_TRUE( finished ) << "~Object() did not finish within 5s -- a cleanup callback that "
        "re-registers another callback deadlocked on mCleanupMutex.";

    if( finished )
    {
        destroyer.join();
        EXPECT_TRUE( reentrantCallSucceeded->load() )
            << "the re-entrant addCleanupCallback() never returned.";
    }
    else
    {
        destroyer.detach();
    }
}

// ---------------------------------------------------------------------------------------------
// Defect: callLater() permanently disabled a (context, slot) pair if the very first call could
// not be delivered, because the pending-registry entry was left behind.
// ---------------------------------------------------------------------------------------------

//! Minimal callLater() target whose invocation count is safe to poll across threads.
class ObjectDefectCallLaterTarget : public Object
{
public:
    //! Slot invoked by callLater().
    void onCall()
    {
        mCallCount.fetch_add( 1 );
    }

    //! Gets how many times onCall() has run.
    int callCount() const
    {
        return mCallCount.load();
    }

private:
    std::atomic<int> mCallCount { 0 };
};

//! Regression test for callLater() silently dropping every future call after one failure.
//!
//! scheduleCallLater() inserts the key into the pending registry before attempting to dispatch. If
//! the target has no dispatcher, dispatch fails and the queued metacall is destroyed -- but the
//! registry entry used to survive. Every later callLater() for the same target then matched that
//! stale entry, took the "already scheduled" branch, and never dispatched again, so the pair stayed
//! dead for the rest of the object's life even once a dispatcher existed. The fix erases the entry
//! when dispatch reports failure.
//!
//! Deterministic: the first callLater() targets an object with no thread affinity (this test binary
//! has no CoreApplication, so the main thread has no dispatcher) and is expected to be lost. The
//! assertion is that the *second* call, after the object is moved to a running worker thread,
//! actually runs. Pre-fix that second call never executes and the wait times out.
TEST( ObjectDefectTest, CallLaterRecoversAfterFirstDispatchFails )
{
    ObjectDefectCallLaterTarget target;

    // No dispatcher anywhere yet -- this call cannot be delivered and is expected to be lost.
    Object::callLater( &target, &ObjectDefectCallLaterTarget::onCall );
    EXPECT_EQ( target.callCount(), 0 ) << "call ran despite there being no dispatcher.";

    Thread worker;
    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }
    target.moveToThread( &worker );

    // Same target, same slot -- must re-arm now that a dispatcher exists.
    Object::callLater( &target, &ObjectDefectCallLaterTarget::onCall );

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( target.callCount() == 0 && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    worker.quit();
    worker.wait();

    EXPECT_EQ( target.callCount(), 1 )
        << "callLater() stayed permanently disabled after its first dispatch failed.";
}

// ---------------------------------------------------------------------------------------------
// Defect: a thread deleted its own event dispatcher while other threads were still calling into
// it through the pointer they had already loaded from ThreadData.
// ---------------------------------------------------------------------------------------------

//! Stress test for dispatcher destruction racing against cross-thread use.
//!
//! ThreadData used to hold the dispatcher as an atomic raw pointer. That made the *pointer* load
//! safe and did nothing for the object's lifetime: a worker finishing deleted its dispatcher while
//! another thread sat between its own `dispatcher.load()` and the `disp->registerTimer(...)` call
//! that followed, i.e. a use-after-free on every one of startTimer(), killTimer(), deleteLater(),
//! dispatchMetaCall() and ~Object(). The dispatcher is now a shared_ptr and callers hold a strong
//! reference for the duration of the call, so a finishing thread merely drops its own reference.
//!
//! This is a best-effort stress test, and an honest one: the window is narrow, and a clean run
//! here is *not* what demonstrates the fix. The real signal came from ThreadSanitizer, which
//! reported this race (as ~EventDispatcherDefault against Object::startTimer /
//! registerTimer / Object::event / Thread::exec) on the old code while this very suite passed.
//! Build with -fsanitize=thread to get that signal; see OPEN-RISKS for the exact command. Under a
//! plain build this only checks the scenario runs to completion without crashing.
TEST( ThreadDefectTest, DispatcherUseDuringThreadShutdownStress )
{
    constexpr int kTrials = 40;

    for( int trial = 0; trial < kTrials; ++trial )
    {
        Thread workerThread;
        workerThread.start();
        while( !workerThread.eventDispatcher() )
        {
            std::this_thread::yield();
        }

        // Objects living on the worker, driven from this thread while the worker shuts down.
        Object subject;
        subject.moveToThread( &workerThread );

        // Queued signal emission is the cross-thread dispatcher path that must stay safe:
        // dispatchMetaCall() loads the target thread's dispatcher and posts to it, which is
        // exactly the load-then-use window this defect lived in. (startTimer()/killTimer() are
        // now thread-confined and can no longer be driven from here.)
        Signal<>         sig;
        Object::connect( sig, &subject, []()
            {
            }, ConnectionType::QueuedConnection );

        std::atomic<bool> stop { false };
        std::thread hammer(
            [&sig, &stop]()
            {
                while( !stop.load( std::memory_order_acquire ) )
                {
                    sig.emit();
                }
            } );

        // Tear the worker down underneath the hammering thread.
        workerThread.quit();
        workerThread.wait();

        stop.store( true, std::memory_order_release );
        hammer.join();
    }

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: an object that called deleteLater() was never destroyed if its thread's event loop
// stopped before the deferred-delete event was dispatched.
// ---------------------------------------------------------------------------------------------

//! Regression test for a pending deleteLater() being dropped when a thread stops.
//!
//! deleteLater() posts a DeferredDeleteEvent; the receiver is only destroyed when that event is
//! dispatched. If the loop stopped first, ~EventDispatcherDefault() freed the pending event but
//! had no way to free its receiver, so the object leaked silently. Thread now drains deferred
//! deletes after run() returns, the way Qt's QThreadPrivate::finish() does.
//!
//! Made deterministic rather than racy: the worker is parked inside a queued slot while the
//! deleteLater() is posted and quit() is called, guaranteeing the delete is still sitting in the
//! queue when the loop is told to stop. On release, exec() sees mExiting and returns without
//! draining -- so the object survives only if the shutdown path handles it.
//!
//! Failure shows up twice over: the flag below stays false, and AddressSanitizer reports the leak.
TEST( ObjectDefectTest, PendingDeleteLaterIsProcessedWhenThreadStops )
{
    Thread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    Object dummyContext;
    dummyContext.moveToThread( &workerThread );

    Signal<> blockSig;
    Object::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        ConnectionType::QueuedConnection );

    blockSig.emit();
    blockEnteredFuture.get();

    // The worker is now parked inside the slot above, so nothing below can be dispatched until
    // it is released.
    auto destroyed = std::make_shared<std::atomic<bool> >( false );
    Object* victim    = new Object();
    victim->moveToThread( &workerThread );
    victim->addCleanupCallback( [destroyed]()
        {
            destroyed->store( true );
        } );
    victim->deleteLater();

    workerThread.quit();
    blockReleasePromise.set_value();
    workerThread.wait();

    EXPECT_TRUE( destroyed->load() )
        << "deleteLater() was still pending when the loop stopped, and the object was leaked "
        "instead of destroyed.";
}

// ---------------------------------------------------------------------------------------------
// Contract: deleteLater() is idempotent -- calling it repeatedly, from any thread, destroys the
// object exactly once. Enforced by a de-bounce guard mirroring Qt's own
// QObjectPrivate::deleteLaterCalled ("De-bounce QDeferredDeleteEvents", qobject.cpp).
// ---------------------------------------------------------------------------------------------

//! Verifies deleteLater() is idempotent across threads.
//!
//! NOT a regression test: this passes with or without the de-bounce guard, and deliberately so --
//! it pins the observable contract, not one particular implementation of it. Two pre-existing
//! mechanisms already make a duplicate DeferredDeleteEvent harmless, and between them they leave
//! no reachable window for a double-dispatch:
//!   - both events drained into the SAME processEvents() batch: the deletedReceivers set in
//!     EventDispatcherDefault::processEvents() skips the second one;
//!   - second event still in the live queue when the first is dispatched: ~Object() calls
//!     removeEventsForReceiver(), which strips it before it can ever be drained.
//! The only ordering those two do not cover -- second event posted after the receiver is already
//! destroyed -- cannot arise from a correct caller, because deleteLater() would itself be running
//! on freed memory. The guard is therefore Qt parity and defense-in-depth (and one less redundant
//! heap allocation), not a fix for a reachable defect; this test locks in the behavior so a
//! future change to either mechanism above cannot silently make repeat calls observable.
//!
//! workerThread is parked inside a blocking slot (the same technique used elsewhere in this
//! file) for the whole time both deleteLater() calls run, and only released afterward. Without
//! that, workerThread -- already running -- could drain and dispatch the first posted event,
//! deleting victim, before the second thread's call had even started; that call would then be
//! made on already-freed memory, which is a bug in the test's own synchronization rather than
//! anything about deleteLater(). An earlier draft of this test omitted the park and ThreadSanitizer
//! caught exactly that.
TEST( ObjectDefectTest, DeleteLaterIsDebounced )
{
    Thread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    Object dummyContext;
    dummyContext.moveToThread( &workerThread );

    Signal<> blockSig;
    Object::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        ConnectionType::QueuedConnection );

    blockSig.emit();
    blockEnteredFuture.get();

    // workerThread is now parked inside the slot above, so nothing posted below can be
    // dispatched -- and victim cannot be deleted -- until it is released.
    auto destroyCount = std::make_shared<std::atomic<int> >( 0 );
    auto* victim = new Object();
    victim->moveToThread( &workerThread );
    victim->addCleanupCallback( [destroyCount]()
        {
            destroyCount->fetch_add( 1 );
        } );

    std::thread secondCaller( [victim]()
        {
            victim->deleteLater();
        } );
    victim->deleteLater();
    secondCaller.join();

    blockReleasePromise.set_value();

    workerThread.quit();
    workerThread.wait();

    EXPECT_EQ( destroyCount->load(), 1 )
        << "deleteLater() called twice destroyed the object more than once, or not at all.";
}

// ---------------------------------------------------------------------------------------------
// Defect: an idle EventDispatcherDefault woke ~10x/second, and wakeUp() could not actually end
// a wait because the wait is predicate-based and wakeUp() changed no state.
// ---------------------------------------------------------------------------------------------

//! Verifies an idle processEvents() blocks instead of polling, and that a posted event
//! still ends the wait promptly.
//!
//! The wait used to be capped at 100ms regardless of whether anything was scheduled, so an idle
//! thread burned a wakeup ten times a second forever. It is now unbounded when no timers are
//! registered. This checks both halves of that: it must not return on its own while idle, and it
//! must still return quickly once there is something to do -- the second half is the important
//! one, since an unbounded wait that misses a notification would hang forever.
TEST( EventDispatcherDefaultDefectTest, IdleWaitBlocksButStillWakesOnPostedEvent )
{
    DefectTestableDispatcher dispatcher;
    Object receiver;

    std::promise<void> returnedPromise;
    auto returnedFuture = returnedPromise.get_future();
    std::thread loop(
        [&dispatcher, &returnedPromise]()
        {
            dispatcher.processEvents();
            returnedPromise.set_value();
        } );

    EXPECT_EQ( returnedFuture.wait_for( std::chrono::milliseconds( 300 ) ), std::future_status::
        timeout )
        << "idle processEvents() returned on its own; it should block until there is work.";

    dispatcher.postEvent( &receiver, new TimerEvent( 1 ) );

    const bool woke
        = returnedFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;
    if( !woke )
    {
        // Force the loop out so this fails on its assertion rather than hanging in join(). The
        // thread must not simply be detached: it would still be inside processEvents() touching
        // `dispatcher`, which is about to be destroyed with this stack frame.
        dispatcher.interrupt();
    }
    loop.join();

    EXPECT_TRUE( woke ) <<
        "posting an event did not wake the idle wait -- notification was missed.";
}

//! Verifies wakeUp() actually ends an idle wait.
//!
//! processEvents() waits on a predicate, so wakeUp()'s bare notify_all() used to be a no-op: with
//! no state change to observe, the waiter re-evaluated the predicate, saw nothing, and slept
//! again. It only ever appeared to work because the wait was capped at 100ms and would have
//! returned anyway. wakeUp() now sets a flag under the mutex that the predicate tests.
TEST( EventDispatcherDefaultDefectTest, WakeUpEndsIdleWait )
{
    DefectTestableDispatcher dispatcher;

    std::promise<void> returnedPromise;
    auto returnedFuture = returnedPromise.get_future();
    std::thread loop(
        [&dispatcher, &returnedPromise]()
        {
            dispatcher.processEvents();
            returnedPromise.set_value();
        } );

    ASSERT_EQ( returnedFuture.wait_for( std::chrono::milliseconds( 300 ) ), std::future_status::
        timeout )
        << "idle processEvents() returned before wakeUp() was called.";

    dispatcher.wakeUp();

    const bool woke
        = returnedFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;
    if( !woke )
    {
        // See IdleWaitBlocksButStillWakesOnPostedEvent: interrupt() rather than detach(), so a
        // regression fails on the assertion instead of hanging the binary in join() or leaving a
        // detached thread using a destroyed dispatcher.
        dispatcher.interrupt();
    }
    loop.join();

    EXPECT_TRUE( woke ) << "wakeUp() did not end the wait.";
}

// ---------------------------------------------------------------------------------------------
// Defect: Timer emitted timeout before stopping a single-shot timer, and touched its own members
// after the emit -- so a slot could observe a stale isActive(), and any member access after user
// code ran was a hazard if that code destroyed the timer.
// ---------------------------------------------------------------------------------------------

//! Verifies a single-shot Timer is already stopped by the time its timeout slot runs.
//!
//! Timer::timerEvent() used to emit first and stop afterwards, so a slot observed isActive()
//! == true for a timer that was about to be stopped, and Timer touched mSingleShot/stop() after
//! arbitrary user code had run. It now stops first and emits last, matching Qt's QTimer ordering.
//!
//! This also covers the reentrancy half of the Timer locking work: the slot calls back into
//! isActive() and start(), which take the same mutex timerEvent() uses. Holding that mutex across
//! the emit would self-deadlock on a non-recursive mutex, so a hang here is a real failure.
TEST( TimerDefectTest, SingleShotIsStoppedBeforeTimeoutIsEmitted )
{
    Thread worker;
    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<bool> activePromise;
    auto activeFuture = activePromise.get_future();

    // Stack-allocated deliberately. A heap timer would have to be reclaimed with deleteLater(),
    // which needs the worker's loop to keep running long enough to process it -- quitting first
    // leaks the object, since the dispatcher's destructor frees the queued event but not its
    // receiver. Destroying it here after wait() avoids depending on that entirely.
    Timer timer;
    timer.moveToThread( &worker );
    timer.setSingleShot( true );

    Object::connect(
        timer.timeout,
        &timer,
        [&timer, &activePromise]()
        {
            // Reentrant calls into the timer's own locked API from inside the emit.
            const bool active = timer.isActive();
            timer.stop();
            activePromise.set_value( active );
        },
        ConnectionType::DirectConnection );

    // start() must run on the worker thread so the timer registers against its dispatcher.
    Object::callLater( &timer, &Timer::start, 10 );

    const bool fired
        = activeFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;

    worker.quit();
    worker.wait();

    ASSERT_TRUE( fired ) <<
        "timeout never fired, or the slot deadlocked against Timer's own mutex.";
    EXPECT_FALSE( activeFuture.get() )
        << "single-shot timer was still active inside its own timeout slot.";
}
