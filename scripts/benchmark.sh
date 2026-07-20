#!/bin/bash
set -euo pipefail

# Usage: benchmark.sh [version...]              run all (or listed) versions normally
#        benchmark.sh -p [version...]           open a menu to wrap producer/consumer
#                                                of specific versions with a prefix
#                                                command (valgrind/gdb/perf/...) taken
#                                                from prefixes.conf. Any version with a
#                                                prefix assigned runs only 1 iteration
#                                                instead of the normal RUNS, since the
#                                                measured time then includes wrapper
#                                                overhead and isn't comparable.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$BASE_DIR/src"
RUNNER_BUILD="$SRC_DIR/runner/build"
DATA_FILE="$BASE_DIR/data/big_output.bin"
RUNNER="$RUNNER_BUILD/runner"
CORES="0,2"
SHM_NAME="abc"
RUNS=10

TIMESTAMP="$(date '+%Y-%m-%d_%H-%M-%S')"
REPORTS_DIR="$BASE_DIR/reports"
REPORTS_JSON_DIR="$REPORTS_DIR/json"
REPORT_TXT="$REPORTS_DIR/benchmark_report_${TIMESTAMP}.txt"
REPORT_JSON="$REPORTS_JSON_DIR/benchmark_report_${TIMESTAMP}.json"
REPORT_OVERVIEW="$REPORTS_DIR/report_overview.txt"
PREFIX_OUTDIR="$REPORTS_DIR/prefix_output/$TIMESTAMP"

VENV_DIR="$SCRIPT_DIR/venv"
PYTHON="$VENV_DIR/bin/python3"

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT INT TERM

PREFIXES_CONF="$SCRIPT_DIR/prefixes.conf"
PREFIX_ORDER=()
declare -A PREFIX_CMDS=()
declare -A PROD_PREFIX=()
declare -A CONS_PREFIX=()

resolve_prefix() {
    local template="$1" version="$2" role="$3"
    template="${template//\{VERSION\}/$version}"
    template="${template//\{ROLE\}/$role}"
    template="${template//\{OUTDIR\}/$PREFIX_OUTDIR}"
    printf '%s' "$template"
}

detect_versions() {
    local versions=()
    for d in "$SRC_DIR"/v*/ "$SRC_DIR"/c_v*/; do
        local vname
        vname="$(basename "$d")"
        if [[ -f "$d/build/producer" && -f "$d/build/consumer" ]]; then
            versions+=("$vname")
        fi
    done
    printf '%s\n' "${versions[@]}" | sort -V
}

load_prefixes() {
    PREFIX_ORDER=("none")
    PREFIX_CMDS["none"]=""

    if [[ ! -f "$PREFIXES_CONF" ]]; then
        echo "NOTE: no prefixes.conf found at $PREFIXES_CONF - only 'none' available" >&2
        return
    fi

    while IFS='|' read -r name cmd || [[ -n "$name" ]]; do
        [[ -z "$name" || "$name" == \#* ]] && continue
        PREFIX_ORDER+=("$name")
        PREFIX_CMDS["$name"]="$cmd"
    done < "$PREFIXES_CONF"
}

run_prefix_menu() {
    local versions=("$@")

    while true; do
        echo "" >&2
        echo "Current prefix assignments:" >&2
        local any=0
        for v in "${versions[@]}"; do
            local pp="${PROD_PREFIX[$v]:-}"
            local cp="${CONS_PREFIX[$v]:-}"
            if [[ -n "$pp" || -n "$cp" ]]; then
                any=1
                printf "  %-8s producer=%-20s consumer=%s\n" "$v" "${pp:-none}" "${cp:-none}" >&2
            fi
        done
        [[ "$any" -eq 0 ]] && echo "  (none yet)" >&2

        echo "" >&2
        local action
        PS3="Choose action: "
        select action in "Assign prefix to a version" "Clear an assignment" "Done - run benchmark" "Cancel"; do
            case "$action" in
                "Assign prefix to a version")
                    local ver
                    PS3="Choose version: "
                    select ver in "${versions[@]}"; do
                        [[ -n "$ver" ]] && break
                        echo "Invalid choice" >&2
                    done

                    local role
                    PS3="Apply to which role: "
                    select role in "producer" "consumer" "both"; do
                        [[ -n "$role" ]] && break
                        echo "Invalid choice" >&2
                    done

                    local pname
                    PS3="Choose prefix: "
                    select pname in "${PREFIX_ORDER[@]}"; do
                        [[ -n "$pname" ]] && break
                        echo "Invalid choice" >&2
                    done

                    local pcmd="${PREFIX_CMDS[$pname]}"
                    if [[ "$role" == "producer" || "$role" == "both" ]]; then
                        PROD_PREFIX["$ver"]="$pcmd"
                    fi
                    if [[ "$role" == "consumer" || "$role" == "both" ]]; then
                        CONS_PREFIX["$ver"]="$pcmd"
                    fi
                    echo "Assigned '$pname' to $ver ($role)." >&2
                    break
                    ;;
                "Clear an assignment")
                    local ver
                    PS3="Choose version to clear: "
                    select ver in "${versions[@]}"; do
                        [[ -n "$ver" ]] && break
                        echo "Invalid choice" >&2
                    done
                    unset 'PROD_PREFIX[$ver]'
                    unset 'CONS_PREFIX[$ver]'
                    echo "Cleared assignments for $ver." >&2
                    break
                    ;;
                "Done - run benchmark")
                    return 0
                    ;;
                "Cancel")
                    echo "Cancelled." >&2
                    exit 0
                    ;;
                *)
                    echo "Invalid choice" >&2
                    ;;
            esac
        done
    done
}

get_file_size() {
    local file="$1"
    local size
    size=$(stat -c%s "$file" 2>/dev/null || echo 0)
    if (( size >= 1073741824 )); then
        echo "$(( size / 1073741824 )) GB"
    elif (( size >= 1048576 )); then
        echo "$(( size / 1048576 )) MB"
    elif (( size >= 1024 )); then
        echo "$(( size / 1024 )) KB"
    else
        echo "${size} bytes"
    fi
}

compute_stats() {
    local file="$1"
    awk '
    BEGIN { min=999999; max=0; sum=0; n=0 }
    {
        vals[NR] = $1
        sum += $1
        n++
        if ($1 < min) min = $1
        if ($1 > max) max = $1
    }
    END {
        if (n == 0) { print "0 0 0 0 0"; exit }
        avg = sum / n
        asort(vals)
        if (n % 2 == 1) {
            median = vals[int(n/2) + 1]
        } else {
            median = (vals[n/2] + vals[n/2 + 1]) / 2.0
        }
        printf "%.6f %.6f %.6f %.6f %d", min, max, avg, median, n
    }
    ' "$file"
}

run_version() {
    local version="$1"
    local producer="$SRC_DIR/$version/build/producer"
    local consumer="$SRC_DIR/$version/build/consumer"
    local vdir="$TMPDIR_WORK/$version"
    mkdir -p "$vdir"

    local prod_prefix cons_prefix
    prod_prefix="$(resolve_prefix "${PROD_PREFIX[$version]:-}" "$version" "producer")"
    cons_prefix="$(resolve_prefix "${CONS_PREFIX[$version]:-}" "$version" "consumer")"

    if [[ -n "$prod_prefix" || -n "$cons_prefix" ]]; then
        mkdir -p "$PREFIX_OUTDIR"
    fi

    local -a prod_prefix_arr=()
    local -a cons_prefix_arr=()
    [[ -n "$prod_prefix" ]] && read -ra prod_prefix_arr <<< "$prod_prefix"
    [[ -n "$cons_prefix" ]] && read -ra cons_prefix_arr <<< "$cons_prefix"

    local runs_this_version="$RUNS"
    if [[ -n "$prod_prefix" || -n "$cons_prefix" ]]; then
        runs_this_version=1
    fi

    for (( i=1; i<=runs_this_version; i++ )); do
        local output
        output=$("$RUNNER" "$DATA_FILE" "${prod_prefix_arr[@]}" "$producer" \
                 "${cons_prefix_arr[@]}" "$consumer" "$CORES" "$SHM_NAME" 2>&1) || true

        local prod_time cons_time identical
        prod_time=$(echo "$output" | grep -oP '\S+\s+\(Core\s+\d+\)\s+time:\s+\K[0-9]+\.[0-9]+' | head -1)
        cons_time=$(echo "$output" | grep -oP '\S+\s+\(Core\s+\d+\)\s+time:\s+\K[0-9]+\.[0-9]+' | tail -1)

        if echo "$output" | grep -q "Files are IDENTICAL"; then
            identical="1"
        else
            identical="0"
        fi

        echo "$prod_time" >> "$vdir/producer_times.txt"
        echo "$cons_time" >> "$vdir/cons_times.txt"
        echo "$identical" >> "$vdir/identical.txt"

        if [[ "$identical" == "0" ]]; then
            local elements mad medianad
            elements=$(echo "$output" | grep -oP 'Elements compared:\s+\K[0-9]+' || echo "0")
            mad=$(echo "$output" | grep -oP 'Mean Absolute Deviation:\s+\K[0-9eE.+-]+' || echo "0")
            medianad=$(echo "$output" | grep -oP 'Median Absolute Deviation:\s+\K[0-9eE.+-]+' || echo "0")
            echo "${elements}|${mad}|${medianad}" > "$vdir/last_diff_metrics.txt"
        fi

        printf "\r  [%s] Run %d/%d done" "$version" "$i" "$runs_this_version" >&2
    done
    printf "\r  [%s] All %d runs complete   \n" "$version" "$runs_this_version" >&2
}

print_separator() {
    printf '%0.s=' {1..80}
    printf '\n'
}

print_table_row() {
    local ver="$1" comp="$2" min="$3" max="$4" avg="$5" med="$6" ident="$7"
    printf "%-10s | %-9s | %8s | %8s | %8s | %9s | %s\n" \
        "$ver" "$comp" "$min" "$max" "$avg" "$med" "$ident"
}

print_overview() {
    local versions=("$@")
    local -a ver_names=()
    local -a ver_pavg=()
    local -a ver_cavg=()
    local -a ver_pmin=()
    local -a ver_cmin=()
    local -a ver_pmed=()
    local -a ver_cmed=()

    for ver in "${versions[@]}"; do
        local vdir="$TMPDIR_WORK/$ver"
        local stats
        stats=$(compute_stats "$vdir/producer_times.txt")
        read -r pmin _ pavg pmed _ <<< "$stats"
        stats=$(compute_stats "$vdir/cons_times.txt")
        read -r cmin _ cavg cmed _ <<< "$stats"

        ver_names+=("$ver")
        ver_pavg+=("$pavg")
        ver_cavg+=("$cavg")
        ver_pmin+=("$pmin")
        ver_cmin+=("$cmin")
        ver_pmed+=("$pmed")
        ver_cmed+=("$cmed")
    done

    local n=${#versions[@]}

    local fastest_avg_ver slowest_avg_ver fastest_med_ver
    fastest_avg_ver=$(awk -v n="$n" '
        BEGIN { best=999999; worst=0; bv=""; sv="" }
        {
            combined = $1 + $2
            name = $3
            if (combined < best) { best = combined; bv = name }
            if (combined > worst) { worst = combined; sv = name }
        }
        END { print bv }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pavg[$i]}" "${ver_cavg[$i]}" "${ver_names[$i]}"
    done))

    slowest_avg_ver=$(awk '
        BEGIN { worst=0; sv="" }
        {
            combined = $1 + $2
            name = $3
            if (combined > worst) { worst = combined; sv = name }
        }
        END { print sv }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pavg[$i]}" "${ver_cavg[$i]}" "${ver_names[$i]}"
    done))

    fastest_med_ver=$(awk '
        BEGIN { best=999999; bv="" }
        {
            combined = $1 + $2
            name = $3
            if (combined < best) { best = combined; bv = name }
        }
        END { print bv }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pmed[$i]}" "${ver_cmed[$i]}" "${ver_names[$i]}"
    done))

    local fastest_single_ver fastest_single_comp fastest_single_time
    fastest_single_time=999999
    for (( i=0; i<n; i++ )); do
        local cur_pmin="${ver_pmin[$i]}"
        local cur_cmin="${ver_cmin[$i]}"
        if awk "BEGIN { exit !($cur_pmin < $fastest_single_time) }"; then
            fastest_single_time="$cur_pmin"
            fastest_single_ver="${ver_names[$i]}"
            fastest_single_comp="producer"
        fi
        if awk "BEGIN { exit !($cur_cmin < $fastest_single_time) }"; then
            fastest_single_time="$cur_cmin"
            fastest_single_ver="${ver_names[$i]}"
            fastest_single_comp="consumer"
        fi
    done

    local fa_pavg fa_cavg fa_total
    fa_pavg=$(awk -v v="$fastest_avg_ver" '
        { if ($3 == v) { print $1; exit } }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pavg[$i]}" "${ver_cavg[$i]}" "${ver_names[$i]}"
    done))
    fa_cavg=$(awk -v v="$fastest_avg_ver" '
        { if ($3 == v) { print $2; exit } }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pavg[$i]}" "${ver_cavg[$i]}" "${ver_names[$i]}"
    done))
    fa_total=$(awk "BEGIN { printf \"%.6f\", $fa_pavg + $fa_cavg }")

    local sa_pavg sa_cavg sa_total
    sa_pavg=$(awk -v v="$slowest_avg_ver" '
        { if ($3 == v) { print $1; exit } }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pavg[$i]}" "${ver_cavg[$i]}" "${ver_names[$i]}"
    done))
    sa_cavg=$(awk -v v="$slowest_avg_ver" '
        { if ($3 == v) { print $2; exit } }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pavg[$i]}" "${ver_cavg[$i]}" "${ver_names[$i]}"
    done))
    sa_total=$(awk "BEGIN { printf \"%.6f\", $sa_pavg + $sa_cavg }")

    local fm_pmed fm_cmed fm_total
    fm_pmed=$(awk -v v="$fastest_med_ver" '
        { if ($3 == v) { print $1; exit } }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pmed[$i]}" "${ver_cmed[$i]}" "${ver_names[$i]}"
    done))
    fm_cmed=$(awk -v v="$fastest_med_ver" '
        { if ($3 == v) { print $2; exit } }
    ' <(for (( i=0; i<n; i++ )); do
        printf '%s %s %s\n' "${ver_pmed[$i]}" "${ver_cmed[$i]}" "${ver_names[$i]}"
    done))
    fm_total=$(awk "BEGIN { printf \"%.6f\", $fm_pmed + $fm_cmed }")

    printf "\n"
    print_separator
    printf "%34s\n" "OVERVIEW"
    print_separator
    printf "  Fastest by average:       %-6s (%ss + %ss = %ss)\n" \
        "$fastest_avg_ver" "$fa_pavg" "$fa_cavg" "$fa_total"
    printf "  Slowest by average:       %-6s (%ss + %ss = %ss)\n" \
        "$slowest_avg_ver" "$sa_pavg" "$sa_cavg" "$sa_total"
    printf "  Fastest single run:       %-6s %s %ss\n" \
        "$fastest_single_ver" "$fastest_single_comp" "$fastest_single_time"
    printf "  Fastest by median:        %-6s (%ss + %ss = %ss)\n" \
        "$fastest_med_ver" "$fm_pmed" "$fm_cmed" "$fm_total"
    print_separator
}

print_report() {
    local versions=("$@")
    local date_str
    date_str="$(date '+%Y-%m-%d %H:%M:%S')"
    local file_size
    file_size="$(get_file_size "$DATA_FILE")"
    local fname
    fname="$(basename "$DATA_FILE")"

    print_separator
    printf "%34s\n" "BENCHMARK REPORT"
    print_separator
    printf "%-24s %s\n" "Date:" "$date_str"
    printf "%-24s %s\n" "Runs per version:" "$RUNS"
    printf "%-24s %s (%s)\n" "Input file:" "$fname" "$file_size"
    printf "%-24s %s\n" "CPU Cores:" "$CORES"
    print_separator

    local any_prefix=0
    for ver in "${versions[@]}"; do
        if [[ -n "${PROD_PREFIX[$ver]:-}" || -n "${CONS_PREFIX[$ver]:-}" ]]; then
            any_prefix=1
            break
        fi
    done
    if [[ "$any_prefix" -eq 1 ]]; then
        printf "\n"
        printf "Active Prefixes (1 run instead of %d for these):\n" "$RUNS"
        for ver in "${versions[@]}"; do
            local pp cp
            pp="$(resolve_prefix "${PROD_PREFIX[$ver]:-}" "$ver" "producer")"
            cp="$(resolve_prefix "${CONS_PREFIX[$ver]:-}" "$ver" "consumer")"
            if [[ -n "$pp" || -n "$cp" ]]; then
                printf "  %-8s producer: %s\n" "$ver" "${pp:-(none)}"
                printf "  %-8s consumer: %s\n" "$ver" "${cp:-(none)}"
            fi
        done
        print_separator
    fi
    printf "\n"

    printf "%-10s | %-9s | %8s | %8s | %8s | %9s | %s\n" \
        "Version" "Component" "Min(s)" "Max(s)" "Avg(s)" "Median(s)" "Identical"
    printf "%-10s-+-%-9s-+-%8s-+-%8s-+-%8s-+-%9s-+-%s\n" \
        "----------" "---------" "--------" "--------" "--------" "---------" "----------"

    for ver in "${versions[@]}"; do
        local vdir="$TMPDIR_WORK/$ver"
        local total identical_count
        total=$(wc -l < "$vdir/identical.txt")
        identical_count=$(grep -c "^1$" "$vdir/identical.txt") || identical_count=0
        local ident_str="${identical_count}/${total}"

        local stats
        stats=$(compute_stats "$vdir/producer_times.txt")
        read -r pmin pmax pavg pmed _ <<< "$stats"
        print_table_row "$ver" "producer" "$pmin" "$pmax" "$pavg" "$pmed" "$ident_str"

        stats=$(compute_stats "$vdir/cons_times.txt")
        read -r cmin cmax cavg cmed _ <<< "$stats"
        print_table_row "" "consumer" "$cmin" "$cmax" "$cavg" "$cmed" ""

        if [[ "$identical_count" -lt "$total" && -f "$vdir/last_diff_metrics.txt" ]]; then
            local metrics_line
            metrics_line=$(cat "$vdir/last_diff_metrics.txt")
            IFS='|' read -r elements mad medianad <<< "$metrics_line"
            if [[ "$elements" != "0" ]]; then
                printf "           |           |          | DIFF: %s elements, MAD=%s, MedianAD=%s\n" \
                    "$elements" "$mad" "$medianad"
            fi
        fi
    done

    print_overview "${versions[@]}"
}

generate_json() {
    local versions=("$@")
    local date_str
    date_str="$(date '+%Y-%m-%dT%H:%M:%S')"
    local file_size
    file_size=$(stat -c%s "$DATA_FILE" 2>/dev/null || echo 0)
    local fname
    fname="$(basename "$DATA_FILE")"

    {
        printf '{\n'
        printf '  "benchmark_metadata": {\n'
        printf '    "date": "%s",\n' "$date_str"
        printf '    "total_runs_per_version": %d,\n' "$RUNS"
        printf '    "input_file_path": "%s",\n' "$fname"
        printf '    "input_file_size_bytes": %s,\n' "$file_size"
        printf '    "cpu_cores_assigned": "%s"\n' "$CORES"
        printf '  },\n'

        printf '  "prefix_assignments": {'
        local first_pref=1
        for ver in "${versions[@]}"; do
            local pp cp
            pp="$(resolve_prefix "${PROD_PREFIX[$ver]:-}" "$ver" "producer")"
            cp="$(resolve_prefix "${CONS_PREFIX[$ver]:-}" "$ver" "consumer")"
            if [[ -n "$pp" || -n "$cp" ]]; then
                if [[ "$first_pref" -eq 1 ]]; then
                    printf '\n'
                    first_pref=0
                else
                    printf ',\n'
                fi
                local pp_esc="${pp//\\/\\\\}"; pp_esc="${pp_esc//\"/\\\"}"
                local cp_esc="${cp//\\/\\\\}"; cp_esc="${cp_esc//\"/\\\"}"
                printf '    "%s": { "producer_prefix": "%s", "consumer_prefix": "%s" }' \
                    "$ver" "$pp_esc" "$cp_esc"
            fi
        done
        [[ "$first_pref" -eq 0 ]] && printf '\n  '
        printf '},\n'

        printf '  "version_results": {\n'

        local first_ver=1
        for ver in "${versions[@]}"; do
            [[ "$first_ver" -eq 1 ]] || printf ',\n'
            first_ver=0

            local vdir="$TMPDIR_WORK/$ver"
            local total identical_count
            total=$(wc -l < "$vdir/identical.txt")
            identical_count=$(grep -c "^1$" "$vdir/identical.txt") || identical_count=0
            local all_identical="false"
            [[ "$identical_count" -eq "$total" ]] && all_identical="true"

            printf '    "%s": {\n' "$ver"

            local stats
            stats=$(compute_stats "$vdir/producer_times.txt")
            read -r pmin pmax pavg pmed pn <<< "$stats"
            printf '      "producer_statistics": {\n'
            printf '        "minimum_time_seconds": %s,\n' "$pmin"
            printf '        "maximum_time_seconds": %s,\n' "$pmax"
            printf '        "average_time_seconds": %s,\n' "$pavg"
            printf '        "median_time_seconds": %s,\n' "$pmed"
            printf '        "raw_run_times_seconds": ['
            local first_val=1
            while IFS= read -r val; do
                [[ "$first_val" -eq 1 ]] || printf ', '
                first_val=0
                printf '%s' "$val"
            done < "$vdir/producer_times.txt"
            printf ']\n'
            printf '      },\n'

            stats=$(compute_stats "$vdir/cons_times.txt")
            read -r cmin cmax cavg cmed cn <<< "$stats"
            printf '      "consumer_statistics": {\n'
            printf '        "minimum_time_seconds": %s,\n' "$cmin"
            printf '        "maximum_time_seconds": %s,\n' "$cmax"
            printf '        "average_time_seconds": %s,\n' "$cavg"
            printf '        "median_time_seconds": %s,\n' "$cmed"
            printf '        "raw_run_times_seconds": ['
            first_val=1
            while IFS= read -r val; do
                [[ "$first_val" -eq 1 ]] || printf ', '
                first_val=0
                printf '%s' "$val"
            done < "$vdir/cons_times.txt"
            printf ']\n'
            printf '      },\n'

            printf '      "file_comparison": {\n'
            printf '        "all_runs_identical": %s,\n' "$all_identical"
            printf '        "identical_run_count": %d,\n' "$identical_count"
            printf '        "total_run_count": %d,\n' "$total"

            if [[ "$all_identical" == "false" && -f "$vdir/last_diff_metrics.txt" ]]; then
                local metrics_line
                metrics_line=$(cat "$vdir/last_diff_metrics.txt")
                IFS='|' read -r elements mad medianad <<< "$metrics_line"
                printf '        "diff_metrics": {\n'
                printf '          "elements_compared": %s,\n' "$elements"
                printf '          "mean_absolute_deviation": %s,\n' "$mad"
                printf '          "median_absolute_deviation": %s\n' "$medianad"
                printf '        }\n'
            else
                printf '        "diff_metrics": null\n'
            fi

            printf '      }\n'
            printf '    }'
        done

        printf '\n  }\n'
        printf '}\n'
    }
}

setup_venv() {
    if [[ ! -x "$PYTHON" ]] || ! "$PYTHON" -c "import plotly, pandas, kaleido" &>/dev/null; then
        echo "Setting up Python virtual environment at $VENV_DIR ..." >&2
        [[ -x "$PYTHON" ]] || python3 -m venv "$VENV_DIR"
        if [[ -f "$SCRIPT_DIR/requirements.txt" ]]; then
            "$VENV_DIR/bin/pip" install -q --timeout 300 --retries 5 -r "$SCRIPT_DIR/requirements.txt"
        fi
        echo "Virtual environment ready." >&2
    fi
}

generate_report_overview() {
    local json_dir="$1"
    local output_file="$2"

    local all_json_files=()
    while IFS= read -r f; do
        all_json_files+=("$f")
    done < <(find "$json_dir" -name 'benchmark_report_*.json' ! -name '*latest*' -type f | sort)

    if [[ ${#all_json_files[@]} -eq 0 ]]; then
        echo "No JSON reports found in $json_dir — skipping overview" >&2
        return
    fi

    local all_count=${#all_json_files[@]}
    local slice=$(( all_count < 10 ? all_count : 10 ))
    local last10_json_files=("${all_json_files[@]: -$slice}")

    "$PYTHON" "$SCRIPT_DIR/generate_overview.py" \
        "$output_file" "$all_count" "${all_json_files[@]}" "${last10_json_files[@]}"
}

main() {
    local use_prefix_menu=0
    if [[ "${1:-}" == "-p" ]]; then
        use_prefix_menu=1
        shift
    fi

    local versions=()
    if [[ $# -eq 0 ]]; then
        mapfile -t versions < <(detect_versions)
    else
        for v in "$@"; do
            if [[ ! -f "$SRC_DIR/$v/build/producer" || ! -f "$SRC_DIR/$v/build/consumer" ]]; then
                echo "ERROR: version '$v' not found (expected built producer/consumer under $SRC_DIR/$v/build/)" >&2
                exit 1
            fi
            versions+=("$v")
        done
    fi

    if [[ ${#versions[@]} -eq 0 ]]; then
        echo "ERROR: No versions found with built producer/consumer in $SRC_DIR" >&2
        exit 1
    fi

    if [[ "$use_prefix_menu" -eq 1 ]]; then
        load_prefixes
        run_prefix_menu "${versions[@]}"
    fi

    setup_venv

    if [[ ! -f "$RUNNER" ]]; then
        echo "ERROR: Runner binary not found at $RUNNER" >&2
        echo "Build it first: cd $SRC_DIR/runner/build && cmake .. && make" >&2
        exit 1
    fi
    if [[ ! -f "$DATA_FILE" ]]; then
        echo "ERROR: Data file not found at $DATA_FILE" >&2
        exit 1
    fi

    echo "Found ${#versions[@]} versions: ${versions[*]}" >&2
    echo "Running $RUNS iterations per version..." >&2
    echo "" >&2

    for ver in "${versions[@]}"; do
        run_version "$ver"
    done

    echo "" >&2
    echo "Computing statistics..." >&2

    mkdir -p "$REPORTS_DIR" "$REPORTS_JSON_DIR"

    print_report "${versions[@]}" | tee "$REPORT_TXT"

    generate_json "${versions[@]}" > "$REPORT_JSON"

    cp "$REPORT_TXT" "$REPORTS_DIR/benchmark_report_latest.txt"
    cp "$REPORT_JSON" "$REPORTS_JSON_DIR/benchmark_report_latest.json"

    generate_report_overview "$REPORTS_JSON_DIR" "$REPORT_OVERVIEW"

    local graphs_dir="$REPORTS_DIR/graphs"
    mkdir -p "$graphs_dir"
    "$PYTHON" "$SCRIPT_DIR/generate_graphs.py" \
        "$graphs_dir" "$REPORT_JSON" "$REPORTS_JSON_DIR"

    echo "" >&2
    echo "Report saved to:      $REPORT_TXT" >&2
    echo "JSON saved to:        $REPORT_JSON" >&2
    echo "Latest txt:           $REPORTS_DIR/benchmark_report_latest.txt" >&2
    echo "Latest json:          $REPORTS_JSON_DIR/benchmark_report_latest.json" >&2
    echo "Report overview:      $REPORT_OVERVIEW" >&2
    echo "Graphs saved to:      $graphs_dir" >&2

    if [[ -d "$PREFIX_OUTDIR" ]] && [[ -n "$(ls -A "$PREFIX_OUTDIR" 2>/dev/null)" ]]; then
        echo "" >&2
        echo "Prefix tool output files:" >&2
        find "$PREFIX_OUTDIR" -type f -printf '  %p\n' >&2

        local perf_data_count
        perf_data_count=$(find "$PREFIX_OUTDIR" -maxdepth 1 -name 'perf_*.data' -type f 2>/dev/null | wc -l)
        if [[ "$perf_data_count" -gt 0 ]]; then
            echo "" >&2
            echo "Running perf analysis ($perf_data_count data files)..." >&2
            if "$SCRIPT_DIR/perf_report.sh" "$PREFIX_OUTDIR" >&2; then
                echo "  Perf report: $PREFIX_OUTDIR/perf_report/perf_index.html" >&2
            else
                echo "  WARNING: perf_report.sh failed — you can re-run manually:" >&2
                echo "    $SCRIPT_DIR/perf_report.sh $PREFIX_OUTDIR" >&2
            fi
        fi
    fi
}

main "$@"
