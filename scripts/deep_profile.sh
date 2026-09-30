#!/bin/bash
set -uo pipefail


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$BASE_DIR/src"

[[ -f "$BASE_DIR/tools/env.sh" ]] && source "$BASE_DIR/tools/env.sh"

DATA_FILE="$BASE_DIR/data/big_output.bin"
SKIP_MASSIF=0
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --data-file)
            DATA_FILE="$2"
            [[ "$DATA_FILE" != /* ]] && DATA_FILE="$BASE_DIR/$DATA_FILE"
            shift 2
            ;;
        --data-file=*)
            DATA_FILE="${1#--data-file=}"
            [[ "$DATA_FILE" != /* ]] && DATA_FILE="$BASE_DIR/$DATA_FILE"
            shift
            ;;
        --skip-massif)
            SKIP_MASSIF=1
            shift
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done
DATA_TAG="$(basename "$DATA_FILE" | sed 's/\.[^.]*$//')"

N="${ARGS[0]:-4}"
[[ ${#ARGS[@]} -ge 1 ]] && ARGS=("${ARGS[@]:1}")
EXPLICIT_VERSIONS=("${ARGS[@]}")


PARANOID="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 999)"
if [[ "$PARANOID" -gt 1 ]]; then
    echo "ERROR: kernel.perf_event_paranoid=$PARANOID blocks perf for non-root users." >&2
    echo "  Fix: sudo sysctl -w kernel.perf_event_paranoid=-1" >&2
    exit 1
fi
if ! command -v perf &>/dev/null; then
    echo "ERROR: perf not found on PATH." >&2
    exit 1
fi
if ! command -v valgrind &>/dev/null; then
    echo "ERROR: valgrind not found on PATH (needed for heap-fragmentation profiling)." >&2
    exit 1
fi
if ! command -v jq &>/dev/null; then
    echo "ERROR: jq not found on PATH (needed to rank versions from the JSON report)." >&2
    exit 1
fi


VERSIONS=()
if [[ ${#EXPLICIT_VERSIONS[@]} -gt 0 ]]; then
    VERSIONS=("${EXPLICIT_VERSIONS[@]}")
    echo "Using explicit versions: ${VERSIONS[*]}" >&2
else
    REPORT_JSON="$BASE_DIR/reports/json/benchmark_report_latest_${DATA_TAG}.json"
    if [[ ! -f "$REPORT_JSON" && "$DATA_TAG" == "big_output" ]]; then
        REPORT_JSON="$BASE_DIR/reports/json/benchmark_report_latest.json"
    fi
    if [[ ! -f "$REPORT_JSON" ]]; then
        echo "ERROR: $REPORT_JSON not found - run a benchmark against this data file first" >&2
        echo "  (e.g. 'DATA_FILE=$DATA_FILE just full' or 'just multi-bench')." >&2
        exit 1
    fi

    mapfile -t RANKED < <(jq -r '
        .version_results
        | to_entries
        | map({version: .key, avg: (.value.producer_statistics.average_time_seconds + .value.consumer_statistics.average_time_seconds)})
        | sort_by(.avg)
        | .[]
        | "\(.version) \(.avg)"
    ' "$REPORT_JSON")

    if [[ ${#RANKED[@]} -eq 0 ]]; then
        echo "ERROR: no versions found in $REPORT_JSON" >&2
        exit 1
    fi

    TOTAL=${#RANKED[@]}
    TAKE=$N
    [[ "$TOTAL" -le "$N" ]] && TAKE=$TOTAL

    echo "Ranked versions (fastest first, from $REPORT_JSON):" >&2
    for line in "${RANKED[@]}"; do
        printf "  %-10s avg %ss\n" "$(awk '{print $1}' <<< "$line")" "$(awk '{print $2}' <<< "$line")" >&2
    done
    echo "" >&2
    echo "Selecting top $TAKE of $TOTAL:" >&2

    for (( i=0; i<TAKE; i++ )); do
        v="$(awk '{print $1}' <<< "${RANKED[$i]}")"
        a="$(awk '{print $2}' <<< "${RANKED[$i]}")"
        VERSIONS+=("$v")
        echo "  $v (avg ${a}s)" >&2
    done

    contains_version() {
        local needle="$1"; shift
        local x
        for x in "$@"; do [[ "$x" == "$needle" ]] && return 0; done
        return 1
    }

    read -r FASTEST_SINGLE_VERSION FASTEST_SINGLE_TIME <<< "$(jq -r '
        .version_results
        | to_entries
        | map(
            {version: .key, t: .value.producer_statistics.minimum_time_seconds},
            {version: .key, t: .value.consumer_statistics.minimum_time_seconds}
          )
        | sort_by(.t)
        | .[0]
        | "\(.version) \(.t)"
    ' "$REPORT_JSON")"

    if [[ -n "$FASTEST_SINGLE_VERSION" ]] && ! contains_version "$FASTEST_SINGLE_VERSION" "${VERSIONS[@]}"; then
        VERSIONS+=("$FASTEST_SINGLE_VERSION")
        echo "  + $FASTEST_SINGLE_VERSION (fastest single run, ${FASTEST_SINGLE_TIME}s, not in top $TAKE by avg)" >&2
    fi

    read -r FASTEST_MEDIAN_VERSION FASTEST_MEDIAN_TIME <<< "$(jq -r '
        .version_results
        | to_entries
        | map({version: .key, med: (.value.producer_statistics.median_time_seconds + .value.consumer_statistics.median_time_seconds)})
        | sort_by(.med)
        | .[0]
        | "\(.version) \(.med)"
    ' "$REPORT_JSON")"

    if [[ -n "$FASTEST_MEDIAN_VERSION" ]] && ! contains_version "$FASTEST_MEDIAN_VERSION" "${VERSIONS[@]}"; then
        VERSIONS+=("$FASTEST_MEDIAN_VERSION")
        echo "  + $FASTEST_MEDIAN_VERSION (fastest by median, ${FASTEST_MEDIAN_TIME}s, not already included)" >&2
    fi
fi

for v in "${VERSIONS[@]}"; do
    if [[ ! -f "$SRC_DIR/$v/build/producer" || ! -f "$SRC_DIR/$v/build/consumer" ]]; then
        echo "ERROR: $v is not built (expected $SRC_DIR/$v/build/{producer,consumer})" >&2
        exit 1
    fi
done

RUNNER="$SRC_DIR/runner/build/runner"
if [[ ! -f "$RUNNER" ]]; then
    echo "ERROR: runner not built at $RUNNER" >&2
    exit 1
fi

if [[ ! -f "$DATA_FILE" ]]; then
    echo "ERROR: data file not found at $DATA_FILE" >&2
    exit 1
fi
CORES="0,4"


TIMESTAMP="$(date '+%Y-%m-%d_%H-%M-%S')"
OUTDIR="$BASE_DIR/reports/deep_profile/${DATA_TAG}_${TIMESTAMP}"
mkdir -p "$OUTDIR"

PREFIXES_CONF="$SCRIPT_DIR/prefixes.conf"
declare -A PREFIX_CMDS=()
while IFS='|' read -r name cmd || [[ -n "$name" ]]; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    PREFIX_CMDS["$name"]="$cmd"
done < "$PREFIXES_CONF"

resolve_prefix() {
    local template="$1" version="$2" role="$3"
    template="${template//\{VERSION\}/$version}"
    template="${template//\{ROLE\}/$role}"
    template="${template//\{OUTDIR\}/$OUTDIR}"
    printf '%s' "$template"
}

run_wrapped() {
    local prefix_name="$1" version="$2" label="$3"
    local producer="$SRC_DIR/$version/build/producer"
    local consumer="$SRC_DIR/$version/build/consumer"
    local template="${PREFIX_CMDS[$prefix_name]:-}"
    if [[ -z "$template" ]]; then
        echo "    ERROR: prefix '$prefix_name' not found in $PREFIXES_CONF" >&2
        return 1
    fi

    local prod_cmd cons_cmd
    prod_cmd="$(resolve_prefix "$template" "$version" "producer")"
    cons_cmd="$(resolve_prefix "$template" "$version" "consumer")"
    local -a prod_arr=() cons_arr=()
    read -ra prod_arr <<< "$prod_cmd"
    read -ra cons_arr <<< "$cons_cmd"

    echo "  [$version] $label ..." >&2
    local run_log="$OUTDIR/.run_${version}_${prefix_name}.log"
    if ! "$RUNNER" "$DATA_FILE" "${prod_arr[@]}" "$producer" "${cons_arr[@]}" "$consumer" "$CORES" "deep_${version}_${prefix_name}_$$" \
        &>"$run_log"; then
        echo "    WARNING: run failed (see $run_log)" >&2
    fi
}


for v in "${VERSIONS[@]}"; do
    echo "" >&2
    echo "=== Deep profiling $v ===" >&2

    run_wrapped "perf-record" "$v" "cycles flame graph"
    run_wrapped "perf-record-cs" "$v" "context-switch flame graph (where switches happen)"
    run_wrapped "perf-record-tlb" "$v" "TLB-miss flame graph (where misses happen)"

    for rep in 1 2 3; do
        run_wrapped "perf-stat-full" "$v" "perf stat (rep $rep/3)"
        for role in producer consumer; do
            src_csv="$OUTDIR/perf_stat_${v}_${role}.csv"
            dst_csv="$OUTDIR/perf_stat_${v}_${role}_rep${rep}.csv"
            [[ -f "$src_csv" ]] && mv "$src_csv" "$dst_csv"
        done
    done

    if [[ "$SKIP_MASSIF" -eq 1 ]]; then
        echo "  (skipping heap fragmentation (massif) - --skip-massif)" >&2
    else
        run_wrapped "valgrind-massif" "$v" "heap fragmentation (massif)"
    fi
done


echo "" >&2
echo "Rendering report..." >&2
"$SCRIPT_DIR/perf_report.sh" "$OUTDIR"

echo "" >&2
echo "Deep profile complete." >&2
echo "  Versions profiled: ${VERSIONS[*]}" >&2
echo "  Dashboard:         $OUTDIR/perf_report/perf_index.html" >&2
