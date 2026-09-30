#!/bin/bash
set -uo pipefail


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$BASE_DIR/src"

[[ -f "$BASE_DIR/tools/env.sh" ]] && source "$BASE_DIR/tools/env.sh"

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <type> <version> [core0,core1] [data_file]" >&2
    exit 1
fi

TYPE="$1"
VERSION="$2"
CORES="${3:-0,4}"
DATA_FILE="${4:-$BASE_DIR/data/small.bin}"

if [[ "$TYPE" == "Release" ]]; then
    BUILD_SUBDIR="build"
else
    BUILD_SUBDIR="build_${TYPE}"
fi

VDIR="$SRC_DIR/$VERSION"
PRODUCER="$VDIR/$BUILD_SUBDIR/producer"
CONSUMER="$VDIR/$BUILD_SUBDIR/consumer"
RUNNER="$SRC_DIR/runner/build/runner"

for f in "$PRODUCER" "$CONSUMER" "$RUNNER" "$DATA_FILE"; do
    if [[ ! -e "$f" ]]; then
        echo "ERROR: missing $f" >&2
        echo "  (build it first: ./scripts/build_all.sh --type $TYPE $VERSION)" >&2
        exit 1
    fi
done

export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1}"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

SHM_NAME="diag_$$"
TIMEOUT_S="${RUN_VARIANT_TIMEOUT:-30}"

echo "Running $VERSION ($TYPE) via runner, timeout ${TIMEOUT_S}s ..." >&2
echo "  producer: $PRODUCER" >&2
echo "  consumer: $CONSUMER" >&2

if timeout --signal=KILL "${TIMEOUT_S}s" "$RUNNER" "$DATA_FILE" "$PRODUCER" "$CONSUMER" "$CORES" "$SHM_NAME"; then
    echo "" >&2
    echo "Completed within ${TIMEOUT_S}s - no hang." >&2
else
    rc=$?
    echo "" >&2
    if [[ $rc -eq 137 ]]; then
        echo "HUNG: still running after ${TIMEOUT_S}s, killed. Surviving processes for this run:" >&2
        pgrep -af "$SHM_NAME" >&2 || true
        echo "" >&2
        echo "Live threads/state of any surviving producer/consumer (if not yet reaped):" >&2
        for p in $(pgrep -f "$VDIR/$BUILD_SUBDIR/(producer|consumer)"); do
            echo "-- pid $p --" >&2
            ps -T -p "$p" -o pid,tid,stat,wchan:20,comm 2>&1 >&2 || true
        done
    else
        echo "Exited with code $rc (sanitizer/error report above, if any)." >&2
    fi
    exit "$rc"
fi
