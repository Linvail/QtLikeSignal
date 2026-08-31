# SPDX-FileCopyrightText: 2026 Evan
# SPDX-License-Identifier: MIT

"""
Generate the C for a Wayland protocol, as a build task rather than as a side effect.

A Wayland protocol is XML. wayland-scanner turns one file into a client header of inline wrappers
and a C file of interface tables, and a client cannot be compiled without them.

The obvious way to do that from a wscript is to call the scanner while the build script is being
evaluated. It works, and it is wrong in two ways that only show up later: the scanner runs on every
single invocation of waf, including a no-op build that has nothing to do, and the generated files
belong to no task, so `waf clean` does not know about them and nothing reports them in the build
log.

Doing it as a task avoids both, and the one thing that makes a *generated header* awkward is handled
by declaring it. Waf orders tasks within a task generator from their ext_in and ext_out, so a task
that declares it produces ".h" is ordered ahead of every compile task that consumes one -- and
creating the tasks before process_source runs is what puts the generated nodes in the tree in time
for that ordering to be computed. No build-group boundary is needed, which matters because a group
would also push every unrelated target declared afterwards into the later group.

Modelled on GGL's ggl-wayland.py, which solves the same problem the same way.
"""

import os

from waflib import Logs, Task, Utils
from waflib.TaskGen import before_method, feature


class wayland_scanner_protocol(Task.Task):
    """
    Run wayland-scanner over one XML protocol, producing its client header and interface tables.

    ext_in and ext_out are what order this ahead of the compiles that read the header; see the
    module docstring. The two outputs are ordered header first, code second, and run() relies on
    that.
    """

    color = "BLUE"
    ext_in = [".xml"]
    ext_out = [".h", ".c"]

    def keyword(self):
        return "Generating"

    def run(self):
        scanner = self.env.WAYLAND_SCANNER_wayland_scanner
        if isinstance(scanner, list):
            scanner = scanner[0]

        source = self.inputs[0].abspath()
        header = self.outputs[0].abspath()
        code = self.outputs[1].abspath()

        result = self.exec_command([scanner, "client-header", source, header])
        if result:
            return result

        return self.exec_command([scanner, "private-code", source, code])


@feature("wayland-protocols")
@before_method("process_source", "apply_incpaths")
def process_wayland_protocols(tgen):
    """
    Add a scanner task per protocol, and put what it generates on the target's source and includes.

    Reads three optional attributes from the task generator:

      wayland_protocol_dir       directory of XML files to generate, relative to the wscript.
                                 Defaults to "deps". These are the protocols that do not ship with
                                 wayland-protocols and therefore have to travel with the source.

      wayland_system_protocols   paths of XML files relative to the wayland-protocols package's own
                                 data directory, which pkg-config reported at configure time.

      wayland_output_dir         where to put the generated files, relative to the wscript's build
                                 directory. Defaults to "." alongside everything else.

    The generated header's directory goes on both includes and export_includes: the first so this
    target compiles, the second because a target that uses this one is entitled to include the
    protocol headers too.
    """
    scanner = tgen.env.WAYLAND_SCANNER_wayland_scanner
    if not scanner:
        tgen.bld.fatal("wayland-protocols: wayland-scanner was not found at configure time")

    sources = []

    protocol_dir = getattr(tgen, "wayland_protocol_dir", "deps")
    protocol_node = tgen.path.find_dir(protocol_dir)
    if protocol_node:
        sources += protocol_node.ant_glob("*.xml")

    system_dir = tgen.env.WAYLAND_PROTOCOLS_pkgdatadir
    for relative in Utils.to_list(getattr(tgen, "wayland_system_protocols", [])):
        if not system_dir:
            tgen.bld.fatal(
                "wayland-protocols: %s was asked for, but wayland-protocols was not found at "
                "configure time" % relative
            )

        node = tgen.bld.root.find_node(os.path.join(system_dir, relative))
        if node is None:
            tgen.bld.fatal(
                "wayland-protocols: %s is not in %s" % (relative, system_dir)
            )
        sources.append(node)

    if not sources:
        tgen.bld.fatal("wayland-protocols: no protocol XML found to generate")

    output_dir = tgen.path.get_bld().find_or_declare(getattr(tgen, "wayland_output_dir", "."))

    tgen.includes = Utils.to_list(getattr(tgen, "includes", []))
    tgen.includes.append(output_dir)
    tgen.export_includes = Utils.to_list(getattr(tgen, "export_includes", []))
    tgen.export_includes.append(output_dir)

    tgen.source = Utils.to_list(getattr(tgen, "source", []))

    for node in sources:
        stem = node.name[: -len(".xml")]
        header = output_dir.find_or_declare("%s-client-protocol.h" % stem)
        code = output_dir.find_or_declare("%s-client-protocol.c" % stem)

        Logs.debug("wayland-protocols: %s -> %s", node.abspath(), header.abspath())

        tgen.create_task("wayland_scanner_protocol", src=node, tgt=[header, code])

        # The interface tables are C, and they have to stay C: they reference
        # wl_surface_interface and friends, which libwayland-client exports with C linkage, so
        # compiling them as C++ would mangle the references and fail to link. The target carries
        # the "c" feature for exactly this.
        tgen.source.append(code)
