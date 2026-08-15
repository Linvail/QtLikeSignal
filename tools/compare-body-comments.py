#!/usr/bin/env python3
"""Reports how far the two libraries' *in-function* comments have drifted apart.

compare-sources.sh compares the code and compare-comments.sh compares the doxygen. This is the
third part: the plain `//` comments inside function bodies, which explain why a particular line
does what it does. The two libraries run the same code, so an explanation that is true of one is
true of the other, and a difference is either drift or one side knowing something the other does
not.

Three kinds of plain comment are deliberately NOT compared:

  * the SPDX header, which src/ carries and external/QtMimic does not;
  * category 2 doc blocks -- development history parked directly above a declaration, which live
    in QtLikeSignal only (see compare-comments.sh for the two categories);
  * anything inside a namespace, class or struct body but not inside a function.

What is left is the comments a maintainer reads while following the code.

Usage: tools/compare-body-comments.py          -- one line per file pair, zero means identical
       tools/compare-body-comments.py -v STEM  -- show the differences for one stem
"""

import difflib
import re
import subprocess
import sys

STEMS = """AbstractEventDispatcher Connection CoreApplication Event EventDispatcherDefault
EventDispatcherLinux EventDispatcherWin32 Global Object Signal Thread ThreadData Timer
ThreadPosix ThreadWin""".split()

OPENS_A_SCOPE = re.compile(r'\b(namespace|class|struct|enum|union)\b')


def body_comments(path):
    """Plain // comment lines that sit inside a function body, normalised."""
    try:
        text = open(path).read()
    except FileNotFoundError:
        return None

    out = []
    stack = []           # one entry per open brace: True if it is a function body
    pending = []         # tokens seen since the last ; { or }, i.e. the signature being built

    for raw in text.split('\n'):
        line = raw.strip()

        if line.startswith('//'):
            # A comment is in-function when the innermost open brace is a function body.
            if stack and stack[-1] and not line.startswith('//!'):
                if 'SPDX' not in line:
                    body = re.sub(r'^//\s?', '', line)
                    body = body.replace('QtLikeSignal', 'LIB').replace('QtMimic', 'LIB')
                    body = body.replace('.hpp', '.h')
                    out.append(body.rstrip())
            continue

        # Strip comment tails and string literals so their braces do not count.
        code = re.sub(r'//.*$', '', raw)
        code = re.sub(r'"(\\.|[^"\\])*"', '""', code)

        for ch in code:
            if ch == '{':
                sig = ' '.join(pending)
                stack.append(not OPENS_A_SCOPE.search(sig))
                pending = []
            elif ch == '}':
                if stack:
                    stack.pop()
                pending = []
            elif ch == ';':
                pending = []
        pending.append(code.strip())

    return out


def main():
    verbose = len(sys.argv) > 2 and sys.argv[1] == '-v'
    want = sys.argv[2] if verbose else None
    total = 0
    for stem in STEMS:
        for ext in ('hpp', 'cpp'):
            a = 'src/QtLikeSignal/%s.%s' % (stem, ext)
            b = 'external/QtMimic/src/QtMimic/%s.%s' % (stem, ext)
            ca, cb = body_comments(a), body_comments(b)
            if ca is None or cb is None:
                continue
            diff = [l for l in difflib.unified_diff(ca, cb, lineterm='', n=0)
                    if l[:1] in '+-' and not l.startswith(('---', '+++'))]
            print('%-32s %5d' % ('%s.%s' % (stem, ext), len(diff)))
            total += len(diff)
            if verbose and stem == want and diff:
                for l in diff:
                    print('    ' + l)
    print('%-32s %5d' % ('TOTAL', total))
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
