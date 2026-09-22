// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The Linux half of QtLikeSignal::CallStack. For the calling thread, it uses backtrace(). For a
//! different thread, it sends a real-time signal. The handler of the signal runs backtrace() on
//! that thread and gives the result back.

#include "QtLikeSignalDebug/CallStack.hpp"

#include "QtLikeSignalDebug/CallStackPlatform.hpp"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace QtLikeSignal
{
    namespace
    {
        //! The number of frames that the handler keeps: kMaxFrames, and space for the handler and
        //! the signal trampoline of the kernel. These two come first, and the capture removes them
        //! before it gives the stack back.
        constexpr int kHandlerFrames = CallStack::kMaxFrames + 4;

        //! The state of the one request. The handler moves it Requested -> Writing -> Done. A
        //! reader that stops its wait moves it Requested -> Abandoned. Then a handler that runs
        //! late finds nothing to answer, and writes nothing.
        enum RequestState : int
        {
            Idle,       //!< No capture is in progress.
            Requested,  //!< A capture sent the signal and waits for the answer.
            Writing,    //!< The handler writes the frames.
            Done,       //!< The handler wrote the frames and posted the semaphore.
            Abandoned   //!< The capture stopped its wait before the handler started.
        };

        // The handler uses these atomics, and a signal handler can use only a lock-free atomic.
        // All Linux targets have both. On a new target, these lines tell if it does not.
        static_assert( std::atomic<int>::is_always_lock_free, "lock-free int needed" );
        static_assert( std::atomic<long>::is_always_lock_free, "lock-free long needed" );

        //! The request in progress.
        //!
        //! There is one, in static storage. A signal handler can reach only what it gets, and it
        //! gets nothing. Also, gCaptureLock lets only one capture run at a time.
        struct Request
        {
            std::atomic<int> mState { Idle };    //!< A RequestState.
            std::atomic<long> mThreadId { 0 };   //!< The thread whose stack the capture wants.
            void* mFrames[kHandlerFrames];       //!< The frames that the handler writes.
            int mCount { 0 };                    //!< How many frames the handler wrote.
            void* mStoppedAt { nullptr };        //!< Where the signal stopped the thread, or null.
            sem_t mAnswered;                     //!< The handler posts this when it is done.
        };

        //! The one request.
        Request gRequest;

        //! A capture holds this lock from start to end, so only one capture runs at a time. It
        //! also guards the three variables below.
        std::mutex gCaptureLock;

        bool gInstalled = false;     //!< True after install() installs the handler; never removed.
        std::string gInstallError;   //!< Why install() failed; empty until then.
        int gSignal = 0;             //!< The signal that the handler is on.

        //! @return the id that the kernel gives the calling thread. Async-signal-safe.
        long currentThreadId()
        {
            return static_cast<long>( ::syscall( SYS_gettid ) );
        }

        //! @return the instruction at which the signal stopped the thread, or null on a processor
        //! that this file does not know.
        void* stoppedAt
            (
            void* aContext  //!< The ucontext_t that the kernel gave to the handler.
            )
        {
            const ucontext_t* const context = static_cast<const ucontext_t*>( aContext );
            #if defined( __x86_64__ )
                return reinterpret_cast<void*>( context->uc_mcontext.gregs[REG_RIP] );
            #elif defined( __i386__ )
                return reinterpret_cast<void*>( context->uc_mcontext.gregs[REG_EIP] );
            #elif defined( __aarch64__ )
                return reinterpret_cast<void*>( context->uc_mcontext.pc );
            #elif defined( __arm__ )
                return reinterpret_cast<void*>( context->uc_mcontext.arm_pc );
            #else
                ( void )context;
                return nullptr;
            #endif
        }

        //! The signal handler: records the stack of this thread, if the request is for this thread.
        //!
        //! It calls only async-signal-safe functions, with one exception that all such handlers
        //! make. backtrace() is not on the POSIX list, because its first call loads the unwinder.
        //! install() made that first call on an ordinary thread, so this call loads nothing.
        void onCaptureSignal
            (
            int aSignal,           //!< The signal: gSignal.
            siginfo_t* aInfo,      //!< Not used.
            void* aContext         //!< The ucontext_t of the stopped thread.
            )
        {
            ( void )aSignal;
            ( void )aInfo;
            const int savedErrno = errno;

            if( gRequest.mThreadId.load( std::memory_order_acquire ) == currentThreadId() )
            {
                int expected = Requested;
                if( gRequest.mState.compare_exchange_strong( expected, Writing,
                    std::memory_order_acq_rel ) )
                {
                    gRequest.mCount = backtrace( gRequest.mFrames, kHandlerFrames );
                    gRequest.mStoppedAt = stoppedAt( aContext );
                    gRequest.mState.store( Done, std::memory_order_release );
                    sem_post( &gRequest.mAnswered );
                }
            }

            errno = savedErrno;
        }

        //! Installs the handler, the first time. Call it with gCaptureLock held.
        //! @return true if the handler is installed.
        bool install()
        {
            if( gInstalled )
            {
                return true;
            }
            if( !gInstallError.empty() )
            {
                return false;
            }

            const int signal = SIGRTMIN + CallStack::kSignalOffset;
            if( signal > SIGRTMAX )
            {
                gInstallError = "SIGRTMIN + " + std::to_string( CallStack::kSignalOffset )
                    + " is past SIGRTMAX on this system";
                return false;
            }

            // Do not take a signal that something else already handles. A new handler replaces
            // the old handler, and that code then fails without a message.
            struct sigaction existing;
            if( sigaction( signal, nullptr, &existing ) != 0 )
            {
                gInstallError = "the handler for SIGRTMIN + "
                    + std::to_string( CallStack::kSignalOffset ) + " could not be read, errno "
                    + std::to_string( errno );
                return false;
            }
            if( existing.sa_handler != SIG_DFL )
            {
                gInstallError = "SIGRTMIN + " + std::to_string( CallStack::kSignalOffset )
                    + " already has a handler, so no other thread's stack can be read";
                return false;
            }

            // The call that loads the unwinder, here, where it can load. See onCaptureSignal().
            void* warmUp[1];
            ( void )backtrace( warmUp, 1 );

            if( sem_init( &gRequest.mAnswered, 0, 0 ) != 0 )
            {
                gInstallError = "sem_init() failed, errno " + std::to_string( errno );
                return false;
            }

            // SA_RESTART, so the kernel restarts a system call that the signal stops, where it
            // can. The class comment names the calls that it cannot restart.
            struct sigaction action;
            std::memset( &action, 0, sizeof action );
            action.sa_sigaction = &onCaptureSignal;
            sigemptyset( &action.sa_mask );
            action.sa_flags = SA_SIGINFO | SA_RESTART;
            if( sigaction( signal, &action, nullptr ) != 0 )
            {
                gInstallError = "the handler could not be installed, errno "
                    + std::to_string( errno );
                return false;
            }

            gSignal = signal;
            gInstalled = true;
            return true;
        }

        //! Waits for the post of the handler, for kAnswerTimeoutMs at most. @return true if the
        //! post came.
        bool waitForAnswer()
        {
            // sem_timedwait() uses a deadline on the wall clock. A clock step during the wait
            // moves only the timeout, and a step that is long enough to matter is longer than the
            // wait.
            timespec deadline;
            clock_gettime( CLOCK_REALTIME, &deadline );
            deadline.tv_sec += CallStack::kAnswerTimeoutMs / 1000;
            deadline.tv_nsec += static_cast<long>( CallStack::kAnswerTimeoutMs % 1000 ) * 1000000L;
            if( deadline.tv_nsec >= 1000000000L )
            {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }

            while( sem_timedwait( &gRequest.mAnswered, &deadline ) != 0 )
            {
                if( errno != EINTR )
                {
                    return false;
                }
            }
            return true;
        }

        //! Gets the frames of the calling thread, from @p aFirst outwards.
        std::vector<void*> callingThreadFrames
            (
            void* aFirst  //!< The return address of CallStack::capture().
            )
        {
            const int kWanted = CallStack::kMaxFrames + CallStackPlatform::kInnerFrames;
            void* frames[kWanted];
            const int count = backtrace( frames, kWanted );
            return CallStackPlatform::fromFrame( frames, count, aFirst );
        }

        //! @return @p aValue in hexadecimal, without a prefix.
        std::string hex
            (
            unsigned long long aValue  //!< The value to write.
            )
        {
            char buffer[24];
            std::snprintf( buffer, sizeof buffer, "%llx", aValue );
            return buffer;
        }

        //! @return the part of @p aPath after its last slash.
        const char* baseName
            (
            const char* aPath  //!< A path.
            )
        {
            const char* const slash = std::strrchr( aPath, '/' );
            return ( slash != nullptr ) ? slash + 1 : aPath;
        }

        //! @return @p aName without its C++ mangling, or @p aName as it is if it is not a C++ name.
        std::string demangle
            (
            const char* aName  //!< A symbol name from the dynamic symbol table.
            )
        {
            int status = 0;
            char* const demangled = abi::__cxa_demangle( aName, nullptr, nullptr, &status );
            if( status != 0 || demangled == nullptr )
            {
                return aName;
            }
            const std::string name( demangled );
            std::free( demangled );
            return name;
        }
    }

    //! Records the id that the kernel gives the calling thread. A Linux thread needs no handle.
    CallStack::Target CallStack::Target::currentThread()
    {
        Target target;
        target.mThreadId = static_cast<unsigned long>( currentThreadId() );
        return target;
    }

    //! Makes this target empty. There is nothing to close.
    void CallStack::Target::release()
    {
        mHandle = nullptr;
        mThreadId = 0;
    }

    //! Captures the stack of the calling thread. See the declaration.
    QT_LIKE_SIGNAL_DEBUG_NOINLINE CallStack CallStack::capture()
    {
        CallStack stack;
        stack.mFrames = callingThreadFrames( QT_LIKE_SIGNAL_DEBUG_RETURN_ADDRESS() );
        return stack;
    }

    //! Captures the stack of @p aTarget. See the declaration, and the class comment for the
    //! method.
    QT_LIKE_SIGNAL_DEBUG_NOINLINE CallStack CallStack::capture
        (
        const Target& aTarget  //!< The thread to read.
        )
    {
        CallStack stack;
        if( !aTarget.isValid() )
        {
            stack.mError = "the target names no thread";
            return stack;
        }

        // The calling thread reads itself directly. A signal to itself also works, but then the
        // handler runs before the send returns, which is a longer path.
        const long threadId = static_cast<long>( aTarget.mThreadId );
        if( threadId == currentThreadId() )
        {
            stack.mFrames = callingThreadFrames( QT_LIKE_SIGNAL_DEBUG_RETURN_ADDRESS() );
            return stack;
        }

        const std::lock_guard<std::mutex> lock( gCaptureLock );
        if( !install() )
        {
            stack.mError = gInstallError;
            return stack;
        }

        // An old answer can stay only if a handler posted after its reader stopped the wait, and
        // the state machine prevents that. This clear makes sure that one bug does not become two.
        while( sem_trywait( &gRequest.mAnswered ) == 0 )
        {
        }

        gRequest.mCount = 0;
        gRequest.mStoppedAt = nullptr;
        gRequest.mThreadId.store( threadId, std::memory_order_release );
        gRequest.mState.store( Requested, std::memory_order_release );

        if( ::syscall( SYS_tgkill, static_cast<long>( ::getpid() ), threadId, gSignal ) != 0 )
        {
            const int error = errno;
            gRequest.mState.store( Idle, std::memory_order_release );
            stack.mError = ( error == ESRCH ) ? std::string( "the thread has ended" )
                : "the signal could not be sent, errno " + std::to_string( error );
            return stack;
        }

        if( !waitForAnswer() )
        {
            int expected = Requested;
            if( gRequest.mState.compare_exchange_strong( expected, Abandoned,
                std::memory_order_acq_rel ) )
            {
                stack.mError = "the thread did not answer within "
                    + std::to_string( kAnswerTimeoutMs ) + " ms; it blocks SIGRTMIN + "
                    + std::to_string( kSignalOffset ) + ", or is stuck in the kernel";
                return stack;
            }

            // The handler started at the end of the wait, and posts very soon.
            while( sem_wait( &gRequest.mAnswered ) != 0 && errno == EINTR )
            {
            }
        }

        // Above the frame that the signal stopped are the handler and the trampoline through
        // which the kernel returns. Where the processor is known, the capture finds the stopped
        // frame by its address. On other processors, it removes the two frames by count, in the
        // order in which the unwinder gives them.
        const int count = gRequest.mCount;
        int first = -1;
        for( int i = 0; i < count && gRequest.mStoppedAt != nullptr; ++i )
        {
            if( gRequest.mFrames[i] == gRequest.mStoppedAt )
            {
                first = i;
                break;
            }
        }
        stack.mExactFirst = ( first >= 0 );
        if( first < 0 )
        {
            first = ( count < 2 ) ? count : 2;
        }

        const int last = std::min( count, first + kMaxFrames );
        stack.mFrames.assign( gRequest.mFrames + first, gRequest.mFrames + last );
        if( count - first > kMaxFrames )
        {
            stack.mError = "cut short at " + std::to_string( kMaxFrames ) + " frames";
        }
        if( stack.mFrames.empty() )
        {
            stack.mError = "the thread answered with no frames";
        }

        gRequest.mState.store( Idle, std::memory_order_release );
        return stack;
    }

    //! Names each frame from the dynamic symbol table, and always gives its module offset.
    std::vector<std::string> CallStackPlatform::describe
        (
        const std::vector<void*>& aFrames,  //!< The frames, innermost first.
        bool aExactFirst                    //!< True if aFrames[0] is not a return address.
        )
    {
        std::vector<std::string> names;
        names.reserve( aFrames.size() );

        for( std::size_t i = 0; i < aFrames.size(); ++i )
        {
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>( aFrames[i] );

            // A return address can be the first byte of the next function. Thus the lookup uses
            // the byte before it, which is inside the call that it returns from.
            const bool exact = ( i == 0 && aExactFirst ) || address == 0;
            const std::uintptr_t lookup = exact ? address : address - 1;

            Dl_info info;
            std::memset( &info, 0, sizeof info );
            if( dladdr( reinterpret_cast<void*>( lookup ), &info ) == 0
                || info.dli_fname == nullptr )
            {
                names.push_back( "?" );
                continue;
            }

            // addr2line takes the module offset, so the text shows it also next to a name.
            const std::string module = baseName( info.dli_fname );
            const std::string offset = module + "+0x"
                + hex( address - reinterpret_cast<std::uintptr_t>( info.dli_fbase ) );
            if( info.dli_sname != nullptr && info.dli_saddr != nullptr )
            {
                names.push_back( module + "!" + demangle( info.dli_sname ) + "+0x"
                    + hex( address - reinterpret_cast<std::uintptr_t>( info.dli_saddr ) ) + " ("
                    + offset + ")" );
            }
            else
            {
                names.push_back( offset );
            }
        }
        return names;
    }
}
