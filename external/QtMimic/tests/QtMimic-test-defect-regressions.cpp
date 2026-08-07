//! @file
//!
//! GoogleTest suite for the QtMimic framework (Object affinity/connections,
//! Thread event loops, and CoreApplication).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Object.hpp"
#include "Thread.hpp"

#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using namespace QtMimic;

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
                thread.join();

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
     * thread.join() between quit() and the second start()) to reach the exact state start() must
     * handle: a finished-but-still-joinable mThread.
     */
    TEST( ThreadDefectTest, RestartAfterQuitWithoutExplicitJoinDoesNotTerminate )
    {
        Thread thread( "restart-worker-2" );
        thread.start();

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

        // Deliberately no thread.join() here -- give the loop a moment to actually finish so mThread
        // is in the "finished but unjoined" state start() must handle, without relying on start()'s
        // own internal join to also cover the "still mid-shutdown" case (that path is exercised by
        // start() unconditionally regardless).
        std::this_thread::sleep_for( 50ms );

        // Before the fix, this line would either be a no-op (mThread.joinable() was true, so the old
        // guard clause returned immediately) or -- if that guard had been naively removed instead of
        // properly fixed -- overwrite a joinable std::thread and call std::terminate(), aborting the
        // whole test process rather than failing gracefully. That abort *is* the failure signal here.
        thread.start();

        std::promise<void> secondRanPromise;
        auto secondRanFuture = secondRanPromise.get_future();
        thread.post( [&secondRanPromise]()
            {
                secondRanPromise.set_value();
            } );
        EXPECT_EQ( secondRanFuture.wait_for( 5s ), std::future_status::ready )
            << "restarted thread never processed the posted task.";

        thread.quit();
        thread.join();
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

        std::promise<void> ranPromise;
        auto ranFuture = ranPromise.get_future();
        ASSERT_TRUE( thread.post( [&ranPromise]()
            {
                ranPromise.set_value();
            } ) );
        ASSERT_EQ( ranFuture.wait_for( 5s ), std::future_status::ready );

        thread.quit();
        thread.join(); // blocks until loop() has actually returned -- mLoopFinished is now true.

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
        thread.quit();
        thread.join(); // fully stopped before the probe is even constructed on it.

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

            auto* probe = new DefectDeleteLaterProbe( dtorCount );
            probe->moveToThread( &thread );

            std::thread deleter( [probe]()
                {
                    probe->deleteLater();
                } );
            thread.quit(); // races the deleter thread's post() from the outside
            deleter.join();
            thread.join();
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

        auto* obj = new Object( worker );
        Object::connect( sig, obj, []
            {
            }, ConnectionType::Queued );
        ASSERT_EQ( obj->thread(), worker );

        worker->quit();
        worker->join();
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
        worker->join();
        delete worker;

        // Let the emit proceed. Pre-fix, it now calls post() on the freed Thread -- a heap-use-
        // after-free that AddressSanitizer reports. Post-fix, posting goes through the surviving
        // ThreadData and is safe.
        race.release.set_value();
        emitter.join();

        gPostRace = nullptr;
        SUCCEED(); // reaching here without an ASan report is the assertion.
    }

} // namespace
