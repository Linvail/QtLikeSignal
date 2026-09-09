from waflib import Logs
import platform


def options(opt):
    opt.load("compiler_c compiler_cxx")
    group = opt.add_option_group("Toolchain Options")
    group.add_option(
        "--no-exceptions",
        dest="no_exceptions",
        action="store_true",
        default=False,
        help=(
            "Build the library targets with C++ exceptions disabled, as an application using "
            "this library does. Applies to QtLikeSignal and QtLikeSignalGui only -- the test "
            "suite uses exceptions deliberately and keeps them."
        ),
    )
    group.add_option(
        "--enable-address-sanitizer-on-Linux",
        dest="enable_address_sanitizer_on_Linux",
        action="store_true",
        default=False,
        help="Enable AddressSanitizer on Linux",
    )
    group.add_option(
        "--enable-thread-sanitizer-on-Linux",
        dest="enable_thread_sanitizer_on_Linux",
        action="store_true",
        default=False,
        help="Enable ThreadSanitizer on Linux",
    )
    group.add_option(
        "--disable-asan-on-win",
        dest="disable_asan_on_win",
        action="store_true",
        default=False,
        help="Disable AddressSanitizer on Windows",
    )


def apply_library_exception_policy(bld, kw):
    """Adds `-fno-exceptions` to *kw* when --no-exceptions was given.

    Applications build this library from source with exceptions off, so the library has to compile
    that way. This is how that is verified with the real include paths, which a standalone syntax
    check cannot reach for the GUI backends.

    Called by each library wscript rather than applied at configure time, because the test suite
    and the benchmarks use exceptions on purpose and must keep them.

    **gcc and clang only, and it refuses on MSVC rather than pretending.** Two reasons, in order of
    which matters more:

    1. MSVC has no `-fno-exceptions`. Omitting `/EHsc` disables unwind semantics rather than
       exceptions, and leaves `try` compiling with warning C4530 instead of the hard error clang
       gives -- so it cannot fail a build the way this check needs to, and would report success
       whether or not the source was clean.
    2. It cannot even be applied here. waf appends `env.CXXFLAGS` after a target's own `cxxflags`,
       so `/EHsc` comes back from the environment however this list is filtered. Removing it would
       mean editing `env.CXXFLAGS` at configure time, which would reach the test suite too.

    A silent no-op would be worse than refusing: the whole value of this option is that it fails
    when the source is wrong, and an option that quietly checks nothing is how a rule stops being
    enforced without anyone noticing.
    """
    if not getattr(bld.options, "no_exceptions", False):
        return kw

    if "msvc" in (bld.env.CXX_NAME or ""):
        bld.fatal(
            "--no-exceptions is not supported on MSVC. MSVC has no -fno-exceptions: dropping "
            "/EHsc disables unwind semantics rather than exceptions, and leaves try/catch "
            "compiling with warning C4530 instead of an error -- so it would report success "
            "whatever the source said. Run the check on linux64-clang or linux64-gcc, which is "
            "where the flag means what it says. See the note beside /EHsc in "
            "tools/toolchain-windows.py."
        )

    kw["cxxflags"] = list(kw.get("cxxflags", [])) + ["-fno-exceptions"]
    return kw


def configure(ctx):

    prior_variant = ctx.variant
    root = ctx.env

    if platform.system() == "Windows":
        ctx.load("toolchain-windows", tooldir="tools")
        ctx.configure_win64_msvc(root)
        ctx.configure_win32_msvc(root)
    elif platform.system() == "Linux":
        ctx.load("toolchain-linux", tooldir="tools")
        ctx.configure_Linux_x64_gcc(root)
        ctx.configure_Linux_x64_clang(root)
        ctx.configure_Windows_x64_Linux_clang(root)

    # Restore the original environment and variant
    ctx.variant = prior_variant
