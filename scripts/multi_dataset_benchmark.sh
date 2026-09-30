#!/bin/bash
set -uo pipefail


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

[[ -f "$BASE_DIR/tools/env.sh" ]] && source "$BASE_DIR/tools/env.sh"

RUNS="${1:-6}"
TOP_N="${2:-6}"

SMALL_FILE="$BASE_DIR/data/big.bin"
BIG_FILE="$BASE_DIR/data/bg_big.bin"
SMALL_TAG="big"
BIG_TAG="bg_big"

for f in "$SMALL_FILE" "$BIG_FILE"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: data file not found: $f" >&2
        exit 1
    fi
done
if ! command -v jq &>/dev/null; then
    echo "ERROR: jq not found on PATH (needed to rank/aggregate versions)." >&2
    exit 1
fi


echo "================================================================================" >&2
echo "  PASS 1/2: $SMALL_TAG ($SMALL_FILE), RUNS=$RUNS, all active versions" >&2
echo "================================================================================" >&2
RUNS="$RUNS" DATA_FILE="$SMALL_FILE" "$SCRIPT_DIR/benchmark.sh"


BIG_HISTORY_JSON="$BASE_DIR/reports/json/benchmark_report_latest_${BIG_TAG}.json"
SELECTED_BIG_VERSIONS=()

if [[ -f "$BIG_HISTORY_JSON" ]]; then
    echo "" >&2
    echo "Found prior $BIG_TAG history ($BIG_HISTORY_JSON) - selecting top $TOP_N" >&2
    mapfile -t SELECTED_BIG_VERSIONS < <(jq -r '
        .version_results
        | to_entries
        | map({version: .key, avg: (.value.producer_statistics.average_time_seconds + .value.consumer_statistics.average_time_seconds)})
        | sort_by(.avg)
        | .[].version
    ' "$BIG_HISTORY_JSON" | head -n "$TOP_N")
    echo "  Selected: ${SELECTED_BIG_VERSIONS[*]}" >&2
else
    echo "" >&2
    echo "No prior $BIG_TAG history found - bootstrapping with all active versions" >&2
    echo "  (subsequent runs will filter to the top $TOP_N from this run's results)" >&2
fi


echo "" >&2
echo "================================================================================" >&2
echo "  PASS 2/2: $BIG_TAG ($BIG_FILE), RUNS=$RUNS" >&2
echo "================================================================================" >&2
if [[ ${#SELECTED_BIG_VERSIONS[@]} -gt 0 ]]; then
    RUNS="$RUNS" DATA_FILE="$BIG_FILE" "$SCRIPT_DIR/benchmark.sh" "${SELECTED_BIG_VERSIONS[@]}"
else
    RUNS="$RUNS" DATA_FILE="$BIG_FILE" "$SCRIPT_DIR/benchmark.sh"
fi


SMALL_JSON="$BASE_DIR/reports/json/benchmark_report_latest_${SMALL_TAG}.json"
BIG_JSON="$BASE_DIR/reports/json/benchmark_report_latest_${BIG_TAG}.json"
TIMESTAMP="$(date '+%Y-%m-%d_%H-%M-%S')"
COMBINED_TXT="$BASE_DIR/reports/combined_report_${TIMESTAMP}.txt"
COMBINED_JSON="$BASE_DIR/reports/combined_report_${TIMESTAMP}.json"

VENV_PYTHON="$SCRIPT_DIR/venv/bin/python3"
PYTHON="$VENV_PYTHON"
[[ -x "$PYTHON" ]] || PYTHON="python3"

echo "" >&2
echo "Aggregating combined report..." >&2
"$PYTHON" "$SCRIPT_DIR/generate_combined_report.py" \
    "$SMALL_JSON" "$BIG_JSON" "$COMBINED_TXT" "$COMBINED_JSON" "$SMALL_TAG" "$BIG_TAG" >&2
cat "$COMBINED_TXT" >&2


echo "" >&2
echo "================================================================================" >&2
echo "  Auto deep-profiling top 4 (+ standout single-run/median) on $BIG_TAG" >&2
echo "================================================================================" >&2
"$SCRIPT_DIR/deep_profile.sh" --data-file "$BIG_FILE" 4

echo "" >&2
echo "Multi-dataset benchmark complete." >&2
echo "  $SMALL_TAG report:  $SMALL_JSON" >&2
echo "  $BIG_TAG report:    $BIG_JSON" >&2
echo "  Combined report:    $COMBINED_TXT" >&2
