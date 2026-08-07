//! @file
//!
//! Implementation of QtMimic::Object (thread affinity + connection lifetime
//! management).
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#include "Object.hpp"

namespace QtMimic
{

    //================================================================
    // Object
    //================================================================

    //! @brief Constructor - creates an Object bound to a thread with thread affinity.
    //! @param aThread The Thread this object lives in. If null (the default), the object
    //!        lives in the current thread (Thread::current()). If no thread is current,
    //!        connections to this object behave as direct connections.
    Object::Object
        (
        Thread* aThread
        )
    // Store the thread's ThreadData, not the Thread itself: it outlives the Thread, so this
    // object's affinity can never become a dangling pointer. currentData() auto-adopts the
    // calling thread exactly as current() does when no thread is given. Held in an Affinity
    // box read at emit time, so a later moveToThread() redirects existing connections too.
        : mAffinity( std::make_shared<Affinity>( aThread ? aThread->data() :
            Thread::currentData() ) )
        , mLife( std::make_shared<int>( 0 ) )
    {
    }

    //! @brief The thread this object currently lives in, or nullptr if it has none -- or if the
    //! Thread it lived in has since been destroyed. This never returns a dangling pointer: the
    //! affinity is stored as a ThreadData (which outlives its Thread), exactly as Qt stores a
    //! refcounted QThreadData rather than a QThread*. Re-read it rather than caching it.
    Thread* Object::thread() const
    {
        const std::shared_ptr<ThreadData> data = threadDataRef();
        return data ? data->thread() : nullptr;
    }

    //! @brief Re-assign this object's thread affinity, following Qt6's QObject::moveToThread rules.
    //! The move is "push-only": the caller must be on the object's current affinity thread, so an
    //! object can be pushed to another thread but never pulled from an arbitrary one. The single
    //! exception, exactly as in Qt, is that an object with no affinity may be pulled to the calling
    //! thread. Affinity is resolved at emit time, so events posted after a successful move --
    //! including through connections made BEFORE it -- are delivered to @p aThread. Passing nullptr
    //! dissociates the object, after which thread() returns nullptr.
    //! @return true if the object now lives in @p aThread (including when it already did, which is a
    //!         successful no-op); false if the move was refused because the caller is not on the
    //!         object's affinity thread. On a false return the affinity is left unchanged.
    bool Object::moveToThread
        (
        Thread* aThread
        )
    {
        Thread* const currentAffinity = thread();
        if( currentAffinity == aThread )
        {
            return true; // already there -- a successful no-op, exactly as Qt returns true
        }

        // Push-only, with Qt's one exception: a thread-less object may be pulled to the caller.
        Thread* const callingThread = Thread::current();
        const bool pullNoAffinityToCaller = ( currentAffinity == nullptr )
            && ( aThread == callingThread );
        if( !pullNoAffinityToCaller && currentAffinity != callingThread )
        {
            return false;
        }

        mAffinity->setData( aThread ? aThread->data() : std::shared_ptr<ThreadData>() );
        return true;
    }

    //! @brief Number of live connections where this object is the receiver (diagnostics/tests).
    std::size_t Object::incomingConnectionCount() const
    {
        std::lock_guard<std::mutex> locker( mIncomingMutex );
        return mIncoming.size();
    }

    //! @brief Queue the object's destruction to its affinity thread.
    //! Qt-like QObject::deleteLater(). If the object has no thread, or its thread has stopped/gone
    //! (post() refuses), deletion happens immediately (a thread-affinity violation is preferable to
    //! a deferred delete that would never run and leak the object).
    void Object::deleteLater()
    {
        // Qt-like behavior: multiple deleteLater() calls coalesce into one.
        if( mDeleteLaterPosted.exchange( true ) )
        {
            return;
        }

        // Post through the affinity ThreadData (never a raw Thread*): it outlives its Thread and its
        // post() returns false once the loop has stopped. A null holder means the object was never
        // given a thread, so fall back to the calling thread (like a parent-less QObject).
        std::shared_ptr<ThreadData> targetData = threadDataRef();
        if( !targetData )
        {
            targetData = Thread::currentData();
        }

        std::weak_ptr<int> life = mLife;
        Object* self = this;

        const bool queued = targetData && targetData->post( [life, self]()
            {
                // Skip if the object was already destroyed another way.
                if( life.lock() )
                {
                    delete self;
                }
            } );

        if( !queued )
        {
            // The target thread's loop has already stopped (or gone away), so post() refused the
            // task rather than accepting one nothing will ever run. Doing nothing here would leak
            // self forever, which is strictly worse than the thread-affinity violation of deleting
            // it synchronously, right here, on whichever thread called deleteLater(). Not the normal
            // path: it only triggers for a thread that has already finished or gone away.
            delete self;
        }
    }

    //! @brief Destructor - disconnects all incoming connections and invalidates the life token.
    //! This prevents any queued (not-yet-run) slot invocations from running after the
    //! object is destroyed.
    Object::~Object()
    {
        // Invalidate first so queued slots that check the token will skip. This
        // also makes each connection's Cleanup destructor a no-op (it sees the life
        // token expired), so disconnect() below won't re-enter mIncomingMutex.
        mLife.reset();

        // Disconnect every connection where this object is the receiver so the
        // signal no longer references this (soon to be destroyed) object.
        std::lock_guard<std::mutex> locker( mIncomingMutex );
        for( auto& handle : mIncoming )
        {
            handle.disconnect();
        }
        mIncoming.clear();
    }

    //! @return a strong reference to this object's current affinity. Safe from any thread while
    //! moveToThread() may be reassigning it (Affinity locks internally).
    std::shared_ptr<ThreadData> Object::threadDataRef() const
    {
        return mAffinity->data();
    }

    //================================================================
    // Object::Cleanup
    //================================================================

    //! Constructor
    Object::Cleanup::Cleanup
        (
        Object* aOwner,
        std::weak_ptr<int> aLife
        )
        : mOwner( aOwner )
        , mLife( aLife )
    {
    }

    //! Destructor - prune this connection from the receiver's mIncoming.
    Object::Cleanup::~Cleanup()
    {
        if( mLife.expired() )
        {
            return; // receiver gone (or being destroyed) - it clears mIncoming.
        }
        std::lock_guard<std::mutex> locker( mOwner->mIncomingMutex );
        auto& v = mOwner->mIncoming;
        v.erase( std::remove( v.begin(), v.end(), mHandle ), v.end() );
    }

} // namespace QtMimic
