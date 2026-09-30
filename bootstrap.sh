#!/bin/bash
set -euo pipefail


BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$BASE_DIR/tools"
BIN_DIR="$TOOLS_DIR/bin"
mkdir -p "$BIN_DIR"


resolve_path() {
    local var_name="$1" default="$2" label="$3"
    local current="${!var_name:-}"
    if [[ -n "$current" ]]; then
        echo "$current"
        return
    fi
    if [[ -e "$default" ]]; then
        echo "$default"
        return
    fi
    if [[ -t 0 ]]; then
        local answer
        read -r -p "$label not found at default ($default). Enter path (or leave blank to skip): " answer >&2
        echo "$answer"
        return
    fi
    echo ""
}

fail_missing() {
    echo "ERROR: $1 not found at '$2'. Set \$$3 or re-run interactively." >&2
    exit 1
}


FLAMEGRAPH_HOME="$(resolve_path FLAMEGRAPH_HOME "/home/pin-by-kod/apps/FlameGraph" "FlameGraph")"
if [[ -z "$FLAMEGRAPH_HOME" || ! -f "$FLAMEGRAPH_HOME/flamegraph.pl" ]]; then
    fail_missing "FlameGraph (flamegraph.pl)" "${FLAMEGRAPH_HOME:-<empty>}" "FLAMEGRAPH_HOME"
fi
ln -sfn "$FLAMEGRAPH_HOME" "$TOOLS_DIR/FlameGraph"
echo "OK  tools/FlameGraph -> $FLAMEGRAPH_HOME"


LSHAZ_HOME="$(resolve_path LSHAZ_HOME "/home/pin-by-kod/apps/lshaz" "lshaz")"
if [[ -z "$LSHAZ_HOME" || ! -x "$LSHAZ_HOME/build/lshaz" ]]; then
    fail_missing "lshaz binary (build/lshaz)" "${LSHAZ_HOME:-<empty>}" "LSHAZ_HOME"
fi
ln -sfn "$LSHAZ_HOME" "$TOOLS_DIR/lshaz"
ln -sfn "$LSHAZ_HOME/build/lshaz" "$BIN_DIR/lshaz"
echo "OK  tools/lshaz -> $LSHAZ_HOME"
echo "OK  tools/bin/lshaz -> $LSHAZ_HOME/build/lshaz"


GCC_BUILD_DIR="$(resolve_path GCC_BUILD_DIR "/home/pin-by-kod/apps/gcc/build-gcc" "gcc build dir")"
if [[ -z "$GCC_BUILD_DIR" || ! -x "$GCC_BUILD_DIR/gcc/xgcc" || ! -x "$GCC_BUILD_DIR/gcc/xg++" ]]; then
    fail_missing "gcc build (gcc/xgcc, gcc/xg++)" "${GCC_BUILD_DIR:-<empty>}" "GCC_BUILD_DIR"
fi

cat > "$BIN_DIR/gcc" <<EOF
#!/bin/bash
exec "$GCC_BUILD_DIR/gcc/xgcc" -B"$GCC_BUILD_DIR/gcc" "\$@"
EOF
chmod +x "$BIN_DIR/gcc"

TESTSUITE_FLAGS="$GCC_BUILD_DIR/x86_64-pc-linux-gnu/libstdc++-v3/scripts/testsuite_flags"
CXX_INCLUDES=""
if [[ -x "$TESTSUITE_FLAGS" ]]; then
    CXX_INCLUDES="$("$TESTSUITE_FLAGS" --build-includes)"
fi

cat > "$BIN_DIR/g++" <<EOF
#!/bin/bash
exec "$GCC_BUILD_DIR/gcc/xg++" -B"$GCC_BUILD_DIR/gcc" $CXX_INCLUDES "\$@"
EOF
chmod +x "$BIN_DIR/g++"
echo "OK  tools/bin/gcc, tools/bin/g++ -> $GCC_BUILD_DIR/gcc/{xgcc,xg++} (-B wrapped)"

SMOKE_DIR="$(mktemp -d)"
trap 'rm -rf "$SMOKE_DIR"' EXIT

cat > "$SMOKE_DIR/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("gcc-ok\n"); return 0; }
EOF
if ! "$BIN_DIR/gcc" "$SMOKE_DIR/hello.c" -o "$SMOKE_DIR/hello_c" 2>"$SMOKE_DIR/gcc.err"; then
    cat "$SMOKE_DIR/gcc.err" >&2
    echo "ERROR: gcc wrapper failed to compile a trivial program" >&2
    exit 1
fi
[[ "$("$SMOKE_DIR/hello_c")" == "gcc-ok" ]] || { echo "ERROR: gcc-compiled smoke test did not run correctly" >&2; exit 1; }

cat > "$SMOKE_DIR/hello.cpp" <<'EOF'
#include <iostream>
int main() { std::cout << "g++-ok\n"; return 0; }
EOF
if ! "$BIN_DIR/g++" "$SMOKE_DIR/hello.cpp" -o "$SMOKE_DIR/hello_cpp" 2>"$SMOKE_DIR/g++.err"; then
    cat "$SMOKE_DIR/g++.err" >&2
    echo "ERROR: g++ wrapper failed to compile a trivial program" >&2
    exit 1
fi
[[ "$("$SMOKE_DIR/hello_cpp")" == "g++-ok" ]] || { echo "ERROR: g++-compiled smoke test did not run correctly" >&2; exit 1; }
echo "OK  gcc/g++ smoke test passed"


CMAKE_BUILD_DIR="$(resolve_path CMAKE_BUILD_DIR "/home/pin-by-kod/apps/cmake-master/bin" "cmake bin dir")"
if [[ -z "$CMAKE_BUILD_DIR" || ! -x "$CMAKE_BUILD_DIR/cmake" ]]; then
    fail_missing "cmake binary" "${CMAKE_BUILD_DIR:-<empty>}" "CMAKE_BUILD_DIR"
fi
for tool in cmake ctest cpack; do
    if [[ -x "$CMAKE_BUILD_DIR/$tool" ]]; then
        ln -sfn "$CMAKE_BUILD_DIR/$tool" "$BIN_DIR/$tool"
    fi
done
echo "OK  tools/bin/{cmake,ctest,cpack} -> $CMAKE_BUILD_DIR ($("$BIN_DIR/cmake" --version | head -1))"


cat > "$TOOLS_DIR/env.sh" <<EOF
export PATH="$BIN_DIR:\$PATH"
export CC="$BIN_DIR/gcc"
export CXX="$BIN_DIR/g++"
EOF
echo "OK  tools/env.sh written"


"$BASE_DIR/scripts/setup_venv.sh"
echo "OK  scripts/venv ready ($("$BASE_DIR/scripts/venv/bin/python3" --version))"


DATA_DIR="$BASE_DIR/data"
BASE_DATA_FILE="$DATA_DIR/big.bin"
if [[ ! -f "$BASE_DATA_FILE" ]]; then
    fail_missing "seed data file" "$BASE_DATA_FILE" "BASE_DATA_FILE"
fi
BASE_DATA_SIZE=$(stat -c%s "$BASE_DATA_FILE")

generate_multiple() {
    local name="$1" multiplier="$2"
    local target="$DATA_DIR/$name"
    local expected=$((BASE_DATA_SIZE * multiplier))
    if [[ -f "$target" && "$(stat -c%s "$target")" == "$expected" ]]; then
        echo "OK  data/$name already present ($((expected / 1024 / 1024))MB)"
        return
    fi
    rm -f "$target"
    for i in $(seq "$multiplier"); do
        dd if="$BASE_DATA_FILE" of="$target" bs=1M oflag=append conv=notrunc status=none
    done
    local actual
    actual="$(stat -c%s "$target")"
    if [[ "$actual" != "$expected" ]]; then
        echo "ERROR: data/$name generation size mismatch (expected $expected, got $actual)" >&2
        exit 1
    fi
    echo "OK  data/$name generated ($((expected / 1024 / 1024))MB, ${multiplier}x data/big.bin)"
}

generate_multiple abg_big.bin 4
generate_multiple bg_big.bin 5
generate_multiple bg2_big.bin 20

echo ""
echo "Bootstrap complete. scripts/benchmark.sh and scripts/perf_report.sh source"
echo "tools/env.sh automatically. For manual builds, run:"
echo "  source tools/env.sh"
