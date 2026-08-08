#include "ThreadData.hpp"

namespace QtLikeSignal
{
    //! @return the Thread this data describes, or nullptr once that Thread has been destroyed.
    //! Safe to call at any time: the ThreadData itself is kept alive by whoever holds it.
    Thread* ThreadData::thread() const
    {
        return mThread.load( std::memory_order_acquire );
    }

    //! Set by Thread's constructor and cleared by ~Thread(). Only Thread may touch it.
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

    //! Records whether the owning thread's body is executing. Only Thread may touch it.
    void ThreadData::setThreadRunning
        (
        bool aRunning    //!< True on start, false once the run body has finished.
        )
    {
        mThreadRunning.store( aRunning, std::memory_order_release );
    }

    //! Gets a strong reference to this thread's dispatcher, or nullptr if none is installed.
    //! Thread-safe.
    std::shared_ptr<AbstractEventDispatcher> ThreadData::dispatcher() const
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        return mDispatcher;
    }

    //! Installs or clears this thread's dispatcher. Thread-safe.
    void ThreadData::setDispatcher
        (
        std::shared_ptr<AbstractEventDispatcher> aDispatcher    //!< Dispatcher to install; nullptr clears it.
        )
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        mDispatcher = std::move( aDispatcher );
    }
}
