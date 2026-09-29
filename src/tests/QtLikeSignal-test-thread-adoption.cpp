// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for thread auto-adoption and the API it enables: every native thread that
//! touches an Object becomes a Thread on demand, so every Object has a thread affinity, and a
//! thread running its own native loop can drain our queue with processEvents() /
//! setWakeCallback().

#include "gtest/gtest.h"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/Thread.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

using namespace QtLikeSignal;

//! Verifies every thread has an identity, created on demand, and that they are distinct.
TEST( ThreadAdoptionTest, EveryNativeThreadIsAdoptedOnDemand )
{
    Thread* const main = Thread::currentThread();
    ASSERT_NE( main, nullptr );
    EXPECT_TRUE( main->isAdopted() );
    EXPECT_EQ( Thread::currentThread(), main ) << "adoption must be stable, not re-created";

    Thread* fromWorker = nullptr;
    std::thread worker( [&fromWorker]()
        {
            fromWorker = Thread::currentThread();
            EXPECT_NE( fromWorker, nullptr );
            EXPECT_TRUE( fromWorker->isAdopted() );
        } );
    worker.join();

    EXPECT_NE( fromWorker, nullptr );
    EXPECT_NE( fromWorker, main ) << "each native thread must get its own Thread";
}

//! Verifies an Object always has a thread affinity, even created off any Thread we started.
TEST( ThreadAdoptionTest, ObjectsAlwaysHaveAThreadAffinity )
{
    Object here;
    EXPECT_EQ( here.thread(), Thread::currentThread() );
    EXPECT_NE( here.thread(), nullptr );

    std::atomic<bool> hadAffinity { false };
    std::thread worker( [&hadAffinity]()
        {
            Object there;
            hadAffinity.store( there.thread() != nullptr
            && there.thread() == Thread::currentThread() );
        } );
    worker.join();

    EXPECT_TRUE( hadAffinity.load() );
}

// ---------------------------------------------------------------------------------------------
// Defect (R19): an Object with no thread affinity received cross-thread *direct* calls.
//
// dispatchMetaCall() resolves ConnectionType::Auto by comparing the receiver's thread with the
// emitting thread. When neither had been started through Thread, both sides were nullptr, compared
// equal, and the slot ran synchronously **on the emitting thread** -- an unsynchronised
// cross-thread call. Qt is explicit that this must not happen: "If a QObject has no thread affinity
// (that is, if thread() returns zero) ... then it cannot receive queued signals or posted events."
//
// Auto-adoption removes the premise rather than special-casing the symptom: there is no such thing
// as an affinity-less Object any more, so the two nullptrs cannot collapse into "same thread".
// ---------------------------------------------------------------------------------------------

//! Regression test for R19: a queued slot must never run on the emitting thread.
TEST( ThreadAdoptionTest, EmitFromAnotherThreadDoesNotRunTheSlotThere )
{
    Thread* const owner = Thread::currentThread();
    ASSERT_NE( owner, nullptr );

    Object receiver;
    ASSERT_EQ( receiver.thread(), owner );

    Signal<int> sig;
    std::atomic<Thread*> ranOn { nullptr };
    std::atomic<int> value { 0 };

    Object::connect( sig, &receiver, [&ranOn, &value]( int aValue )
        {
            ranOn.store( Thread::currentThread() );
            value.store( aValue );
        }, ConnectionType::Auto );

    std::thread emitter( [&sig]()
        {
            sig.emit( 7 );
        } );
    emitter.join();

    // Pre-fix this ran inline on `emitter`. It must instead be queued for the receiver's thread,
    // and therefore not have run at all yet -- nothing is draining this thread's queue.
    EXPECT_EQ( ranOn.load(), nullptr )
        << "the slot ran on the emitting thread; an object whose thread is not running a loop must "
        "have the call queued, not executed cross-thread.";
    EXPECT_EQ( value.load(), 0 );

    // And it is genuinely queued rather than dropped: draining delivers it, here.
    owner->processEvents();
    EXPECT_EQ( ranOn.load(), owner );
    EXPECT_EQ( value.load(), 7 );
}

//! Verifies processEvents() drains queued work for a thread with no loop of its own.
//!
//! This is the adopted-thread workflow from the mission: a thread with its own native loop calls
//! processEvents() to service our events instead of running exec().
TEST( ThreadAdoptionTest, ProcessEventsDrainsQueuedWorkWithoutAnExecLoop )
{
    Thread* const owner = Thread::currentThread();
    Object receiver;

    std::atomic<int> calls { 0 };
    Signal<> sig;
    Object::connect( sig, &receiver, [&calls]()
        {
            calls.fetch_add( 1 );
        }, ConnectionType::Queued );

    std::thread emitter( [&sig]()
        {
            sig.emit();
            sig.emit();
        } );
    emitter.join();

    EXPECT_EQ( calls.load(), 0 ) << "queued work must not run until the thread drains it";
    owner->processEvents();
    EXPECT_EQ( calls.load(), 2 );
}

//! Verifies processEvents() is rejected from a thread other than the one it belongs to.
TEST( ThreadAdoptionTest, ProcessEventsFromAnotherThreadIsRejected )
{
    Thread* const owner = Thread::currentThread();
    Object receiver;

    std::atomic<int> calls { 0 };
    Signal<> sig;
    Object::connect( sig, &receiver, [&calls]()
        {
            calls.fetch_add( 1 );
        }, ConnectionType::Queued );

    std::thread emitter( [&sig]()
        {
            sig.emit();
        } );
    emitter.join();

    std::thread intruder( [owner]()
        {
            owner->processEvents();   // wrong thread: must warn and do nothing
        } );
    intruder.join();

    EXPECT_EQ( calls.load(), 0 )
        << "processEvents() ran another thread's handlers on the wrong thread.";

    owner->processEvents();
    EXPECT_EQ( calls.load(), 1 );
}

//! Verifies an idle processEvents() returns instead of parking the thread.
//!
//! **The case the adopted-thread workflow actually depends on.** Every other test here posts work
//! first and drains second, so the queue is never empty and the wait is never reached. A real
//! native loop is not that tidy: it wakes for its own reasons, drains ours on the way past, and
//! finds nothing. If that call blocks, the loop that owns the thread is gone -- and it cannot get
//! itself back, because only a QtLikeSignal call releases the wait and the loop has no reason to
//! make one.
//!
//! AllEvents is the default for exactly this reason, so the plain call is the one under test.
TEST( ThreadAdoptionTest, IdleProcessEventsReturnsWithoutBlocking )
{
    // Run on a thread of its own, and watched with a timeout, so that a regression *fails* here
    // rather than hanging the suite: the old behaviour was an indefinite wait, and a test that
    // never returns reports nothing at all.
    std::promise<void> returnedPromise;
    auto returnedFuture = returnedPromise.get_future();

    std::thread native( [&returnedPromise]()
        {
            // Adopted on the first call, like any native thread that touches QtLikeSignal.
            Thread* const self = Thread::currentThread();

            // Nothing posted, no timer registered: this pass has nothing to do and nothing to wait
            // for, which is precisely when the wait used to be entered with no deadline.
            self->processEvents();
            self->processEvents();

            returnedPromise.set_value();
        } );

    EXPECT_EQ( returnedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "an idle processEvents() did not return, so it is waiting for work that is never "
        "coming rather than giving the thread back to the loop that owns it.";

    native.join();
}

//! Verifies a non-blocking pass still dispatches everything that was already queued.
//!
//! Not waiting must not mean not working: the drain is the whole point of the call, and the flag
//! only says what to do once there is nothing left.
TEST( ThreadAdoptionTest, NonBlockingProcessEventsStillDrainsTheQueue )
{
    Thread* const owner = Thread::currentThread();
    Object receiver;

    std::atomic<int> calls { 0 };
    Signal<> sig;
    Object::connect( sig, &receiver, [&calls]()
        {
            calls.fetch_add( 1 );
        }, ConnectionType::Queued );

    sig.emit();
    sig.emit();

    owner->processEvents( AbstractEventDispatcher::ProcessEventsFlag::AllEvents );

    EXPECT_EQ( calls.load(), 2 )
        << "a non-blocking pass skipped work that was already queued.";
}

//! Verifies setWakeCallback() notifies a thread's own native loop when work is posted.
//!
//! The other half of the adopted-thread workflow: the callback runs on the posting thread and
//! nudges the native loop, so it knows to call processEvents() rather than polling for work.
TEST( ThreadAdoptionTest, WakeCallbackFiresWhenWorkIsPosted )
{
    Thread* const owner = Thread::currentThread();
    Object receiver;

    std::mutex mutex;
    std::condition_variable woken;
    bool wakeSeen = false;

    owner->setWakeCallback( [&]()
        {
            {
                std::lock_guard<std::mutex> lock( mutex );
                wakeSeen = true;
            }
            woken.notify_all();
        } );

    std::atomic<int> calls { 0 };
    Signal<> sig;
    Object::connect( sig, &receiver, [&calls]()
        {
            calls.fetch_add( 1 );
        }, ConnectionType::Queued );

    std::thread emitter( [&sig]()
        {
            sig.emit();
        } );

    {
        std::unique_lock<std::mutex> lock( mutex );
        EXPECT_TRUE( woken.wait_for( lock, std::chrono::seconds( 5 ), [&wakeSeen]()
            {
                return wakeSeen;
            } ) ) << "posting work did not invoke the wake callback, so a native loop would never "
            "learn that there is anything to drain.";
    }
    emitter.join();

    owner->processEvents();
    EXPECT_EQ( calls.load(), 1 );

    // Leave the thread as it was found: this Thread outlives the test.
    owner->setWakeCallback( nullptr );
}
