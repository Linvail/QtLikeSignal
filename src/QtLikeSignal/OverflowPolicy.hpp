// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! What a post should do when the receiving thread's event queue is full.

#ifndef QT_LIKE_SIGNAL_OVERFLOWPOLICY_HPP
#define QT_LIKE_SIGNAL_OVERFLOWPOLICY_HPP

namespace QtLikeSignal
{
    //! What a post should do when the receiving thread's event queue is full.
    //!
    //! Carried by the post rather than set on the queue, because two producers feeding one thread
    //! want different answers: a map tile pipeline is happy to lose an old tile, and a button press
    //! is not.
    //!
    //! A queue is only full if the application gave it a capacity --
    //! AbstractEventDispatcher::setEventQueueCapacity(). The default is unbounded, and then
    //! DropNewest and DropOldest never do anything. Coalesce is the exception: it acts at any
    //! depth, because an update-request that is already pending does not need a second one whether
    //! the queue holds three events or a thousand.
    //!
    //! There is deliberately no Block. It would park the producer inside the dispatcher until the
    //! consumer drains, and the consumer is user code free to post back -- two threads each waiting
    //! for a drain only the other can perform. Nothing here prevents that cycle, and the rest of
    //! this dispatcher is careful never to run user code under its own lock for the same reason. A
    //! defensible version is a bounded wait that degrades to DropNewest rather than hanging; that
    //! is not this.
    enum class OverflowPolicy
    {
        //! Refuse this event. The queue keeps everything it already had.
        //!
        //! The default everywhere, because it is the only policy that destroys nothing the caller
        //! did not hand over: the post returns false, the new event is deleted, and what was queued
        //! is untouched.
        DropNewest,

        //! Evict the oldest droppable event to make room, then admit this one.
        //!
        //! For a stream where the freshest value is the only one worth having -- a speed reading, a
        //! tile that has been superseded. A deferred delete is never the victim: dropping one leaks
        //! the object it names, and under a flood that would leak exactly when memory is scarce.
        DropOldest,

        //! Drop this event if an equivalent one is already queued for the same receiver.
        //!
        //! Equivalent means same receiver and same event type, which is why this is refused for a
        //! queued signal: every one of those carries Event::MetaCall, so coalescing them would
        //! collapse unrelated slots into one call. The metacall equivalent is callLater(), which
        //! deduplicates on the callable's identity instead.
        //!
        //! The only policy that acts below the ceiling as well as at it. Coalescing that waited for
        //! the queue to fill would let a hundred identical repaints accumulate and call it healthy.
        Coalesce
    };
}

#endif // QT_LIKE_SIGNAL_OVERFLOWPOLICY_HPP
