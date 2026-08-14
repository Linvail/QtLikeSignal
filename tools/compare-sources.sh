#!/bin/bash
#
# Reports how far QtLikeSignal and QtMimic have drifted apart in *code*.
#
# The two libraries are meant to be identical except for comments, the namespace, the header
# extension, and the copyright line. This strips those four away and counts what is left: every
# remaining differing line is a real divergence in an identifier, a signature, or a statement.
#
# Usage: tools/compare-sources.sh          -- one line per file pair, zero means identical
#        tools/compare-sources.sh -v FILE  -- show the differences for one stem
#
# Zero everywhere is the goal. A pair that is not zero is either work in progress or a bug in one
# of the two, since a behaviour that matters in one library matters in the other.
# Strips /* */ blocks first -- one library still uses them for doxygen and the other does not, so
# leaving them in counts a comment-style difference as a code difference -- then // and //! lines,
# then the four legal differences, then blank lines.
norm() {
  perl -0777 -pe 's{/\*.*?\*/}{}gs' "$1" \
    | sed -e 's|//!.*||' -e 's|//.*||' \
    | sed -e 's/QtLikeSignal/LIB/g' -e 's/QtMimic/LIB/g' \
          -e 's/QT_LIKE_SIGNAL_/LIBGUARD_/g' -e 's/QT_MIMIC_/LIBGUARD_/g' \
          -e 's/\.hpp/.h/g' -e 's/_HPP/_H/g' -e 's/_HPP/_H/g' \
          -e 's/[[:space:]]\+$//' \
    | grep -v '^[[:space:]]*$'
}
for stem in AbstractEventDispatcher Connection CoreApplication Event EventDispatcherDefault EventDispatcherLinux EventDispatcherWin32 Global Object Signal Thread ThreadData Timer; do
  for ext_q in h hpp; do
    a="src/$stem.$ext_q"; [ -f "$a" ] && break
  done
  b="external/QtMimic/src/$stem.hpp"
  if [ -f "$a" ] && [ -f "$b" ]; then
    n=$(diff <(norm "$a") <(norm "$b") | grep -c '^[<>]')
    printf '%-28s %5s\n' "$stem.h" "$n"
  fi
  a="src/$stem.cpp"; b="external/QtMimic/src/$stem.cpp"
  if [ -f "$a" ] && [ -f "$b" ]; then
    n=$(diff <(norm "$a") <(norm "$b") | grep -c '^[<>]')
    printf '%-28s %5s\n' "$stem.cpp" "$n"
  fi
done
for stem in ThreadPosix ThreadWin; do
  a="src/$stem.cpp"; b="external/QtMimic/src/$stem.cpp"
  n=$(diff <(norm "$a") <(norm "$b") | grep -c '^[<>]')
  printf '%-28s %5s\n' "$stem.cpp" "$n"
done
