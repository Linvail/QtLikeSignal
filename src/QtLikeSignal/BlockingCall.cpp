// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! BlockingCall and BlockingCallGuard.

#include "QtLikeSignal/BlockingCall.hpp"

#include <utility>

namespace QtLikeSignal
{
    namespace Private
    {
        //! Blocks until settle() has run, and reports what it was told.
        //!
        //! The predicate is mSettled rather than mRan, so a call that was discarded still wakes the
        //! waiter -- it just wakes it with false. Waiting on mRan would sleep forever on exactly
        //! the cases this latch exists to survive.
        //!
        //! No timeout, deliberately. A deadline here would turn "the receiving thread is busy" into
        //! a failure the caller cannot tell apart from "the receiving thread is gone", and the
        //! second is already reported by the return value. A caller that needs a deadline wants
        //! Queued and a reply signal, not this.
        //!
        //! @return true if the slot ran; false if the call was discarded undelivered.
        bool BlockingCall::wait()
        {
            std::unique_lock<std::mutex> lock( mMutex );
            mCv.wait( lock, [this]()
                {
                    return mSettled;
                } );
            return mRan;
        }

        //! Records the outcome and wakes the waiter.
        //!
        //! Idempotent in the sense that matters: the first call decides the answer and later ones
        //! cannot downgrade it. Only one guard ever settles a given latch, so this is belt and
        //! braces rather than a load-bearing guarantee.
        //!
        //! Notified with the lock still held. The waiter is about to take the same mutex, so
        //! dropping it first only trades one wake for one more contention, and holding it removes
        //! any question about the latch being destroyed between the unlock and the notify.
        void BlockingCall::settle
            (
            bool aRan   //!< True if the slot was reached; false if the call was discarded.
            )
        {
            std::lock_guard<std::mutex> lock( mMutex );
            if( mSettled )
            {
                return;
            }
            mRan     = aRan;
            mSettled = true;
            mCv.notify_all();
        }

        //! Takes one end of @p aCall.
        BlockingCallGuard::BlockingCallGuard
            (
            std::shared_ptr<BlockingCall> aCall  //!< The latch to settle. Never null in use.
            )
            : mCall( std::move( aCall ) )
        {
        }

        //! Takes over @p aOther's end, leaving it with nothing to settle.
        //!
        //! The move is what carries the guard from the caller's temporary closure into the event's
        //! own storage, so the moved-from guard must not settle when that temporary dies -- the
        //! call has not happened yet at that point, and settling would release the waiter early.
        BlockingCallGuard::BlockingCallGuard
            (
            BlockingCallGuard&& aOther
            ) noexcept
            : mCall( std::move( aOther.mCall ) )
            , mRan( aOther.mRan )
        {
            aOther.mCall.reset();
            aOther.mRan = false;
        }

        //! Settles the latch, whatever it was that destroyed this guard.
        //!
        //! **The destructor is the whole design.** Settling from the call site would only cover the
        //! path where the slot runs; a receiver destroyed before the loop reached the event, or a
        //! dispatcher torn down with the event still queued, would leave the waiter asleep with
        //! nothing left alive to wake it. Both of those destroy the event, the event destroys its
        //! callable, and the callable holds this guard -- so tying the settle to destruction covers
        //! every disposal path there is, including ones added later.
        //!
        //! It also covers a slot that throws: the guard is destroyed while the exception unwinds,
        //! and the waiter is released rather than being made to pay for the receiver's mistake.
        BlockingCallGuard::~BlockingCallGuard()
        {
            if( mCall )
            {
                mCall->settle( mRan );
            }
        }

        //! Records that the slot was reached, so the waiter is told the call really happened.
        void BlockingCallGuard::markRan()
        {
            mRan = true;
        }
    }
}
