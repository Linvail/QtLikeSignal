from waflib.Configure import conf
from waflib import Logs, Options
import glob, os


#: Optional system libraries the native Linux builds look for, as (pkg-config name, apt package).
#:
#: No uselib name here: check_cfg() derives it from the package, uppercased, so "x11" is the X11
#: uselib and "libdecor-0" is LIBDECOR-0. The define follows from that with every non-alphanumeric
#: turned into an underscore -- HAVE_X11, HAVE_WAYLAND_CLIENT, HAVE_LIBDECOR_0 -- which is also what
#: GGL's Wayland code spells its libdecor guard as. Naming a store by hand would only restate what
#: waf already decides, and give a second name to keep in step.
OPTIONAL_PACKAGES = [
    ("x11", "libx11-dev"),
    ("libinput", "libinput-dev"),
    ("libudev", "libudev-dev"),
    ("wayland-client", "libwayland-dev"),
    ("wayland-cursor", "libwayland-dev"),
    ("libdecor-0", "libdecor-0-dev"),
    ("egl", "libegl-dev"),
    ("gl", "libgl-dev"),
    ("wayland-egl", "libwayland-dev"),
]


#: pkg-config packages consulted for a variable rather than for compiler flags.
#:
#: Wayland is the one platform whose build needs more than headers and a library: the protocol is
#: XML, and the C for it is generated. Both pieces are found the same way everything else is --
#: pkg-config knows where wayland-scanner lives and where wayland-protocols keeps its XML, so
#: neither has to be guessed at a hardcoded path.
#:
#: These *do* name a store, and it is the one place where doing so earns its keep: the variable
#: lands in env under "<store>_<variable>", and the default store for "wayland-scanner" would put a
#: hyphen in that key -- which a build script could then only read as env["WAYLAND-SCANNER_..."],
#: never as the attribute access used everywhere else.
OPTIONAL_PACKAGE_VARIABLES = [
    ("wayland-scanner", "WAYLAND_SCANNER", "wayland_scanner", "libwayland-bin"),
    ("wayland-protocols", "WAYLAND_PROTOCOLS", "pkgdatadir", "wayland-protocols"),
]


def _check_optional_packages(ctx):
    """
    Ask pkg-config about the optional system libraries, recording what it finds.

    check_cfg() rather than looking for a header under /usr/include, which is what this replaced.
    Three things come out of that:

      * The answer is the *package's* answer. pkg-config reports the include directories, the
        library directories and the full library list, including transitive ones -- a header probe
        reports only that a file exists at a path the probe happened to guess, and then the build
        has to name "-lX11" by hand and hope the library is where the linker looks.

      * It is per toolchain. The check runs inside the configured environment, so the cross
        toolchain simply does not run it and cannot pick up host X11 while targeting Windows. A
        path test in a build script has no idea which target it is answering for.

      * It reaches the build as a uselib. `uselib_store="X11"` sets env.HAVE_X11 along with
        INCLUDES_X11, LIBPATH_X11 and LIB_X11, so a target writes use=["X11"] and gets all of them,
        propagated down the use chain like any other dependency. That is what removed the hand-kept
        list of link libraries every consumer of QtMimicGui used to have to repeat.

    mandatory=False throughout: a missing package is a fact to record, not a reason to fail.
    """
    for package, package_name in OPTIONAL_PACKAGES:
        ctx.check_cfg(
            package=package,
            args=["--cflags", "--libs"],
            msg="Checking for %s" % package,
            errmsg="not found (apt install %s)" % package_name,
            mandatory=False,
        )

    # Variables rather than flags: these two packages are asked where something *is*, not how to
    # compile against it. The answers land in env as <STORE>_<variable> -- WAYLAND_SCANNER_wayland_
    # scanner and WAYLAND_PROTOCOLS_pkgdatadir -- and the build script reads them from there.
    for package, store, variable, package_name in OPTIONAL_PACKAGE_VARIABLES:
        ctx.check_cfg(
            package=package,
            uselib_store=store,
            variables=[variable],
            msg="Checking for %s" % package,
            errmsg="not found (apt install %s)" % package_name,
            mandatory=False,
        )


@conf
def configure_Linux_x64_gcc(ctx, root):
    prev_variant = ctx.variant

    env_name = "linux64-gcc"
    Logs.info("Configuring %s" % env_name)
    ctx.setenv(env_name, root)

    ctx.load("gcc gxx")
    ctx.load("gccdeps", tooldir="submodules/external/waf/waflib/extras")

    ctx.env.append_unique("CXXFLAGS", ["-std=c++17"])

    # Ignoring the result of a [[nodiscard]] function is an error, not a warning.
    #
    # Scoped to this one diagnostic rather than -Werror: a warning-free build is not a goal here
    # and turning every warning fatal would make an unrelated one block the build. This one is
    # different in kind. [[nodiscard]] is only ever written where dropping the result loses
    # information the caller needed -- Thread::post() returning false means the task will never
    # run -- so a call that discards it is a defect at the call site, and the compiler is the only
    # thing that can catch it at every one of them.
    #
    # Added after a queued call posted during thread startup was silently dropped for the life of
    # the project. See README.md, "Compiler configuration".
    ctx.env.append_unique("CXXFLAGS", ["-Werror=unused-result"])

    ctx.env.ENV_VALID = True

    # Before the debug and release environments are derived from this one, so both inherit what was
    # found rather than each having to ask again.
    _check_optional_packages(ctx)

    """
    For debug build
    """
    base_env = ctx.env
    ctx.setenv("%s-debug" % env_name, base_env)

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_unique(flag, ["-g", "-O0"])

    """
    For release build
    """
    ctx.setenv("%s-release" % env_name, base_env)

    ctx.env.append_unique("DEFINES", ["NDEBUG"])

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_unique(flag, ["-O2"])

    # Restore
    ctx.variant = prev_variant


@conf
def configure_Linux_x64_clang(ctx, root):
    prev_variant = ctx.variant

    env_name = "linux64-clang"
    Logs.info("Configuring %s" % env_name)
    ctx.setenv(env_name, root)

    ctx.load("clang clangxx")
    ctx.load("gccdeps", tooldir="submodules/external/waf/waflib/extras")

    ctx.env.append_unique("CXXFLAGS", ["-std=c++17"])

    # Ignoring the result of a [[nodiscard]] function is an error, not a warning.
    #
    # Scoped to this one diagnostic rather than -Werror: a warning-free build is not a goal here
    # and turning every warning fatal would make an unrelated one block the build. This one is
    # different in kind. [[nodiscard]] is only ever written where dropping the result loses
    # information the caller needed -- Thread::post() returning false means the task will never
    # run -- so a call that discards it is a defect at the call site, and the compiler is the only
    # thing that can catch it at every one of them.
    #
    # Added after a queued call posted during thread startup was silently dropped for the life of
    # the project. See README.md, "Compiler configuration".
    ctx.env.append_unique("CXXFLAGS", ["-Werror=unused-result"])

    ctx.env.ENV_VALID = True

    # Before the debug and release environments are derived from this one, so both inherit what was
    # found rather than each having to ask again.
    _check_optional_packages(ctx)

    """
    For debug build
    """
    base_env = ctx.env
    ctx.setenv("%s-debug" % env_name, base_env)

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_unique(flag, ["-g", "-O0"])

    """
    For release build
    """
    ctx.setenv("%s-release" % env_name, base_env)

    ctx.env.append_unique("DEFINES", ["NDEBUG"])

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_unique(flag, ["-O2"])

    # Restore
    ctx.variant = prev_variant


@conf
def configure_Windows_x64_Linux_clang(ctx, root):
    prev_variant = ctx.variant

    env_name = "linux-2-win64-clang"
    Logs.info("Configuring %s" % env_name)
    ctx.setenv(env_name, root)

    ctx.load("clang clangxx")
    ctx.load("gccdeps", tooldir="submodules/external/waf/waflib/extras")

    # Tell Clang executable wrapper to compile/link for Windows target
    target_flags = ["-target", "x86_64-pc-windows-gnu"]
    ctx.env.append_value("CC", target_flags)
    ctx.env.append_value("CXX", target_flags)
    ctx.env.append_value("LINK_CC", target_flags)
    ctx.env.append_value("LINK_CXX", target_flags)

    # Standard Windows target patterns (.exe, .dll, etc.)
    ctx.gcc_modifier_win32()
    ctx.gxx_modifier_win32()

    ctx.env.DEST_OS = "win32"
    ctx.env.DEST_BINFMT = "pe"

    # The same Windows system uselibs the MSVC toolchains declare, so a target that says
    # use=["USER32"] builds identically under both. Deliberately not the X11 and libinput ones the
    # native Linux toolchains look for: this one targets Windows, and picking up the host's X11
    # because the build machine happens to have it is exactly the mistake a path probe would make.
    ctx.env.LIB_USER32 = ["user32"]
    ctx.env.LIB_GDI32 = ["gdi32"]
    ctx.env.LIB_OPENGL32 = ["opengl32"]

    # Same Unicode setting as the MSVC toolchains in toolchain-windows.py, so that this
    # cross-compile builds the *same* program rather than a near-copy. The generic-text Win32 entry
    # points -- CreateWindowEx, RegisterClass, TextOut -- resolve to the W forms when UNICODE is
    # defined and to the A forms when it is not, so without this the two toolchains compiled the
    # same sources through different halves of the SDK. Both mappings are valid, which is why the
    # difference stayed invisible until a call was written that only suited one of them.
    #
    # This does not change the entry point: mingw-w64 switches main() to wmain() only when
    # -municode is passed, and it is not.
    ctx.env.append_value("DEFINES", ["UNICODE", "_UNICODE"])

    ctx.env.append_unique("CXXFLAGS", ["-std=c++17"])

    # Ignoring the result of a [[nodiscard]] function is an error, not a warning.
    #
    # Scoped to this one diagnostic rather than -Werror: a warning-free build is not a goal here
    # and turning every warning fatal would make an unrelated one block the build. This one is
    # different in kind. [[nodiscard]] is only ever written where dropping the result loses
    # information the caller needed -- Thread::post() returning false means the task will never
    # run -- so a call that discards it is a defect at the call site, and the compiler is the only
    # thing that can catch it at every one of them.
    #
    # Added after a queued call posted during thread startup was silently dropped for the life of
    # the project. See README.md, "Compiler configuration".
    ctx.env.append_unique("CXXFLAGS", ["-Werror=unused-result"])

    # Static linking options for standalone Windows executable
    ctx.env.append_value("CXXFLAGS", ["-pthread"])
    ctx.env.append_value(
        "LINKFLAGS", ["-static", "-static-libgcc", "-static-libstdc++", "-pthread"]
    )
    ctx.env.append_value("LINKFLAGS", ["-fuse-ld=lld"])
    ctx.env.STLIB_MARKER = []
    ctx.env.SHLIB_MARKER = []

    # Automatically detect and configure MinGW standard C++ header/library search paths
    cpp_paths = glob.glob("/usr/lib/gcc/x86_64-w64-mingw32/*/include/c++")
    if cpp_paths:
        cpp_path = cpp_paths[0]
        target_cpp_path = "%s/x86_64-w64-mingw32" % cpp_path
        ctx.env.append_value(
            "CXXFLAGS", ["-isystem", cpp_path, "-isystem", target_cpp_path]
        )

        lib_path = os.path.dirname(os.path.dirname(cpp_path))
        ctx.env.append_value(
            "LINKFLAGS", ["-L", lib_path, "-L", "/usr/x86_64-w64-mingw32/lib"]
        )
    else:
        Logs.warn(
            "Warning: Could not locate 64-bit MinGW GCC C++ headers under /usr/lib/gcc/x86_64-w64-mingw32/"
        )

    ctx.env.ENV_VALID = True

    """
    For debug build
    """
    base_env = ctx.env
    ctx.setenv("%s-debug" % env_name, base_env)

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_unique(flag, ["-g", "-O0", "-gcodeview"])
    ctx.env.append_value("LINKFLAGS", ["-Wl,-pdb="])

    """
    For release build
    """
    ctx.setenv("%s-release" % env_name, base_env)

    ctx.env.append_unique("DEFINES", ["NDEBUG"])

    for flag in ("CFLAGS", "CXXFLAGS"):
        ctx.env.append_unique(flag, ["-O2"])

    # Restore
    ctx.variant = prev_variant
