// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The Windows half of QtLikeSignal::CallStack. For the calling thread, it uses
//! CaptureStackBackTrace(). For a different thread, it copies the registers and the stack while
//! the thread is suspended, and walks the copy with DbgHelp after the thread runs again.

#include "QtLikeSignalDebug/CallStack.hpp"

#include "QtLikeSignalDebug/CallStackPlatform.hpp"

#include <windows.h>
// After windows.h, because dbghelp.h needs it and does not include it.
#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// The headers declare this only for Windows 8 and later targets. Windows 7 with KB2533623 also
// has it.
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
    #define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace QtLikeSignal
{
    namespace
    {
        //! How much of the stack of a different thread a capture copies, up from its stack pointer.
        //!
        //! The walk also reads the frames above this limit, but from the live stack after the
        //! thread runs again, so they can be torn. Half of the default 1 MiB reservation holds the
        //! stack of any usual handler.
        const std::size_t kSnapshotBytes = 512 * 1024;

        //! The longest symbol name that a lookup gives, in characters. A longer name is cut.
        const ULONG kMaxSymbolName = 512;

        //! Where the search for a module's path gives up, in characters. A Windows path reaches
        //! about 32767 characters, and a buffer that doubles from MAX_PATH passes that on the
        //! eighth try, so this is a stop for an API that answers nonsense rather than a real limit.
        const std::size_t kMaxModulePath = 64 * 1024;

        //! The DbgHelp entry points that this file calls, found at run time.
        //!
        //! **This file loads DbgHelp and does not link it**, for the reason that ThreadWin.cpp
        //! gives for the thread-name API. An import goes into each program that links this
        //! library, and each toolchain that builds it then needs dbghelp.lib. The DLL is in
        //! System32 on each supported Windows.
        //!
        //! **DbgHelp is single-threaded**, so each call from this file into DbgHelp holds mLock.
        //! A different library in the process that calls DbgHelp can still race with this file.
        //! One caller alone cannot prevent that.
        struct DbgHelp
        {
            std::mutex mLock;        //!< Held around each call into DbgHelp, and each capture.
            bool mLoaded { false };  //!< True after load() finds all the entry points below.
            std::string mError;      //!< Why load() failed; empty until then.

            //! SymInitialize(), or null before load().
            decltype( &::SymInitialize ) mSymInitialize { nullptr };

            //! SymSetOptions(), or null before load().
            decltype( &::SymSetOptions ) mSymSetOptions { nullptr };

            //! SymRefreshModuleList(), or null before load().
            decltype( &::SymRefreshModuleList ) mSymRefreshModuleList { nullptr };

            //! SymFromAddr(), or null before load().
            decltype( &::SymFromAddr ) mSymFromAddr { nullptr };

            //! SymGetLineFromAddr64(), or null before load().
            decltype( &::SymGetLineFromAddr64 ) mSymGetLineFromAddr64 { nullptr };

            //! SymFunctionTableAccess64(), or null before load().
            decltype( &::SymFunctionTableAccess64 ) mSymFunctionTableAccess64 { nullptr };

            //! SymGetModuleBase64(), or null before load().
            decltype( &::SymGetModuleBase64 ) mSymGetModuleBase64 { nullptr };

            //! StackWalk64(), or null before load().
            decltype( &::StackWalk64 ) mStackWalk64 { nullptr };
        };

        //! The one DbgHelp record of the process.
        DbgHelp& dbgHelp()
        {
            static DbgHelp api;
            return api;
        }

        //! Finds one entry point and puts it in @p aFunction. @return false if the DLL does not
        //! have it.
        //! @tparam Function The type of the pointer to the entry point, as decltype() gives it.
        template <typename Function>
        bool resolve
            (
            HMODULE aModule,       //!< The loaded DbgHelp.
            const char* aName,     //!< The name of the entry point.
            Function& aFunction    //!< Gets the address of the entry point, or null.
            )
        {
            aFunction = reinterpret_cast<Function>( GetProcAddress( aModule, aName ) );
            return aFunction != nullptr;
        }

        //! Loads DbgHelp and starts its symbol handler, the first time. Call it with mLock held.
        //! @return true if DbgHelp is available.
        bool load
            (
            DbgHelp& aApi  //!< The record to fill.
            )
        {
            if( aApi.mLoaded )
            {
                return true;
            }
            if( !aApi.mError.empty() )
            {
                return false;
            }

            // Only from System32, so that a dbghelp.dll next to the program does not load. The
            // process never frees it, because the symbol handler lives as long as the process.
            const HMODULE module = LoadLibraryExW( L"dbghelp.dll", nullptr,
                LOAD_LIBRARY_SEARCH_SYSTEM32 );
            if( module == nullptr )
            {
                aApi.mError = "dbghelp.dll could not be loaded, error "
                    + std::to_string( GetLastError() );
                return false;
            }

            const bool resolved = resolve( module, "SymInitialize", aApi.mSymInitialize )
                && resolve( module, "SymSetOptions", aApi.mSymSetOptions )
                && resolve( module, "SymRefreshModuleList", aApi.mSymRefreshModuleList )
                && resolve( module, "SymFromAddr", aApi.mSymFromAddr )
                && resolve( module, "SymGetLineFromAddr64", aApi.mSymGetLineFromAddr64 )
                && resolve( module, "SymFunctionTableAccess64", aApi.mSymFunctionTableAccess64 )
                && resolve( module, "SymGetModuleBase64", aApi.mSymGetModuleBase64 )
                && resolve( module, "StackWalk64", aApi.mStackWalk64 );
            if( !resolved )
            {
                aApi.mError = "dbghelp.dll lacks an entry point that reading a stack needs";
                return false;
            }

            // Names without decoration, line numbers, and no dialog when a symbol file is not
            // there. Deferred loads, because a process has many modules and a report names few.
            aApi.mSymSetOptions( SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES
                | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS );

            // TRUE: add the modules that are already loaded. A failure is not fatal. The usual
            // cause is that something else in the process started the handler first, and then
            // each lookup below still works. Thus the lookups give the real answer.
            ( void )aApi.mSymInitialize( GetCurrentProcess(), nullptr, TRUE );
            aApi.mLoaded = true;
            return true;
        }

        //! The part of the stack of a suspended thread that the capture copied. The walk reads
        //! this copy in place of the stack.
        struct Snapshot
        {
            DWORD64 mBase { 0 };                //!< The address of the copy: the stack pointer.
            DWORD64 mSize { 0 };                //!< How many bytes the copy has.
            std::vector<unsigned char> mBytes;  //!< The copy.
        };

        //! The snapshot that the current walk reads.
        //!
        //! The memory reader of StackWalk64 has no parameter for it. The capture sets it and clears
        //! it with the lock of DbgHelp held, and holds that lock for the full walk. Thus there is
        //! never more than one.
        const Snapshot* gWalking = nullptr;

        //! The memory reader for StackWalk64: the stack from the snapshot, all else from the
        //! process.
        //!
        //! Code and unwind tables do not change while the thread runs. Thus a live read of them
        //! gives the same bytes as the snapshot. The stack does change, so the walk does not read
        //! it live.
        BOOL CALLBACK readMemory
            (
            HANDLE aProcess,    //!< The process: this process.
            DWORD64 aAddress,   //!< The first byte to read.
            PVOID aBuffer,      //!< Where to put the bytes.
            DWORD aSize,        //!< How many bytes to read.
            LPDWORD aRead       //!< Gets how many bytes the read gave. Can be null.
            )
        {
            const Snapshot* const snapshot = gWalking;
            if( snapshot != nullptr && aAddress >= snapshot->mBase
                && aAddress - snapshot->mBase <= snapshot->mSize
                && aSize <= snapshot->mSize - ( aAddress - snapshot->mBase ) )
            {
                const std::size_t offset = static_cast<std::size_t>( aAddress - snapshot->mBase );
                std::memcpy( aBuffer, snapshot->mBytes.data() + offset, aSize );
                if( aRead != nullptr )
                {
                    *aRead = aSize;
                }
                return TRUE;
            }

            // ReadProcessMemory() and not a copy, so a bad address gives an error, not a fault.
            SIZE_T read = 0;
            const BOOL ok = ReadProcessMemory( aProcess,
                reinterpret_cast<LPCVOID>( static_cast<std::uintptr_t>( aAddress ) ), aBuffer,
                aSize, &read );
            if( aRead != nullptr )
            {
                *aRead = static_cast<DWORD>( read );
            }
            return ok;
        }

        //! Sets @p aFrame so that the walk starts where @p aContext stopped.
        //! @return the machine type that StackWalk64 needs, or 0 on a processor that this file
        //! does not handle.
        DWORD startFrame
            (
            const CONTEXT& aContext,  //!< The registers of the thread.
            STACKFRAME64& aFrame      //!< The frame to set.
            )
        {
            std::memset( &aFrame, 0, sizeof aFrame );
            aFrame.AddrPC.Mode = AddrModeFlat;
            aFrame.AddrFrame.Mode = AddrModeFlat;
            aFrame.AddrStack.Mode = AddrModeFlat;
            #if defined( _M_X64 ) || defined( __x86_64__ )
                aFrame.AddrPC.Offset = aContext.Rip;
                aFrame.AddrFrame.Offset = aContext.Rsp;
                aFrame.AddrStack.Offset = aContext.Rsp;
                return IMAGE_FILE_MACHINE_AMD64;
            #elif defined( _M_IX86 ) || defined( __i386__ )
                aFrame.AddrPC.Offset = aContext.Eip;
                aFrame.AddrFrame.Offset = aContext.Ebp;
                aFrame.AddrStack.Offset = aContext.Esp;
                return IMAGE_FILE_MACHINE_I386;
            #else
                ( void )aContext;
                return 0;
            #endif
        }

        //! @return the stack pointer in @p aContext, or 0 on a processor that this file does not
        //! handle.
        std::uintptr_t stackPointer
            (
            const CONTEXT& aContext  //!< The registers of the thread.
            )
        {
            #if defined( _M_X64 ) || defined( __x86_64__ )
                return static_cast<std::uintptr_t>( aContext.Rsp );
            #elif defined( _M_IX86 ) || defined( __i386__ )
                return static_cast<std::uintptr_t>( aContext.Esp );
            #else
                ( void )aContext;
                return 0;
            #endif
        }

        #if defined( _M_IX86 ) || defined( __i386__ )
        //! Follows the chain of frame pointers from @p aContext through @p aSnapshot. The EBP of
        //! each frame points to the saved EBP of its caller, and the return address is just above.
        //!
        //! **Why x86 walks two times.** An x86 frame has no unwind table. Thus StackWalk64 unwinds
        //! it from the frame data of the PDB when there is some, and by a guess when there is not.
        //! A busy thread often stops inside a system DLL, which has no PDB here. Then the guess can
        //! be wrong, and the walk can stop about twelve frames later. The chain needs no symbols,
        //! and each system DLL and each build without optimisation keeps it. It misses only code
        //! that is built without frame pointers, and there StackWalk64 is better. Thus both walks
        //! run, and the capture keeps the deeper walk.
        std::vector<void*> framePointerWalk
            (
            const CONTEXT& aContext,    //!< The registers of the thread.
            const Snapshot& aSnapshot   //!< The stack of the thread.
            )
        {
            std::vector<void*> frames;
            frames.push_back( reinterpret_cast<void*>( std::uintptr_t { aContext.Eip } ) );

            DWORD64 ebp = aContext.Ebp;
            while( frames.size() < static_cast<std::size_t>( CallStack::kMaxFrames ) )
            {
                // Both words must be inside the copy and aligned. A chain that leaves the stack
                // ends here.
                if( ebp < aSnapshot.mBase || ( ebp & 3 ) != 0
                    || ebp - aSnapshot.mBase + 2 * sizeof( DWORD ) > aSnapshot.mSize )
                {
                    break;
                }
                DWORD link[2];
                std::memcpy( link,
                    aSnapshot.mBytes.data() + static_cast<std::size_t>( ebp - aSnapshot.mBase ),
                    sizeof link );
                if( link[1] == 0 )
                {
                    break;
                }
                frames.push_back( reinterpret_cast<void*>( std::uintptr_t { link[1] } ) );

                // The frame of a caller is always higher on the stack. A link that is not higher
                // is not part of the chain.
                if( link[0] <= ebp )
                {
                    break;
                }
                ebp = link[0];
            }
            return frames;
        }
        #endif

        //! Gets the frames of the calling thread, from @p aFirst outwards.
        std::vector<void*> callingThreadFrames
            (
            void* aFirst  //!< The return address of CallStack::capture().
            )
        {
            const int kWanted = CallStack::kMaxFrames + CallStackPlatform::kInnerFrames;
            void* frames[kWanted];
            const USHORT count = CaptureStackBackTrace( 0, kWanted, frames, nullptr );
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

        //! @return the part of @p aPath after its last separator.
        const char* baseName
            (
            const char* aPath  //!< A path, with either separator.
            )
        {
            const char* name = aPath;
            for( const char* c = aPath; *c != '\0'; ++c )
            {
                if( *c == '\\' || *c == '/' )
                {
                    name = c + 1;
                }
            }
            return name;
        }

        //! @return the file name of the module that holds @p aAddress, or "?" if no module holds
        //! it.
        std::string moduleAt
            (
            DWORD64 aAddress,  //!< An address in the module.
            DWORD64& aBase     //!< Gets the base address of the module, or 0.
            )
        {
            aBase = 0;

            // UNCHANGED_REFCOUNT: this call takes no reference on the module, so it has no
            // reference to free.
            HMODULE module = nullptr;
            if( GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>( static_cast<std::uintptr_t>( aAddress ) ), &module )
                == FALSE )
            {
                return "?";
            }

            // An HMODULE is the address at which the module is loaded.
            aBase = static_cast<DWORD64>( reinterpret_cast<std::uintptr_t>( module ) );

            // GetModuleFileNameW, and a buffer that grows, because a module can sit under a path
            // of more than MAX_PATH characters. The call then fills the buffer, returns its size
            // and truncates, which a fixed buffer cannot tell from the whole name. The narrow call
            // cannot read such a path at all unless the program is built long-path aware, so the
            // name is read wide and converted here.
            std::vector<wchar_t> wide( MAX_PATH );
            for( ;; )
            {
                const DWORD length = GetModuleFileNameW( module, wide.data(),
                    static_cast<DWORD>( wide.size() ) );
                if( length == 0 )
                {
                    return "?";
                }
                if( length < wide.size() )
                {
                    break;
                }
                if( wide.size() >= kMaxModulePath )
                {
                    return "?";
                }
                wide.resize( wide.size() * 2 );
            }

            const int bytes = WideCharToMultiByte( CP_UTF8, 0, wide.data(), -1, nullptr, 0,
                nullptr, nullptr );
            if( bytes <= 0 )
            {
                return "?";
            }
            std::vector<char> path( static_cast<std::size_t>( bytes ) );
            if( WideCharToMultiByte( CP_UTF8, 0, wide.data(), -1, path.data(), bytes, nullptr,
                nullptr ) <= 0 )
            {
                return "?";
            }
            return baseName( path.data() );
        }
    }

    //! Opens a handle to the calling thread, with the rights that a capture needs.
    CallStack::Target CallStack::Target::currentThread()
    {
        Target target;
        const DWORD id = GetCurrentThreadId();

        // A real handle, not GetCurrentThread(). GetCurrentThread() gives a pseudo handle that
        // means "the thread that uses it", and in a capture that is the wrong thread.
        const HANDLE handle = OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
            | THREAD_QUERY_INFORMATION, FALSE, id );
        if( handle != nullptr )
        {
            target.mHandle = handle;
            target.mThreadId = id;
        }
        return target;
    }

    //! Closes the handle, if there is one, and makes this target empty.
    void CallStack::Target::release()
    {
        if( mHandle != nullptr )
        {
            CloseHandle( static_cast<HANDLE>( mHandle ) );
        }
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

        // A thread that suspends itself never resumes. Also, the calling thread needs no
        // snapshot, because it does not move while it reads itself.
        if( aTarget.mThreadId == GetCurrentThreadId() )
        {
            stack.mFrames = callingThreadFrames( QT_LIKE_SIGNAL_DEBUG_RETURN_ADDRESS() );
            return stack;
        }

        DbgHelp& api = dbgHelp();
        const std::lock_guard<std::mutex> lock( api.mLock );
        if( !load( api ) )
        {
            stack.mError = api.mError;
            return stack;
        }

        // Do each allocation before the thread is suspended. The thread can stop while it holds
        // the heap lock, and an allocation then waits for that lock forever.
        Snapshot snapshot;
        snapshot.mBytes.resize( kSnapshotBytes );
        CONTEXT context;
        std::memset( &context, 0, sizeof context );
        context.ContextFlags = CONTEXT_FULL;

        const HANDLE thread = static_cast<HANDLE>( aTarget.mHandle );
        if( SuspendThread( thread ) == static_cast<DWORD>( -1 ) )
        {
            stack.mError = "the thread could not be suspended, error "
                + std::to_string( GetLastError() ) + "; has it ended?";
            return stack;
        }

        // From here to ResumeThread(), only system calls run: nothing that takes a lock in this
        // process. GetThreadContext() comes first, because SuspendThread() only asks the kernel
        // to stop the thread, and GetThreadContext() waits until the thread stops.
        const BOOL haveContext = GetThreadContext( thread, &context );
        const DWORD contextError = ( haveContext != FALSE ) ? 0 : GetLastError();
        if( haveContext != FALSE )
        {
            const std::uintptr_t sp = stackPointer( context );
            MEMORY_BASIC_INFORMATION region;
            if( sp != 0 && VirtualQuery( reinterpret_cast<LPCVOID>( sp ), &region, sizeof region )
                != 0 )
            {
                // The committed pages from the stack pointer up to the base of the stack are one
                // region. Thus the end of the region is the limit of the read.
                const std::uintptr_t end = reinterpret_cast<std::uintptr_t>( region.BaseAddress )
                    + region.RegionSize;
                const std::size_t wanted = std::min<std::size_t>( end - sp,
                    snapshot.mBytes.size() );
                SIZE_T copied = 0;
                ( void )ReadProcessMemory( GetCurrentProcess(), reinterpret_cast<LPCVOID>( sp ),
                    snapshot.mBytes.data(), wanted, &copied );
                snapshot.mBase = sp;
                snapshot.mSize = copied;
            }
        }
        ResumeThread( thread );

        if( haveContext == FALSE )
        {
            stack.mError = "the thread's registers could not be read, error "
                + std::to_string( contextError );
            return stack;
        }

        STACKFRAME64 frame;
        const DWORD machine = startFrame( context, frame );
        if( machine == 0 )
        {
            stack.mError = "reading another thread's stack is not supported on this processor";
            return stack;
        }

        // StackWalk64 changes the registers that it gets while it unwinds them.
        const CONTEXT stopped = context;

        // Without this, a module that loaded after the handler started has no unwind data.
        ( void )api.mSymRefreshModuleList( GetCurrentProcess() );

        gWalking = &snapshot;
        stack.mExactFirst = true;
        bool cutShort = false;
        while( api.mStackWalk64( machine, GetCurrentProcess(), thread, &frame, &context,
            &readMemory, api.mSymFunctionTableAccess64, api.mSymGetModuleBase64, nullptr )
            != FALSE )
        {
            // A zero program counter ends the walk, because StackWalk64 keeps saying yes at the
            // bottom of a stack. The first frame is the exception: a thread that called through a
            // null pointer stops at address 0, and that is the one stack a reader most wants. Keep
            // it and let the walk carry on to the caller, which is the frame that names the bug.
            if( frame.AddrPC.Offset == 0 && !stack.mFrames.empty() )
            {
                break;
            }
            if( stack.mFrames.size() == static_cast<std::size_t>( kMaxFrames ) )
            {
                cutShort = true;
                break;
            }
            stack.mFrames.push_back(
                reinterpret_cast<void*>( static_cast<std::uintptr_t>( frame.AddrPC.Offset ) ) );
        }
        gWalking = nullptr;

        #if defined( _M_IX86 ) || defined( __i386__ )
            std::vector<void*> chain = framePointerWalk( stopped, snapshot );
            if( chain.size() > stack.mFrames.size() )
            {
                stack.mFrames.swap( chain );
                cutShort = ( stack.mFrames.size() == static_cast<std::size_t>( kMaxFrames ) );
            }
        #else
            ( void )stopped;
        #endif

        if( cutShort )
        {
            stack.mError = "cut short at " + std::to_string( kMaxFrames ) + " frames";
        }
        if( stack.mFrames.empty() )
        {
            stack.mError = "the walk found no frames";
        }
        return stack;
    }

    //! Names each frame from the list of modules and from the PDBs that DbgHelp can find.
    std::vector<std::string> CallStackPlatform::describe
        (
        const std::vector<void*>& aFrames,  //!< The frames, innermost first.
        bool aExactFirst                    //!< True if aFrames[0] is not a return address.
        )
    {
        std::vector<std::string> names;
        names.reserve( aFrames.size() );

        DbgHelp& api = dbgHelp();
        const std::lock_guard<std::mutex> lock( api.mLock );
        const bool haveDbgHelp = load( api );
        if( haveDbgHelp )
        {
            ( void )api.mSymRefreshModuleList( GetCurrentProcess() );
        }

        // SYMBOL_INFO ends in a name of one character, and the caller gives it space to grow.
        std::vector<unsigned char> symbolBuffer( sizeof( SYMBOL_INFO ) + kMaxSymbolName );
        SYMBOL_INFO* const symbol = reinterpret_cast<SYMBOL_INFO*>( symbolBuffer.data() );

        for( std::size_t i = 0; i < aFrames.size(); ++i )
        {
            const DWORD64 address =
                static_cast<DWORD64>( reinterpret_cast<std::uintptr_t>( aFrames[i] ) );

            // A return address can be the first byte of the next line, or of the next function.
            // Thus the lookup uses the byte before it, which is inside the call that it returns
            // from.
            const bool exact = ( i == 0 && aExactFirst ) || address == 0;
            const DWORD64 lookup = exact ? address : address - 1;

            DWORD64 moduleBase = 0;
            std::string text = moduleAt( lookup, moduleBase );

            std::memset( symbolBuffer.data(), 0, symbolBuffer.size() );
            symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
            symbol->MaxNameLen = kMaxSymbolName;
            DWORD64 displacement = 0;
            if( haveDbgHelp
                && api.mSymFromAddr( GetCurrentProcess(), lookup, &displacement, symbol ) != FALSE )
            {
                text += "!";
                text += symbol->Name;
                text += "+0x" + hex( address - symbol->Address );
            }
            else if( moduleBase != 0 )
            {
                text += "+0x" + hex( address - moduleBase );
            }

            IMAGEHLP_LINE64 line;
            std::memset( &line, 0, sizeof line );
            line.SizeOfStruct = sizeof line;
            DWORD lineDisplacement = 0;
            if( haveDbgHelp
                && api.mSymGetLineFromAddr64( GetCurrentProcess(), lookup, &lineDisplacement,
                &line ) != FALSE
                && line.FileName != nullptr )
            {
                text += " (" + std::string( baseName( line.FileName ) ) + ":"
                    + std::to_string( line.LineNumber ) + ")";
            }

            names.push_back( text );
        }
        return names;
    }
}
