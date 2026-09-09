// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for the QtLikeSignal framework (Object affinity/connections,
//! Thread event loops, and CoreApplication).

#include "gtest/gtest.h"

//! Whether this build is instrumented, asked in the two spellings that exist.
//!
//! GCC answers with the plain macros __SANITIZE_ADDRESS__ and __SANITIZE_THREAD__. Clang answers
//! through __has_feature() and defines the GCC spellings only in recent versions, so asking just
//! one of the two silently does nothing on the other compiler -- and this repo builds both
//! linux64-gcc and linux64-clang.
//!
//! Nothing below is compiled into an ordinary build. An uninstrumented QtLikeSignal-test carries no
//! suppression text at all, which is what keeps the list from drifting out of sight and being
//! believed long after it stopped being true.
#if defined( __has_feature )
    #  if __has_feature( address_sanitizer ) && !defined( QT_LIKE_SIGNAL_TEST_ASAN )
        #    define QT_LIKE_SIGNAL_TEST_ASAN 1
    #  endif
    #  if __has_feature( thread_sanitizer ) && !defined( QT_LIKE_SIGNAL_TEST_TSAN )
        #    define QT_LIKE_SIGNAL_TEST_TSAN 1
    #  endif
#endif

#if defined( __SANITIZE_ADDRESS__ ) && !defined( QT_LIKE_SIGNAL_TEST_ASAN )
    #  define QT_LIKE_SIGNAL_TEST_ASAN 1
#endif

#if defined( __SANITIZE_THREAD__ ) && !defined( QT_LIKE_SIGNAL_TEST_TSAN )
    #  define QT_LIKE_SIGNAL_TEST_TSAN 1
#endif

#if defined( QT_LIKE_SIGNAL_TEST_ASAN )

    //! Leaks this suite should not fail on, handed to LeakSanitizer before it reports.
    //!
    //! The runtime looks this symbol up weakly at startup and appends whatever it returns to the
    //! suppression list, so the entries travel inside the binary. That is the whole reason it is
    //! used here rather than a .txt file and an LSAN_OPTIONS every caller has to remember: a
    //! developer who runs the ASan build straight out of install/ gets the same answer CI does, and
    //! a suppression cannot be silently lost by a command line that forgot to point at it.
    //!
    //! WHY THESE THREE
    //!
    //! All of them are reached from libdecor and from nowhere else in this binary. Wayland has no
    //! server-side decoration, so PlatformIntegrationWayland asks libdecor to draw the title bar;
    //! the libdecor-cairo plugin renders that text with pango, pango initialises fontconfig, and
    //! fontconfig builds a process-wide font cache it never frees. libdecor also asks the XDG
    //! settings portal for the cursor theme over D-Bus, and on a machine with no portal running --
    //! WSLg is one -- that call fails and libdecor-cairo leaks the DBusError string it was handed.
    //!
    //! None of it is ours. QtLikeSignalGui releases what it owns: libdecor_unref() and
    //! libdecor_frame_unref() both run in PlatformIntegrationWayland's teardown.
    //!
    //! HOW THAT WAS ESTABLISHED, so it can be rechecked rather than believed:
    //!
    //!   1. QTLIKESIGNAL_WAYLAND_DECORATIONS=0 -> 0 bytes leaked, 5/5 GuiWayland tests pass. The entire
    //!      report belongs to the libdecor branch.
    //!   2. --gtest_repeat=3 grew the total by 516 bytes per extra pass against a 16878-byte first
    //!      pass: a fixed cache plus 129 bytes per Wayland connection, which is exactly what a
    //!      single-test run reports. Nothing here grows without bound.
    //!   3. With these three in place LSan accounts for 565 of 565 suppressed allocations
    //!      (553 fontconfig, 8 dbus, 4 libdecor) and the suite exits 0.
    //!
    //! The patterns name the third-party libraries rather than a call site of ours, so a leak
    //! allocated by QtLikeSignal code stays visible wherever it is freed. Delete an entry the day
    //! its library stops leaking; "leak:libdbus-1.so" in particular should go quiet on a desktop
    //! running a settings portal, and is only reachable at all through libdecor.
    extern "C" const char* __lsan_default_suppressions();

    extern "C" const char* __lsan_default_suppressions()
    {
        return "leak:libfontconfig.so\n"
               "leak:libdbus-1.so\n"
               "leak:libdecor\n";
    }

#endif  // QT_LIKE_SIGNAL_TEST_ASAN

#if defined( QT_LIKE_SIGNAL_TEST_TSAN )

    //! Races this suite should not fail on, handed to ThreadSanitizer before it reports.
    //!
    //! Found and embedded the same way as the LeakSanitizer list above, and for the same underlying
    //! reason: libdecor draws the title bar.
    //!
    //! libdecor-cairo renders that text with pango, and pango starts its own thread to initialise
    //! fontconfig -- TSan names it "[pango] FcInit", with a second "[pango] FcFontSetMatch" -- then
    //! reads the same fontconfig state from the calling thread without an edge TSan can see. Every
    //! report is that one race: a pango thread writing a font cache while the main thread reads it
    //! through strcmp, strlen or g_str_has_suffix.
    //!
    //! The only QtLikeSignal frame in any of them is PlatformIntegrationWayland::createWindow(),
    //! which is where libdecor_decorate() is called. Neither side of the racing access is our
    //! memory, and no QtLikeSignal object appears in the stacks at all. Running with
    //! QTLIKESIGNAL_WAYLAND_DECORATIONS=0 removes every finding, on linux64-clang debug and release
    //! alike.
    //!
    //! The patterns name the two libraries the racing accesses are in. That is deliberately
    //! narrower than "called_from_lib:libdecor-0.so.0", which would have silenced these too but
    //! would also have silenced a genuine race in QtLikeSignal code that libdecor happened to call
    //! into -- and Window, WindowSystemInterface and the event dispatcher are all reachable that
    //! way through libdecor's callbacks. A suppression wide enough to hide a defect we could
    //! actually cause is the wrong trade even when it is shorter.
    //!
    //! Only QtLikeSignal-test is covered here. QtLikeSignal-Performance-Tests has its own,
    //! unrelated set in src/perf/tsan-suppressions.txt, which exists because the installed Qt 6 is
    //! not built with -fsanitize=thread; do not merge the two lists.
    extern "C" const char* __tsan_default_suppressions();

    extern "C" const char* __tsan_default_suppressions()
    {
        return "race:libfontconfig.so\n"
               "race:libglib-2.0.so\n";
    }

#endif  // QT_LIKE_SIGNAL_TEST_TSAN

int main
    (
    int argc,
    char** argv
    )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
