from waflib.Configure import conf
from waflib import Logs, Options
import glob, os


@conf
def configure_Linux_x64_gcc(ctx, root):
    prev_variant = ctx.variant

    env_name = "linux64-gcc"
    Logs.info("Configuring %s" % env_name)
    ctx.setenv(env_name, root)

    ctx.load("gcc gxx")
    ctx.load("gccdeps", tooldir="submodules/external/waf/waflib/extras")

    ctx.env.append_unique("CXXFLAGS", ["-std=c++17"])

    ctx.env.ENV_VALID = True

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

    ctx.env.ENV_VALID = True

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

    ctx.env.append_unique("CXXFLAGS", ["-std=c++17"])

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
