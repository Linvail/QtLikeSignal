# Use objcopy or strip to deal with the debug info of the built executables
# based on a global option.

from waflib import Options, Utils, Logs
from waflib.TaskGen import feature, after_method
from waflib.Task import Task
import shutil
import platform


def options(opt):
    group = opt.add_option_group("Debug info options")

    group.add_option(
        "--strip-debug",
        action="store_true",
        default=True,
        help="Strip debug info into a separate file.",
    )
    group.add_option(
        "--install-debug-info",
        action="store_true",
        default=True,
        help="Install the stripped debug info file when doing waf install.",
    )
    group.add_option(
        "--keep-unstripped-binary",
        action="store_true",
        default=False,
        help="Keep unstripped binary.",
    )


def configure(ctx):
    if platform.system() == "Windows":
        return
    # Try to find objcopy and strip, but do not fail if we are using MSVC (where they aren't used/available)
    ctx.find_program("objcopy", var="OBJCOPY", mandatory=False)
    ctx.find_program("strip", var="STRIP", mandatory=False)


class strip_debug_task(Task):
    color = "PINK"

    def sig_explicit_deps(self):
        # Signed against the link task rather than against this task's own input.
        #
        # run() below rewrites the binary it is given: it strips the debug sections out of it and
        # adds a .gnu_debuglink section to it. Waf signs a task by hashing its inputs, so the
        # binary hashed before the run never matches the one left behind, this task is never
        # considered up to date, and it is scheduled again on every single build.
        #
        # Re-running it is not harmless. --only-keep-debug rebuilds the .debug file from a binary
        # whose debug info has already been removed, replacing megabytes of DWARF with nothing,
        # while --add-gnu-debuglink refuses to replace a link that is already there -- it warns
        # "debuglink section already exists", and exits 0, so the build still reports success. The
        # binary is left pointing at a .debug file whose CRC no longer matches it, which every
        # consumer of a debuglink then ignores. In other words the second build of the day silently
        # threw away the symbols the first one produced.
        #
        # The link task's signature changes exactly when the binary is relinked, which is exactly
        # when the debug info has to be extracted again, so it is the right thing to depend on.
        self.m.update(self.generator.link_task.signature())
        for node in self.dep_nodes:
            self.m.update(node.get_bld_sig())

    def run(self):
        # inputs[0] is the unstripped binary (created by link_task)
        # outputs[0] is the .debug file
        # outputs[1] is the .unstripped file (optional)

        executable = self.inputs[0].abspath()
        debug_file = self.outputs[0].abspath()

        # 1. Keep a copy of the built binary (unstripped) if requested
        if len(self.outputs) > 1:
            unstripped = self.outputs[1].abspath()
            try:
                shutil.copy2(executable, unstripped)
            except IOError as e:
                self.generator.bld.fatal("Failed to copy unstripped binary: %s" % e)

        # Retrieve tools from env, defaulting to standard command names if not set
        objcopy_cmd = Utils.to_list(self.env.OBJCOPY or "objcopy")
        strip_cmd = Utils.to_list(self.env.STRIP or "strip")

        # 2. Extract debug symbols into a separate file ending in .debug
        cmd1 = objcopy_cmd + ["--only-keep-debug", executable, debug_file]
        ret = self.exec_command(cmd1)
        if ret:
            return ret

        # 3. Strip the debug symbols from the original executable
        cmd2 = strip_cmd + ["--strip-debug", executable]
        ret = self.exec_command(cmd2)
        if ret:
            return ret

        # 4. Link the stripped executable to the debug file
        cmd3 = objcopy_cmd + ["--add-gnu-debuglink=" + debug_file, executable]
        ret = self.exec_command(cmd3)
        return ret


@feature("debug-info")
@after_method("apply_link")
def process_debug_info(self):
    if not Options.options.strip_debug:
        return

    # Skip if MSVC is used (MSVC generates PDB files separately, no stripping is needed)
    cc_name = self.env.CC_NAME or self.env.CXX_NAME or ""
    if "msvc" in cc_name:
        return

    link_task = getattr(self, "link_task", None)
    if not link_task:
        return

    binary_node = link_task.outputs[0]
    debug_node = binary_node.change_ext(".debug")

    outputs = [debug_node]

    keep_unstripped = Options.options.keep_unstripped_binary
    if keep_unstripped:
        # Keep a copy of the built binary (unstripped), named as <original name>.unstripped
        unstripped_node = binary_node.parent.find_or_declare(
            binary_node.name + ".unstripped"
        )
        outputs.append(unstripped_node)

    # Strip the debug info into a separate file.
    strip_task = self.create_task("strip_debug_task", src=binary_node, tgt=outputs)

    # Force the main binary install task to execute after the strip task
    install_task = getattr(self, "install_task", None)
    if install_task:
        install_task.set_run_after(strip_task)

    # If user is doing waf install (there is a install_task), check if --install-debug-info is set.
    # If yes, install the stripped debug info file along with the executable.
    # If no, do not install the stripped debug info file.
    if Options.options.install_debug_info:
        inst_to = getattr(self, "install_path", getattr(link_task, "inst_to", None))
        if inst_to:
            self.add_install_files(install_to=inst_to, install_from=[debug_node])
