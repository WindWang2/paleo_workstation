#!/usr/bin/env bash
# vendor/bootstrap.sh — Phase 0 binary-vendoring fetch (§39 spike-0, ET0 结论).
# Preflight -> download+sha256-verify per vendor/manifest.json -> extract to vendor/prefix.
# On hosts where qgis is already installed at the exact pin (e.g. Arch pacman 4.2.x),
# detection short-circuits the QGIS leg. Fallback route (source superbuild) is NOT
# implemented here — see docs/phase0/et0-vendor-comparison.md fallback clause.
set -euo pipefail
cd "$(dirname "$0")/.."
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
[ "${free_gb:-0}" -ge 15 ] || fail "disk ${free_gb}GB < 15GB" "free space (binary route needs ~10GB; superbuild 60GB)"
echo "  PASS toolchain (cmake $cmake_ver, glibc $glibc, ${free_gb}GB free)"

echo "== vendor deps =="
# QGIS+GDAL+Qt6: prefer exact-pin system packages when present (Arch: pacman qgis==4.2.x);
# otherwise extract qgis.org deb closure into vendor/prefix (resolute/trixie pinned).
if pacman -Q qgis 2>/dev/null | grep -qE 'qgis 4\.2\.'; then
  note "qgis 4.2.x already installed system-wide — dev machine provisioned, vendoring skipped (dist/CI path still uses manifest fetch)"
elif [ -d vendor/prefix/qgis ]; then
  note "vendor/prefix/qgis present — skipping fetch (idempotent)"
else
  echo "  qgis system package absent — deb-closure fetch (ET1):"
  bash vendor/fetch-deps.sh
fi

# ONNX Runtime: always vendored (no distro guarantee of dev headers).
ORT_DIR=vendor/onnxruntime
if [ -f "$ORT_DIR/lib/libonnxruntime.so" ]; then
  note "onnxruntime vendored already"
else
  ort_ver=$(grep -oE '"version": *"[0-9.]+"' vendor/manifest.json | head -1 | grep -oE '[0-9.]+')
  ort_url=$(grep -oE '"url": *"[^"]+onnxruntime[^"]+"' vendor/manifest.json | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')
  ort_sha=$(grep -oE '"sha256": *"[a-f0-9]{64}"' vendor/manifest.json | head -1 | grep -oE '[a-f0-9]{64}')
  if [ -n "$ort_url" ]; then
    note "fetching onnxruntime $ort_ver"
    curl -fSL "$ort_url" -o vendor/ort.tgz
    echo "$ort_sha  vendor/ort.tgz" | sha256sum -c - || fail "ort sha256 mismatch" "re-pin vendor/manifest.json"
    mkdir -p "$ORT_DIR" && tar xzf vendor/ort.tgz -C "$ORT_DIR" --strip-components=1 && rm vendor/ort.tgz
  else
    echo "  !! no onnxruntime url in vendor/manifest.json — spike3 agent populates it"
  fi
fi

echo "== selfcheck (tail of bootstrap per §44.1) =="
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build
QT_QPA_PLATFORM=offscreen ./build/paleo_selfcheck
