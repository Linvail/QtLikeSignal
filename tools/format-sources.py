#!/usr/bin/env python3
"""Formats every C++ source in the repository with uncrustify.cfg.

The point of a script rather than a shell one-liner is the check it runs first. uncrustify does
**not** reject an option it has never heard of: it exits 0, says nothing, and formats as though the
line were absent. uncrustify.cfg uses two options that are not upstream --
`pp_indent_namespace` and `pp_indent_class`, added by a local patch -- so an older binary silently
produces different output and looks like it worked. This script reads the option names out of the
config, asks the binary which ones it knows, and refuses to run if any are missing.

HOW TO RUN IT. Say `python` explicitly unless you are in WSL:

    Windows PowerShell   python tools\\format-sources.py
    Git Bash             python tools/format-sources.py
    WSL                  ./tools/format-sources.py      (or python3 tools/format-sources.py)

Only WSL can use the shebang, because that is the only one of the three with `python3` on PATH.
PowerShell given a bare `tools/format-sources.py` does not run it at all -- it hands the file to
the .py association, which opens it in an editor and looks exactly like nothing happening.

Options:

    --check              report what would change, exit 1 if anything does; write nothing
    --diff               print the diffs; write nothing
    --uncrustify PATH    use a particular binary
    <paths>              limit the run to some files or directories

--check is the one to put in CI, and the one to run before committing a config change: it says which
files a reformat would touch without touching them.

Works from either side of WSL. On Windows, if uncrustify is not on PATH it falls back to `wsl
uncrustify` and translates the paths, since the patched build lives in the WSL filesystem.
"""

import argparse
import difflib
import os
import re
import subprocess
import sys
import tempfile

# Where sources live. submodules/ (googletest, waf), out/ and install/ are deliberately absent:
# they are not ours to format.
SOURCE_ROOTS = ["src"]
SOURCE_SUFFIXES = (".cpp", ".hpp")

CONFIG = "uncrustify.cfg"

# An option assignment in uncrustify.cfg: 'name = value', ignoring comments and blank lines.
RE_CFG_OPTION = re.compile(r"^\s*([a-z_0-9]+)\s*=")

# A line of `uncrustify --show-config`, which lists every option the binary knows.
RE_KNOWN_OPTION = re.compile(r"^([a-z_0-9]+)\s*=")


def repo_root():
    """The repository root, derived from this file's location rather than the caller's cwd."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def to_tool_path(path, via_wsl):
    """Translates a path for the binary. Only WSL-from-Windows needs anything done to it."""
    if not via_wsl:
        return path
    drive, rest = os.path.splitdrive(os.path.abspath(path))
    return "/mnt/" + drive[0].lower() + rest.replace("\\", "/")


def find_uncrustify(explicit):
    """Locates the binary.

    @return (argv prefix, via_wsl), or (None, False) if there is none to be found.
    """
    if explicit:
        return ([explicit], False)

    from shutil import which
    found = which("uncrustify")
    if found:
        return ([found], False)

    # The patched build lives in the WSL filesystem, so a Windows-side run borrows it.
    if os.name == "nt" and which("wsl"):
        probe = subprocess.run(["wsl", "uncrustify", "--version"],
                               capture_output=True, text=True)
        if probe.returncode == 0:
            return (["wsl", "uncrustify"], True)

    return (None, False)


def run(argv, **kwargs):
    """Runs a command and returns its CompletedProcess, capturing both streams as text.

    A missing binary comes back as a returncode of 127 rather than an exception, so every caller
    can treat "could not run it" and "it failed" the same way.
    """
    try:
        return subprocess.run(argv, capture_output=True, text=True, **kwargs)
    except (FileNotFoundError, NotADirectoryError, PermissionError) as error:
        return subprocess.CompletedProcess(argv, 127, "", str(error))


def fail(message):
    """Writes an error to stderr, with stdout flushed first so the two stay in order."""
    sys.stdout.flush()
    print(message, file=sys.stderr)
    sys.stderr.flush()


def check_options_are_known(uncr, cfg_path, via_wsl):
    """Fails loudly if uncrustify.cfg names an option this binary does not have.

    This is the whole reason the script exists; see the module docstring. Returns a list of unknown
    option names, empty when all is well.
    """
    with open(cfg_path, encoding="utf-8") as handle:
        wanted = []
        for line in handle:
            if line.lstrip().startswith("#"):
                continue
            match = RE_CFG_OPTION.match(line)
            if match:
                wanted.append(match.group(1))

    shown = run(uncr + ["--show-config"])
    if shown.returncode != 0:
        fail("error: could not run '{}':\n       {}".format(
            " ".join(uncr + ["--show-config"]), shown.stderr.strip()))
        sys.exit(2)

    known = set()
    for line in shown.stdout.splitlines():
        match = RE_KNOWN_OPTION.match(line)
        if match:
            known.add(match.group(1))

    return [name for name in dict.fromkeys(wanted) if name not in known]


def gather(roots, limits, root):
    """Every source file under @p roots, or under @p limits when the caller named some."""
    bases = limits if limits else roots
    out = []
    for base in bases:
        full = base if os.path.isabs(base) else os.path.join(root, base)
        if os.path.isfile(full):
            out.append(full)
            continue
        for dirpath, dirnames, filenames in os.walk(full):
            dirnames[:] = [d for d in dirnames if d not in (".git", "out", "install", "submodules")]
            for name in sorted(filenames):
                if name.endswith(SOURCE_SUFFIXES):
                    out.append(os.path.join(dirpath, name))
    return sorted(set(out))


def format_one(uncr, cfg, path, via_wsl):
    """Formats one file, without writing it.

    @return the formatted text, or None if uncrustify failed.
    """
    handle, tmp = tempfile.mkstemp(suffix=".unc")
    os.close(handle)
    try:
        result = run(uncr + ["-c", to_tool_path(cfg, via_wsl),
                             "-f", to_tool_path(path, via_wsl),
                             "-o", to_tool_path(tmp, via_wsl),
                             "-l", "CPP", "-q"])
        if result.returncode != 0:
            fail("error: uncrustify failed on {}:\n       {}".format(
                path, result.stderr.strip()))
            return None
        with open(tmp, "rb") as produced:
            return produced.read()
    finally:
        os.unlink(tmp)


def main():
    parser = argparse.ArgumentParser(
        description="Format the repository's C++ sources with uncrustify.cfg.")
    parser.add_argument("paths", nargs="*",
                        help="files or directories to limit the run to; default is every source")
    parser.add_argument("--check", action="store_true",
                        help="report what would change and exit 1 if anything would; write nothing")
    parser.add_argument("--diff", action="store_true",
                        help="print a unified diff per file that would change; write nothing")
    parser.add_argument("--uncrustify", metavar="PATH",
                        help="binary to use, instead of the one on PATH")
    args = parser.parse_args()

    root = repo_root()
    cfg = os.path.join(root, CONFIG)
    if not os.path.isfile(cfg):
        fail("error: {} not found".format(cfg))
        return 2

    uncr, via_wsl = find_uncrustify(args.uncrustify)
    if uncr is None:
        fail("error: uncrustify not found. Build and install it, or pass --uncrustify PATH.")
        return 2

    probe = run(uncr + ["--version"])
    if probe.returncode != 0:
        fail("error: cannot run '{}':\n       {}".format(
            " ".join(uncr), probe.stderr.strip() or "exit {}".format(probe.returncode)))
        return 2
    print("uncrustify: {} ({})".format(probe.stdout.strip(), " ".join(uncr)))

    unknown = check_options_are_known(uncr, cfg, via_wsl)
    if unknown:
        fail("\nerror: this uncrustify does not know {} option(s) that {} sets:\n{}\n"
             "\n       It would ignore them silently and format differently, so this script stops\n"
             "       rather than let that happen. pp_indent_namespace and pp_indent_class come\n"
             "       from a local patch; see the note in {} section 8.".format(
                 len(unknown), CONFIG,
                 "\n".join("         " + name for name in unknown), CONFIG))
        return 2

    files = gather(SOURCE_ROOTS, args.paths, root)
    if not files:
        print("no sources found")
        return 0

    changed = []
    for path in files:
        with open(path, "rb") as original:
            before = original.read()
        after = format_one(uncr, cfg, path, via_wsl)
        if after is None:
            return 2
        if after == before:
            continue

        changed.append(path)
        shown = os.path.relpath(path, root).replace("\\", "/")
        if args.diff:
            print("\n".join(difflib.unified_diff(
                before.decode("utf-8", "replace").splitlines(),
                after.decode("utf-8", "replace").splitlines(),
                fromfile=shown, tofile=shown + " (formatted)", lineterm="")))
        elif args.check:
            print("  would change: {}".format(shown))
        else:
            with open(path, "wb") as target:
                target.write(after)
            print("  formatted:    {}".format(shown))

    print("\n{} file(s) scanned, {} {}.".format(
        len(files), len(changed),
        "would change" if (args.check or args.diff) else "changed"))

    # Non-zero on --check so CI can gate on it. A plain run reports success even when it rewrote
    # something, because rewriting is what it was asked to do.
    return 1 if (changed and args.check) else 0


if __name__ == "__main__":
    sys.exit(main())
