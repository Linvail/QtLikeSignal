// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::LogCategory - a named, independently switchable logging channel, and the two
//! macros that declare and define one.

#ifndef QT_LIKE_SIGNAL_LOG_CATEGORY_HPP
#define QT_LIKE_SIGNAL_LOG_CATEGORY_HPP

#include "QtLikeSignal/LogLevel.hpp"

#include <atomic>

namespace QtLikeSignal
{
    //! A named logging channel that can be switched on and off independently of every other one.
    //!
    //! A category exists so that the Wayland backend's chatter can be turned up without turning
    //! everything up. It holds a threshold rather than four flags: isEnabled() is a relaxed load
    //! and an integer compare, which is what makes it cheap enough to sit in front of a log call
    //! in hot code.
    //!
    //! **Thread-safe** in the sense QtLikeSignal/Global.hpp defines: isEnabled() and setThreshold()
    //! may be called concurrently from any thread. As with every such query the answer may be stale
    //! on return -- another thread may raise the threshold between the check and the record being
    //! built -- which for logging is harmless and is why the load is relaxed rather than acquiring.
    //!
    //! Do not construct one directly. Use the macros at the bottom of this file, which put the
    //! object in a function-local static and so cannot be caught by static initialisation order.
    class LogCategory
    {
    public:
        //! Longest DLT context id there is, and therefore the longest short id a category may have.
        static constexpr int kShortIdLength = 4;

        LogCategory
            (
            const char* aName,
            const char* aShortId
            );

        ~LogCategory();

        //! Copying would put two objects in the registry describing one channel, and assignment
        //! would move a live object's identity out from under a filter rule that had matched it.
        LogCategory
            (
            const LogCategory&
            ) = delete;

        LogCategory& operator=
            (
            const LogCategory&
            ) = delete;

        //! @return the dotted name, as filter rules match it. Never null.
        const char* name() const
        {
            return mName;
        }

        //! @return the four-character short id, NUL-terminated. Never null.
        const char* shortId() const
        {
            return mShortId;
        }

        //! @return true when a record at @p aLevel would be emitted on this category.
        //!
        //! Relaxed rather than acquiring: nothing is published through this flag, and the cost of
        //! an acquire on every suppressed log call in a render loop buys nothing a logging
        //! facility can use.
        bool isEnabled
            (
            LogLevel aLevel  //!< The level being considered.
            ) const
        {
            return static_cast<int>( aLevel ) >= mThreshold.load( std::memory_order_relaxed );
        }

        //! @return the lowest level this category currently emits.
        LogLevel threshold() const
        {
            return static_cast<LogLevel>( mThreshold.load( std::memory_order_relaxed ) );
        }

        //! Sets the lowest level this category emits. Levels below @p aLevel are suppressed.
        void setThreshold
            (
            LogLevel aLevel  //!< New threshold.
            )
        {
            mThreshold.store( static_cast<int>( aLevel ), std::memory_order_relaxed );
        }

        //! Signature of the callback visitAll() hands each category to.
        using Visitor = void ( * )( LogCategory& aCategory, void* aContext );

        //! Calls @p aVisitor once for every registered category, with the registry held.
        //!
        //! A raw function pointer and a void* rather than a std::function, for two reasons: this
        //! is called with a mutex held, where a std::function's small-object optimisation failing
        //! would mean allocating under a lock; and it lets the whole logging core stay clear of
        //! <functional>, which the rest of QtLikeSignal pays for only where templates need it.
        //!
        //! @p aVisitor must not construct or destroy a LogCategory, and must not call visitAll()
        //! again -- the registry is not reentrant.
        static void visitAll
            (
            Visitor aVisitor,
            void* aContext
            );

        //! Signature of the callback that decides a newly registered category's threshold.
        using ThresholdResolver = LogLevel ( * )( const LogCategory& aCategory );

        //! Installs the callback consulted by every category constructed from now on.
        //!
        //! This exists because categories are function-local statics: one can come into existence
        //! at any point in the run, including long after filter rules were installed. Without the
        //! hook, a category first used after Log::setFilterRules() would quietly ignore the rules
        //! that were meant to cover it, which is the kind of defect that only shows up as "the
        //! logging did not work" in a vehicle.
        //!
        //! @return the resolver that was installed before this call, or null if there was none.
        static ThresholdResolver setThresholdResolver
            (
            ThresholdResolver aResolver  //!< New resolver, or null to fall back to the default.
            );

        //! The threshold a category gets with no resolver installed: Debug off, Info and above on.
        static LogLevel defaultThreshold()
        {
            return LogLevel::Info;
        }

    private:
        const char* mName;  //!< Dotted name. Points at a literal owned by the defining macro.

        //! Four-character DLT context id plus its terminator. Copied rather than pointed at,
        //! because the DLT sink registers a context from it and wants a stable NUL-terminated
        //! buffer of exactly this shape.
        char mShortId[ kShortIdLength + 1 ];

        //! Lowest level this category emits, held as the underlying integer so that isEnabled()
        //! is a plain compare with no conversion.
        std::atomic<int> mThreshold;

        //! Next category in the intrusive registry list, or null at the end. Guarded by the
        //! registry mutex in LogCategory.cpp, not by an atomic: it is written twice in a
        //! category's life and read only by visitAll().
        LogCategory* mNext;
    };
}

//! Declares a logging category, for a header.
//!
//! Expands to the declaration of a function, not of an object, so that the category is a
//! function-local static in the translation unit that defines it. That is what makes a category
//! usable from another object's constructor: there is no order in which the two statics could be
//! initialised wrongly, because the category is not constructed until it is first asked for.
//!
//! Qt reaches the same shape through Q_DECLARE_LOGGING_CATEGORY for the same reason.
#define QTLIKESIGNAL_DECLARE_LOG_CATEGORY( aCategory ) \
        const QtLikeSignal::LogCategory& aCategory();

//! Defines a logging category. Put this in exactly one .cpp file.
//!
//! @p aName is the dotted name filter rules match, such as "qtlikesignal.timer". @p aShortId is the
//! four-character id the DLT sink registers a context under, such as "TIMR" -- DLT context ids are
//! exactly four characters, which is why a category carries two names rather than one.
#define QTLIKESIGNAL_DEFINE_LOG_CATEGORY( aCategory, aName, aShortId )        \
        const QtLikeSignal::LogCategory& aCategory()                              \
        {                                                                    \
            static QtLikeSignal::LogCategory sCategory( aName, aShortId );        \
            return sCategory;                                                \
        }

#endif // QT_LIKE_SIGNAL_LOG_CATEGORY_HPP
