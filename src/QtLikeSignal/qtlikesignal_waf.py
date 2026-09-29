# SPDX-FileCopyrightText: 2026 Evan
# SPDX-License-Identifier: MIT

"""Build policy that the three library wscripts share: QtLikeSignal, QtLikeSignalGui and
QtLikeSignalDebug.

This module is in src/QtLikeSignal, beside the library, and not in tools/, because a different
repository can use QtLikeSignal as a git submodule and recurse into src/QtLikeSignal only. Its build
then loads its own tools/, and never this repository's. A module in that directory with the same
name as one here would replace it without a message, because Python keeps one module for each
name. Thus the name is unique, and the file is always in the tree that the consumer recurses into.

src/QtLikeSignal/wscript imports it directly: waf puts the directory of a wscript on sys.path while
it runs that wscript. The other two wscripts are in sibling directories, where that is not true, so
they load it by path with waflib.Context.load_tool().
"""


def apply_library_exception_policy(bld, kw):
    """Compiles the target in *kw* with exceptions off when --no-exceptions was given.

    Applications build this library from source with exceptions off, so the library has to compile
    that way. This is how that is verified with the real include paths, which a standalone syntax
    check cannot reach for the GUI backends.

    Called by each library wscript rather than applied at configure time, because the test suite
    and the benchmarks use exceptions on purpose and must keep them.

    The option is read with a default, so a build that does not declare --no-exceptions -- a
    repository that uses QtLikeSignal as a submodule, for example -- builds with exceptions on.

    **gcc and clang** get `-fno-exceptions`, which makes a bare `try` or `throw` a hard error.

    **MSVC** has no such flag, so this uses the three flags that a build without exceptions uses
    there. Unreal Engine's build tool, for one, compiles a module that way:

    - No `/EH` option. Without it the compiler has no unwind semantics and does not define
      `__cpp_exceptions`, so the `#if defined( __cpp_exceptions )` guards in the library take the
      same branch that they take under `-fno-exceptions`.
    - `/D_HAS_EXCEPTIONS=0`. This is the switch of Microsoft's STL. The STL headers then contain no
      `try` or `throw`, and an error that would throw ends the process with a fail-fast instead
      (exit code 0xC0000409, not `abort()`). Microsoft does not document it for user code, and the
      default build does not use it. It is here because the applications this check stands for
      compile with it, and a header that compiles only without it would fail in their build.
    - `/we4530`. Without `/EH`, MSVC compiles a `try` with warning C4530 and returns 0. This makes
      that warning an error, so a bare `try` fails the build as it does under gcc and clang.

    `/EHsc` comes from `env.CXXFLAGS`, and waf puts that after the target's own `cxxflags`, so it
    cannot be overridden from *kw*'s flags. Thus the target gets its own copy of the environment,
    without the `/EH` options. The copy is for this target only, so the test suite and the
    benchmarks keep `/EHsc`.
    """
    if not getattr(bld.options, "no_exceptions", False):
        return kw

    if "msvc" in (bld.env.CXX_NAME or ""):
        env = kw.get("env") or bld.env.derive()
        env.CXXFLAGS = [
            flag for flag in env.CXXFLAGS if not flag.upper().startswith(("/EH", "-EH"))
        ]
        kw["env"] = env
        kw["defines"] = list(kw.get("defines", [])) + ["_HAS_EXCEPTIONS=0"]
        kw["cxxflags"] = list(kw.get("cxxflags", [])) + ["/we4530"]
        return kw

    kw["cxxflags"] = list(kw.get("cxxflags", [])) + ["-fno-exceptions"]
    return kw


def use_user32(bld, kw):
    """Adds the uselib USER32 to *kw* on Windows.

    The event dispatchers of QtLikeSignal post and wait for window messages, and QtLikeSignalGui has
    a window procedure, so both link user32.

    This repository declares LIB_USER32 in its Windows toolchains. A repository that uses
    QtLikeSignal as a submodule has its own toolchains, and they need not declare it. Thus it is
    declared here if it is missing. It is a uselib and not `lib`, because QtLikeSignal compiles to
    objects only, and waf gives the uselibs of a used target to the target that links it, but not
    its `lib`.
    """
    if bld.env.DEST_OS != "win32":
        return kw

    if not bld.env.LIB_USER32:
        bld.env.LIB_USER32 = ["user32"]

    kw["use"] = list(kw.get("use", [])) + ["USER32"]
    return kw
