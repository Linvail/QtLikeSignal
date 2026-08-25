"""
Helper to apply sanitizer settings to the build context.

Integration
------------------------------------------------
# In the root wscript file, put:
def set_defaults(ctx):
    ctx.load("sanitizer")

# Load in options context.
def options(opt):
    opt.load("sanitizer")

# Call apply_sanitizer_settings() before you create the task generators.
def build(bld):
    bld.apply_sanitizer_settings()
    bld(target="MyTarget", ..., **kwargs)
------------------------------------------------

"""

import sys

from waflib.Configure import conf
from waflib import Options, Logs, Context


@conf
def apply_sanitizer_settings(bld):
    """
    Apply sanitizer settings to the build context.
    Should be called before you create the task generators.
    """
    if bld.ASan_enabled and bld.TSan_enabled:
        Logs.warn(
            "Cannot enable both AddressSanitizer and ThreadSanitizer at the same time."
            " AddressSanitizer will be used."
        )

    Logs.debug(
        f"sanitizer: Applying sanitizer settings. ASan enabled: {bld.ASan_enabled}, TSan enabled: {bld.TSan_enabled}"
    )

    if bld.env.CXX and (
        "clang++" in bld.env.CXX[0].lower() or "g++" in bld.env.CXX[0].lower()
    ):  # Clang or GCC compiler
        if bld.ASan_enabled:
            Logs.info("AddressSanitizer is enabled.")
            bld.env.append_unique("CFLAGS", ["-fsanitize=address"])
            bld.env.append_unique("CXXFLAGS", ["-fsanitize=address"])
            bld.env.append_unique("LINKFLAGS", ["-fsanitize=address"])
        elif bld.TSan_enabled:
            Logs.info("ThreadSanitizer is enabled.")
            bld.env.append_unique("CFLAGS", ["-fsanitize=thread"])
            bld.env.append_unique("CXXFLAGS", ["-fsanitize=thread"])
            bld.env.append_unique("LINKFLAGS", ["-fsanitize=thread"])
    elif bld.env.CXX and "cl" in bld.env.CXX[0].lower():  # MSVC compiler
        if bld.ASan_enabled:
            Logs.info("AddressSanitizer is enabled.")
            bld.env.append_value("CFLAGS", ["/fsanitize=address"])
            bld.env.append_value("CXXFLAGS", ["/fsanitize=address"])
        elif bld.TSan_enabled:
            Logs.warn("ThreadSanitizer is not supported on Windows with MSVC.")


def _ASan_enabled(ctx):
    """
    Return whether AddressSanitizer is enabled for the current build.

    This is a property of the Context.Context class, and can be accessed as
    ``ctx.ASan_enabled``.
    """
    if Options.options.enable_asan == "yes":
        return True
    elif Options.options.enable_asan == "no":
        return False


def _TSan_enabled(ctx):
    """
    Return whether ThreadSanitizer is enabled for the current build.

    This is a property of the Context.Context class, and can be accessed as
    ``ctx.TSan_enabled``.
    """
    if Options.options.enable_tsan == "yes":
        return True
    elif Options.options.enable_tsan == "no":
        return False


def options(opt):
    """
    Add options to enable AddressSanitizer and ThreadSanitizer to the Sanitizer group.
    """
    group = opt.add_option_group("Sanitizer options")

    group.add_option(
        "--enable-asan",
        dest="enable_asan",
        choices=["yes", "no"],
        default="no",
        help="Enable AddressSanitizer, default is 'no'",
    )

    group.add_option(
        "--enable-tsan",
        dest="enable_tsan",
        choices=["yes", "no"],
        default="no",
        help="Enable ThreadSanitizer, default is 'no'",
    )

    Context.Context.ASan_enabled = property(_ASan_enabled)
    Context.Context.TSan_enabled = property(_TSan_enabled)


def configure(cfg):
    """
    Hook the ``waf configure`` command.

    This is executed when sanitizer is first loaded.

    It currently ensures that ``--enable-asan``, and ``--enable-tsan``,
    when given to ``waf configure``, throw an error.
    """
    # Verify that no enable_asan or enable_tsan was specified
    # at configure time (unless this configure was due to an
    # auto-reconfigure
    if "configure" in sys.argv and (
        Options.options.enable_asan != "no" or Options.options.enable_tsan != "no"
    ):
        cfg.fatal(
            "Cannot specify '--enable-asan' or '--enable-tsan' at configure time."
            "Use 'waf set-defaults' instead."
        )

