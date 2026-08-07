//! @file
//!
//! GoogleTest suite for the QtMimic framework (Object affinity/connections,
//! Thread event loops, and CoreApplication)
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "QtMimic-test-types.hpp"

#include "gtest/gtest.h"

namespace
{
    using namespace QtMimic;

    // ------------------------------------------------------------------------------------------------------
    // The Kamikaze Slot test.
    // The goal of this test is to ensure that a slot can safely disconnect itself during an emission without
    // crashing the program.
    // ------------------------------------------------------------------------------------------------------
    struct KamikazeReceiver : public Object
    {
        Connection mConn;
        int mExecutionCount = 0;
        int mLastValue = 0;

        void onTriggered
            (
            int value
            )
        {
            mExecutionCount++;
            mLastValue = value;

            // THE TEST: Disconnect exactly while the Signal is iterating
            // over its internal list of connections.
            if( mConn.connected() )
            {
                mConn.disconnect();
            }
        }

    };

    // A normal receiver to ensure iteration doesn't break after a mutation
    struct NormalReceiver : public Object
    {
        int mExecutionCount = 0;
        int mLastValue = 0;

        void onTriggered
            (
            int value
            )
        {
            mExecutionCount++;
            mLastValue = value;
        }

    };

    TEST( ObjectTest, KamikazeSlot_DisconnectsItselfDuringEmissionWithoutCrashing )
    {
        Signal<int> sig;

        KamikazeReceiver kamikaze;
        NormalReceiver bystander;

        // 1. Connect the kamikaze slot and save its connection handle
        kamikaze.mConn = Object::connect( sig, &kamikaze, &KamikazeReceiver::onTriggered );

        // 2. Connect a normal slot immediately after it
        Connection normalConn = Object::connect( sig, &bystander, &NormalReceiver::onTriggered );

        // --- FIRST EMISSION ---
        // Expected behavior:
        // - kamikaze runs, count becomes 1. It disconnects itself.
        // - The iterator must survive the mutation.
        // - bystander runs, count becomes 1.
        sig.emit( 42 );

        EXPECT_EQ( kamikaze.mExecutionCount, 1 );
        EXPECT_EQ( kamikaze.mLastValue, 42 );
        EXPECT_EQ( bystander.mExecutionCount, 1 );
        EXPECT_EQ( bystander.mLastValue, 42 );

        // --- SECOND EMISSION ---
        // Expected behavior:
        // - kamikaze is disconnected, so it does not run (count remains 1).
        // - bystander is still connected, so it runs (count becomes 2).
        sig.emit( 43 );

        EXPECT_EQ( kamikaze.mExecutionCount, 1 );
        EXPECT_EQ( kamikaze.mLastValue, 42 );
        EXPECT_EQ( bystander.mExecutionCount, 2 );
        EXPECT_EQ( bystander.mLastValue, 43 );
    }

    // ------------------------------------------------------------------------------------------------------
    // Chain Reaction test.
    // The goal of this test is to ensure that a new connection made during an emission does not run in the current
    // emission, but does run in subsequent emissions.
    // ------------------------------------------------------------------------------------------------------
    struct ChainReactionReceiver : public Object
    {
        Signal<>& mSig;
        int mExecutionsOnA = 0;
        int mExecutionsOnB = 0;

        ChainReactionReceiver
            (
            Signal<>& s
            )
            : mSig( s )
        {
        }

        void slotA()
        {
            mExecutionsOnA++;

            // THE TEST: Connect a new slot to the same signal
            // while the signal is currently iterating through its list.
            Object::connect( mSig, this, &ChainReactionReceiver::slotB );
        }

        void slotB()
        {
            mExecutionsOnB++;
        }

    };

    TEST( ObjectTest, ChainReaction_NewConnectionDoesNotRunInCurrentEmission )
    {
        Signal<> sig;
        ChainReactionReceiver receiver( sig );

        // Only connect slotA initially
        Object::connect( sig, &receiver, &ChainReactionReceiver::slotA );

        // --- FIRST EMISSION ---
        // Expected behavior:
        // - slotA runs (count = 1).
        // - slotA connects slotB.
        // - The iterator must not crash from the underlying list expanding.
        // - slotB does NOT run yet (count = 0).
        sig.emit();

        EXPECT_EQ( receiver.mExecutionsOnA, 1 );
        EXPECT_EQ( receiver.mExecutionsOnB, 0 );

        // --- SECOND EMISSION ---
        // Expected behavior:
        // - slotA runs again (count = 2) and connects slotB a second time.
        // - The FIRST slotB connection runs (count = 1).
        sig.emit();

        EXPECT_EQ( receiver.mExecutionsOnA, 2 );
        EXPECT_EQ( receiver.mExecutionsOnB, 1 );
    }

    // ------------------------------------------------------------------------------------------------------
    // Nuke test.
    // The goal of the "Nuke" test is to ensure that a massive, systemic state change during an emission
    // doesn't crash the program, and that subsequent pending slots in the emission queue are properly aborted.
    // ------------------------------------------------------------------------------------------------------
    struct NukeReceiver : public Object
    {
        Signal<>& mSig;
        int& mExecutionCount;
        int mNukeThreshold;

        NukeReceiver
            (
            Signal<>& s,
            int& count,
            int threshold
            )
            : mSig( s )
            , mExecutionCount( count )
            , mNukeThreshold( threshold )
        {
        }

        void onTriggered()
        {
            mExecutionCount++;

            // THE TEST: When we hit the threshold, wipe out all connections.
            if( mExecutionCount == mNukeThreshold )
            {
                mSig.disconnectAll();
            }
        }

    };

    TEST( ObjectTest, TheNuke_DisconnectAllDuringEmissionAbortsRemaining )
    {
        Signal<> sig;
        int executionCount = 0;

        const int totalConnections = 1000;
        const int nukeAt = 500;

        NukeReceiver receiver( sig, executionCount, nukeAt );

        // Connect the same receiver 1,000 times
        for( int i = 0; i < totalConnections; ++i )
        {
            Object::connect( sig, &receiver, &NukeReceiver::onTriggered );
        }

        // --- FIRST EMISSION ---
        // Expected behavior:
        // - Iteration starts.
        // - Slots 1 through 500 run normally.
        // - Slot 500 triggers disconnectAll().
        // - The emission loop MUST NOT CRASH.
        // - Slots 501 through 1000 MUST NOT RUN.
        sig.emit();

        EXPECT_EQ( executionCount, nukeAt );

        // --- SECOND EMISSION ---
        // Expected behavior:
        // - Everything is disconnected, nothing should run.
        executionCount = 0;
        sig.emit();

        EXPECT_EQ( executionCount, 0 );
    }

    // ------------------------------------------------------------------------------------------------------
    // Connect/Disconnect Storm test.
    // The goal of this test is to hammer the signal's internal storage from multiple threads simultaneously.
    // We want to force race conditions by having one thread constantly iterating over the connection list
    // (via emit()) while several other threads are aggressively modifying that exact same list.
    // ------------------------------------------------------------------------------------------------------
    struct StormReceiver : public Object
    {
        std::atomic<int>& mExecutionCount;

        StormReceiver
            (
            std::atomic<int>& count
            )
            : mExecutionCount( count )
        {
        }

        void onTriggered()
        {
            // Must be atomic because multiple threads might execute this concurrently
            // if the signal uses queued/multithreaded dispatch, or if the emitter
            // thread executes it.
            mExecutionCount.fetch_add( 1, std::memory_order_relaxed );
        }

    };

    TEST( ObjectTest, TheStorm_ConcurrentConnectDisconnectAndEmit )
    {
        Signal<> sig;
        std::atomic<int> executionCount { 0 };
        StormReceiver receiver( executionCount );

        std::atomic<bool> keepEmitting { true };

        // 1. Thread A: The Emitter
        // Constantly iterates over the connection list and fires slots.
        std::thread emitter( [&]()
            {
                while( keepEmitting.load( std::memory_order_relaxed ) )
                {
                    sig.emit();
                }
            } );

        // 2. Threads B, C, D, etc.: The Mutators
        // Constantly add and remove connections to the same signal.
        const int numMutators = 4;
        const int iterationsPerMutator = 10000;
        std::vector<std::thread> mutators;

        for( int i = 0; i < numMutators; ++i )
        {
            mutators.emplace_back( [&]()
                {
                    for( int j = 0; j < iterationsPerMutator; ++j )
                    {
                        // Connect a slot
                        // Must use ConnectionType::Direct because the main thread is stuck waiting on join instead of
                        // running an event loop, so queued connections would never run.
                        Connection conn = Object::connect( sig, &receiver,
                            &StormReceiver::onTriggered, ConnectionType::Direct );

                        // Force the mutator thread to briefly give up the CPU,
                        // leaving the connection alive long enough for the emitter to hit it.
                        std::this_thread::yield();

                        // Immediately disconnect it.
                        // This maximizes contention on the Signal's internal state.
                        conn.disconnect();
                    }
                } );
        }

        // 3. Wait for all mutators to finish their chaos
        for( auto& t : mutators )
        {
            t.join();
        }

        // 4. Shut down the emitter
        keepEmitting.store( false, std::memory_order_relaxed );
        emitter.join();

        // If the test reaches this point without a segmentation fault,
        // deadlock, or data race (detectable via ThreadSanitizer), it passes.
        SUCCEED();

        // We can also log the execution count just to observe how many
        // emissions managed to hit a valid connection mid-storm.
        std::cout << "Total successful slot executions: " << executionCount.load() << "\n";
    }

    // ------------------------------------------------------------------------------------------------------
    // Massive Fan-In test.
    // The goal of this test is to hammer the receiver's event queue from dozens of threads simultaneously.
    // We want to ensure that the internal queue holding cross-thread events is thread-safe, doesn't drop
    // any emissions under heavy contention, and doesn't duplicate them.
    // ------------------------------------------------------------------------------------------------------
    struct FanInReceiver : public QtMimic::Object
    {
        FanInReceiver
            (
            Thread* t
            )
            : Object( t )
        {
        }

        // We use atomic here just in case the library has a routing bug and
        // mistakenly runs a direct connection. If the library is perfect,
        // a plain 'int' would be safe since the event loop is single-threaded.
        std::atomic<int> executionCount { 0 };

        void onTriggered()
        {
            // Execution happens entirely on the worker thread's event loop
            executionCount.fetch_add( 1, std::memory_order_relaxed );
        }

    };

    TEST( ObjectTest, MassiveFanIn_NoDroppedEventsUnderHeavyLoad )
    {
        // 1. Setup the receiving thread (Thread A)
        Thread worker( "receiver_thread" );
        worker.start();

        FanInReceiver receiver( &worker );

        const int numSenders = 50;
        const int emitsPerSender = 1000;
        const int totalExpected = numSenders * emitsPerSender;

        // Create 50 distinct signals
        std::vector<std::unique_ptr<Signal<> > > signals;
        for( int i = 0; i < numSenders; ++i )
        {
            signals.push_back( std::make_unique<Signal<> >() );
            Object::connect( *signals[i], &receiver, &FanInReceiver::onTriggered );
        }

        // 2. Launch 50 sender threads
        // They will all wake up and blast the receiver's event queue simultaneously.
        std::vector<std::thread> senderThreads;
        for( int i = 0; i < numSenders; ++i )
        {
            senderThreads.emplace_back( [&, i]()
                {
                    for( int j = 0; j < emitsPerSender; ++j )
                    {
                        signals[i]->emit();
                    }
                } );
        }

        // 3. Wait for all senders to finish emitting
        for( auto& t : senderThreads )
        {
            t.join();
        }

        // 4. Wait for the worker thread to process all 50,000 queued events.
        // Use barrier task pattern to wait for the queue to drain.
        std::mutex barrierMutex;
        std::condition_variable barrierCv;
        bool barrierDone = false;

        worker.post( [&]()
            {
                {
                    std::lock_guard<std::mutex> lock( barrierMutex );
                    barrierDone = true;
                }
                barrierCv.notify_one();
            } );

        {
            std::unique_lock<std::mutex> lock( barrierMutex );
            // We give it 5 seconds. Processing 50k simple events should take milliseconds.
            ASSERT_TRUE( barrierCv.wait_for( lock, std::chrono::seconds( 5 ), [&]
                {
                    return barrierDone;
                } ) );
        }

        // 5. The ultimate validation: Did any events get dropped or double-counted?
        EXPECT_EQ( receiver.executionCount.load(), totalExpected );

        worker.quit();
        worker.join();
    }

    //------------------------------------------------------------------------------------------------------
    // Deep Argument Copying test.
    // The goal of this test is to verify that our library smartly shares the payload (e.g., using std::shared_ptr
    // or perfectly forwarding it) rather than aggressively copying it.
    // Note that even highly optimized Qt will copy the payload once per connected receiver, so we can't
    // avoid that entirely.
    //------------------------------------------------------------------------------------------------------
    struct HeavyPayload : public Object
    {
        static std::atomic<int> copyCount;
        std::vector<std::string> data;

        HeavyPayload()
            : data( 1000, "chunk" )
        {
        }

        // Copy Constructor
        HeavyPayload
            (
            const HeavyPayload& other
            )
            : data( other.data )
        {
            copyCount.fetch_add( 1, std::memory_order_relaxed );
        }

        // Move Constructor
        HeavyPayload
            (
            HeavyPayload&& other
            ) noexcept
            : data( std::move( other.data ) )
        {
        }

    };

    std::atomic<int> HeavyPayload::copyCount { 0 };

    struct CopyTrackerReceiver : public Object
    {
        CopyTrackerReceiver
            (
            Thread* t
            )
            : Object( t )
        {
        }

        void onTriggered
            (
            const HeavyPayload& payload
            )
        {
            // Just reading the data to ensure it arrived safely
            EXPECT_EQ( payload.data.size(), 1000 );
        }

    };

    TEST( ObjectTest, DeepArgumentCopying_QueuedEventsMinimizeCopies )
    {
        Thread worker( "worker_thread" );
        worker.start();

        Signal<HeavyPayload> sig;
        HeavyPayload::copyCount.store( 0 );

        const int numReceivers = 100;
        std::vector<std::unique_ptr<CopyTrackerReceiver> > receivers;

        for( int i = 0; i < numReceivers; ++i )
        {
            receivers.push_back( std::make_unique<CopyTrackerReceiver>( &worker ) );
            Object::connect( sig, receivers.back().get(), &CopyTrackerReceiver::onTriggered,
                ConnectionType::Queued );
        }

        // Emit a heavy payload.
        HeavyPayload initialPayload;
        sig.emit( initialPayload );

        // Barrier wait to drain the worker queue
        std::mutex barrierMutex;
        std::condition_variable barrierCv;
        bool barrierDone = false;
        worker.post( [&]()
            {
                {
                    std::lock_guard<std::mutex> lock( barrierMutex );
                    barrierDone = true;
                }
                barrierCv.notify_one();
            } );

        {
            std::unique_lock<std::mutex> lock( barrierMutex );
            ASSERT_TRUE( barrierCv.wait_for( lock, std::chrono::seconds( 2 ), [&]
                {
                    return barrierDone;
                } ) );
        }

        // THE VALIDATION:
        // We expect only 1 copy per connected receiver, plus 2 copies for emit().
        // Copies 1 & 2: boost::signals2 internals. When a boost signal is fired, it copies
        // the arguments into an internal "combiner" state before it starts looping over the slots.
        EXPECT_EQ( HeavyPayload::copyCount.load(), numReceivers + 2 );

        worker.quit();
        worker.join();
    }

    TEST( ObjectTest, DeepArgumentCopying_QueuedEventsMinimizeCopiesWithConstRefSignal )
    {
        Thread worker( "worker_thread" );
        worker.start();

        Signal<const HeavyPayload&> sig;
        HeavyPayload::copyCount.store( 0 );

        const int numReceivers = 100;
        std::vector<std::unique_ptr<CopyTrackerReceiver> > receivers;

        for( int i = 0; i < numReceivers; ++i )
        {
            receivers.push_back( std::make_unique<CopyTrackerReceiver>( &worker ) );
            Object::connect( sig, receivers.back().get(), &CopyTrackerReceiver::onTriggered,
                ConnectionType::Queued );
        }

        // Emit a heavy payload.
        HeavyPayload initialPayload;
        sig.emit( initialPayload );

        // Barrier wait to drain the worker queue
        std::mutex barrierMutex;
        std::condition_variable barrierCv;
        bool barrierDone = false;
        worker.post( [&]()
            {
                {
                    std::lock_guard<std::mutex> lock( barrierMutex );
                    barrierDone = true;
                }
                barrierCv.notify_one();
            } );

        {
            std::unique_lock<std::mutex> lock( barrierMutex );
            ASSERT_TRUE( barrierCv.wait_for( lock, std::chrono::seconds( 2 ), [&]
                {
                    return barrierDone;
                } ) );
        }

        // We expect only 1 copy per connected receiver.
        EXPECT_EQ( HeavyPayload::copyCount.load(), numReceivers );

        worker.quit();
        worker.join();
    }

    //------------------------------------------------------------------------------------------------------
    // The Black Hole Event Loop Test
    // This test verifies that if a thread is destroyed while it still has unprocessed events sitting in its
    // queue, it properly destroys those events and releases the captured arguments.
    //------------------------------------------------------------------------------------------------------

    // A payload that counts exactly how many instances of itself exist in memory.
    struct LeakDetectorPayload
    {
        static std::atomic<int> aliveCount;

        LeakDetectorPayload()
        {
            aliveCount.fetch_add( 1, std::memory_order_relaxed );
        }

        LeakDetectorPayload
            (
            const LeakDetectorPayload&
            )
        {
            aliveCount.fetch_add( 1, std::memory_order_relaxed );
        }

        LeakDetectorPayload
            (
            LeakDetectorPayload&&
            ) noexcept
        {
            aliveCount.fetch_add( 1, std::memory_order_relaxed );
        }

        ~LeakDetectorPayload()
        {
            aliveCount.fetch_sub( 1, std::memory_order_relaxed );
        }

    };

    std::atomic<int> LeakDetectorPayload::aliveCount { 0 };

    struct BlackHoleReceiver : public Object
    {
        BlackHoleReceiver
            (
            Thread* t
            )
            : Object( t )
        {
        }

        void onTriggered
            (
            const LeakDetectorPayload& payload
            )
        {
        }

    };

    TEST( ObjectTest, BlackHole_UnprocessedEventsDoNotLeakArguments )
    {
        // Reset counter just in case
        LeakDetectorPayload::aliveCount.store( 0 );

        {
            // We dynamically allocate the thread so we can explicitly destroy it
            auto worker = std::make_unique<Thread>( "worker_thread" );
            worker->start();

            Signal<const LeakDetectorPayload&> sig;
            auto receiver = std::make_unique<BlackHoleReceiver>( worker.get() );

            // Force a queued connection
            Object::connect( sig, receiver.get(), &BlackHoleReceiver::onTriggered,
                ConnectionType::Queued );

            // Emit the signal 10,000 times to flood the worker's queue.
            LeakDetectorPayload payload;
            for( int i = 0; i < 10000; ++i )
            {
                sig.emit( payload );
            }

            // IMMEDIATELY destroy the receiver so the weak_ptr life tokens become invalid.
            receiver.reset();

            // IMMEDIATELY quit and destroy the thread.
            // It likely hasn't had time to process even a fraction of the 10,000 events.
            worker->quit();
            worker->join();
            worker.reset();
        } // payload goes out of scope here

        // THE VALIDATION:
        // If the thread's internal event queue (e.g., std::queue<std::function>)
        // clears itself properly on destruction, all captured lambdas are destroyed.
        // This releases the std::shared_ptr<std::tuple>, destroying the LeakDetectorPayloads.
        // If this is > 0, the Thread class is leaking its unexecuted tasks!
        EXPECT_EQ( LeakDetectorPayload::aliveCount.load(), 0 );
    }

    //------------------------------------------------------------------------------------------------------
    // Rapid-Fire Setup/Tear Down Test
    // Because the connect function adds cleanup tokens to the receiver (aReceiver->mIncoming), we need to
    // ensure that when a receiver is destroyed, boost::signals2 cleanly prunes the connection from the
    // signal. If it doesn't, a long-lived signal will slowly leak memory as it accumulates dead connections.
    //
    // AddressSanitizer is required for this test.
    //------------------------------------------------------------------------------------------------------

    struct TransientReceiver : public Object
    {
        int executionCount = 0;
        void onTriggered()
        {
            executionCount++;
        }

    };

    TEST( ObjectTest, RapidFire_ReceiverDestructionCleansUpSignal )
    {
        // A long-lived signal
        Signal<> longLivedSignal;

        // Create and destroy 100,000 receivers and connections
        for( int i = 0; i < 100000; ++i )
        {
            TransientReceiver receiver;

            // Connect the short-lived receiver
            Object::connect( longLivedSignal, &receiver, &TransientReceiver::onTriggered,
                ConnectionType::Direct );

            // Ensure it works
            longLivedSignal.emit();
            EXPECT_EQ( receiver.executionCount, 1 );

            // receiver goes out of scope here, triggering Object::~Object.
            // This MUST disconnect the slot from longLivedSignal.
        }

        // THE VALIDATION:
        // We emit the signal one last time.
        // It must not crash (meaning no dangling pointers were left behind).
        longLivedSignal.emit();

        EXPECT_EQ( longLivedSignal.receivers(), 0 );
    }
}
