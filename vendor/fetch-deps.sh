#!/usr/bin/env bash
# vendor/fetch-deps.sh — ET1: pinned deb-closure fetch + extract to vendor/prefix.
#
# Binary-vendoring leg for hosts WITHOUT a system QGIS 4.2.x (Debian>=13 /
# Ubuntu>=25.04 only — apt-based). Resolves the package closure with apt
# metadata, downloads every .deb to vendor/cache/debs, verifies the apt-provided
# SHA256, then extracts into vendor/prefix/ (merged filesystem — QGIS lives at
# vendor/prefix/usr/...).
#
# Usage:
#   ./vendor/fetch-deps.sh [--print-only] [--pkgs "pkg1 pkg2 ..."]
#   --print-only   resolve + list URIs/sizes/hashes, no download (dry run)
#
# Seed package list comes from vendor/deb-packages.txt (one per line, '#'
# comments ok) — created with a sensible default if missing.
set -euo pipefail
cd "$(dirname "$0")/.."

LOG_DIR=vendor/logs; mkdir -p "$LOG_DIR"
CACHE=vendor/cache/debs
PREFIX=vendor/prefix
PKGFILE=vendor/deb-packages.txt
PRINT_ONLY=0
EXTRA_PKGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --print-only) PRINT_ONLY=1; shift ;;
    --pkgs) [ $# -ge 2 ] || fail "--pkgs needs a quoted list" "--pkgs \"qgis libgdal-dev\""; EXTRA_PKGS="$2"; shift 2 ;;
    *) EXTRA_PKGS="$EXTRA_PKGS $1"; shift ;;
  esac
done

fail() { echo "FAIL fetch-deps: $1" >&2; echo "       fix: $2" >&2; exit 1; }

command -v apt-get >/dev/null || \
  fail "apt-get absent — deb closure requires a Debian>=13/Ubuntu>=25.04 host" \
       "on other distros install system qgis>=4.2 or use a Debian container"

# --- seed package list -------------------------------------------------------
if [ ! -f "$PKGFILE" ]; then
  cat > "$PKGFILE" <<'EOF'
# Seed packages for the vendored QGIS runtime (qgis.org repo or distro).
# The full dependency closure is resolved from apt metadata at fetch time.
# Pin to 4.2.x-compatible suites: Debian trixie / Ubuntu 25.04+ (glibc >= 2.41).
qgis
qgis-providers
libqgis-dev
qt6-base-dev
libgdal-dev
libproj-dev
libgeos-dev
EOF
  echo "  .. wrote default $PKGFILE — edit to pin/remove packages"
fi
SEEDS=$(grep -vE '^\s*(#|$)' "$PKGFILE" | tr '\n' ' ')$EXTRA_PKGS
echo "== fetch-deps: seeds = $SEEDS"

# --- resolve the closure via apt metadata ------------------------------------
# --print-uris emits lines: 'url' filename size SHA256:hash   (apt>=2.x)
echo "== resolving closure (apt --print-uris) =="
URIS=$(apt-get install --download-only --reinstall --print-uris -y $SEEDS 2>/dev/null \
       | grep -oE "'[^']+' +[^ ]+\.deb +[0-9]+ +SHA256:[a-f0-9]{64}" || true)
if [ -z "$URIS" ]; then
  # Older apt prints 'url' filename size md5 — retry without hash requirement.
  URIS=$(apt-get install --download-only --reinstall --print-uris -y $SEEDS 2>/dev/null \
         | grep -oE "'[^']+' +[^ ]+\.deb +[0-9]+ +[^ ]+" || true)
fi
[ -n "$URIS" ] || fail "apt resolved nothing" \
  "check seed names in $PKGFILE and that the qgis.org/debian repo is in sources.list"

COUNT=$(printf '%s\n' "$URIS" | wc -l)
TOTAL_KB=$(printf '%s\n' "$URIS" | awk '{s+=$3} END {printf "%d", s/1024}')
echo "  .. $COUNT packages, ~${TOTAL_KB} MiB"

if [ "$PRINT_ONLY" = "1" ]; then
  printf '%s\n' "$URIS"
  exit 0
fi

# --- download + hash-verify ---------------------------------------------------
mkdir -p "$CACHE"
printf '%s\n' "$URIS" | while read -r quoted name size hash; do
  url=${quoted#\'}; url=${url%\'}
  [ -f "$CACHE/$name" ] || { echo "  .. fetch $name"; curl -fsSL "$url" -o "$CACHE/$name"; }
  if printf '%s' "$hash" | grep -q 'SHA256:'; then
    want=${hash#SHA256:}
    got=$(sha256sum "$CACHE/$name" | cut -d' ' -f1)
    [ "$got" = "$want" ] || { echo "FAIL sha256 $name: $got != $want" >&2; rm -f "$CACHE/$name"; exit 1; }
  fi
done

# --- extract merged prefix ----------------------------------------------------
mkdir -p "$PREFIX"
for f in "$CACHE"/*.deb; do dpkg-deb -x "$f" "$PREFIX"; done

# Record the resolved closure for reproducibility/audit.
dpkg-deb -f "$CACHE"/*.deb Package Version 2>/dev/null | paste -d'=' - - \
  > vendor/deb-closure.lock || true
echo "  OK vendored $COUNT debs -> $PREFIX (lock: vendor/deb-closure.lock)"
echo "  NOTE set QGIS_PREFIX_PATH=$PREFIX/usr and LD_LIBRARY_PATH=$PREFIX/usr/lib"
