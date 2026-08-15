#!/bin/bash
#
# Reports how far the two libraries' *comments* have drifted apart.
#
# compare-sources.sh strips comments and compares the code; this is the other half. The doxygen a
# caller reads -- purpose, usage, constraints, @param and @return -- describes an API the two
# libraries share exactly, so it has to say the same thing on both sides. Anything else is a
# difference a reader would have to reconcile by hand.
#
# Two categories of comment, and only one of them is compared:
#
#   Category 1  //! doxygen. The contract. Must be identical in both libraries.
#   Category 2  //  plain comments. Development history, measurements, defect write-ups. These
#               live in QtLikeSignal only, are invisible to doxygen, and are NOT compared.
#
# So this extracts the //! lines, folds the library name and header extension the way
# compare-sources.sh does, and counts what is left.
#
# Usage: tools/compare-comments.sh          -- one line per file pair, zero means identical
#        tools/compare-comments.sh -v STEM  -- show the differences for one stem
#
# Exit status: 0 if every pair is identical, 1 otherwise.

# QtMimic carries a copyright line in its @file block and QtLikeSignal carries an SPDX tag instead,
# so that line and the '//!' separator above it are dropped before comparing. Everything else in a
# '//!' comment is contract and is compared.
doc() {
  perl -0777 -pe 's{^[ \t]*//!\s*\n[ \t]*//!.*Copyright.*\n}{}mg; s{^[ \t]*//!.*Copyright.*\n}{}mg' "$1" \
    | grep -E '^[[:space:]]*//!' \
    | sed -e 's/QtLikeSignal/LIB/g' -e 's/QtMimic/LIB/g' \
          -e 's/\.hpp/.h/g' \
          -e 's/^[[:space:]]*//' -e 's/[[:space:]]\+$//'
}

STEMS="AbstractEventDispatcher Connection CoreApplication Event EventDispatcherDefault
EventDispatcherLinux EventDispatcherWin32 Global Object Signal Thread ThreadData Timer
ThreadPosix ThreadWin"

VERBOSE_STEM="${2:-}"
STATUS=0

for stem in $STEMS; do
  for ext in hpp cpp; do
    a="src/QtLikeSignal/$stem.$ext"
    b="external/QtMimic/src/QtMimic/$stem.$ext"
    [ -f "$a" ] && [ -f "$b" ] || continue

    n=$( diff <( doc "$a" ) <( doc "$b" ) | grep -c '^[<>]' )
    printf '%-32s %5s\n' "$stem.$ext" "$n"
    [ "$n" -gt 0 ] && STATUS=1

    if [ "${1:-}" = "-v" ] && [ "$stem" = "$VERBOSE_STEM" ]; then
      diff <( doc "$a" ) <( doc "$b" ) | sed 's/^/    /'
    fi
  done
done

exit "$STATUS"
