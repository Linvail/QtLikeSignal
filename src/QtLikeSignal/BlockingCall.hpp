// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The latch a blocking cross-thread call waits on, and the guard that settles it.

#ifndef QT_LIKE_SIGNAL_BLOCKING_CALL_HPP
#define QT_LIKE_SIGNAL_BLOCKING_CALL_HPP

#include <condition_variable>
#include <memory>
#include <mutex>

namespace QtLikeSignal
{
    namespace Private
    {
        //! The rendezvous between a thread that blocks and the thread that runs its call.
        //!
        //! Internal. Held by shared_ptr on both sides, because the two ends are released in an
        //! order nobody controls: the waiter may give up its reference the instant it wakes, and
        //! the event carrying the other end may outlive that by however long the receiving thread
        //! takes to destroy it.
        //!
        //! **Settled exactly once, and always.** A queued call can end three ways -- it runs, its
        //! receiver dies before the loop reaches it, or the loop is torn down with the event still
        //! in it -- and a waiter that is only woken by the first would hang forever on the other
        //! two. BlockingCallGuard is what makes that guarantee: it settles from its *destructor*,
        //! so every path that disposes of the event settles the latch, including the ones that
        //! never call anything.
        class BlockingCall
        {
        public:
            BlockingCall() = default;

            BlockingCall
                (
                const BlockingCall&
                ) = delete;

            BlockingCall& operator=
                (
                const BlockingCall&
                ) = delete;

            //! Blocks until the call is settled. Returns whether the slot actually ran.
            bool wait();

            //! Records the outcome and wakes the waiter. See the definition.
            void settle
                (
                bool aRan   //!< True if the slot was reached; false if the call was discarded.
                );

        private:
            std::mutex mMutex;              //!< Guards mSettled and mRan.
            std::condition_variable mCv;    //!< Wakes wait() when settle() has run.
            bool mSettled { false };        //!< True once settle() has run; the wait predicate.
            bool mRan { false };            //!< What wait() reports. Meaningless until mSettled.
        };

        //! Settles a BlockingCall when it is destroyed, whatever it was that destroyed it.
        //!
        //! Internal. Lives inside the queued call's own callable, so that disposing of the call in
        //! any way at all settles the latch. Moving one is how it reaches the event's storage;
        //! copying is not allowed, because two guards over one latch would settle it as soon as the
        //! first copy died rather than when the call was really over.
        class BlockingCallGuard
        {
        public:
            //! Takes one end of @p aCall.
            explicit BlockingCallGuard
                (
                std::shared_ptr<BlockingCall> aCall  //!< The latch to settle. Never null in use.
                );

            //! Takes over @p aOther's end, leaving it with nothing to settle.
            BlockingCallGuard
                (
                BlockingCallGuard&& aOther
                ) noexcept;

            BlockingCallGuard
                (
                const BlockingCallGuard&
                ) = delete;

            BlockingCallGuard& operator=
                (
                const BlockingCallGuard&
                ) = delete;

            BlockingCallGuard& operator=
                (
                BlockingCallGuard&&
                ) = delete;

            //! Settles the latch. See the definition for why this is the destructor's job.
            ~BlockingCallGuard();

            //! Records that the slot was reached, so the waiter is told the call really happened.
            void markRan();

        private:
            std::shared_ptr<BlockingCall> mCall;  //!< Null in a moved-from guard, which settles nothing.
            bool mRan { false };                  //!< Passed to settle() on destruction.
        };
    }
}

#endif // QT_LIKE_SIGNAL_BLOCKING_CALL_HPP
