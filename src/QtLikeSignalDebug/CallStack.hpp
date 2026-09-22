// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::CallStack - the position of a thread: its return addresses, innermost
//! first, and the text that a report shows for them.

#ifndef QT_LIKE_SIGNAL_DEBUG_CALL_STACK_HPP
#define QT_LIKE_SIGNAL_DEBUG_CALL_STACK_HPP

#include <string>
#include <vector>

namespace QtLikeSignal
{
    //! The call stack of one thread at one moment, and its text.
    //!
    //! **The purpose.** LoopHealth tells a watchdog that a loop is stuck, and in which receiver and
    //! which event. It cannot tell *where* in the handler the loop is stuck, and "stuck in a
    //! metacall on the main window" can mean most of a program. The call stack of the stuck thread
    //! gives the rest of the answer. This class reads that stack from the thread of the watchdog,
    //! while the other thread is still stuck.
    //!
    //! @code
    //!   // One time, on the thread to watch:
    //!   QtLikeSignal::CallStack::Target worker = QtLikeSignal::CallStack::Target::currentThread();
    //!
    //!   // Later, on the thread of the watchdog:
    //!   const QtLikeSignal::CallStack stack = QtLikeSignal::CallStack::capture( worker );
    //!   qCCritical( gLogApp ) << stack.toString();
    //! @endcode
    //!
    //! **An optional library.** QtLikeSignal depends only on the C++ runtime and on the threads
    //! of the platform. To read a stack needs more: DbgHelp on Windows, and a signal and the
    //! dynamic loader on Linux. Thus this class is in QtLikeSignalDebug, and a program that wants
    //! it links that library. The build makes QtLikeSignalDebug for Windows and Linux only; on a
    //! different platform there is no target to link.
    //!
    //! **How a capture reads another thread** is different on each platform. Know the condition of
    //! each method before you rely on it:
    //!
    //! - **Windows** suspends the thread, copies its registers and its stack, and resumes it. Then
    //!   it walks the copy with DbgHelp. On x86 it also walks the frame pointers and keeps the
    //!   deeper walk, because x86 has no unwind tables to make the walk of DbgHelp exact. Only
    //!   system calls run while the thread is suspended. Thus a thread that stops while it holds
    //!   the heap lock, or the lock of DbgHelp, cannot deadlock the reader.
    //! - **Linux** sends the thread a real-time signal, SIGRTMIN + kSignalOffset. The handler of
    //!   the signal records the stack and posts a semaphore. The capture installs the handler the
    //!   first time that it needs it, and does not install it if something else already handles
    //!   that signal. **A thread that blocks the signal, or that is in an uninterruptible sleep,
    //!   does not answer.** Then the capture comes back empty after kAnswerTimeoutMs, and error()
    //!   tells why. The signal can also stop a system call that the thread is blocked in. Most of
    //!   these calls restart, but some -- poll(), nanosleep(), sem_wait() -- return EINTR. Code
    //!   that does not try them again sees a failure.
    //! - **Under ThreadSanitizer on Linux**, the sanitizer installs its own signal handler. It runs
    //!   the handler of the capture only when the thread next goes into the runtime of the
    //!   sanitizer. Then the stack has frames of that runtime above the frames of the thread. A
    //!   thread that is blocked in a system call that the sanitizer does not intercept, for example
    //!   a raw futex wait, does not answer.
    //!
    //! **Names need symbols.** toString() names each frame from the debug information that it can
    //! find: the PDB on Windows, and the dynamic symbol table on Linux. On Linux, a function that
    //! is not exported shows as its module and offset. This includes each static function, and
    //! each function of a program that is not linked with -rdynamic. addr2line changes the module
    //! and offset into a name offline. The text always shows the address, so a report is useful
    //! also with no symbols.
    //!
    //! Each function is thread-safe. Captures run one at a time.
    class CallStack
    {
    public:
        //! The maximum number of frames that one capture keeps. A deeper stack loses its outermost
        //! frames.
        static constexpr int kMaxFrames = 64;

        //! How long capture( const Target& ) waits for the thread to answer, on Linux.
        static constexpr int kAnswerTimeoutMs = 1000;

        //! The real-time signal that Linux uses, as an offset from SIGRTMIN.
        //!
        //! SIGRTMIN is not a constant: the C library keeps the first real-time signals for itself,
        //! and tells the rest at run time. Thus an offset names the signal.
        static constexpr int kSignalOffset = 5;

        //! A thread that a different thread can capture. The thread makes it for itself, then
        //! gives it to the reader.
        //!
        //! **The thread makes it, and nothing finds it later.** The calling thread is the only
        //! thread that each platform can name with certainty. QtLikeSignal::Thread does not
        //! give out its handle, and an adopted thread has no handle from QtLikeSignal at all.
        //!
        //! **Capture it only while its thread runs.** On Windows it holds a handle, so a late
        //! capture only fails. On Linux it holds a thread id. After the thread ends, the kernel can
        //! give that id to a new thread, and a late capture then reads the new thread.
        //!
        //! Move-only. It is empty when it is default-constructed, when it was moved from, or when
        //! the platform refused it.
        class Target
        {
        public:
            //! Makes an empty target.
            Target() = default;

            //! Releases the thread that this target names.
            ~Target();

            //! Takes the thread of @p aOther, and makes @p aOther empty.
            Target
                (
                Target&& aOther  //!< The target to take from.
                ) noexcept;

            //! Releases the thread of this target, takes the thread of @p aOther, and makes
            //! @p aOther empty.
            Target& operator=
                (
                Target&& aOther  //!< The target to take from.
                ) noexcept;

            //! Not copyable: a target owns a handle, and only one target can release it.
            Target
                (
                const Target&
                ) = delete;

            //! Not copyable, for the same reason.
            Target& operator=
                (
                const Target&
                ) = delete;

            //! @return a target for the calling thread; empty if the platform refused one.
            static Target currentThread();

            //! @return true if this target names a thread.
            bool isValid() const
            {
                return mThreadId != 0;
            }

        private:
            //! Lets the captures read the handle and the id.
            friend class CallStack;

            //! Releases what this target holds, and makes it empty.
            void release();

            //! Windows: a thread handle that this target owns. Unused on other platforms.
            void* mHandle { nullptr };

            //! The id of the OS thread; 0 when the target is empty.
            unsigned long mThreadId { 0 };
        };

        //! Makes an empty stack with no error.
        CallStack() = default;

        //! Captures the stack of the calling thread. The first frame is in the caller of this
        //! function.
        static CallStack capture();

        //! Captures the stack of @p aTarget from any thread, also from the thread of the target.
        //!
        //! The stack comes back empty, and error() tells why, when the target is empty, when the
        //! thread ended, or when the thread did not answer. The class comment tells what each
        //! platform does to the thread while the capture reads it.
        static CallStack capture
            (
            const Target& aTarget  //!< The thread to read.
            );

        //! @return true if the capture got no frames.
        bool empty() const
        {
            return mFrames.empty();
        }

        //! @return the frames, innermost first.
        //!
        //! Most frames are return addresses, and each one points just after the call that it made.
        //! For the stack of a different thread, the first frame is the exact instruction at which
        //! the thread stopped.
        const std::vector<void*>& frames() const
        {
            return mFrames;
        }

        //! @return why the capture came back empty or cut short; empty when there was no problem.
        const std::string& error() const
        {
            return mError;
        }

        //! @return one line for each frame, innermost first, each with a newline at the end:
        //!
        //! @code
        //!   #0  0x00007ff6a1b2c3d4  app.exe!Worker::spin+0x2a (Worker.cpp:88)
        //! @endcode
        //!
        //! A line has no name and no file when no symbols give them. An empty stack gives one line
        //! that shows error(). The lookup of names is slow, because it can read debug files from
        //! the disk. Thus call this function where you write a report, not on a path that must be
        //! fast.
        std::string toString() const;

    private:
        std::vector<void*> mFrames;  //!< The frames, innermost first.
        std::string mError;          //!< Why the capture failed, or stopped early.

        //! True when mFrames[0] is the instruction at which the thread stopped, not a return
        //! address.
        //!
        //! toString() needs this. It looks up a return address one byte back, inside the call that
        //! the address returns from. But the exact instruction can be the first byte of a
        //! function, and one byte back from that is the end of a different function.
        bool mExactFirst { false };
    };
}

#endif // QT_LIKE_SIGNAL_DEBUG_CALL_STACK_HPP
