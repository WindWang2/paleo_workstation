#!/usr/bin/env bash
# vendor/bootstrap.sh — Phase 0 binary-vendoring fetch (§39 spike-0, ET0 结论).
# Preflight -> download+sha256-verify per vendor/manifest.json -> extract to vendor/prefix.
# The Linux binary route is supplied from the locked project prefix.
#
# 用法：bootstrap.sh [fetch-only]
#   fetch-only  只取依赖（deb 闭包 + ONNX Runtime），跳过尾部的 configure/
#               build/selfcheck——CI lint job 用（compile_commands 场景无需
#               全量编译；与 paleo-dev.ps1 bootstrap fetch-only 同语义）。
set -euo pipefail
cd "$(dirname "$0")/.."
FETCH_ONLY=0
if [ "${1:-}" = "fetch-only" ]; then FETCH_ONLY=1; fi
LOG_DIR=vendor/logs; mkdir -p "$LOG_DIR"
fail() { echo "FAIL preflight: $1" >&2; echo "       fix: $2" >&2; exit 1; }
note() { echo "  .. $1"; }

echo "== preflight =="
command -v cmake >/dev/null || fail "cmake missing" "pacman -S cmake / apt install cmake"
cmake_ver=$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
[ "$(printf '%s\n3.28\n' "$cmake_ver" | sort -V | head -1)" = "3.28" ] || fail "cmake $cmake_ver < 3.28" "upgrade cmake"
command -v ninja >/dev/null || fail "ninja missing" "pacman -S ninja / apt install ninja-build"
command -v g++ >/dev/null || command -v clang++ >/dev/null || fail "no C++ compiler" "install gcc/clang"
command -v pkg-config >/dev/null || fail "pkg-config missing" "pacman -S pkgconf"
glibc=$(ldd --version | grep -oE '[0-9]+\.[0-9]+$' | head -1)
[ "$(printf '%s\n2.41\n' "$glibc" | sort -V | head -1)" = "2.41" ] || \
  fail "glibc $glibc < 2.41 (binary vendoring floor)" "use ExternalProject superbuild route or newer distro"
free_gb=$(df -BG --output=avail . | tail -1 | tr -dc 0-9)
echo "  PASS toolchain (cmake $cmake_ver, glibc $glibc, ${free_gb}GB free)"

echo "== vendor deps =="
# The Resolute package closure requires the distribution ABI even for a reused tree.
[ "$(printf '%s\n2.43\n' "$glibc" | sort -V | head -1)" = "2.43" ] ||
  fail "glibc $glibc < 2.43 (deb closure lock is Ubuntu 26.04)" \
       "use Ubuntu 26.04 or build the vendor/superbuild route for this host"

export QGIS_PREFIX_PATH="$PWD/vendor/prefix/usr"
lock_sha=$(tr -d '\r' < vendor/deb-closure.lock | sha256sum | cut -d' ' -f1)
if [[ -f "$QGIS_PREFIX_PATH/include/qgis/qgsapplication.h" &&
      "$(cat vendor/prefix/.paleo-deb-closure.sha256 2>/dev/null)" == "$lock_sha" ]] &&
   ! compgen -G "$QGIS_PREFIX_PATH/lib/qgis/plugins/libplugin_*.so" >/dev/null; then
  note "vendored QGIS prefix present at $QGIS_PREFIX_PATH (matches deb-closure.lock)"
else
  [ "${free_gb:-0}" -ge 15 ] || fail "disk ${free_gb}GB < 15GB" "free space for the vendored QGIS closure"
  bash vendor/fetch-deps.sh
fi

# ONNX Runtime: always vendored (no distro guarantee of dev headers).
python3 vendor/fetch-onnxruntime.py

if [ "$FETCH_ONLY" = 1 ]; then
  echo "== fetch-only: skip configure/build/selfcheck (deps vendored above) =="
  exit 0
fi

bash vendor/fetch-ranuts-document.sh --ensure

echo "== build and selfcheck =="
./paleo-dev build
./paleo-dev selfcheck
