// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::ThreadPool -- a few worker threads and one queue of work between them.

#ifndef QT_LIKE_SIGNAL_THREAD_POOL_HPP
#define QT_LIKE_SIGNAL_THREAD_POOL_HPP

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Runnable.hpp"
#include "QtLikeSignal/Signal.hpp"
#include "QtLikeSignal/TaskHandle.hpp"

#include <climits>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace QtLikeSignal
{
    //! Runs work on a small set of worker threads. Qt's QThreadPool, with the parts a frame loop
    //! does not want left out.
    //!
    //! @code
    //!   QtLikeSignal::ThreadPool pool( 2 );
    //!
    //!   pool.submit( [path]()
    //!       {
    //!           decode( path );
    //!       } );
    //!
    //!   pool.waitForDone();
    //! @endcode
    //!
    //! **The program owns the pool.** There is no global instance to reach for, because a library
    //! that owns hidden threads decides for the program how many threads exist and when they
    //! start. A program that must account for every thread it has can.
    //!
    //! **The workers start with the pool and end with it.** There is no expiry: Qt ends a thread
    //! that has waited 30 seconds, which suits an application with bursts of work and not a program
    //! that draws every frame. Here the threads, their stacks and their names exist from the
    //! constructor onwards, so nothing about them depends on timing.
    //!
    //! **A worker is a QtLikeSignal::Thread**, so work sees the library it is part of:
    //! Thread::currentThread() inside a task is that worker, an Object made there belongs to it,
    //! and Object::callLater() reaches another thread's loop. A worker runs the queue rather than
    //! an event loop, so Thread::post() to a worker is never delivered and a Timer started in a
    //! task never fires.
    //!
    //! **Work is ranked, not scheduled.** A task with a higher priority runs before one with a
    //! lower priority that is still waiting, and tasks of one priority run in the order they were
    //! submitted. Priority never interrupts a task that has started: a worker inside run() stays
    //! there.
    //!
    //! **Use it from any thread.** submit(), clear(), waitForDone() and the counters are safe to
    //! call from any thread at any time, which is the point: the pool is the hand-off between
    //! threads. It is not an Object and has no thread affinity of its own.
    //!
    //! **Shutdown waits.** The destructor drops what is queued and waits for what is already
    //! running, so a worker never touches the program's memory after the pool is gone.
    class ThreadPool
    {
    public:
        //! The worker count ThreadPool() picks when it is given none: one less than the machine
        //! reports, so the thread that asked for the work keeps a core, clamped to 1 to 8.
        static int defaultWorkerCount();

        explicit ThreadPool
            (
            int aWorkerCount = 0
            );

        ~ThreadPool();

        ThreadPool
            (
            const ThreadPool&
            ) = delete;

        ThreadPool& operator=
            (
            const ThreadPool&
            ) = delete;

        TaskHandle submit
            (
            Runnable* aRunnable,
            int aPriority = 0
            );

        //! Submits any callable that takes no argument: a lambda, a function, a bound member.
        //!
        //! The callable is copied into a Runnable the pool makes and deletes. A callable too large
        //! for the memory left, or that cannot be allocated, is refused with a log record and
        //! never runs. @p aPriority ranks it against the work already waiting; see the other
        //! submit().
        //!
        //! @tparam Callable anything that `aWork()` compiles for. A callable that returns a value
        //!         is accepted and the value is dropped. The enable_if is what keeps a pointer to
        //!         a class derived from Runnable out of this overload: that pointer is an exact
        //!         match for a template and only a conversion for submit( Runnable* ), so without
        //!         it the wrong overload would win and the error would be inside this class.
        //! @return a handle to the task, which may be ignored.
        template <typename Callable,
            typename = typename std::enable_if<std::is_invocable<Callable&>::value>::type>
        TaskHandle submit
            (
            Callable&& aWork,   //!< The work. Copied, so it must outlive nothing.
            int aPriority = 0   //!< Higher runs first. Equal priorities keep their order.
            )
        {
            using Work = typename std::decay<Callable>::type;

            //! Holds one callable and runs it. One class per callable type, made by this template.
            class CallableRunnable : public Runnable
            {
            public:
                //! Keeps @p aWork until the pool runs it.
                explicit CallableRunnable
                    (
                    Callable&& aCallableWork   //!< The work.
                    )
                    : mWork( std::forward<Callable>( aCallableWork ) )
                {
                    setAutoDelete( true );
                }

                //! Runs the callable.
                virtual void run() override
                {
                    mWork();
                }

            private:
                Work mWork;   //!< The callable.
            };

            // nothrow, because the library is built to compile without exceptions: a failed
            // allocation is a null pointer that submit() refuses and reports, not a throw.
            CallableRunnable* const wrapped =
                new( std::nothrow ) CallableRunnable( std::forward<Callable>( aWork ) );
            return submit( wrapped, aPriority );
        }

        //! Runs @p aWork on a worker and @p aThen on @p aReceiver's own thread, with the result.
        //!
        //! This is the hand-off a frame needs: read and decode away from the loop, use the answer
        //! where the answer belongs. @p aWork returns a value, or nothing; @p aThen takes that
        //! value, or nothing.
        //!
        //! @code
        //!   pool.submit( &view,
        //!       []()                          // on a worker
        //!       {
        //!           return decodeFile( "logo.png" );
        //!       },
        //!       [&view]( ImageData aImage )   // on the view's thread
        //!       {
        //!           view.setImage( std::move( aImage ) );
        //!       } );
        //! @endcode
        //!
        //! **The continuation is dropped if @p aReceiver dies first.** It is carried by an
        //! ordinary queued connection with the receiver as its context, so the receiver's
        //! destruction disconnects it under the same lock that delivery takes. A receiver
        //! destroyed while the work is still running costs the result, which is what the caller
        //! wanted: nobody is left to give it to.
        //!
        //! **The value moves, and a move-only value is welcome.** The result never travels
        //! through the signal: it waits beside it and is moved into @p aThen when the continuation
        //! runs. A decoded image, which cannot be copied at all, comes home with no copy.
        //!
        //! **@p aReceiver must be alive when this is called**, which is the ordinary rule for
        //! reaching an object at all, and it must not be null: a hand-off with nobody to hand to
        //! is refused with a log record rather than run for nothing. Submitting for a receiver on
        //! another thread that is being destroyed at this moment is the caller's race, not one
        //! this class can settle.
        //!
        //! @return a handle to the task, which says nothing about the continuation: a task is
        //!         finished when the work is, whether or not the receiver was still there.
        template <typename Work, typename Continuation>
        TaskHandle submit
            (
            Object* aReceiver,        //!< Whose thread the continuation runs on.
            Work&& aWork,             //!< The work, on a worker.
            Continuation&& aThen,     //!< What to do with the result, on the receiver's thread.
            int aPriority = 0         //!< Higher runs first. Equal priorities keep their order.
            )
        {
            using Result = decltype( aWork() );

            // A reference would name something the worker produced, and the continuation runs
            // later, on another thread: by then the frame that owned it is gone. Refused here,
            // with a sentence, rather than in the middle of std::optional, which cannot hold a
            // reference either. Work that means to share something long-lived returns a pointer.
            static_assert( !std::is_reference<Result>::value,
                "ThreadPool::submit(): work handed to a pool must return a value, not a "
                "reference. The result is used on another thread, after the work that made it "
                "has returned." );

            return submitWithResult<Result>( aReceiver, std::forward<Work>( aWork ),
                std::forward<Continuation>( aThen ), aPriority );
        }

        bool waitForDone
            (
            unsigned long aTime = ULONG_MAX
            );

        std::size_t clear();

        //! Returns how many worker threads the pool has. Fixed for its life.
        int workerCount() const
        {
            return static_cast<int>( mWorkers.size() );
        }

        std::size_t queuedCount() const;

        std::size_t activeCount() const;

    private:
        class Worker;

        //! One queued piece of work: what to run, how it ranks, who deletes it, and where it
        //! stands.
        struct Task
        {
            Runnable* mRunnable { nullptr };        //!< The work. Never null in the queue.
            std::shared_ptr<TaskState> mState;      //!< Shared with every handle to this task.
            int mPriority { 0 };                    //!< Higher runs first. See submit().
            bool mAutoDelete { false };             //!< True if the pool deletes it after run().
        };

        //! Carries one result home: the value, and a signal that says it is there.
        //!
        //! **The signal carries nothing.** A queued connection copies what it emits into the event
        //! it posts, and copies it again into the slot -- so a result that travelled through the
        //! signal would be copied twice, and a move-only result, which is what a decoded image is,
        //! would not compile at all. The value waits here instead, and the continuation moves it
        //! out on the receiver's thread.
        //!
        //! Both sides are safe without a lock of its own: the worker writes the value before it
        //! emits, and the receiver reads it after the delivery, with the signal's lock and the
        //! event queue between the two.
        //!
        //! @tparam Result what the work returns, or void.
        template <typename Result>
        struct HandOff
        {
            //! What the work returned. A char for work that returns nothing, never filled in.
            using Stored = typename std::conditional<std::is_void<Result>::value, char,
                    Result>::type;

            //! The result, once the work has produced it.
            std::optional<Stored> mResult;

            //! Emitted when the work is finished, and connected to the receiver.
            Signal<> mDone;
        };

        //! Holds one hand-off for the task, and disconnects its signal when the task is done.
        //!
        //! **This is what stops a hand-off outliving its task.** The slot connected to `mDone`
        //! captures the hand-off, and `mDone` lives inside that hand-off, so the two keep each
        //! other alive: a cycle that nothing breaks, with the connection sitting on the receiver's
        //! incoming list for as long as the receiver lives. A view that decodes a thousand images
        //! would collect a thousand of each.
        //!
        //! Disconnecting breaks it, and doing that here means it happens on every path: after the
        //! work emitted, and equally when the task was cancelled or dropped and never emitted at
        //! all. What is already in flight is not affected -- a queued delivery is an event holding
        //! its own copy of the slot, which keeps the hand-off alive until the continuation has run.
        //!
        //! @tparam Result what the work returns, or void.
        template <typename Result>
        class HandOffOwner
        {
        public:
            //! Takes the hand-off @p aHandOff for the life of the task.
            explicit HandOffOwner
                (
                std::shared_ptr<HandOff<Result> > aHandOff   //!< The hand-off. Never null.
                )
                : mHandOff( std::move( aHandOff ) )
            {
            }

            HandOffOwner
                (
                HandOffOwner&&
                ) = default;

            HandOffOwner& operator=
                (
                HandOffOwner&&
                ) = default;

            //! Disconnects the signal, which is what breaks the cycle described above.
            ~HandOffOwner()
            {
                if( mHandOff )
                {
                    mHandOff->mDone.disconnectAll();
                }
            }

            //! Returns the hand-off.
            HandOff<Result>& handOff() const
            {
                return *mHandOff;
            }

        private:
            //! The hand-off. Shared with the slot, and with any delivery in flight.
            std::shared_ptr<HandOff<Result> > mHandOff;
        };

        template <typename Result, typename Work, typename Continuation>
        TaskHandle submitWithResult
            (
            Object* aReceiver,
            Work&& aWork,
            Continuation&& aThen,
            int aPriority
            )
        {
            if( aReceiver == nullptr )
            {
                qCWarning( gLogThread ) <<
                    "ThreadPool::submit: refused; the work has a result and no receiver to give"
                                        << "it to";
                return TaskHandle();
            }

            std::shared_ptr<HandOff<Result> > handOff( new( std::nothrow ) HandOff<Result>() );
            if( !handOff )
            {
                return TaskHandle();
            }

            // Connected here, on the submitting thread, rather than on the worker: a connection
            // made with the receiver as its context is dropped when the receiver dies, and making
            // it before the work starts is what closes the window where a result arrives with
            // nowhere to go.
            // Not a mutable lambda: a connected slot is invoked as const, so the continuation
            // must be callable that way too, which every ordinary lambda is.
            Object::connect( handOff->mDone, aReceiver,
                [handOff, then = std::forward<Continuation>( aThen )]()
                {
                    deliverResult<Result>( *handOff, then );
                } );

            // The owner rather than the pointer: when this task is destroyed -- after it ran, or
            // when it was cancelled and dropped -- the connection goes with it.
            return submit( [owner = HandOffOwner<Result>( handOff ),
                work = std::forward<Work>( aWork )]() mutable
                {
                    storeResult<Result>( owner.handOff(), work );
                    owner.handOff().mDone.emit();
                }, aPriority );
        }

        //! Runs @p aWork on the worker and puts what it returned beside the signal.
        template <typename Result, typename Work>
        static void storeResult
            (
            HandOff<Result>& aHandOff,   //!< Where the result waits.
            Work& aWork                  //!< The work to run.
            )
        {
            if constexpr( std::is_void<Result>::value )
            {
                aWork();
            }
            else
            {
                aHandOff.mResult.emplace( aWork() );
            }
        }

        //! Calls @p aThen on the receiver's thread, with the result moved out of @p aHandOff.
        template <typename Result, typename Continuation>
        static void deliverResult
            (
            HandOff<Result>& aHandOff,   //!< Where the result waited.
            const Continuation& aThen    //!< What the caller wants done with it.
            )
        {
            if constexpr( std::is_void<Result>::value )
            {
                aThen();
            }
            else
            {
                aThen( std::move( *aHandOff.mResult ) );
            }
        }

        void enqueue
            (
            const Task& aTask
            );

        void workerLoop();

        void runTask
            (
            const Task& aTask
            );

        static void dropTask
            (
            const Task& aTask
            );

        //! Guards the queue, the counters and the stopping flag. Held for the bookkeeping only,
        //! never while a task runs.
        mutable std::mutex mMutex;

        //! Wakes a worker when work arrives, or when the pool is stopping.
        std::condition_variable mWorkArrived;

        //! Wakes waitForDone() when the queue is empty and no task is running.
        std::condition_variable mWentQuiet;

        //! The work waiting for a worker, oldest first.
        std::deque<Task> mQueue;

        //! The workers. Started by the constructor, joined by the destructor.
        std::vector<std::unique_ptr<Worker> > mWorkers;

        //! How many tasks are running right now.
        std::size_t mActiveCount { 0 };

        //! True once the destructor has asked the workers to stop.
        bool mStopping { false };
    };
}

#endif // QT_LIKE_SIGNAL_THREAD_POOL_HPP
