// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

// Regression tests for the defects found in the code review of technology/G* (see CHANGES.md /
// the accompanying patch). Kept separate from test_gobject.cpp / test_gthread.cpp / test_gtimer.cpp
// since these specifically target crash/UAF/leak/race scenarios rather than day-to-day API
// behavior, and several of them are stress tests rather than single-shot deterministic checks --
// see each test's doc comment for what it actually proves and how to get the strongest signal
// out of it (most benefit from being run under AddressSanitizer and/or ThreadSanitizer; the build
// enables one or the other, never both -- see tools/toolchain-linux.py -- so run it under each in
// turn, since they catch disjoint classes of defect).
#include <gtest/gtest.h>
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Thread.hpp"
#include "QtLikeSignal/Timer.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/EventDispatcherDefault.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#if defined( __linux__ )
    #include <fstream>
    #include <string>
#endif

//! True when this translation unit is compiled under AddressSanitizer.
//!
//! Clang exposes it through __has_feature, GCC through a predefined macro; check both.
#if defined( __has_feature )
    #if __has_feature( address_sanitizer )
        #define QLS_ADDRESS_SANITIZER_ACTIVE 1
    #endif
#endif
#if defined( __SANITIZE_ADDRESS__ ) && !defined( QLS_ADDRESS_SANITIZER_ACTIVE )
    #define QLS_ADDRESS_SANITIZER_ACTIVE 1
#endif

using namespace QtLikeSignal;

namespace
{
    //! Spins until @p aReady is true, or gives up. True if it became true.
    //!
    //! A bounded poll rather than a sleep: the tests below wait on another thread reaching a point,
    //! and a fixed sleep either wastes time or flakes under load.
    template <typename Predicate>
    bool waitFor
        (
        const Predicate& aReady,     //!< Condition to wait for.
        int aTimeoutMs = 3000        //!< Give up after this long.
        )
    {
        const auto deadline
            = std::chrono::steady_clock::now() + std::chrono::milliseconds( aTimeoutMs );
        while( std::chrono::steady_clock::now() < deadline )
        {
            if( aReady() )
            {
                return true;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return aReady();
    }

    //! Resident set size in kB, or -1 where it is not implemented.
    //!
    //! Only Linux is implemented; the tests that need it skip elsewhere rather than pretending.
    long residentSetKb()
    {
        #if defined( __linux__ )
            std::ifstream status( "/proc/self/status" );
            std::string key;
            long value = 0;
            while( status >> key )
            {
                if( key == "VmRSS:" )
                {
                    status >> value;
                    return value;
                }
            }
            return -1;
        #else
            return -1;
        #endif
    }

    //! Object that records its own destruction, so deferred deletion can be observed.
    class DestructionRecorder : public Object
    {
    public:
        explicit DestructionRecorder
            (
            std::shared_ptr<std::atomic<bool> > aFlag
            )
            : mFlag( std::move( aFlag ) )
        {
        }

        virtual ~DestructionRecorder() override
        {
            mFlag->store( true );
        }

    private:
        std::shared_ptr<std::atomic<bool> > mFlag;
    };

    //! Counts its own destructions, for the tests that must tell "destroyed once" from
    //! "destroyed twice". DestructionRecorder's bool flag cannot make that distinction.
    class DestructionCounter : public Object
    {
    public:
        explicit DestructionCounter
            (
            std::shared_ptr<std::atomic<int> > aCount
            )
            : mCount( std::move( aCount ) )
        {
        }

        virtual ~DestructionCounter() override
        {
            mCount->fetch_add( 1 );
        }

    private:
        std::shared_ptr<std::atomic<int> > mCount;
    };

    //! Creates an Object on a short-lived native thread and returns it after that thread has exited.
    //!
    //! The returned object is "orphaned": auto-adoption gave its creating thread a Thread, and that
    //! Thread was destroyed when the native thread exited, so the object's affinity now reports
    //! thread() == nullptr while still holding the dead thread's ThreadData -- and, crucially, the
    //! live dispatcher that ThreadData still owns.
    template <typename Factory>
    auto makeOrphanedObject
        (
        Factory aFactory
        ) -> decltype( aFactory() )
    {
        decltype( aFactory() ) created = nullptr;
        std::thread creator( [&created, &aFactory]()
            {
                created = aFactory();
            } );
        creator.join();
        return created;
    }
}

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
        ConnectionType::Queued );

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
    Object::connect( sig, victim, &DefectUafTestReceiver::onValue, ConnectionType::Queued
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
        }, ConnectionType::Queued );
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
        shortTimer.getTimeout(),
        &context,
        [&firePromise]()
        {
            firePromise.set_value( std::chrono::steady_clock::now() );
        },
        ConnectionType::Direct );

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
        ConnectionType::Queued );
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
    using EventDispatcherDefault::unregisterTimer;
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
                }, ConnectionType::Queued );
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
            }, ConnectionType::Direct );
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
            }, ConnectionType::Direct );
    }
    EXPECT_EQ( longLivedSignal.receivers(), 0u )
        << "dead slots accumulated on the sender across many short-lived receivers.";

    // The other direction: a connection ended while its receiver is still alive must prune its own
    // mIncoming entry, so the receiver is not left holding a stale handle to disconnect later.
    {
        Object receiver;
        Connection handle = Object::connect( longLivedSignal, &receiver, []( int )
            {
            }, ConnectionType::Direct );
        EXPECT_EQ( longLivedSignal.receivers(), 1u );

        Object::disconnect( handle );
        EXPECT_EQ( longLivedSignal.receivers(), 0u );

        // Reconnecting after the manual disconnect must still work, and destroying the receiver
        // must still clean up -- i.e. the pruning above did not corrupt mIncoming.
        Object::connect( longLivedSignal, &receiver, []( int )
            {
            }, ConnectionType::Direct );
        EXPECT_EQ( longLivedSignal.receivers(), 1u );
    }
    EXPECT_EQ( longLivedSignal.receivers(), 0u );

    // A live emit must still reach a live receiver -- the teardown must not over-disconnect.
    Object liveReceiver;
    int calls = 0;
    Object::connect( longLivedSignal, &liveReceiver, [&calls]( int )
        {
            ++calls;
        }, ConnectionType::Direct );
    longLivedSignal.emit( 1 );
    EXPECT_EQ( calls, 1 );
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

    // Park the target on a Thread that exists but has not been started yet. A Thread only acquires
    // a dispatcher when its loop starts (or when it is auto-adopted), so this is now the way to get
    // an object whose thread genuinely cannot deliver. Leaving the target on the main thread no
    // longer works: every thread is adopted and given a dispatcher, so the main thread would
    // happily queue the call and the failure path would never be exercised.
    //
    // The same Thread is started later rather than moving the target to a different one, because
    // moveToThread() is push-only -- once the target lives on `worker`, only `worker` may re-home
    // it, and a thread that never runs never will. Qt has that property too.
    Thread worker;
    ASSERT_TRUE( target.moveToThread( &worker ) );
    ASSERT_EQ( worker.eventDispatcher(), nullptr );

    // No dispatcher on the target's thread -- this call cannot be delivered and is expected to be
    // lost.
    Object::callLater( &target, &ObjectDefectCallLaterTarget::onCall );
    EXPECT_EQ( target.callCount(), 0 ) << "call ran despite there being no dispatcher.";

    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }

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
            }, ConnectionType::Queued );

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
        ConnectionType::Queued );

    blockSig.emit();
    blockEnteredFuture.get();

    // The worker is now parked inside the slot above, so nothing below can be dispatched until
    // it is released.
    auto destroyed = std::make_shared<std::atomic<bool> >( false );
    DestructionRecorder* victim = new DestructionRecorder( destroyed );
    victim->moveToThread( &workerThread );
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
        ConnectionType::Queued );

    blockSig.emit();
    blockEnteredFuture.get();

    // workerThread is now parked inside the slot above, so nothing posted below can be
    // dispatched -- and victim cannot be deleted -- until it is released.
    auto destroyCount = std::make_shared<std::atomic<int> >( 0 );
    auto* victim = new DestructionCounter( destroyCount );
    victim->moveToThread( &workerThread );

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
        timer.getTimeout(),
        &timer,
        [&timer, &activePromise]()
        {
            // Reentrant calls into the timer's own locked API from inside the emit.
            const bool active = timer.isActive();
            timer.stop();
            activePromise.set_value( active );
        },
        ConnectionType::Direct );

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

// ---------------------------------------------------------------------------------------------
// Defect (R23, fixed 2026-08-08): an object whose thread had been destroyed still had a live
// dispatcher, so work posted to it was accepted by a queue nothing would ever drain.
//
// Thread::threadBody() cleared the dispatcher when a worker finished, but ~Thread() did not, so an
// adopted thread's ThreadData kept a live EventDispatcherDefault after the thread was gone. The
// object was then in a state the design did not anticipate: thread() == nullptr, yet posting
// succeeded. Fixed in three places -- dispatchMetaCallTo() and deleteLater() now refuse to queue
// when the target thread is gone, and ~Thread() drains deferred deletes and releases the dispatcher
// before nulling the back-pointer, so the two can no longer disagree.
//
// QtMimic forecloses this. ~Thread() closes the mailbox *before* nulling the back-pointer, stating
// the invariant outright -- "Done BEFORE clearing the back-pointer, so the invariant 'thread() ==
// nullptr implies not accepting' holds" -- and connectImpl() additionally drops when
// ctxData->thread() == nullptr. Measured side by side over five rounds of 200k emits, QtMimic grows
// 276 kB then flattens; QtLikeSignal grows ~30 MB every round without bound.
// ---------------------------------------------------------------------------------------------

//! Verifies deleteLater() on an orphaned object deletes it instead of leaking it.
//!
//! Deterministic -- no timing, no sampling. Before the fix this queued a DeferredDeleteEvent into
//! the dead thread's still-live dispatcher, reported success, and the object was never destroyed.
//! It now falls back to a synchronous delete, the same trade QtMimic makes when its post() refuses
//! the task: "Doing nothing here would leak self forever, which is strictly worse than the
//! thread-affinity violation of deleting it synchronously."
TEST( ObjectDefectTest, DeleteLaterOnAnOrphanedObjectDeletesItSynchronously )
{
    auto destroyed = std::make_shared<std::atomic<bool> >( false );

    DestructionRecorder* orphan = makeOrphanedObject( [destroyed]()
        {
            return new DestructionRecorder( destroyed );
        } );

    ASSERT_NE( orphan, nullptr );
    ASSERT_EQ( orphan->thread(), nullptr )
        << "the creating thread's Thread should have been destroyed when that thread exited";

    orphan->deleteLater();

    const bool wasDestroyed = destroyed->load();
    EXPECT_TRUE( wasDestroyed )
        << "deleteLater() on an object whose thread is gone queued a DeferredDeleteEvent into a "
        "dispatcher nothing will ever drain, so the object was leaked outright instead of being "
        "deleted synchronously.";

    if( !wasDestroyed )
    {
        // Reclaim it so this known-failing test does not also leak on every run.
        delete orphan;
    }
}

//! Verifies queued calls to an orphaned object are dropped rather than accumulating without bound.
//!
//! Sampled over repeated rounds rather than once, which is what distinguishes a genuine leak from
//! allocator arena high-water -- a single measurement cannot tell them apart, since even correct
//! code grows on the first round and then flattens. Before the fix this retained ~30 MB per round,
//! climbing forever, and LeakSanitizer reported nothing because every byte stayed reachable.
TEST( ObjectDefectTest, QueuedCallsToAnOrphanedObjectAreDropped )
{
    if( residentSetKb() < 0 )
    {
        GTEST_SKIP() << "resident-set sampling is implemented for Linux only";
    }

    #if defined( QLS_ADDRESS_SANITIZER_ACTIVE )
        // AddressSanitizer holds freed allocations in a quarantine so it can detect use-after-free,
        // so resident memory grows with the number of frees regardless of whether anything is
        // retained. Emitting allocates one std::function per call even when the resulting metacall
        // is correctly dropped, so 800k emits fill the quarantine and this measurement reports ~37 MB
        // for entirely healthy code. Confirmed: with ASAN_OPTIONS=quarantine_size_mb=1 the same
        // binary passes. Run this under ThreadSanitizer or without a sanitizer, where the number
        // means what it claims to.
        GTEST_SKIP() << "resident-set growth is not a meaningful signal under AddressSanitizer "
            "(its quarantine retains freed blocks); re-run under TSan, or with "
            "ASAN_OPTIONS=quarantine_size_mb=1";
    #endif

    Object* orphan = makeOrphanedObject( []()
        {
            return new Object();
        } );
    ASSERT_NE( orphan, nullptr );
    ASSERT_EQ( orphan->thread(), nullptr );

    Signal<> sig;
    Object::connect( sig, orphan, []()
        {
        }, ConnectionType::Queued );

    constexpr int kRounds = 4;
    constexpr int kEmitsPerRound = 200000;
    std::vector<long> growthPerRound;

    for( int round = 0; round < kRounds; ++round )
    {
        const long before = residentSetKb();
        for( int i = 0; i < kEmitsPerRound; ++i )
        {
            sig.emit();
        }
        growthPerRound.push_back( residentSetKb() - before );
    }

    // Ignore the first round: that is where the allocator's arena grows, and it does so even when
    // the events are correctly dropped. Steady-state growth is the signal.
    long steadyStateGrowth = 0;
    for( size_t i = 1; i < growthPerRound.size(); ++i )
    {
        steadyStateGrowth += growthPerRound[i];
    }

    delete orphan;

    EXPECT_LT( steadyStateGrowth, 4096 )
        << "after the first round, " << ( kRounds - 1 ) << " further rounds of " << kEmitsPerRound
        << " queued emits to an object whose thread is gone retained " << steadyStateGrowth
        << " kB. They are being queued into a dispatcher nothing will ever drain, instead of being "
        "dropped.";
}

// ---------------------------------------------------------------------------------------------
// Defect (R26): a repeating timer's next deadline was computed from when the dispatcher noticed the
// timer rather than from the deadline that had just elapsed, so any lateness was folded into the
// cadence permanently instead of being absorbed.
//
// Qt's calculateNextTimeout() (qtimerinfo_unix.cpp) does both halves:
//
//     t->timeout += t->interval;          // advance from the previous DEADLINE
//     if (t->timeout < now) {             // unless that is already past...
//         t->timeout = now;
//         t->timeout += t->interval;      // ...then resynchronise instead of building a backlog
//     }
//
// Note the resynchronisation: after a stall longer than one interval Qt also gives up on the
// original cadence. So the divergence is narrower than it first looks -- it is only lateness of
// *less than one interval* that Qt absorbs and this code used to accumulate.
// ---------------------------------------------------------------------------------------------

//! Records when its timer fired, relative to a caller-supplied origin.
class TimerCadenceRecorder : public Object
{
public:
    //! Fire times since mOrigin, in milliseconds.
    std::vector<double> mFireOffsetsMs;

    //! Origin the offsets are measured from; set before registering the timer.
    std::chrono::steady_clock::time_point mOrigin;

protected:
    virtual void timerEvent
        (
        TimerEvent* aEvent
        ) override
    {
        ( void )aEvent;
        mFireOffsetsMs.push_back(
            std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - mOrigin ).count() );
    }

};

//! Verifies one late pass does not shift a repeating timer's cadence.
//!
//! Deliberately whitebox, driving a standalone dispatcher rather than a running loop: that is what
//! makes "the pass is late by a controlled amount" a single precise sleep instead of a contrived
//! load generator. The lateness (20 ms) is under one interval (50 ms), which is exactly the band
//! where Qt preserves the schedule -- past a whole interval Qt resynchronises too, so asserting
//! anything there would be asserting non-Qt behaviour.
//!
//! Second fire lands at ~400 ms if the deadline drives the schedule, or ~480 ms if the moment of
//! noticing does, so the threshold sits 40 ms clear of either.
//!
//! The intervals are deliberately large. An earlier version used 50 ms / 70 ms, which separated the
//! two outcomes by only 20 ms -- fine on Linux, but it failed on Windows at 112.7 ms against a
//! 110 ms threshold. That was not the defect resurfacing: Windows' default timer resolution is about
//! 15.6 ms and a wait only ever overshoots, so 112.7 ms was the *correct* 100 ms deadline plus
//! granularity (the broken behaviour could not have produced less than 120 ms). Scaling everything
//! up by 4x makes that fixed overshoot small next to the 80 ms that actually distinguishes the two.
TEST( EventDispatcherDefaultDefectTest, LatePassDoesNotShiftARepeatingTimersCadence )
{
    // Declared before the dispatcher so the dispatcher is destroyed first and never holds a stale
    // receiver pointer.
    TimerCadenceRecorder recorder;
    DefectTestableDispatcher dispatcher;

    constexpr int kIntervalMs = 200;
    constexpr int kLatePassMs = 280;  // 80 ms past the first deadline, still short of the second

    recorder.mOrigin = std::chrono::steady_clock::now();
    dispatcher.registerTimer( 1, kIntervalMs, &recorder );

    // Leave the dispatcher unserviced straight through the first deadline.
    std::this_thread::sleep_for( std::chrono::milliseconds( kLatePassMs ) );

    dispatcher.processEvents();
    ASSERT_EQ( recorder.mFireOffsetsMs.size(), 1u )
        << "the overdue timer should fire on the first pass that services it";

    // Guards the premise rather than the behaviour: if the sleep above overshot so far that the
    // *second* deadline had also passed, the resynchronisation branch legitimately takes over and
    // this test is no longer measuring what it claims to. Fail with that explanation instead of a
    // confusing cadence mismatch.
    ASSERT_LT( recorder.mFireOffsetsMs[0], 2.0 * kIntervalMs )
        << "the first pass was more than a whole interval late, so the dispatcher correctly "
        "resynchronised and there is no preserved cadence left to assert";

    // Drive the dispatcher until the timer fires again. Each pass blocks until the next deadline,
    // so this does not spin.
    while( recorder.mFireOffsetsMs.size() < 2 )
    {
        dispatcher.processEvents();
    }

    const double secondFireMs = recorder.mFireOffsetsMs[1];
    EXPECT_LT( secondFireMs, 440.0 )
        << "the second fire landed at " << secondFireMs << " ms. The first pass was "
        << ( kLatePassMs - kIntervalMs ) << " ms late, and that lateness has been folded into the "
        "schedule: the next deadline was computed from when the dispatcher noticed the timer ("
        << kLatePassMs << " + " << kIntervalMs << ") instead of from the deadline that elapsed ("
        << kIntervalMs << " + " << kIntervalMs << "). Every late pass shifts the cadence again.";
}

//! Records which timers fired, and kills a nominated one from inside the first handler.
class TimerKillDuringDispatchRecorder : public Object
{
public:
    DefectTestableDispatcher* mDispatcher { nullptr };  //!< Dispatcher to kill through.
    int mIdToKill { -1 };                                //!< Killed on the first fire, then cleared.
    std::vector<int> mFiredIds;                          //!< Ids delivered to this object, in order.

protected:
    virtual void timerEvent
        (
        TimerEvent* aEvent
        ) override
    {
        mFiredIds.push_back( aEvent->timerId() );
        if( mIdToKill >= 0 && mDispatcher )
        {
            const int toKill = mIdToKill;
            mIdToKill = -1;
            mDispatcher->unregisterTimer( toKill );
        }
    }

};

//! Verifies a timer killed during a dispatch pass does not still fire in that same pass.
//!
//! Timers expiring together are collected into one batch and then dispatched with the lock
//! released, so killing one from inside another's handler happens *after* its event already exists.
//! Delivering it anyway is wrong on its own terms -- the timer was stopped -- and became actively
//! dangerous once timer ids started being recycled: the id can be reissued to a new timer, and the
//! stale event would then fire that one instead.
//!
//! Note this is the batch, not the posted-event queue. TimerEvents are only ever created inside the
//! collection loop, so purging mEventQueue alone would not have covered this at all.
TEST( EventDispatcherDefaultDefectTest, TimerKilledDuringDispatchDoesNotStillFire )
{
    // Declared before the dispatcher so the dispatcher outlives nothing it points at.
    TimerKillDuringDispatchRecorder recorder;
    DefectTestableDispatcher dispatcher;

    recorder.mDispatcher = &dispatcher;
    recorder.mIdToKill = 2;

    // Interval 0 so both are already due and land in a single batch.
    dispatcher.registerTimer( 1, 0, &recorder );
    dispatcher.registerTimer( 2, 0, &recorder );

    dispatcher.processEvents();

    EXPECT_EQ( recorder.mFiredIds.size(), 1u )
        << "timer 2 was killed from inside timer 1's handler but still fired in the same pass, "
        "because its event had already been collected into the dispatch batch.";
    ASSERT_FALSE( recorder.mFiredIds.empty() );
    EXPECT_EQ( recorder.mFiredIds[0], 1 );

    dispatcher.unregisterTimer( 1 );
}

// ---------------------------------------------------------------------------------------------
// Defect (R24, fixed 2026-08-08): timer ids were consumed monotonically and never returned to a
// pool, so the counter climbed until it wrapped and eventually handed out -1 -- the value
// startTimer() returns to mean failure and Timer::stop() tests against.
//
// Qt releases ids (QAbstractEventDispatcherPrivate::releaseTimerId, backed by a lock-free QFreeList)
// so a program that starts and stops timers forever reuses a small set. Object now does the same
// through a process-wide pool, tracks the ids it owns so ~Object() can return them, and reuses them
// FIFO rather than LIFO -- see TimerIdPool in Object.cpp for why that ordering is load-bearing.
// ---------------------------------------------------------------------------------------------

//! Verifies starting and stopping a timer repeatedly reuses ids instead of consuming fresh ones.
//!
//! Tests the underlying property -- recycling -- rather than the 2^31 wrap non-recycling eventually
//! caused, which is not reachable in a test.
TEST( ObjectDefectTest, TimerIdsAreRecycled )
{
    Object owner;
    ASSERT_EQ( owner.thread(), Thread::currentThread() )
        << "startTimer() is thread-confined; this object must live here";

    constexpr int kCycles = 200;
    std::vector<int> ids;
    ids.reserve( kCycles );

    for( int i = 0; i < kCycles; ++i )
    {
        const int id = owner.startTimer( 1000 );
        ASSERT_GT( id, 0 ) << "the adopted thread should have a dispatcher to register with";
        owner.killTimer( id );
        ids.push_back( id );
    }

    const int idSpan = ids.back() - ids.front();
    EXPECT_LE( idSpan, 10 )
        << kCycles << " start/kill cycles on a single timer spanned " << ( idSpan + 1 )
        << " ids (from " << ids.front() << " to " << ids.back()
        << "). Ids are not being returned to the pool, so the counter climbs until it wraps and "
        "eventually collides with the -1 failure sentinel.";
}

// ---------------------------------------------------------------------------------------------
// Defect (R28, fixed 2026-08-13): an object destroyed during a dispatch pass still received the
// rest of that pass.
//
// processEvents() takes its work out of the shared containers before it dispatches -- the event
// queue is swapped into a local batch, expired timers are collected into another -- so that no lock
// is held while a handler runs. ~Object() -> removeEventsForReceiver() could only see the
// containers, never the batches, so an object deleted from inside any handler left its remaining
// entries in the pass and each one was dispatched through a freed pointer.
//
// The timer half crashed outright; the queued half did not, which made it the more dangerous of the
// two. Both batches are now published under the dispatcher's mutex, and removeEventsForReceiver()
// cancels the destroyed receiver's entries in them -- the same mechanism unregisterTimer() has used
// on the timer batch since R24, wired to the other canceller as well.
// ---------------------------------------------------------------------------------------------

//! Fires on a timer and deletes a nominated sibling from inside the handler.
class TimerHandlerSiblingKiller : public Object
{
public:
    Object* mVictim { nullptr };            //!< Deleted on the first fire, then cleared.
    int mFireCount { 0 };                    //!< Fires delivered to this object.

protected:
    virtual void timerEvent
        (
        TimerEvent* aEvent
        ) override
    {
        ( void )aEvent;
        ++mFireCount;
        delete mVictim;
        mVictim = nullptr;
    }

};

//! Counts the timer events it is given, so a delivery to a destroyed object is visible as a count.
class TimerFireCounter : public Object
{
public:
    int mFireCount { 0 };  //!< Fires delivered to this object.

protected:
    virtual void timerEvent
        (
        TimerEvent* aEvent
        ) override
    {
        ( void )aEvent;
        ++mFireCount;
    }

};

//! Verifies an object deleted from inside a sibling's timer handler is not then sent its own timer.
//!
//! Both timers use the same interval, so both TimerEvents are collected into one batch before
//! either handler runs. The first handler deletes the second object; its event is already in the
//! batch by then.
//!
//! Before the fix this **segfaulted** in a plain build -- Object::event() reading the vtable of a
//! freed object -- and AddressSanitizer reported a heap-use-after-free in
//! EventDispatcherDefault::processEvents(). A crash is the assertion here; the surviving-object
//! checks below are what keep the test honest if the crash ever becomes a silent corruption again.
TEST( EventDispatcherDefaultDefectTest, ObjectDeletedDuringTimerDispatchIsNotThenSentItsOwnTimer )
{
    // The current thread's own dispatcher, not a standalone one: the defect is in the interaction
    // between ~Object() and the pass, and ~Object() cancels through the dispatcher its affinity
    // names. A detached dispatcher would never be reached and the test would pass vacuously.
    Thread* thread = Thread::currentThread();
    ASSERT_NE( thread, nullptr );

    auto* killer = new TimerHandlerSiblingKiller();
    auto* victim = new TimerFireCounter();
    killer->mVictim = victim;

    constexpr int kIntervalMs = 10;
    ASSERT_GT( killer->startTimer( kIntervalMs ), 0 );
    ASSERT_GT( victim->startTimer( kIntervalMs ), 0 );

    // Well past both deadlines, so the collection loop takes both into one batch.
    std::this_thread::sleep_for( std::chrono::milliseconds( kIntervalMs * 4 ) );
    thread->processEvents();

    EXPECT_EQ( killer->mFireCount, 1 )
        << "the surviving object's own timer should still have been delivered.";
    EXPECT_EQ( killer->mVictim, nullptr ) << "the handler did not run the deletion it exists for.";

    delete killer;
}

//! Destroys a nominated object from inside a *nested* dispatch pass run from its timer handler.
class NestedPassSiblingKiller : public Object
{
public:
    Object* mVictim { nullptr };  //!< deleteLater()'d and then reaped by the nested pass.
    int mFireCount { 0 };          //!< Fires delivered to this object.

protected:
    virtual void timerEvent
        (
        TimerEvent* aEvent
        ) override
    {
        ( void )aEvent;
        ++mFireCount;
        if( mVictim )
        {
            mVictim->deleteLater();
            mVictim = nullptr;

            // Nested pass. It drains the deferred delete just queued, so the victim is destroyed
            // one dispatch pass *inside* the one that is still holding its TimerEvent.
            Thread::currentThread()->processEvents();
        }
    }

};

//! Verifies a destruction inside a nested pass also cancels the outer pass's entries.
//!
//! Nested event processing is ordinary in Qt-shaped code -- a handler runs its own loop and returns
//! later. The outer pass is suspended, not finished: it still holds a batch it will go on
//! dispatching. So the cancellation raised by ~Object() inside the nested pass has to reach the
//! outer batch too, which is why running passes are published as a chain rather than as one frame.
//!
//! With a single published frame this test crashes exactly like the non-nested one, and for a
//! sharper reason: the nested pass would not merely hide the outer frame, it would clear the
//! publication on the way out and leave the rest of the outer pass unprotected.
TEST( EventDispatcherDefaultDefectTest, DeletionInANestedPassCancelsTheOuterPassEntriesToo )
{
    Thread* thread = Thread::currentThread();
    ASSERT_NE( thread, nullptr );

    auto* killer = new NestedPassSiblingKiller();
    auto* victim = new TimerFireCounter();
    killer->mVictim = victim;

    // Registration order is batch order, so the killer's timer is collected ahead of the victim's
    // and the victim is destroyed while its own TimerEvent is still waiting in that batch.
    constexpr int kIntervalMs = 10;
    ASSERT_GT( killer->startTimer( kIntervalMs ), 0 );
    ASSERT_GT( victim->startTimer( kIntervalMs ), 0 );

    std::this_thread::sleep_for( std::chrono::milliseconds( kIntervalMs * 4 ) );
    thread->processEvents();

    EXPECT_EQ( killer->mFireCount, 1 );
    EXPECT_EQ( killer->mVictim, nullptr ) << "the handler did not run the deletion it exists for.";

    delete killer;
}

//! Deletes itself from inside a queued call, and reports that it ran to a counter the test owns.
class QueuedCallSelfDeleter : public Object
{
public:
    int* mCallCount { nullptr };  //!< Points at the test's own stack, so it outlives this object.

    void onCall
        (
        int aValue
        )
    {
        ( void )aValue;
        ++( *mCallCount );
        delete this;
    }

};

//! Verifies an object deleted in a queued call is not then sent a deferred delete from that batch.
//!
//! The queue holds a metacall and then a deferred delete for the same object, so both are taken
//! into one batch. The metacall destroys the object; the deferred delete is already in the batch by
//! then, and dispatching it runs `delete this` a second time. Before the fix this **segfaulted**.
//!
//! This ordering is the queued path's *observable* half, and it is the only one worth asserting on.
//! Its mirror image -- two ordinary metacalls, the first deleting the receiver -- is undefined
//! behaviour that today touches nothing: Object::event() is not virtual, and its MetaCall branch
//! reads only the event, never `this`, so the second entry calls a member function on a destroyed
//! object without dereferencing a single byte of it. Not even AddressSanitizer can see that, so
//! there is nothing a test could assert. The fix covers both; only this one can be pinned.
//!
//! Note also what does *not* cover this: the deletedReceivers guard in processEvents() records a
//! receiver only once a DeferredDeleteEvent has destroyed it. Here the object is destroyed by an
//! ordinary metacall, so the guard never learns about it.
TEST( ObjectDefectTest, DeferredDeleteInTheSameBatchDoesNotDeleteAnAlreadyDeletedObject )
{
    Thread* thread = Thread::currentThread();
    ASSERT_NE( thread, nullptr );

    int callCount = 0;

    Signal<int> signal;
    auto* receiver = new QueuedCallSelfDeleter();
    receiver->mCallCount = &callCount;

    Object::connect( signal, receiver, &QueuedCallSelfDeleter::onCall, ConnectionType::Queued );

    signal.emit( 1 );          // MetaCallEvent, dispatched first
    receiver->deleteLater();   // DeferredDeleteEvent, same batch, dispatched second

    thread->processEvents();

    EXPECT_EQ( callCount, 1 )
        << "the queued call should have run exactly once and destroyed the receiver.";
}

// ---------------------------------------------------------------------------------------------
// Contract change (R31, 2026-08-13): a wake callback may now re-enter the dispatcher.
//
// **Not a bug fix.** The old contract said the opposite -- Thread::setWakeCallback() documented
// that the callback "must not block or re-enter the dispatcher" -- so a callback that re-entered
// was misuse, and the deadlock it hit was the caller's. This file is otherwise regression tests for
// defects; this one is here because it guards a promise, not because it once failed.
//
// The constraint was withdrawn because it was inconsistent and untestable. wakeWaiter() may invoke
// the callback, and postEvent(), wakeUp() and interrupt() already released mMutex before calling
// it; only registerTimer(), unregisterTimer() and takeTimersForReceiver() held it. So a re-entrant
// callback passed every test anyone would write -- postEvent() is the path you reach for -- and
// deadlocked the first time a timer happened to wake the loop. Now every path releases first.
// ---------------------------------------------------------------------------------------------

//! Verifies a wake callback may call back into the dispatcher, whatever woke it.
//!
//! Structured to fail rather than hang. If the wake moves back under the lock the worker never
//! returns from registerTimer(), so the future times out, the test fails with a message, and the
//! thread is detached onto a deliberately leaked dispatcher -- detaching it onto a stack object
//! would leave it holding a pointer into this frame.
TEST( EventDispatcherDefaultDefectTest, WakeCallbackMayReEnterTheDispatcherFromEveryPath )
{
    // Heap-allocated and released only on success; see above.
    auto* dispatcher = new DefectTestableDispatcher();
    Object receiver;

    std::atomic<int> callbackCount { 0 };
    dispatcher->setWakeCallback(
        [dispatcher, &receiver, &callbackCount]()
        {
            // Re-entrant by design: this is what a native loop's nudge does when it decides to
            // drain our queue rather than signal a descriptor.
            if( callbackCount.fetch_add( 1 ) < 8 )
            {
                dispatcher->postEvent( &receiver, new TimerEvent( 99 ) );
            }
        } );

    std::promise<void> donePromise;
    auto doneFuture = donePromise.get_future();
    std::thread worker(
        [dispatcher, &receiver, &donePromise]()
        {
            dispatcher->registerTimer( 1, 1000, &receiver );        // wakes with a timer change
            dispatcher->unregisterTimer( 1 );                        // and again on removal
            dispatcher->postEvent( &receiver, new TimerEvent( 1 ) );  // the path that always worked
            donePromise.set_value();
        } );

    const bool finished
        = doneFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;

    EXPECT_TRUE( finished )
        << "a wake callback that calls back into the dispatcher deadlocked it. registerTimer() and "
        "unregisterTimer() invoke wakeWaiter() -- and therefore the callback -- while still holding "
        "mMutex, which is not recursive.";

    if( !finished )
    {
        worker.detach();
        return;   // dispatcher deliberately leaked: the worker still points at it
    }

    worker.join();
    EXPECT_GT( callbackCount.load(), 0 ) << "the callback was never reached at all";
    delete dispatcher;
}

// ---------------------------------------------------------------------------------------------
// Defect (R32, fixed 2026-08-14): moveToThread() left already-posted events behind.
//
// Timers were carried across; nothing carried the events. So a queued call posted just before a
// move ran on the thread the object had *left* -- silently, because nothing re-checks affinity once
// an event is queued, and that is the one guarantee a queued connection exists to provide.
// deleteLater() was the sharper case: its DeferredDeleteEvent stranded the same way, so the
// destructor ran on the wrong thread and tripped ~Object()'s own cross-thread warning, whose advice
// is to use deleteLater().
//
// Qt migrates them, in QObjectPrivate::setThreadData_helper(): it walks the old thread's
// postEventList, re-adds every entry whose receiver is the moving object to the target's list, and
// wakes the target. Confirmed against Qt 6.11.1 by running the same program against both.
// ---------------------------------------------------------------------------------------------

//! Records which thread ran it, through a pointer the test owns so it survives anything.
//!
//! The storage is atomic because this is the one member of this helper that is genuinely shared:
//! onCall() runs on the worker, while the test's waitFor() polls the same variable from the main
//! thread with no lock between them. As a plain Thread* that was a data race -- reported by
//! ThreadSanitizer under linux64-clang, and filed as R33 in OPEN-RISKS-20260816.md. The tests still
//! passed, because gtest checks assertions rather than thread safety, which is exactly why the
//! sanitizer signal on the test binaries is worth keeping clean.
class ThreadRecordingReceiver : public Object
{
public:
    std::atomic<Thread*>* mRanOn { nullptr };  //!< Points at the test's own storage.

    void onCall
        (
        int aValue
        )
    {
        ( void )aValue;
        if( mRanOn )
        {
            mRanOn->store( Thread::currentThread() );
        }
    }

};

//! Verifies a queued call posted before a move runs on the thread the object moved *to*.
TEST( ObjectDefectTest, MoveToThreadCarriesAlreadyPostedEventsToTheNewThread )
{
    Thread* mainThread = Thread::currentThread();
    ASSERT_NE( mainThread, nullptr );

    Thread worker( "r32-worker" );
    worker.start();
    // Waits for the dispatcher, not for isRunning(): start() sets the running flag before the run
    // body creates the dispatcher, so the two are not the same moment. This test is about carrying
    // events to a thread that can already take them; the parked-event path has its own test below.
    ASSERT_TRUE( waitFor( [&worker]()
        {
            return worker.eventDispatcher() != nullptr;
        } ) );

    std::atomic<Thread*> ranOn { nullptr };
    Signal<int> signal;
    ThreadRecordingReceiver receiver;   // lives on this thread
    receiver.mRanOn = &ranOn;
    Object::connect( signal, &receiver, &ThreadRecordingReceiver::onCall,
        ConnectionType::Queued );

    signal.emit( 1 );                    // lands in THIS thread's queue
    ASSERT_TRUE( receiver.moveToThread( &worker ) );

    ASSERT_TRUE( waitFor( [&ranOn]()
        {
            return ranOn.load() != nullptr;
        } ) )
        << "the queued call never ran at all after the move.";

    EXPECT_EQ( ranOn.load(), &worker )
        << "the call was posted while the receiver lived on this thread, and ran there even though "
        "the receiver had moved. moveToThread() must carry already-posted events across, as Qt's "
        "setThreadData_helper() does; a queued connection promises the slot runs on the receiver's "
        "thread, and this is the one case where that promise was silently broken.";

    // Hand it back so the stack object is destroyed on its own thread.
    worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } );
    worker.quit();
    worker.wait();
}

//! Verifies a deleteLater() issued before a move destroys the object on the thread it moved *to*.
//!
//! Separate from the test above because the event type is what matters: a stranded
//! DeferredDeleteEvent runs `delete this` on the wrong thread, which is the case ~Object()'s
//! cross-thread-destruction warning exists to catch -- reached, before this fix, by following that
//! warning's own advice.
TEST( ObjectDefectTest, MoveToThreadCarriesAPendingDeleteLaterToTheNewThread )
{
    Thread worker( "r32-delete-worker" );
    worker.start();
    ASSERT_TRUE( waitFor( [&worker]()
        {
            return worker.eventDispatcher() != nullptr;
        } ) );

    std::atomic<Thread*> destroyedOn { nullptr };

    class Victim : public Object
    {
    public:
        std::atomic<Thread*>* mDestroyedOn { nullptr };
        ~Victim() override
        {
            if( mDestroyedOn )
            {
                mDestroyedOn->store( Thread::currentThread() );
            }
        }

    };

    auto* victim = new Victim();
    victim->mDestroyedOn = &destroyedOn;

    victim->deleteLater();                        // DeferredDeleteEvent into THIS thread's queue
    ASSERT_TRUE( victim->moveToThread( &worker ) );

    ASSERT_TRUE( waitFor( [&destroyedOn]()
        {
            return destroyedOn.load() != nullptr;
        } ) )
        << "the pending deleteLater() never ran, so the object leaked.";

    EXPECT_EQ( destroyedOn.load(), &worker )
        << "deleteLater() was called while the object lived on this thread, and the destructor ran "
        "here even though the object had moved -- a cross-thread destruction reached by following "
        "the advice in ~Object()'s own warning.";

    worker.quit();
    worker.wait();
}

//! Verifies the migration survives the canonical idiom: move first, start the thread afterwards.
//!
//! The destination has no dispatcher at all at that point -- a Thread creates one in its run body --
//! so there is nowhere to post. The events are parked on the destination's ThreadData and handed to
//! the dispatcher the moment it is installed. Qt has no equivalent problem because its queue lives
//! in QThreadData rather than in the dispatcher.
TEST( ObjectDefectTest, EventsMovedToAnUnstartedThreadAreDeliveredWhenItStarts )
{
    Thread worker( "r32-unstarted-worker" );      // deliberately not started yet

    std::atomic<Thread*> ranOn { nullptr };
    Signal<int> signal;
    ThreadRecordingReceiver receiver;
    receiver.mRanOn = &ranOn;
    Object::connect( signal, &receiver, &ThreadRecordingReceiver::onCall,
        ConnectionType::Queued );

    signal.emit( 1 );
    ASSERT_TRUE( receiver.moveToThread( &worker ) );
    EXPECT_EQ( ranOn.load(), nullptr ) << "nothing should have run before the thread exists";

    worker.start();

    ASSERT_TRUE( waitFor( [&ranOn]()
        {
            return ranOn.load() != nullptr;
        } ) )
        << "the event was posted before the destination had a dispatcher, and was dropped instead "
        "of being held until one existed.";
    EXPECT_EQ( ranOn.load(), &worker );

    worker.post( [&receiver]()
        {
            receiver.moveToThread( nullptr );
        } );
    worker.quit();
    worker.wait();
}

// ---------------------------------------------------------------------------------------------
// Thread lifecycle defects. Paired: this block is identical in both libraries.
// ---------------------------------------------------------------------------------------------

namespace
{
    using namespace std::chrono_literals;
    using namespace QtLikeSignal;

    //! Posts a marker task and blocks until it has run, proving the thread's loop is up and
    //! draining.
    //!
    //! Needed because post() now goes through the thread's event dispatcher, which threadBody()
    //! creates as it starts; a post() issued between start() returning and that dispatcher existing
    //! is refused rather than queued. Retrying is the documented way to wait it out -- the shared
    //! suites' waitUntilRunning() does exactly this. Before the dispatcher port these tests could
    //! post straight after start(), because the mailbox lived in ThreadData and existed from
    //! construction.
    inline bool waitUntilRunning
        (
        Thread& aThread,       //!< The thread to wait for.
        int aTimeoutMs = 5000  //!< How long to wait before giving up.
        )
    {
        std::promise<void> ran;
        std::future<void> ranFuture = ran.get_future();
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds( aTimeoutMs );

        while( !aThread.post( [&ran]()
            {
                ran.set_value();
            } ) )
        {
            if( std::chrono::steady_clock::now() > deadline )
            {
                return false;
            }
            std::this_thread::yield();
        }
        return ranFuture.wait_for( std::chrono::milliseconds( aTimeoutMs ) ) ==
               std::future_status::ready;
    }

    // ---------------------------------------------------------------------------------------------
    // Defect: Thread could never be restarted after quit(). mRunning was set true only in the
    // constructor and never reset, so start() called again after a previous quit() spawned a thread
    // whose loop() immediately observed !mRunning and exited after at most one task-drain pass,
    // never resuming real operation.
    // ---------------------------------------------------------------------------------------------

    /**
     * @brief Regression test verifying a restarted Thread stays alive for continuous work, not just
     * a single lucky task.
     *
     * The naive fix attempt (posting one task immediately after start() and waiting for it) does NOT
     * reliably catch this bug: even with mRunning never reset, a restarted loop() still drains
     * whatever is *already* in mTasks before it re-checks and honors the stale !mRunning -- so a task
     * posted fast enough, right after start(), can slip through and run anyway before the loop dies,
     * masking the defect. The real, documented symptom (see start()'s comment) is that the restarted
     * loop processes at most one batch then exits, never resuming continuous operation. This test
     * proves *that* specifically: after the first task is confirmed to have run, it waits past that
     * point and posts a *second* task. Before the fix, the loop has already exited by then, so the
     * second task is pushed into a queue nothing is left to drain, and never runs.
     */
    TEST( ThreadDefectTest, RestartedThreadStaysAliveForContinuousWork )
    {
        Thread thread( "restart-worker" );

        auto runOneCycle = [&thread]()
            {
                thread.start();
                if( !waitUntilRunning( thread ) )
                {
                    return false; // loop never came up; nothing further to check this cycle
                }

                std::promise<void> firstRanPromise;
                auto firstRanFuture = firstRanPromise.get_future();
                thread.post( [&firstRanPromise]()
                    {
                        firstRanPromise.set_value();
                    } );
                if( firstRanFuture.wait_for( 5s ) != std::future_status::ready )
                {
                    return false; // first task never ran; nothing further to check this cycle
                }

                // Give a genuinely buggy loop time to have already exited after that first batch.
                std::this_thread::sleep_for( 100ms );

                std::promise<void> secondRanPromise;
                auto secondRanFuture = secondRanPromise.get_future();
                thread.post( [&secondRanPromise]()
                    {
                        secondRanPromise.set_value();
                    } );
                const bool secondRan = secondRanFuture.wait_for( 5s ) == std::future_status::ready;

                thread.quit();
                thread.wait();

                return secondRan;
            };

        EXPECT_TRUE( runOneCycle() ) <<
            "thread did not stay alive for continuous work on its first run.";
        EXPECT_TRUE( runOneCycle() ) <<
            "restarted thread did not stay alive for continuous work -- "
            "the loop likely exited after one batch instead of resuming.";
    }

    /**
     * @brief Regression test for the specific failure mode: restarting without ever having waited on
     * the previous run's thread must not crash (std::terminate() from destroying a joinable
     * std::thread) or deadlock.
     *
     * The previous run's std::thread object is deliberately left unjoined by the caller (no
     * thread.wait() between quit() and the second start()) to reach the exact state start() must
     * handle: a finished-but-still-joinable mThread.
     */
    TEST( ThreadDefectTest, RestartAfterQuitWithoutExplicitJoinDoesNotTerminate )
    {
        Thread thread( "restart-worker-2" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        // Post a task and wait for it, which also guarantees the loop has actually begun running
        // before quit() is requested below.
        std::promise<void> firstRanPromise;
        auto firstRanFuture = firstRanPromise.get_future();
        thread.post( [&firstRanPromise]()
            {
                firstRanPromise.set_value();
            } );
        ASSERT_EQ( firstRanFuture.wait_for( 5s ), std::future_status::ready );

        thread.quit();

        // Deliberately no thread.wait() here -- give the loop a moment to actually finish so mThread
        // is in the "finished but unjoined" state start() must handle, without relying on start()'s
        // own internal wait to also cover the "still mid-shutdown" case (that path is exercised by
        // start() unconditionally regardless).
        std::this_thread::sleep_for( 50ms );

        // Before the fix, this line would either be a no-op (mThread.joinable() was true, so the old
        // guard clause returned immediately) or -- if that guard had been naively removed instead of
        // properly fixed -- overwrite a joinable std::thread and call std::terminate(), aborting the
        // whole test process rather than failing gracefully. That abort *is* the failure signal here.
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        std::promise<void> secondRanPromise;
        auto secondRanFuture = secondRanPromise.get_future();
        thread.post( [&secondRanPromise]()
            {
                secondRanPromise.set_value();
            } );
        EXPECT_EQ( secondRanFuture.wait_for( 5s ), std::future_status::ready )
            << "restarted thread never processed the posted task.";

        thread.quit();
        thread.wait();
    }

    // ---------------------------------------------------------------------------------------------
    // Defect: deleteLater() (and post() generally) could silently strand a task if it raced quit()
    // -- accepted into mTasks (or lost before reaching it) with nothing left to ever drain it, e.g.
    // leaking an Object whose deleteLater() lost this race. post() now reports failure (via
    // mLoopFinished, checked atomically with the loop's own stop decision) instead of silently
    // stranding the task, and deleteLater() falls back to a synchronous delete on that failure.
    // ---------------------------------------------------------------------------------------------

    struct DefectDeleteLaterProbe : public Object
    {
        explicit DefectDeleteLaterProbe
            (
            std::atomic<int>& aDtorCount
            )
            : mDtorCount( aDtorCount )
        {
        }

        ~DefectDeleteLaterProbe() override
        {
            mDtorCount.fetch_add( 1 );
        }

        std::atomic<int>& mDtorCount;
    };

    /**
     * @brief Verifies post() reports failure, deterministically, once a thread's loop has fully
     * stopped -- the state deleteLater()'s fallback depends on to avoid leaking.
     */
    TEST( ThreadDefectTest, PostFailsAfterLoopHasFullyStopped )
    {
        Thread thread( "post-after-stop" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );

        std::promise<void> ranPromise;
        auto ranFuture = ranPromise.get_future();
        ASSERT_TRUE( thread.post( [&ranPromise]()
            {
                ranPromise.set_value();
            } ) );
        ASSERT_EQ( ranFuture.wait_for( 5s ), std::future_status::ready );

        thread.quit();
        thread.wait(); // blocks until loop() has actually returned -- mLoopFinished is now true.

        bool ran = false;
        EXPECT_FALSE( thread.post( [&ran]()
            {
                ran = true;
            } ) )
            << "post() should report failure once the loop has fully stopped.";
        EXPECT_FALSE( ran );
    }

    /**
     * @brief Verifies deleteLater() does not leak when its target thread's loop has already fully
     * stopped -- it must fall back to deleting synchronously rather than stranding the task.
     */
    TEST( ThreadDefectTest, DeleteLaterDoesNotLeakAfterThreadHasFullyStopped )
    {
        Thread thread( "deletelater-after-stop" );
        thread.start();
        ASSERT_TRUE( waitUntilRunning( thread ) );
        thread.quit();
        thread.wait(); // fully stopped before the probe is even constructed on it.

        std::atomic<int> dtorCount { 0 };
        auto* probe = new DefectDeleteLaterProbe( dtorCount );
        probe->moveToThread( &thread );

        probe->deleteLater();

        EXPECT_EQ( dtorCount.load(), 1 )
            << "deleteLater() stranded the object instead of falling back to a synchronous delete.";
    }

    /**
     * @brief Stress test for the race itself: quit() and post() (via deleteLater()) hammered
     * concurrently, many times, checking that every single deleteLater() call is accounted for --
     * either it actually ran (queued successfully into a still-live loop) or the object was deleted
     * synchronously (post() correctly reported failure). Before the fix, a task could be silently
     * accepted into mTasks moments before the loop committed to stopping, with nothing left to ever
     * drain it -- neither outcome, i.e. a genuine leak. This is a best-effort stress test: the race
     * window is narrow, so a clean run raises confidence but isn't proof by itself; what makes it
     * meaningful is running many iterations, ideally under AddressSanitizer/LeakSanitizer.
     */
    TEST( ThreadDefectTest, ConcurrentDeleteLaterAndQuitStress )
    {
        constexpr int kIterations = 300;
        std::atomic<int> dtorCount { 0 };

        for( int i = 0; i < kIterations; ++i )
        {
            Thread thread( "race-worker" );
            thread.start();
            ASSERT_TRUE( waitUntilRunning( thread ) );

            auto* probe = new DefectDeleteLaterProbe( dtorCount );
            probe->moveToThread( &thread );

            std::thread deleter( [probe]()
                {
                    probe->deleteLater();
                } );
            thread.quit(); // races the deleter thread's post() from the outside
            deleter.join();
            thread.wait();
        }

        EXPECT_EQ( dtorCount.load(), kIterations )
            << "one or more probes were neither run via the queue nor deleted synchronously -- "
            "a genuine leak through the race window.";
    }

    // ---------------------------------------------------------------------------------------------
    // Defect: Object stored its thread affinity as a raw Thread*, and connections captured that raw
    // pointer into their closures, dereferencing it (ctxThread->isCurrent(), ctxThread->post()) on
    // every emit -- with no guarantee the Thread still existed by then. Two distinct triggers, both
    // confirmed as real heap-use-after-frees under AddressSanitizer (in Thread::isCurrent(), called
    // from the connection wrapper):
    //
    //   1. an auto-adopted dummy Thread, destroyed at native thread exit (thread_local teardown);
    //   2. an explicit, user-owned Thread, destroyed whenever the user chooses.
    //
    // Fixed by adopting Qt's model: affinity is now held as a shared_ptr<ThreadData> (which outlives
    // its Thread) rather than a Thread*, and ~Thread() nulls the back-pointer -- so thread() reports
    // nullptr instead of dangling. Mirrors QObjectPrivate::threadData (a refcounted QThreadData*,
    // never a QThread*) plus ~QThread()'s `d->data->thread.storeRelease(nullptr)`.
    //
    // Both tests below need AddressSanitizer to fail loudly if they regress: the defect is a
    // use-after-free read of a few bytes inside a freed allocation, which is not guaranteed to crash
    // or produce an observably wrong result without a sanitizer's poisoning (this project's debug
    // builds enable it by default).
    // ---------------------------------------------------------------------------------------------

    /**
     * @brief Trigger 1: an auto-adopted dummy Thread destroyed at native thread exit, while a live
     * connection still references it.
     *
     * Note the shape here is load-bearing. An earlier draft made the context Object stack-local to
     * the adopting thread's own lambda and produced NO crash even pre-fix, because ~Object() (which
     * disconnects its own connections) ran during normal stack unwinding strictly BEFORE the
     * thread_local dummy's destructor -- so the connection was already gone before anything could
     * dereference it. The context must be heap-allocated and deliberately outlive the adopting
     * thread, so the connection (and the affinity it captured) is still reachable afterward.
     */
    TEST( ThreadDefectTest, AutoAdoptedDummyOutlivesAdoptingThreadWhileReferenced )
    {
        Signal<> sig;
        Object* context = nullptr;

        std::thread adopting( [&]()
            {
                context = new Object(); // auto-adopts a dummy Thread local to THIS thread
                Object::connect( sig, context, []
                {
                }, ConnectionType::Queued );
                // Deliberately not deleted here: context, and the connection referencing the
                // adopted dummy Thread through it, must outlive this thread.
            } );
        adopting.join();

        // The dummy Thread is destroyed now, but context still holds its ThreadData, so the
        // connection resolves thread() == nullptr instead of dereferencing freed memory.
        EXPECT_EQ( context->thread(), nullptr )
            <<
            "affinity should report nullptr once the adopted Thread is gone, not a stale pointer.";

        sig.emit(); // resolves the affinity through ThreadData; must not touch freed memory

        delete context;
        SUCCEED(); // reaching here without an ASan report is the assertion.
    }

    /**
     * @brief Trigger 2: an explicit, user-owned Thread destroyed while an Object still lives on it
     * and a connection still references it.
     *
     * This case was NOT covered by the first attempt at fixing this defect (which only kept
     * auto-adopted dummies alive) and still reproduced the identical heap-use-after-free in
     * Thread::isCurrent(). It is what motivated moving to Qt's ThreadData model, which covers every
     * Thread rather than just the adopted ones.
     */
    TEST( ThreadDefectTest, ExplicitThreadDestroyedWhileObjectStillReferencesIt )
    {
        Signal<> sig;

        auto* worker = new Thread( "explicit-worker" );
        worker->start();
        ASSERT_TRUE( waitUntilRunning( *worker ) );

        auto* obj = new Object( worker );
        Object::connect( sig, obj, []
            {
            }, ConnectionType::Queued );
        ASSERT_EQ( obj->thread(), worker );

        worker->quit();
        worker->wait();
        delete worker; // destroyed while obj still has affinity to it

        EXPECT_EQ( obj->thread(), nullptr )
            << "affinity should report nullptr once its Thread is destroyed, not a stale pointer.";

        sig.emit(); // must not dereference the destroyed Thread

        delete obj;
        SUCCEED(); // reaching here without an ASan report is the assertion.
    }

    // ---------------------------------------------------------------------------------------------
    // Defect (FIXED): the queued-connection path used to resolve the receiver's affinity Thread into
    // a RAW Thread* and then, a few statements later, call ctxThread->post() on it:
    //
    //     Thread* ctxThread = ctxData->thread();      // (1) non-null here
    //     ... copy the emitted arguments ...
    //     ctxThread->post( ... );                     // (2) dereferences ctxThread
    //
    // The captured shared_ptr<ThreadData> kept the ThreadData alive, but NOT the Thread: if another
    // thread destroyed that Thread between (1) and (2), ~Thread() only nulled the ThreadData
    // back-pointer -- the raw ctxThread already loaded at (1) then dangled, and post() at (2) was a
    // heap-use-after-free. The existing ExplicitThreadDestroyedWhileObjectStillReferencesIt test does
    // NOT catch this: it destroys the Thread and only THEN emits single-threaded, so thread() reads
    // nullptr and takes the direct path -- it never holds a stale non-null Thread* across a
    // concurrent destruction.
    //
    // Fixed by adopting Qt6's model: the event mailbox (task queue + its mutex/condvar/accepting
    // flag) now lives in ThreadData, not Thread, and the queued path posts through the ThreadData
    // (kept alive by the captured shared_ptr) instead of a Thread*. Posting therefore never
    // dereferences a Thread; if the Thread has stopped, ThreadData::post() returns false and the
    // invocation is dropped. Mirrors QCoreApplication::postEvent() locking
    // QThreadData::postEventList and appending to the list owned BY that (Thread-outliving)
    // QThreadData. See qtbase/src/corelib/kernel/qcoreapplication.cpp (postEvent /
    // lockThreadPostEventList) and QThreadData::postEventList in qthread_p.h.
    //
    // The test below is DETERMINISTIC. The signal carries its argument by const-reference, so the
    // ONLY copy of that argument is the make_shared<tuple<...>> copy that happens in connectImpl
    // exactly between resolving the affinity and posting. WindowProbe's copy constructor therefore
    // runs inside that window; it parks the emitting thread there while the test thread frees the
    // Thread, then lets the emit proceed into the post. Pre-fix this was a use-after-free; post-fix
    // the post goes through the surviving ThreadData and is safe. Runs under AddressSanitizer.
    // ---------------------------------------------------------------------------------------------

    //! Shared coordination between the test thread and WindowProbe's copy constructor.
    struct PostRaceControl
    {
        std::atomic<bool> armed { false }; //!< true once the test wants the next copy to park
        std::atomic<bool> fired { false }; //!< ensures only the first in-window copy parks
        std::promise<void> inWindow;      //!< signalled from inside the danger window
        std::promise<void> release;       //!< test sets this to let the parked copy proceed
    };

    PostRaceControl* gPostRace = nullptr;

    //! Its copy constructor is invoked by connectImpl's queued path precisely between resolving the
    //! receiver's affinity and posting to it. It reports that it is in the window, then blocks until
    //! the test has destroyed the Thread -- turning the race into a deterministic sequence.
    struct WindowProbe
    {
        WindowProbe() = default;

        WindowProbe
            (
            const WindowProbe&
            )
        {
            if( gPostRace && gPostRace->armed.load() && !gPostRace->fired.exchange( true ) )
            {
                gPostRace->inWindow.set_value();
                gPostRace->release.get_future().wait();
            }
        }

        WindowProbe& operator=
            (
            const WindowProbe&
            ) = default;

    };

    struct WindowProbeReceiver : Object
    {
        explicit WindowProbeReceiver
            (
            Thread* aThread
            )
            : Object( aThread )
        {
        }

        void onProbe
            (
            const WindowProbe&
            )
        {
        }

    };

    /**
     * @brief Proves the queued-post use-after-free is fixed: an emit that has already resolved the
     * receiver's affinity races a concurrent destruction of that thread; posting now goes through
     * the surviving ThreadData, so it is safe and the invocation is simply dropped.
     */
    TEST( ThreadDefectTest, QueuedPostRacesConcurrentThreadDestruction )
    {
        auto* worker = new Thread( "post-race-victim" );
        worker->start();
        ASSERT_TRUE( waitUntilRunning( *worker ) );

        WindowProbeReceiver receiver( worker ); // affinity == worker
        Signal<const WindowProbe&> sig;
        Object::connect( sig, &receiver, &WindowProbeReceiver::onProbe ); // Auto

        PostRaceControl race;
        gPostRace = &race;
        race.armed.store( true );

        // Emit from a thread OTHER than the worker so the Auto connection takes the queued path:
        // resolve ctxThread == worker, copy the argument (parks here), then ctxThread->post().
        std::thread emitter( [&]()
            {
                WindowProbe probe;
                sig.emit( probe );
            } );

        // Wait until the emit is parked in the window, i.e. ctxThread has already been resolved to
        // the still-alive worker and post() has not yet been called.
        race.inWindow.get_future().wait();

        // Destroy the worker Thread while the emit still holds it as a raw ctxThread pointer.
        worker->quit();
        worker->wait();
        delete worker;

        // Let the emit proceed. Pre-fix, it now calls post() on the freed Thread -- a heap-use-
        // after-free that AddressSanitizer reports. Post-fix, posting goes through the surviving
        // ThreadData and is safe.
        race.release.set_value();
        emitter.join();

        gPostRace = nullptr;
        SUCCEED(); // reaching here without an ASan report is the assertion.
    }



}
