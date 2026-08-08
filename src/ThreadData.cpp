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
