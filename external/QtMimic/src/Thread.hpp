//! @file
//!
//! QtMimic::Thread - an event-loop thread. Every Object "lives" in one
//! Thread; queued slot invocations are executed in that thread's event loop.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_THREAD_HPP
#define QT_MIMIC_THREAD_HPP

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Signal.hpp"
#include "ThreadData.hpp"

#include <memory>

namespace QtMimic
{

    //----------------------------------------------------------------
    //! @class Thread
    //!
    //! An event-loop thread. Objects bound to this thread have their queued slots
    //! and posted tasks executed here, one at a time, in FIFO order.
    //!
    //! Usage:
    //! @code
    //!   QtMimic::Thread worker("worker");
    //!   worker.start();        // begins the event loop
    //!   ...
    //!   worker.quit();         // ask the loop to drain and exit (also done by dtor)
    //! @endcode
    //----------------------------------------------------------------
    class Thread
    {
    public:
        using LifecycleSignalType = Signal<>;
        explicit Thread
            (
            const std::string& aName = std::string()
            );

        ~Thread();

        Thread
            (
            const Thread&
            ) = delete;

        Thread& operator=
            (
            const Thread&
            ) = delete;

        Thread
            (
            Thread&&
            ) = delete;

        Thread& operator=
            (
            Thread&&
            ) = delete;

        void start();

        int exec();

        void setDispatcher
            (
            std::function<void( int aTimeoutMs )> aDispatcher
            );

        bool post
            (
            std::function<void()> aTask
            );

        void processEvents();

        void setWakeCallback
            (
            std::function<void()> aWake
            );

        void setWaiter
            (
            std::function<void( int aTimeoutMs )> aWaiter,
            int aTimeoutMs = -1
            );

        void quit();

        void exit
            (
            int aCode
            );

        void join();

        bool isCurrent() const;

        std::thread::id id() const;

        const std::string& name() const;

        Connection connectStarted
            (
            const LifecycleSignalType::Slot& aSlot
            );

        Connection connectFinished
            (
            const LifecycleSignalType::Slot& aSlot
            );

        static Thread* current();

        static std::shared_ptr<ThreadData> currentData();

        std::shared_ptr<ThreadData> data() const;

    private:
        void run();

        void loop();

        void adopt();

        //! Per-thread state that outlives this Thread. Created in the constructor with a
        //! back-pointer to this, cleared in ~Thread(). Anything that needs to remember "which
        //! thread" holds this rather than a raw Thread*, so it can never dangle. It also owns the
        //! event mailbox (task queue + its mutex/condvar/accepting flag + wake callback), so posting
        //! goes through the ThreadData and never dereferences this Thread.
        std::shared_ptr<ThreadData> mData;
        std::string mName;                        //!< Thread name
        std::thread mThread;                      //!< Underlying OS thread
        std::thread::id mId;                      //!< Cached id of mThread
        bool mAdopted = false; //!< True if this represents an already-running native thread
        int mExitCode = 0; //!< Value returned by exec()
        std::function<void( int )> mDispatcher; //!< External event pump (optional)
        std::function<void( int )> mWaiter;   //!< External blocking wait (optional)
        int mWaiterTimeoutMs = -1;            //!< Timeout for external wait (optional)
        LifecycleSignalType mStarted;         //!< Emitted when loop starts
        LifecycleSignalType mFinished;        //!< Emitted when loop exits
    };

} // namespace QtMimic

#endif // QT_MIMIC_THREAD_HPP
