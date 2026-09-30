#!/bin/bash
set -euo pipefail


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"
PYTHON="$VENV_DIR/bin/python3"

if [[ -x "$PYTHON" ]] && "$PYTHON" -c "import plotly, pandas, kaleido" &>/dev/null; then
    exit 0
fi

echo "Setting up Python virtual environment at $VENV_DIR ..." >&2

if [[ ! -x "$PYTHON" ]]; then
    python3 -m venv --without-pip "$VENV_DIR"
fi

if ! "$PYTHON" -m pip --version &>/dev/null; then
    GET_PIP="$(mktemp)"
    trap 'rm -f "$GET_PIP"' EXIT
    curl -fsSL https://bootstrap.pypa.io/get-pip.py -o "$GET_PIP"
    "$PYTHON" "$GET_PIP" --quiet
fi

if [[ -f "$SCRIPT_DIR/requirements.txt" ]]; then
    "$PYTHON" -m pip install -q --timeout 300 --retries 5 -r "$SCRIPT_DIR/requirements.txt"
fi

echo "Virtual environment ready." >&2
