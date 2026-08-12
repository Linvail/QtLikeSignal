//! @file
//!
//! Out-of-line members of QtMimic::ThreadData.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "ThreadData.hpp"

namespace QtMimic
{
    //! @return the Thread this data describes, or nullptr once that Thread has been destroyed.
    //! Safe to call at any time: the ThreadData itself is kept alive by whoever holds it.
    Thread* ThreadData::thread() const
    {
        return mThread.load( std::memory_order_acquire );
    }

    //! @brief Set by Thread's constructor and cleared by ~Thread(). Only Thread may touch it.
    void ThreadData::setThread
        (
        Thread* aThread    //!< The owning thread, or nullptr once it is gone.
        )
    {
        mThread.store( aThread, std::memory_order_release );
    }

    //! @return true while the owning thread's body is executing.
    //!
    //! Safe to call from any thread at any time, including once that Thread has been destroyed --
    //! it reports false, because the run body clears this before the Thread can be torn down.
    //! Callers holding only this ThreadData can therefore ask "is that thread still running?"
    //! without dereferencing a Thread* that may already have been freed.
    bool ThreadData::isThreadRunning() const
    {
        return mThreadRunning.load( std::memory_order_acquire );
    }

    //! @brief Record whether the owning thread's body is executing. Only Thread may touch it.
    void ThreadData::setThreadRunning
        (
        bool aRunning    //!< True on start, false once the run body has finished.
        )
    {
        mThreadRunning.store( aRunning, std::memory_order_release );
    }

    //! @return a strong reference to this thread's dispatcher, or nullptr if none is installed.
    //! Thread-safe.
    std::shared_ptr<AbstractEventDispatcher> ThreadData::dispatcher() const
    {
        std::lock_guard<std::mutex> locker( mDispatcherMutex );
        return mDispatcher;
    }

    //! @brief Install or clear this thread's dispatcher. Thread-safe.
    void ThreadData::setDispatcher
        (
        std::shared_ptr<AbstractEventDispatcher> aDispatcher  //!< Dispatcher to install; nullptr clears.
        )
    {
        std::lock_guard<std::mutex> locker( mDispatcherMutex );
        mDispatcher = std::move( aDispatcher );
    }

} // namespace QtMimic
