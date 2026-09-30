#!/bin/bash
set -uo pipefail


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

[[ -f "$BASE_DIR/tools/env.sh" ]] && source "$BASE_DIR/tools/env.sh"

LSHAZ_BIN="$BASE_DIR/tools/bin/lshaz"
if [[ ! -x "$LSHAZ_BIN" ]]; then
    echo "ERROR: lshaz not found at $LSHAZ_BIN. Run ./bootstrap.sh first." >&2
    exit 1
fi
if ! command -v jq &>/dev/null; then
    echo "ERROR: jq is required to parse lshaz JSON output but was not found on PATH." >&2
    exit 1
fi

LSHAZ_DIR="$BASE_DIR/reports/lshaz"
EXPLAINED_FILE="$LSHAZ_DIR/.explained_rules"
EXPLANATIONS_MD="$LSHAZ_DIR/explanations.md"
mkdir -p "$LSHAZ_DIR"
touch "$EXPLAINED_FILE"
[[ -f "$EXPLANATIONS_MD" ]] || echo "# lshaz explanations" > "$EXPLANATIONS_MD"

if [[ $# -gt 0 ]]; then
    TARGET_DIRS=()
    for arg in "$@"; do
        if [[ -d "$arg" ]]; then
            TARGET_DIRS+=("$(cd "$arg" && pwd)")
        elif [[ -d "$BASE_DIR/src/$arg" ]]; then
            TARGET_DIRS+=("$BASE_DIR/src/$arg")
        else
            echo "ERROR: no such directory: $arg (looked at ./$arg and $BASE_DIR/src/$arg)" >&2
            exit 1
        fi
    done
else
    TARGET_DIRS=()
    while IFS= read -r dir; do
        [[ "$(basename "$dir")" == nr_* ]] && continue
        TARGET_DIRS+=("$dir")
    done < <(find "$BASE_DIR/src" -mindepth 1 -maxdepth 1 -type d -exec test -f '{}/CMakeLists.txt' \; -print | sort)
fi

if [[ ${#TARGET_DIRS[@]} -eq 0 ]]; then
    echo "ERROR: no target directories found under $BASE_DIR/src" >&2
    exit 1
fi

ALL_RULE_IDS_FILE="$(mktemp)"
trap 'rm -f "$ALL_RULE_IDS_FILE"' EXIT

for dir in "${TARGET_DIRS[@]}"; do
    dirname_only="$(basename "$dir")"

    if [[ -f "$dir/compile_commands.json" || -f "$dir/build/compile_commands.json" ]]; then
        echo "Scanning $dirname_only ..." >&2
        json_out="$LSHAZ_DIR/scan_${dirname_only}.json"
        txt_out="$LSHAZ_DIR/scan_${dirname_only}.txt"
        "$LSHAZ_BIN" scan "$dir" -f json -o "$json_out" || true
        "$LSHAZ_BIN" scan "$dir" > "$txt_out" 2>&1 || true

        if [[ -s "$json_out" ]]; then
            jq -r '.diagnostics[]?.ruleID // empty' "$json_out" >> "$ALL_RULE_IDS_FILE"
        fi
    else
        echo "Initializing $dirname_only (no compile_commands.json found) ..." >&2
        "$LSHAZ_BIN" init "$dir" || echo "  WARN: lshaz init failed for $dirname_only" >&2
    fi
done

sort -u "$ALL_RULE_IDS_FILE" -o "$ALL_RULE_IDS_FILE"

NEW_COUNT=0
while IFS= read -r rule_id; do
    [[ -z "$rule_id" ]] && continue
    if grep -qxF "$rule_id" "$EXPLAINED_FILE" 2>/dev/null; then
        continue
    fi
    echo "Explaining new rule: $rule_id" >&2
    {
        echo ""
        echo "## $rule_id"
        echo "_First seen: $(date '+%Y-%m-%d %H:%M:%S')_"
        echo ""
        echo '```'
        "$LSHAZ_BIN" explain "$rule_id" 2>&1
        echo '```'
    } >> "$EXPLANATIONS_MD"
    echo "$rule_id" >> "$EXPLAINED_FILE"
    NEW_COUNT=$((NEW_COUNT + 1))
done < "$ALL_RULE_IDS_FILE"

echo "" >&2
echo "lshaz check complete: $(wc -l < "$ALL_RULE_IDS_FILE" | tr -d ' ') rule(s) found this run, $NEW_COUNT newly explained." >&2
echo "Explanations: $EXPLANATIONS_MD" >&2
