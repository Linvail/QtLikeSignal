"""
One-line status messages for build-time wscripts.

Integration
------------------------------------------------
# Add "build-msg" to local_tools in both wscript_options and wscript_configure.

def build(bld):
    bld.build_msg("QtLikeSignalGui backends", "x11, drm, wayland", color="GREEN")
------------------------------------------------

WHY THIS EXISTS RATHER THAN bld.msg()

bld.msg() is a configure-time API. Context.start_msg() writes the label through Context.to_log()
and then again through Logs.pprint(), and end_msg() does the same with the value. In a
ConfigurationContext that is harmless, because self.logger is set and to_log() goes to config.log
while only pprint() reaches the console. A BuildContext has no logger, so to_log() takes its
fallback branch and writes to stderr instead -- without a trailing newline. The result is one
mangled line carrying everything twice:

    ----------------------------------------QtLikeSignalGui backendsQtLikeSignalGui backends   : x11, drm, waylandx11, drm, wayland

waflib is not modified to fix that. This helper simply does not use the configure-time API: it
formats the line itself and emits it with a single Logs.pprint(), which is the coloured equivalent
of Logs.info() and cannot interleave with anything.

WHY THE clangdb GUARD

waf's own waflib/extras/clang_compilation_database.py patches BuildContext.execute_build to run the
clangdb command ahead of every build (see new_execute_build there). That command recurses through
every wscript exactly as the build does, so each of these messages is produced twice per `waf
build` and twice per `waf install` -- once for compile_commands.json, once for the real work. The
duplicate pass is wanted; its console output is not. Skipping it here is what makes each message
appear once.
"""

from waflib.Configure import conf
from waflib import Logs


#: Width the label is padded to, matching the default Context.line_just so these lines sit flush
#: with the "Checking for ..." block that waf prints at configure time.
LABEL_WIDTH = 40


@conf
def build_msg(bld, label, result, color="GREEN"):
    """
    Print one aligned "label : result" line, once, in colour.

    :param bld: the build context.
    :param label: what is being reported, e.g. "QtLikeSignalGui backends".
    :param result: the outcome, e.g. "x11, drm, wayland" or "skipped (install libx11-dev)".
    :param color: a waf colour name; GREEN for something built, YELLOW for something skipped.
    """
    if bld.cmd == "clangdb":
        # The compile_commands.json pass. It evaluates every wscript for a second time; saying
        # everything twice is the only difference the reader would see.
        return

    # Logs.info rather than Logs.pprint, for two reasons beyond it being the plainer call. pprint
    # writes to stderr, while the compile lines this sits among go to stdout through Logs.info, so
    # the two streams could interleave when the output is piped. And pprint formats as
    # "msg + reset + ' ' + label", which leaves a trailing space on every line when there is no
    # label to print. Colouring the string here avoids both.
    Logs.info(
        "%s%s : %s%s"
        % (Logs.colors(color), label.ljust(LABEL_WIDTH), result, Logs.colors.NORMAL)
    )


def options(opt):
    """
    Nothing to add. Present so the module can be named in local_tools alongside the others, which
    is what imports it and registers build_msg() on the context for every waf command.
    """
    pass


def configure(cfg):
    """
    Nothing to configure. Present for the same reason as options().
    """
    pass
