#!/bin/sh
#
# Reports how far each QtLikeSignal test file has drifted from its QtMimic counterpart.
#
# The two suites are meant to hold the same tests in the same order under the same names, in
# separate files (see history/TEST-UNIFICATION-PLAN-20260810.md). Nothing in the build enforces
# that, so this measures it: it normalises the differences that are legal by design -- the library
# name, the header extension -- and counts the lines that are left.
#
# Each pair carries a budget rather than a target of zero, because some divergence is legitimate:
# a test guarded by a feature macro for an API only one side has, and the file-header prose, will
# never match. Ratchet a budget down as work lands; never up without a comment saying why.
#
# Usage:  tools/compare-test-suites.sh [-v]
#           -v  also print the differing lines for every pair over budget
#
# Exit status: 0 if every pair is within budget, 1 otherwise.

set -eu

ROOT=$( cd "$( dirname "$0" )/.." && pwd )
QLS="$ROOT/src/tests"
MIMIC="$ROOT/external/QtMimic/tests"

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

# stem : budget -- the number of differing lines this pair is currently allowed.
#
# These are ratchets, not targets. Each is set a little above where that pair actually stands
# today, so the check is green now and goes red the moment a pair drifts further apart. Lower a
# budget whenever a phase of the plan brings a pair closer; timer and stress show what the far end
# looks like, where all that is left is the file header and the include block.
PAIRS="
timer:25
stress:20
object:1300
thread:450
coreapplication:450
thread-priority:25
"

# Deliberately NOT compared: defect-regressions. A regression suite is a record of what actually
# broke in that codebase, so the two are supposed to differ -- QtMimic never had QtLikeSignal's
# dispatcher bugs and QtLikeSignal never had QtMimic's. Tracking them here would imply a
# convergence that should not happen. See the plan's category C.

# Collapses the differences that are legal by design, so what remains is real drift.
normalise()
{
    sed -e 's/QtLikeSignal/LIB/g' \
        -e 's/QtMimic/LIB/g' \
        -e 's/LIB-test-types\.hpp/LIB-test-types.h/' \
        -e 's/\.hpp"/.h"/' \
        "$1"
}

TMP=$( mktemp -d )
trap 'rm -rf "$TMP"' EXIT

STATUS=0
printf '%-24s %10s %10s   %s\n' "PAIR" "DIFFERING" "BUDGET" "RESULT"
printf '%s\n' "----------------------------------------------------------------"

for entry in $PAIRS; do
    stem=${entry%%:*}
    budget=${entry##*:}

    a="$QLS/QtLikeSignal-test-$stem.cpp"
    b="$MIMIC/QtMimic-test-$stem.cpp"

    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        printf '%-24s %10s %10s   %s\n' "$stem" "-" "$budget" "MISSING one side"
        STATUS=1
        continue
    fi

    normalise "$a" > "$TMP/a"
    normalise "$b" > "$TMP/b"
    n=$( diff "$TMP/a" "$TMP/b" | grep -c '^[<>]' || true )

    if [ "$n" -le "$budget" ]; then
        printf '%-24s %10s %10s   %s\n' "$stem" "$n" "$budget" "ok"
    else
        printf '%-24s %10s %10s   %s\n' "$stem" "$n" "$budget" "OVER BUDGET"
        STATUS=1
        if [ "$VERBOSE" -eq 1 ]; then
            diff "$TMP/a" "$TMP/b" | sed 's/^/    /'
        fi
    fi
done

printf '%s\n' "----------------------------------------------------------------"
if [ "$STATUS" -eq 0 ]; then
    echo "All pairs within budget."
else
    echo "At least one pair has drifted. Re-run with -v to see the differences."
fi

exit "$STATUS"
