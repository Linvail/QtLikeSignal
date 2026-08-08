from waflib import Logs
import platform


def options(opt):
    opt.load("compiler_c compiler_cxx")
    group = opt.add_option_group("Toolchain Options")
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
