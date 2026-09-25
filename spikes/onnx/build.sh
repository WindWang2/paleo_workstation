#!/usr/bin/env bash
# Spike build+run for ort_check. Links vendored ONNX Runtime via rpath
# ($ORIGIN/../../vendor/onnxruntime/lib) so the binary is relocatable
# within the repo tree and needs no env vars.
set -euo pipefail

SPIKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SPIKE_DIR/../.." && pwd)"
ORT_DIR="$REPO_ROOT/vendor/onnxruntime"

g++ -std=c++17 -O2 -Wall -Wextra \
    -I"$ORT_DIR/include" \
    "$SPIKE_DIR/ort_check.cpp" \
    -L"$ORT_DIR/lib" -lonnxruntime \
    -Wl,-rpath,'$ORIGIN/../../vendor/onnxruntime/lib' \
    -o "$SPIKE_DIR/ort_check"

echo "== ldd (onnxruntime resolution) =="
ldd "$SPIKE_DIR/ort_check" | grep onnxruntime

echo "== run =="
"$SPIKE_DIR/ort_check" "$SPIKE_DIR/toy.onnx"
