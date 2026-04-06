#!/usr/bin/env bash
set -euo pipefail

# ── Python virtual environment ─────────────────────────────────────────────────
# On the development server sydudcaf01 a dedicated Python 3.11 venv is required
# because the system Python does not have grpcio-tools installed globally.
# On any other host, we trust whatever python3 is already in PATH.
_HOST="$(hostname -s 2>/dev/null || hostname)"
if [[ "$_HOST" == "sydudcaf01" ]]; then
    if [[ -f "$HOME/Python311_venv/bin/activate" ]]; then
        # shellcheck source=/dev/null
        source "$HOME/Python311_venv/bin/activate"
    else
        echo "WARNING: $HOME/Python311_venv not found on sydudcaf01 — using system python3"
    fi
fi

# ── protoc binary ──────────────────────────────────────────────────────────────
# The "protoc" Compiler/Generator
PC=/data/local/stow/grpc-1.67.0/bin/protoc

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROTO_DIR="$REPO_ROOT/src/proto"
OUT_DIR="$SCRIPT_DIR"
PROTO_FILE="$PROTO_DIR/RemSvc.proto"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    if command -v python >/dev/null 2>&1; then
        PYTHON=python
    else
        echo "Python interpreter not found. Set PYTHON or install Python."
        exit 1
    fi
fi

if ! "$PYTHON" -m grpc_tools.protoc --version >/dev/null 2>&1; then
    echo "grpc_tools.protoc is not available in $PYTHON. Install grpcio-tools."
    exit 1
fi

echo "Generating Python bindings from $PROTO_FILE into $OUT_DIR"
"$PYTHON" -m grpc_tools.protoc \
    -I "$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_FILE"
