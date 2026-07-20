#!/bin/bash
set -euo pipefail

# Usage: perf_report.sh [prefix_output_dir]
#   If no dir given, auto-detects the latest under reports/prefix_output/.
#   Processes all perf_*.data files and generates:
#     - ASCII top-20 functions report
#     - Flame graph + icicle chart SVGs
#     - CPU-time-by-function bar charts (HTML + PNG)
#     - HTML index page

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"
PYTHON="$VENV_DIR/bin/python3"

FLAMEGRAPH_DIR="/home/pinkod/FlameGraph"
FLAMEGRAPH_PL="$FLAMEGRAPH_DIR/flamegraph.pl"
STACKCOLLAPSE_PL="$FLAMEGRAPH_DIR/stackcollapse-perf.pl"

TOP_N=20

# ── resolve input directory ──────────────────────────────────────────────────

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

# ── find perf data files ─────────────────────────────────────────────────────

DATA_FILES=()
while IFS= read -r f; do
    DATA_FILES+=("$f")
done < <(find "$PREFIX_DIR" -maxdepth 1 -name 'perf_*.data' -type f | sort)

if [[ ${#DATA_FILES[@]} -eq 0 ]]; then
    echo "ERROR: No perf_*.data files found in $PREFIX_DIR" >&2
    exit 1
fi

echo "Found ${#DATA_FILES[@]} perf data file(s) in $PREFIX_DIR" >&2

# ── check tools ──────────────────────────────────────────────────────────────

for tool in perf "$FLAMEGRAPH_PL" "$STACKCOLLAPSE_PL"; do
    if [[ ! -f "$tool" ]] && ! command -v "$tool" &>/dev/null; then
        echo "ERROR: Required tool not found: $tool" >&2
        exit 1
    fi
done

# ── helper: extract version and role from filename ────────────────────────────

parse_perf_filename() {
    local fname
    fname="$(basename "$1" .data)"
    # pattern: perf_{VERSION}_{ROLE}
    # version can contain underscores (e.g. c_v3, v7_4)
    # role is the last segment: producer or consumer
    local role="${fname##*_}"
    local version="${fname#perf_}"
    version="${version%_${role}}"
    echo "$version" "$role"
}

# ── helper: run perf report and capture top-N ────────────────────────────────

get_perf_report() {
    local data_file="$1"
    local top_n="${2:-$TOP_N}"
    perf report -i "$data_file" --stdio --no-children \
        --sort=dso,symbol -n 2>/dev/null \
        | awk -v top_n="$top_n" '
        BEGIN { count=0 }
        {
            # lines like: "    67.83%          1169  producer          [.] shm_write                      -      -"
            # strip leading spaces, match: PCT% SAMPLES DSO [.] SYMBOL
            gsub(/^[[:space:]]+/, "")
            if (match($0, /^([0-9]+\.[0-9]+)%[[:space:]]+([0-9]+)[[:space:]]+([^[:space:]]+)[[:space:]]+(\[.\]|[k])[[:space:]]*(.*)$/, m)) {
                if (count < top_n) {
                    pct = m[1]
                    samples = m[2]
                    dso = m[3]
                    symbol = m[5]
                    gsub(/[[:space:]]+$/, "", symbol)
                    gsub(/[[:space:]]+-[[:space:]]*-[[:space:]]*$/, "", symbol)
                    gsub(/[[:space:]]+$/, "", symbol)
                    if (symbol == "") symbol = "(self)"
                    printf "%s|%s|%s|%s\n", pct, samples, dso, symbol
                    count++
                }
            }
        }
    '
}

# ── helper: parse perf stat log ──────────────────────────────────────────────

parse_perf_stat() {
    local log_file="$1"
    if [[ ! -f "$log_file" ]]; then
        return
    fi
    echo ""
    echo "  perf stat output:"
    echo "  ─────────────────"
    while IFS= read -r line; do
        # skip empty lines and comments
        [[ -z "$line" || "$line" == \#* ]] && continue
        echo "  $line"
    done < "$log_file"
}

# ── process each data file ───────────────────────────────────────────────────

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

for data_file in "${DATA_FILES[@]}"; do
    read -r version role <<< "$(parse_perf_filename "$data_file")"

    echo "Processing: $data_file ($version / $role)" >&2

    # ── ASCII section ────────────────────────────────────────────────────────
    {
        echo ""
        echo "────────────────────────────────────────────────────────────────────────────────"
        printf "  %-12s  %s\n" "Version:" "$version"
        printf "  %-12s  %s\n" "Role:" "$role"
        printf "  %-12s  %s\n" "Data file:" "$(basename "$data_file")"
        echo "────────────────────────────────────────────────────────────────────────────────"
        echo ""

        # Total samples and event count
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

    # ── collect plot data ────────────────────────────────────────────────────
    get_perf_report "$data_file" "$TOP_N" | while IFS='|' read -r pct samples dso symbol; do
        printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$symbol" "$pct" "$samples" "$dso" "$role" "$version"
    done >> "$PLOT_DATA_FILE"

    # ── flame graph ──────────────────────────────────────────────────────────
    perf script -i "$data_file" 2>/dev/null \
        | "$STACKCOLLAPSE_PL" --all 2>/dev/null \
        | "$FLAMEGRAPH_PL" --title "${version} ${role} — Flame Graph" \
            --subtitle "CPU cycles" \
            --width 1200 > "$FLAME_DIR/${version}_${role}_flame.svg" 2>/dev/null

    # ── icicle chart (inverted flame graph) ──────────────────────────────────
    perf script -i "$data_file" 2>/dev/null \
        | "$STACKCOLLAPSE_PL" --all 2>/dev/null \
        | "$FLAMEGRAPH_PL" --inverted --title "${version} ${role} — Icicle Chart" \
            --subtitle "CPU cycles (inverted)" \
            --width 1200 > "$FLAME_DIR/${version}_${role}_icicle.svg" 2>/dev/null

    echo "  Generated flame graph + icicle chart for $version/$role" >&2
done

# ── append perf stat logs if any ─────────────────────────────────────────────

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

# ── generate plotly bar charts ───────────────────────────────────────────────

if [[ -s "$PLOT_DATA_FILE" ]] && [[ -x "$PYTHON" ]]; then
    "$PYTHON" "$SCRIPT_DIR/perf_graphs.py" "$PLOT_DATA_FILE" "$GRAPHS_DIR"
fi

# ── generate HTML index ──────────────────────────────────────────────────────

generate_html_index() {
    local report_dir="$1"
    local html_file="$report_dir/perf_index.html"

    # collect SVGs
    local flame_svgs=()
    local icicle_svgs=()
    while IFS= read -r f; do flame_svgs+=("$f"); done < <(find "$FLAME_DIR" -name '*_flame.svg' | sort)
    while IFS= read -r f; do icicle_svgs+=("$f"); done < <(find "$FLAME_DIR" -name '*_icicle.svg' | sort)

    # collect plotly charts
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

<h2>ASCII Report</h2>
<button class="ascii-toggle" onclick="var e=document.getElementById('ascii-block');e.style.display=e.style.display==='none'?'block':'none';">
  Show / Hide ASCII Report
</button>
<div id="ascii-block" style="display:none">
<pre>
HTMLEOF

    # embed the ASCII report, escaping HTML
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

# ── summary ──────────────────────────────────────────────────────────────────

echo "" >&2
echo "Perf analysis complete:" >&2
echo "  Report directory: $REPORT_DIR" >&2
echo "  ASCII report:     $ASCII_REPORT" >&2
echo "  Flame graphs:     $FLAME_DIR/" >&2
echo "  Bar charts:       $GRAPHS_DIR/" >&2
echo "  HTML index:       $REPORT_DIR/perf_index.html" >&2
