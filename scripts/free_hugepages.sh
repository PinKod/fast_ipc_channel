#!/bin/bash
set -uo pipefail


STALE_AGE_SECONDS=60
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --age) STALE_AGE_SECONDS="$2"; shift 2 ;;
        --age=*) STALE_AGE_SECONDS="${1#--age=}"; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

echo "=== Hugepage status (before) ==="
grep -i huge /proc/meminfo
echo

echo "=== Scanning for producer/consumer processes holding hugetlb pages (stale age: ${STALE_AGE_SECONDS}s) ==="
found_any=0
killed_any=0

for status_file in /proc/[0-9]*/status; do
    pid="$(basename "$(dirname "$status_file")")"
    [[ -r "$status_file" ]] || continue

    hugetlb_kb="$(awk '/^HugetlbPages:/ {print $2}' "$status_file" 2>/dev/null)"
    [[ -n "$hugetlb_kb" && "$hugetlb_kb" -gt 0 ]] || continue

    comm="$(cat "/proc/$pid/comm" 2>/dev/null || echo "?")"
    case "$comm" in
        producer|consumer) ;;
        *) continue ;;
    esac

    etimes="$(ps -o etimes= -p "$pid" 2>/dev/null | tr -d ' ')"
    [[ -n "$etimes" ]] || continue

    exe_path="$(readlink "/proc/$pid/exe" 2>/dev/null || echo "?")"
    found_any=1
    printf 'PID %-7s %-9s %6s kB hugetlb  running %5ss  %s\n' \
        "$pid" "$comm" "$hugetlb_kb" "$etimes" "$exe_path"

    if [[ "$etimes" -lt "$STALE_AGE_SECONDS" ]]; then
        echo "  -> within ${STALE_AGE_SECONDS}s grace period, leaving it alone"
        continue
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "  -> stale (>= ${STALE_AGE_SECONDS}s), would kill (--dry-run, not killing)"
        continue
    fi

    echo "  -> stale (>= ${STALE_AGE_SECONDS}s), killing"
    if kill -KILL "$pid" 2>/dev/null; then
        killed_any=1
    else
        echo "  -> kill failed - owned by another user? try with sudo"
    fi
done

[[ "$found_any" -eq 0 ]] && echo "(none found)"
echo

if [[ "$killed_any" -eq 1 ]]; then
    sleep 0.2
fi

echo "=== Hugepage status (after) ==="
grep -i huge /proc/meminfo
