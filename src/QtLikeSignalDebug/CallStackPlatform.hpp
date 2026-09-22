// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! What the platform half of QtLikeSignal::CallStack supplies to the shared half. Private: only the
//! files in src/QtLikeSignalDebug include this header.

#ifndef QT_LIKE_SIGNAL_DEBUG_CALL_STACK_PLATFORM_HPP
#define QT_LIKE_SIGNAL_DEBUG_CALL_STACK_PLATFORM_HPP

#include <string>
#include <vector>

// The stack of the calling thread starts at the frame that CallStack::capture() returns to. The
// capture finds that frame by its address, not by a count of frames to skip. The count is not
// fixed: an optimiser can inline a helper, and an instrumented build can add a frame
// (AddressSanitizer intercepts backtrace()). The address belongs to capture() only if capture()
// keeps a frame of its own. Thus capture() must not be inlined, which link-time code generation
// can otherwise do across the library boundary.
#if defined( _MSC_VER )
    #include <intrin.h>
    #define QT_LIKE_SIGNAL_DEBUG_NOINLINE __declspec( noinline )
    #define QT_LIKE_SIGNAL_DEBUG_RETURN_ADDRESS() _ReturnAddress()
#else
    #define QT_LIKE_SIGNAL_DEBUG_NOINLINE __attribute__( ( noinline ) )
    #define QT_LIKE_SIGNAL_DEBUG_RETURN_ADDRESS() __builtin_return_address( 0 )
#endif

namespace QtLikeSignal
{
    namespace CallStackPlatform
    {
        //! The maximum number of frames, more than CallStack::kMaxFrames, that a capture of the
        //! calling thread reads: space for the frames between the capture call and the caller.
        //! The capture removes those frames.
        const int kInnerFrames = 8;

        //! @return the frames of @p aFrames from the one that is equal to @p aFirst outwards, at
        //! most kMaxFrames of them.
        //!
        //! When no frame is equal, it returns all of @p aFrames, up to that limit. A report with
        //! some frames of this library at the top is better than a report without the frame of
        //! the caller.
        std::vector<void*> fromFrame
            (
            void* const* aFrames,  //!< The frames of the calling thread, innermost first.
            int aCount,            //!< How many frames there are.
            void* aFirst           //!< The return address of CallStack::capture().
            );

        //! Names each frame for CallStack::toString(), with one entry for each frame.
        //!
        //! Each entry has as much of "module!function+0x1c (file.cpp:42)" as the symbols give, and
        //! is never empty. It has no address, because toString() writes the address itself.
        std::vector<std::string> describe
            (
            const std::vector<void*>& aFrames,  //!< The frames, innermost first.
            bool aExactFirst                    //!< True if aFrames[0] is not a return address.
            );

    }
}

#endif // QT_LIKE_SIGNAL_DEBUG_CALL_STACK_PLATFORM_HPP
