// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of QtLikeSignal::LogCategory, and the process-wide registry of categories that
//! Log::setFilterRules() walks.

#include "QtLikeSignal/LogCategory.hpp"

#include "QtLikeSignal/Log.hpp"

#include <cstring>
#include <mutex>

namespace QtLikeSignal
{
    namespace
    {
        //! Guards the registry list head and every mNext in it.
        //!
        //! A function-local static rather than a namespace-scope object: a category can be
        //! constructed from another translation unit's static initialiser, which would otherwise
        //! be a race with this mutex's own construction.
        std::mutex& registryMutex()
        {
            static std::mutex sMutex;
            return sMutex;
        }

        //! Head of the intrusive list of every live category. Guarded by registryMutex().
        //!
        //! Zero-initialised before any dynamic initialisation runs, so a category constructed
        //! from a static initialiser links into a list that is already valid rather than one that
        //! has not been constructed yet.
        LogCategory* gFirst = nullptr;

        //! The installed threshold resolver, or null. Atomic rather than mutex-guarded because it
        //! is read by every category construction and written approximately once.
        std::atomic<LogCategory::ThresholdResolver> gResolver { nullptr };
    }

    //! Constructs a category and registers it.
    //!
    //! The threshold is settled here rather than left at a default, by asking whatever resolver
    //! Log has installed. See LogCategory::setThresholdResolver() for why that matters.
    //!
    //! The resolver is called *after* the registry lock is released. Holding it across a call into
    //! Log would pair the two locks in the opposite order from setFilterRules(), which takes its
    //! own lock and then walks the registry -- and two orders is a deadlock waiting for the first
    //! program that creates a category from one thread while another installs rules.
    LogCategory::LogCategory
        (
        const char* aName,    //!< Dotted name, such as "qtlikesignal.timer". Must outlive the category.
        const char* aShortId  //!< Four-character DLT context id, such as "TIMR".
        )
        : mName( aName != nullptr ? aName : "" )
        , mThreshold( static_cast<int>( defaultThreshold() ) )
        , mNext( nullptr )
    {
        // Before anything else, and in particular before the registry lock below: this is what
        // makes QTLIKESIGNAL_LOG_RULES work. The level check lives in the logging macro and runs
        // before any record exists, so a facility that waited for the first record to read the
        // environment would already have suppressed it. A category is always constructed before it
        // can be checked, which makes here the earliest hook there is.
        Log::ensureInitialised();

        // Truncate rather than reject: a category with an over-long id is a mistake worth
        // surviving, and there is nowhere to report it from -- this runs before main() in the
        // usual case, and reporting it would mean logging from inside the logging facility.
        std::memset( mShortId, 0, sizeof( mShortId ) );
        if( aShortId != nullptr )
        {
            std::size_t length = std::strlen( aShortId );
            if( length > static_cast<std::size_t>( kShortIdLength ) )
            {
                length = static_cast<std::size_t>( kShortIdLength );
            }
            std::memcpy( mShortId, aShortId, length );
        }

        {
            std::lock_guard<std::mutex> guard( registryMutex() );
            mNext = gFirst;
            gFirst = this;
        }

        const ThresholdResolver resolver = gResolver.load( std::memory_order_acquire );
        if( resolver != nullptr )
        {
            setThreshold( resolver( *this ) );
        }
    }

    //! Unregisters the category.
    //!
    //! Categories are function-local statics and so are destroyed during exit, in an order nobody
    //! controls. Unlinking matters anyway: a visitAll() from another static's destructor would
    //! otherwise walk into freed storage.
    LogCategory::~LogCategory()
    {
        std::lock_guard<std::mutex> guard( registryMutex() );
        LogCategory** link = &gFirst;
        while( *link != nullptr )
        {
            if( *link == this )
            {
                *link = mNext;
                return;
            }
            link = &( *link )->mNext;
        }
    }

    //! Calls @p aVisitor once for every registered category, with the registry held.
    void LogCategory::visitAll
        (
        Visitor aVisitor,  //!< Callback; ignored if null.
        void* aContext     //!< Passed through to each call unchanged.
        )
    {
        if( aVisitor == nullptr )
        {
            return;
        }

        std::lock_guard<std::mutex> guard( registryMutex() );
        for( LogCategory* category = gFirst; category != nullptr; category = category->mNext )
        {
            aVisitor( *category, aContext );
        }
    }

    //! Installs the callback consulted by every category constructed from now on.
    LogCategory::ThresholdResolver LogCategory::setThresholdResolver
        (
        ThresholdResolver aResolver  //!< New resolver, or null to fall back to the default.
        )
    {
        return gResolver.exchange( aResolver, std::memory_order_acq_rel );
    }
}
