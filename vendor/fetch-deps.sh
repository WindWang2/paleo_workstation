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
#   ./vendor/fetch-deps.sh [--print-only] [--update-lock]
#   --update-lock  explicitly refresh the committed package closure
#   --print-only   list the locked URIs/sizes/hashes, no download
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
UPDATE_LOCK=0
LOCK=vendor/deb-closure.lock
while [ $# -gt 0 ]; do
  case "$1" in
    --print-only) PRINT_ONLY=1; shift ;;
    --update-lock) UPDATE_LOCK=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL fetch-deps: $1" >&2; echo "       fix: $2" >&2; exit 1; }

# --print-only 只读锁文件（格式校验 + 列表打印），宿主不需要 apt/python——
# CI 冒烟门（ci.yml lint job）跑在任何 runner 上都不应被工具链前置检查绊倒。
if [ "$PRINT_ONLY" != "1" ]; then
  command -v apt-get >/dev/null || \
    fail "apt-get absent — deb closure requires a Debian>=13/Ubuntu>=25.04 host" \
         "on other distros install system qgis>=4.2 or use a Debian container"
  command -v python3 >/dev/null || fail "python3 absent" "install python3 to resolve signed apt SHA256 metadata"
fi

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
SEEDS=$(grep -vE '^\s*(#|$)' "$PKGFILE" | tr '\n' ' ')
echo "== fetch-deps: seeds = $SEEDS"

# --- resolve the closure via apt metadata ------------------------------------
# --print-uris emits lines: 'url' filename size SHA256:hash   (apt>=2.x)
if [ "$UPDATE_LOCK" = 1 ]; then
  echo "== explicitly refreshing closure (apt --print-uris) =="
  URIS=$(apt-get install --download-only --reinstall --print-uris -y $SEEDS 2>/dev/null) || \
    fail "apt could not resolve seed packages" "check the QGIS apt repository and $PKGFILE"
  URIS=$(printf '%s\n' "$URIS" | python3 vendor/lock-debs.py) || \
    fail "could not match apt URIs to signed SHA-256 metadata" "check apt sources and update indexes"
  printf '%s\n' "$URIS" > "$LOCK.tmp"
else
  [ -s "$LOCK" ] || fail "missing pinned closure $LOCK" "generate it on the target distro with --update-lock and commit it"
  URIS=$(cat "$LOCK")
fi

COUNT=$(printf '%s\n' "$URIS" | wc -l)
TOTAL_KB=$(printf '%s\n' "$URIS" | awk '{s+=$3} END {printf "%d", s/1048576}')
echo "  .. $COUNT packages, ~${TOTAL_KB} MiB"

if [ "$PRINT_ONLY" = "1" ]; then
  if [ "$UPDATE_LOCK" = 1 ]; then mv "$LOCK.tmp" "$LOCK"; fi
  # 冒烟门语义：顺带校验每条锁目格式合法（与下载路径同一断言），零下载。
  printf '%s\n' "$URIS" | while read -r quoted name size hash; do
    [[ "$quoted" == \'http*\' && "$name" =~ ^[A-Za-z0-9][A-Za-z0-9.+_%:~-]*\.deb$ &&
       "$size" =~ ^[0-9]+$ && "$hash" =~ ^SHA256:[a-f0-9]{64}$ ]] || \
      fail "invalid lock entry: $quoted $name $size $hash" "refresh $LOCK with --update-lock"
  done
  printf '%s\n' "$URIS"
  exit 0
fi

# --- download + hash-verify ---------------------------------------------------
mkdir -p "$CACHE"
printf '%s\n' "$URIS" | while read -r quoted name size hash; do
  [[ "$quoted" == \'http*\' && "$name" =~ ^[A-Za-z0-9][A-Za-z0-9.+_%:~-]*\.deb$ &&
     "$size" =~ ^[0-9]+$ && "$hash" =~ ^SHA256:[a-f0-9]{64}$ ]] || \
    fail "invalid lock entry" "refresh $LOCK with --update-lock"
  url=${quoted#\'}; url=${url%\'}
  [ -f "$CACHE/$name" ] || { echo "  .. fetch $name"; curl -fsSL "$url" -o "$CACHE/$name"; }
  [ "$(stat -c %s "$CACHE/$name")" = "$size" ] || fail "size mismatch for $name" "delete the cached archive and retry"
  want=${hash#SHA256:}
  got=$(sha256sum "$CACHE/$name" | cut -d' ' -f1)
  [ "$got" = "$want" ] || { echo "FAIL sha256 $name: $got != $want" >&2; rm -f "$CACHE/$name"; exit 1; }
done

# --- extract merged prefix ----------------------------------------------------
STAGING=${PREFIX}.new
rm -rf -- "$STAGING"
mkdir -p "$STAGING"
printf '%s\n' "$URIS" | while read -r quoted name size hash; do
  dpkg-deb -x "$CACHE/$name" "$STAGING"
done
rm -rf -- "$PREFIX"
mv "$STAGING" "$PREFIX"

# Record the resolved closure for reproducibility/audit.
if [ "$UPDATE_LOCK" = 1 ]; then mv "$LOCK.tmp" "$LOCK"; fi
echo "  OK vendored $COUNT debs -> $PREFIX (lock: vendor/deb-closure.lock)"
echo "  NOTE set QGIS_PREFIX_PATH=$PREFIX/usr and LD_LIBRARY_PATH=$PREFIX/usr/lib"
