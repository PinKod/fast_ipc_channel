#!/bin/bash
set -euo pipefail


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

[[ -f "$BASE_DIR/tools/env.sh" ]] && source "$BASE_DIR/tools/env.sh"

VENV_DIR="$SCRIPT_DIR/venv"
PYTHON="$VENV_DIR/bin/python3"

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$BASE_DIR/tools/FlameGraph}"
FLAMEGRAPH_PL="$FLAMEGRAPH_DIR/flamegraph.pl"
STACKCOLLAPSE_PL="$FLAMEGRAPH_DIR/stackcollapse-perf.pl"

TOP_N=20


if [[ $# -ge 1 ]]; then
    PREFIX_DIR="$1"
else
    PREFIX_DIR="$(ls -dt "$BASE_DIR/reports/prefix_output"/*/ 2>/dev/null | head -1)"
    if [[ -z "$PREFIX_DIR" ]]; then
        echo "ERROR: No prefix_output directories found under $BASE_DIR/reports/prefix_output/" >&2
        exit 1
    fi
fi

if [[ ! -d "$PREFIX_DIR" ]]; then
    echo "ERROR: Directory not found: $PREFIX_DIR" >&2
    exit 1
fi

REPORT_DIR="$PREFIX_DIR/perf_report"
FLAME_DIR="$REPORT_DIR/flamegraphs"
GRAPHS_DIR="$REPORT_DIR/perf_graphs"
mkdir -p "$FLAME_DIR" "$GRAPHS_DIR"


DATA_FILES=()
while IFS= read -r f; do
    DATA_FILES+=("$f")
done < <(find "$PREFIX_DIR" -maxdepth 1 -name 'perf_*.data' -type f | sort)

if [[ ${#DATA_FILES[@]} -eq 0 ]]; then
    echo "ERROR: No perf_*.data files found in $PREFIX_DIR" >&2
    exit 1
fi

echo "Found ${#DATA_FILES[@]} perf data file(s) in $PREFIX_DIR" >&2


for tool in perf "$FLAMEGRAPH_PL" "$STACKCOLLAPSE_PL"; do
    if [[ ! -f "$tool" ]] && ! command -v "$tool" &>/dev/null; then
        echo "ERROR: Required tool not found: $tool" >&2
        exit 1
    fi
done


parse_perf_filename() {
    local fname
    fname="$(basename "$1" .data)"
    local role="${fname##*_}"
    local rest="${fname%_${role}}"
    rest="${rest#perf_}"

    local metric=""
    for m in cycles contextswitches tlbmiss; do
        if [[ "$rest" == "${m}_"* ]]; then
            metric="$m"
            rest="${rest#${m}_}"
            break
        fi
    done
    [[ -z "$metric" ]] && metric="cycles"

    echo "$metric" "$rest" "$role"
}


get_perf_report() {
    local data_file="$1"
    local top_n="${2:-$TOP_N}"
    perf report -i "$data_file" --stdio --no-children \
        --sort=dso,symbol -n 2>/dev/null \
        | awk -v top_n="$top_n" '
        BEGIN { count=0 }
        /^[[:space:]]*[0-9]+\.[0-9]+%/ {
            if (count >= top_n) next
            pct = $1
            gsub(/%/, "", pct)
            samples = $2
            dso = $3
            marker = 0
            for (i = 4; i <= NF; i++) {
                if ($i == "[.]" || $i == "[k]") { marker = i; break }
            }
            if (marker == 0) next
            symbol = ""
            for (i = marker + 1; i <= NF; i++) {
                if ($i == "-") continue
                symbol = (symbol == "") ? $i : symbol " " $i
            }
            if (symbol == "") symbol = "(self)"
            printf "%s|%s|%s|%s\n", pct, samples, dso, symbol
            count++
        }
    '
}


parse_perf_stat() {
    local log_file="$1"
    if [[ ! -f "$log_file" ]]; then
        return
    fi
    echo ""
    echo "  perf stat output:"
    echo "  ─────────────────"
    while IFS= read -r line; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        echo "  $line"
    done < "$log_file"
}


ASCII_REPORT="$REPORT_DIR/perf_report.txt"
PLOT_DATA_FILE="$REPORT_DIR/.plot_data.tsv"

> "$ASCII_REPORT"
> "$PLOT_DATA_FILE"
echo -e "function\toverhead_pct\tsamples\tdso\trole\tversion" >> "$PLOT_DATA_FILE"

{
    echo "================================================================================"
    echo "                    PERF ANALYSIS REPORT"
    echo "================================================================================"
    echo "Date:                    $(date '+%Y-%m-%d %H:%M:%S')"
    echo "Source directory:         $PREFIX_DIR"
    echo "Data files:              ${#DATA_FILES[@]}"
    echo "Top-N functions:         $TOP_N"
    echo "================================================================================"
} >> "$ASCII_REPORT"

declare -A METRIC_LABELS=(
    [cycles]="CPU Cycles"
    [contextswitches]="Context Switches"
    [tlbmiss]="TLB Misses"
)

for data_file in "${DATA_FILES[@]}"; do
    read -r metric version role <<< "$(parse_perf_filename "$data_file")"
    metric_label="${METRIC_LABELS[$metric]:-$metric}"

    echo "Processing: $data_file ($version / $role / $metric)" >&2

    if ! perf report -i "$data_file" --stdio --no-children -n >/dev/null 2>&1; then
        echo "  WARNING: no samples in $(basename "$data_file") (event too rare for this run's duration) - skipping" >&2
        continue
    fi

    {
        echo ""
        echo "────────────────────────────────────────────────────────────────────────────────"
        printf "  %-12s  %s\n" "Version:" "$version"
        printf "  %-12s  %s\n" "Role:" "$role"
        printf "  %-12s  %s\n" "Metric:" "$metric_label"
        printf "  %-12s  %s\n" "Data file:" "$(basename "$data_file")"
        echo "────────────────────────────────────────────────────────────────────────────────"
        echo ""

        perf report -i "$data_file" --stdio --no-children -n 2>/dev/null \
            | grep -E '^# (Samples|Event count)' | sed 's/^# /  /'

        echo ""
        printf "  %-8s  %10s  %-30s  %s\n" "Overhead" "Samples" "DSO" "Symbol"
        printf "  %-8s  %10s  %-30s  %s\n" "--------" "----------" "------------------------------" "────────────────────────────────"

        get_perf_report "$data_file" "$TOP_N" | while IFS='|' read -r pct samples dso symbol; do
            printf "  %6s%%  %10s  %-30s  %s\n" "$pct" "$samples" "$dso" "$symbol"
        done
        echo ""
    } >> "$ASCII_REPORT"

    if [[ "$metric" == "cycles" ]]; then
        get_perf_report "$data_file" "$TOP_N" | while IFS='|' read -r pct samples dso symbol; do
            printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$symbol" "$pct" "$samples" "$dso" "$role" "$version"
        done >> "$PLOT_DATA_FILE"
    fi

    perf script -i "$data_file" 2>/dev/null \
        | "$STACKCOLLAPSE_PL" --all 2>/dev/null \
        | "$FLAMEGRAPH_PL" --title "${version} ${role} — ${metric_label} Flame Graph" \
            --subtitle "sampled at each ${metric_label,,} occurrence" \
            --width 1200 > "$FLAME_DIR/${version}_${role}_${metric}_flame.svg" 2>/dev/null

    perf script -i "$data_file" 2>/dev/null \
        | "$STACKCOLLAPSE_PL" --all 2>/dev/null \
        | "$FLAMEGRAPH_PL" --inverted --title "${version} ${role} — ${metric_label} Icicle Chart" \
            --subtitle "sampled at each ${metric_label,,} occurrence (inverted)" \
            --width 1200 > "$FLAME_DIR/${version}_${role}_${metric}_icicle.svg" 2>/dev/null

    echo "  Generated flame graph + icicle chart for $version/$role ($metric)" >&2
done


STAT_LOGS=()
while IFS= read -r f; do
    STAT_LOGS+=("$f")
done < <(find "$PREFIX_DIR" -maxdepth 1 -name 'perf_stat_*.log' -type f | sort)

if [[ ${#STAT_LOGS[@]} -gt 0 ]]; then
    {
        echo "================================================================================"
        echo "  PERF STAT COUNTERS"
        echo "================================================================================"
    } >> "$ASCII_REPORT"

    for log_file in "${STAT_LOGS[@]}"; do
        fname="$(basename "$log_file" .log)"
        version="${fname#perf_stat_}"
        role="${version##*_}"
        version="${version%_${role}}"
        {
            echo ""
            printf "  %-12s  %s\n" "Version:" "$version"
            printf "  %-12s  %s\n" "Role:" "$role"
            parse_perf_stat "$log_file"
            echo ""
        } >> "$ASCII_REPORT"
    done
fi


STAT_CSVS=()
while IFS= read -r f; do
    STAT_CSVS+=("$f")
done < <(find "$PREFIX_DIR" -maxdepth 1 -name 'perf_stat_*.csv' -type f | sort)

STAT_SUMMARY_LINES=()

if [[ ${#STAT_CSVS[@]} -gt 0 ]]; then
    declare -A STAT_GROUP_SEEN=()
    STAT_GROUP_KEYS=()
    for f in "${STAT_CSVS[@]}"; do
        base="$(basename "$f" .csv)"
        key="${base%_rep[0-9]*}"
        if [[ -z "${STAT_GROUP_SEEN[$key]:-}" ]]; then
            STAT_GROUP_SEEN[$key]=1
            STAT_GROUP_KEYS+=("$key")
        fi
    done

    {
        echo "================================================================================"
        echo "  PERF STAT (aggregated across repetitions)"
        echo "================================================================================"
    } >> "$ASCII_REPORT"

    for key in "${STAT_GROUP_KEYS[@]}"; do
        rest="${key#perf_stat_}"
        role="${rest##*_}"
        version="${rest%_${role}}"

        group_files=()
        for f in "${STAT_CSVS[@]}"; do
            [[ "$(basename "$f" .csv)" == "$key"* ]] && group_files+=("$f")
        done

        declare -A EV=()
        while IFS='=' read -r ev val; do
            [[ -z "$ev" ]] && continue
            EV["$ev"]="$val"
        done < <(awk -F, '
            !/^#/ && NF>=3 {
                val=$1; event=$3
                gsub(/^[ \t]+|[ \t]+$/, "", event)
                sum[event]+=val
                cnt[event]++
            }
            END { for (e in sum) printf "%s=%.6f\n", e, sum[e]/cnt[e] }
        ' "${group_files[@]}")

        cycles="${EV[cpu_core/cycles/]:-0}"
        instr="${EV[cpu_core/instructions/]:-0}"
        cmiss="${EV[cpu_core/cache-misses/]:-0}"
        cref="${EV[cpu_core/cache-references/]:-0}"
        dtlbl="${EV[cpu_core/dTLB-load-misses/]:-0}"
        cswitch="${EV[context-switches]:-0}"
        pfaults="${EV[page-faults]:-0}"

        ipc=$(awk -v c="$cycles" -v i="$instr" 'BEGIN { printf "%.4f", (c>0)? i/c : 0 }')
        cache_miss_pct=$(awk -v m="$cmiss" -v r="$cref" 'BEGIN { printf "%.3f", (r>0)? m/r*100 : 0 }')
        tlb_per_kinstr=$(awk -v t="$dtlbl" -v i="$instr" 'BEGIN { printf "%.4f", (i>0)? t/i*1000 : 0 }')

        {
            echo ""
            printf "  %-12s  %s\n" "Version:" "$version"
            printf "  %-12s  %s\n" "Role:" "$role"
            printf "  %-12s  %s (%d rep(s))\n" "Repetitions:" "${#group_files[@]}" "${#group_files[@]}"
            printf "  %-24s  %s\n" "Cycles (avg):" "$cycles"
            printf "  %-24s  %s\n" "Instructions (avg):" "$instr"
            printf "  %-24s  %s\n" "IPC:" "$ipc"
            printf "  %-24s  %s%%\n" "Cache-miss rate:" "$cache_miss_pct"
            printf "  %-24s  %s\n" "dTLB-load-misses (avg):" "$dtlbl"
            printf "  %-24s  %s\n" "dTLB-miss per 1k instr:" "$tlb_per_kinstr"
            printf "  %-24s  %s\n" "Context switches (avg):" "$cswitch"
            printf "  %-24s  %s\n" "Page faults (avg):" "$pfaults"
        } >> "$ASCII_REPORT"

        STAT_SUMMARY_LINES+=("$version|$role|${#group_files[@]}|$ipc|$cache_miss_pct|$tlb_per_kinstr|$cswitch|$pfaults")
        unset EV
    done
fi


MASSIF_FILES=()
while IFS= read -r f; do
    MASSIF_FILES+=("$f")
done < <(find "$PREFIX_DIR" -maxdepth 1 -name 'massif_*.out' -type f | sort)

MASSIF_SUMMARY_LINES=()

if [[ ${#MASSIF_FILES[@]} -gt 0 ]]; then
    if ! command -v ms_print &>/dev/null; then
        echo "WARNING: ms_print not found - skipping massif fragmentation summaries" >&2
    else
        {
            echo "================================================================================"
            echo "  MEMORY FOOTPRINT / HEAP FRAGMENTATION (massif --pages-as-heap=yes)"
            echo "  (page-granularity tracking: sees mmap'd allocations - most large buffers"
            echo "   here go through mmap, not brk - but can't show malloc-internal"
            echo "   fragmentation the way default massif mode would for brk-only heaps."
            echo "   'extra' near 0% is expected/correct when a workload mainly does large"
            echo "   page-sized mmap allocations; 'peak total' is the real signal to read.)"
            echo "================================================================================"
        } >> "$ASCII_REPORT"

        for f in "${MASSIF_FILES[@]}"; do
            base="$(basename "$f" .out)"
            rest="${base#massif_}"
            role="${rest##*_}"
            version="${rest%_${role}}"

            txt_file="$REPORT_DIR/massif_${version}_${role}.txt"
            ms_print "$f" > "$txt_file" 2>/dev/null

            read -r peak_n peak_total peak_useful peak_extra <<< "$(awk '
                /^[[:space:]]*n[[:space:]]+time\(i\)/ { in_table=1; next }
                in_table && /^[[:space:]]*[0-9]+[[:space:]]/ {
                    n=$1; total=$3; useful=$4; extra=$5
                    gsub(/,/,"",total); gsub(/,/,"",useful); gsub(/,/,"",extra)
                    if (total+0 > maxtotal+0) { maxtotal=total+0; maxuseful=useful+0; maxextra=extra+0; maxn=n }
                }
                END { printf "%s %s %s %s\n", maxn+0, maxtotal+0, maxuseful+0, maxextra+0 }
            ' "$txt_file")"

            frag_pct=$(awk -v t="$peak_total" -v e="$peak_extra" 'BEGIN { printf "%.2f", (t>0)? e/t*100 : 0 }')

            {
                echo ""
                printf "  %-12s  %s\n" "Version:" "$version"
                printf "  %-12s  %s\n" "Role:" "$role"
                printf "  %-24s  %s bytes (snapshot #%s)\n" "Peak heap total:" "$peak_total" "$peak_n"
                printf "  %-24s  %s bytes\n" "  useful:" "$peak_useful"
                printf "  %-24s  %s bytes\n" "  extra (fragmentation):" "$peak_extra"
                printf "  %-24s  %s%%\n" "Fragmentation ratio:" "$frag_pct"
            } >> "$ASCII_REPORT"

            MASSIF_SUMMARY_LINES+=("$version|$role|$peak_total|$peak_useful|$peak_extra|$frag_pct|massif_${version}_${role}.txt")
        done
    fi
fi


if [[ -s "$PLOT_DATA_FILE" ]] && [[ -x "$PYTHON" ]]; then
    "$PYTHON" "$SCRIPT_DIR/perf_graphs.py" "$PLOT_DATA_FILE" "$GRAPHS_DIR"
fi


generate_html_index() {
    local report_dir="$1"
    local html_file="$report_dir/perf_index.html"

    local flame_svgs=()
    local icicle_svgs=()
    while IFS= read -r f; do flame_svgs+=("$f"); done < <(find "$FLAME_DIR" -name '*_flame.svg' | sort)
    while IFS= read -r f; do icicle_svgs+=("$f"); done < <(find "$FLAME_DIR" -name '*_icicle.svg' | sort)

    local chart_htmls=()
    local chart_pics=()
    while IFS= read -r f; do chart_htmls+=("$f"); done < <(find "$GRAPHS_DIR" -name '*.html' | sort)
    while IFS= read -r f; do chart_pics+=("$f"); done < <(find "$GRAPHS_DIR" -name '*.png' | sort)

    local svg_cards=""
    for svg in "${flame_svgs[@]}" "${icicle_svgs[@]}"; do
        local fname
        fname="$(basename "$svg")"
        local label="${fname%.svg}"
        label="${label//_/ }"
        local kind="Flame Graph"
        [[ "$fname" == *_icicle.* ]] && kind="Icicle Chart"
        svg_cards+="  <div class=\"card\">
    <div class=\"card-header\">$label</div>
    <div class=\"card-tag\">$kind</div>
    <a class=\"open-full\" href=\"flamegraphs/$fname\" target=\"_blank\" rel=\"noopener\">Open full size ↗</a>
    <object data=\"flamegraphs/$fname\" type=\"image/svg+xml\" class=\"svg-embed\">
      <img src=\"flamegraphs/$fname\" alt=\"$label\">
    </object>
  </div>
"
    done

    local chart_cards=""
    for i in "${!chart_htmls[@]}"; do
        local html_f="${chart_htmls[$i]}"
        local png_f="${chart_pics[$i]:-}"
        local fname
        fname="$(basename "$html_f" .html)"
        local label="${fname//_/ }"
        chart_cards+="  <div class=\"card\">
    <div class=\"card-header\">$label</div>
    <div class=\"card-tag\">Bar Chart</div>
    <a href=\"perf_graphs/$fname.html\" target=\"_blank\">
      <img src=\"perf_graphs/$fname.png\" alt=\"$label\">
    </a>
  </div>
"
    done

    local stat_table_rows=""
    for line in "${STAT_SUMMARY_LINES[@]:-}"; do
        [[ -z "$line" ]] && continue
        IFS='|' read -r s_version s_role s_reps s_ipc s_cachemiss s_tlb s_cswitch s_pfaults <<< "$line"
        stat_table_rows+="<tr><td>$s_version</td><td>$s_role</td><td>$s_reps</td><td>$s_ipc</td><td>${s_cachemiss}%</td><td>$s_tlb</td><td>$s_cswitch</td><td>$s_pfaults</td></tr>
"
    done

    local massif_table_rows=""
    for line in "${MASSIF_SUMMARY_LINES[@]:-}"; do
        [[ -z "$line" ]] && continue
        IFS='|' read -r m_version m_role m_total m_useful m_extra m_frag m_file <<< "$line"
        massif_table_rows+="<tr><td>$m_version</td><td>$m_role</td><td>$m_total</td><td>$m_useful</td><td>$m_extra</td><td>${m_frag}%</td><td><a href=\"$m_file\" target=\"_blank\">details</a></td></tr>
"
    done

    cat > "$html_file" <<'HTMLEOF'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Perf Analysis Report</title>
<style>
  * { box-sizing: border-box; }
  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
         margin: 0; padding: 20px; background: #0d1117; color: #c9d1d9; }
  h1 { font-size: 1.5em; margin-bottom: 4px; color: #f0f6fc; }
  h2 { font-size: 1.2em; color: #8b949e; border-bottom: 1px solid #21262d;
       padding-bottom: 6px; margin-top: 32px; }
  .subtitle { color: #8b949e; font-size: .9em; margin-bottom: 24px; }
  .row { display: flex; flex-wrap: wrap; gap: 16px; margin-bottom: 16px; }
  .card {
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
    overflow: hidden; width: 420px; box-shadow: 0 1px 4px rgba(0,0,0,.3);
    transition: box-shadow .15s;
  }
  .card:hover { box-shadow: 0 4px 12px rgba(0,0,0,.5); }
  .card-header { padding: 10px 12px 4px; font-weight: 600; font-size: .9em; color: #f0f6fc; }
  .card-tag { padding: 0 12px 8px; font-size: .75em; color: #8b949e; text-transform: uppercase; letter-spacing: .5px; }
  .card object.svg-embed { width: 100%; height: 300px; display: block; }
  .card img { width: 100%; height: auto; display: block; }
  .card a { display: block; text-decoration: none; color: inherit; }
  .open-full {
    display: inline-block; margin: 0 12px 8px; padding: 3px 10px;
    font-size: .75em; border: 1px solid #30363d; border-radius: 6px;
    color: #58a6ff; text-decoration: none;
  }
  .open-full:hover { background: #21262d; }
  pre {
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 16px; overflow-x: auto; font-size: .82em; line-height: 1.5;
    color: #c9d1d9; max-height: 600px; overflow-y: auto;
  }
  .ascii-toggle {
    background: #21262d; color: #c9d1d9; border: 1px solid #30363d;
    border-radius: 6px; padding: 8px 16px; cursor: pointer; font-size: .85em;
    margin: 8px 0;
  }
  .ascii-toggle:hover { background: #30363d; }
  table { border-collapse: collapse; width: 100%; margin-bottom: 24px; font-size: .85em; }
  th, td { text-align: left; padding: 8px 12px; border-bottom: 1px solid #21262d; }
  th { color: #8b949e; text-transform: uppercase; font-size: .75em; letter-spacing: .5px; }
  td a { color: #58a6ff; }
</style>
</head>
<body>
<h1>Perf Analysis Report</h1>
<p class="subtitle">HTMLEOF

    echo "Generated: $(date '+%Y-%m-%d %H:%M:%S') &mdash; Source: $(basename "$PREFIX_DIR")" >> "$html_file"

    cat >> "$html_file" <<'HTMLEOF'
</p>

<h2>Flame Graphs &amp; Icicle Charts</h2>
<div class="row">
HTMLEOF

    echo "$svg_cards" >> "$html_file"

    cat >> "$html_file" <<'HTMLEOF'
</div>

<h2>CPU Time by Function</h2>
<div class="row">
HTMLEOF

    echo "$chart_cards" >> "$html_file"

    cat >> "$html_file" <<'HTMLEOF'
</div>

<h2>Perf Stat Summary (averaged across repetitions)</h2>
<table>
<tr><th>Version</th><th>Role</th><th>Reps</th><th>IPC</th><th>Cache-miss rate</th><th>dTLB-miss / 1k instr</th><th>Context switches</th><th>Page faults</th></tr>
HTMLEOF

    echo "$stat_table_rows" >> "$html_file"

    cat >> "$html_file" <<'HTMLEOF'
</table>

<h2>Heap Fragmentation (massif peak snapshot)</h2>
<table>
<tr><th>Version</th><th>Role</th><th>Peak total (B)</th><th>Useful (B)</th><th>Extra/fragmentation (B)</th><th>Fragmentation ratio</th><th>Full report</th></tr>
HTMLEOF

    echo "$massif_table_rows" >> "$html_file"

    cat >> "$html_file" <<'HTMLEOF'
</table>

<h2>ASCII Report</h2>
<button class="ascii-toggle" onclick="var e=document.getElementById('ascii-block');e.style.display=e.style.display==='none'?'block':'none';">
  Show / Hide ASCII Report
</button>
<div id="ascii-block" style="display:none">
<pre>
HTMLEOF

    sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g' "$ASCII_REPORT" >> "$html_file"

    cat >> "$html_file" <<'HTMLEOF'
</pre>
</div>

</body>
</html>
HTMLEOF

    echo "HTML index written to: $html_file" >&2
}

generate_html_index "$REPORT_DIR"


echo "" >&2
echo "Perf analysis complete:" >&2
echo "  Report directory: $REPORT_DIR" >&2
echo "  ASCII report:     $ASCII_REPORT" >&2
echo "  Flame graphs:     $FLAME_DIR/" >&2
echo "  Bar charts:       $GRAPHS_DIR/" >&2
echo "  HTML index:       $REPORT_DIR/perf_index.html" >&2
