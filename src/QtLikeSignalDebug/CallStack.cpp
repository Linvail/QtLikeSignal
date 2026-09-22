// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The half of QtLikeSignal::CallStack that all platforms share: the moves of a Target, and
//! the text of the frames. CallStackWin.cpp and CallStackLinux.cpp capture the frames and name
//! them.

#include "QtLikeSignalDebug/CallStack.hpp"

#include "QtLikeSignalDebug/CallStackPlatform.hpp"

#include <cstdint>
#include <cstdio>

namespace QtLikeSignal
{
    //! Removes the frames above @p aFirst. See the declaration.
    std::vector<void*> CallStackPlatform::fromFrame
        (
        void* const* aFrames,  //!< The frames of the calling thread, innermost first.
        int aCount,            //!< How many frames there are.
        void* aFirst           //!< The return address of CallStack::capture().
        )
    {
        int first = 0;
        while( first < aCount && aFrames[first] != aFirst )
        {
            ++first;
        }
        if( first == aCount )
        {
            first = 0;
        }

        const int last = ( aCount - first > CallStack::kMaxFrames ) ? first + CallStack::kMaxFrames
            : aCount;
        return std::vector<void*>( aFrames + first, aFrames + last );
    }

    //! Releases the thread that this target names.
    CallStack::Target::~Target()
    {
        release();
    }

    //! Takes the thread of @p aOther, and makes @p aOther empty.
    CallStack::Target::Target
        (
        Target&& aOther  //!< The target to take from.
        ) noexcept
        : mHandle( aOther.mHandle )
        , mThreadId( aOther.mThreadId )
    {
        aOther.mHandle = nullptr;
        aOther.mThreadId = 0;
    }

    //! Releases the thread of this target, takes the thread of @p aOther, and makes @p aOther
    //! empty.
    CallStack::Target& CallStack::Target::operator=
        (
        Target&& aOther  //!< The target to take from.
        ) noexcept
    {
        if( this != &aOther )
        {
            release();
            mHandle = aOther.mHandle;
            mThreadId = aOther.mThreadId;
            aOther.mHandle = nullptr;
            aOther.mThreadId = 0;
        }
        return *this;
    }

    //! Gives one line for each frame, innermost first. The declaration shows the format.
    std::string CallStack::toString() const
    {
        if( mFrames.empty() )
        {
            return "(no call stack: " + ( mError.empty() ? std::string( "nothing was captured" )
                : mError ) + ")\n";
        }

        const std::vector<std::string> names = CallStackPlatform::describe( mFrames, mExactFirst );

        std::string text;
        for( std::size_t i = 0; i < mFrames.size(); ++i )
        {
            // Zeros pad the address to the width of a pointer, so the columns of the report align.
            char head[48];
            std::snprintf( head, sizeof head, "#%-2u 0x%0*llx  ", static_cast<unsigned>( i ),
                static_cast<int>( 2 * sizeof( void* ) ),
                static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( mFrames[i] ) ) );
            text += head;
            text += names[i];
            text += '\n';
        }

        // When the capture stopped early, a line after the last frame says so. A reader looks for
        // more frames there.
        if( !mError.empty() )
        {
            text += "(" + mError + ")\n";
        }
        return text;
    }
}
