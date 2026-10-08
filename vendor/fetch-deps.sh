#!/usr/bin/env bash
# vendor/fetch-deps.sh — ET1: pinned deb-closure fetch + extract to vendor/prefix.
#
# Binary-vendoring leg for hosts WITHOUT a system QGIS 4.2.x (apt-based).
# The committed lock is resolved on Ubuntu 26.04 (resolute): its binaries need
# glibc >= 2.43 at run time — Debian 13 (glibc 2.41) can download/extract it but
# not execute it (#76). Older hosts: superbuild or a resolute container. Resolves the package closure with apt
# metadata, downloads every .deb to vendor/cache/debs, verifies the apt-provided
# SHA256, then extracts into vendor/prefix/ (merged filesystem — QGIS lives at
# vendor/prefix/usr/...).
#
# Usage:
#   ./vendor/fetch-deps.sh [--print-only] [--update-lock] [--check-urls]
#   --update-lock  explicitly refresh the committed package closure
#   --print-only   list the locked URIs/sizes/hashes, no download
#   --check-urls   HEAD-probe every locked URL (pool, then snapshot fallback);
#                  exit 1 if any archive is unreachable — CI lock-rot smoke (#76)
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
CHECK_URLS=0
LOCK=vendor/deb-closure.lock
# #76：Ubuntu pool 会在安全更新后删掉旧版本（锁里的 URL 随之 404）。锁内容
# 由 SHA256 钉死，所以从 snapshot.ubuntu.com 的同一时间点取同一文件是安全的
# 回退源。时间戳在 --update-lock 时写入 $SNAPSHOT_FILE；可用
# PALEO_DEB_SNAPSHOT 覆盖。
SNAPSHOT_FILE=vendor/deb-closure.snapshot
DEB_SNAPSHOT=${PALEO_DEB_SNAPSHOT:-$( [ -s "$SNAPSHOT_FILE" ] && tr -d '[:space:]' < "$SNAPSHOT_FILE" || echo 20260926T000000Z )}
while [ $# -gt 0 ]; do
  case "$1" in
    --print-only) PRINT_ONLY=1; shift ;;
    --update-lock) UPDATE_LOCK=1; shift ;;
    --check-urls) CHECK_URLS=1; PRINT_ONLY=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL fetch-deps: $1" >&2; echo "       fix: $2" >&2; exit 1; }

# --print-only 只读锁文件（格式校验 + 列表打印），宿主不需要 apt/python——
# CI 冒烟门（ci.yml lint job）跑在任何 runner 上都不应被工具链前置检查绊倒。
snapshot_url_for() { # 仅 Ubuntu 官方 pool 有 snapshot；其他源回空串
  case "$1" in
    http://archive.ubuntu.com/ubuntu/pool/*|https://archive.ubuntu.com/ubuntu/pool/*|\
    http://security.ubuntu.com/ubuntu/pool/*|https://security.ubuntu.com/ubuntu/pool/*)
      echo "https://snapshot.ubuntu.com/ubuntu/${DEB_SNAPSHOT}/pool/${1#*/ubuntu/pool/}" ;;
    *) echo "" ;;
  esac
}

if [ "$PRINT_ONLY" != "1" ]; then
  command -v apt-get >/dev/null || \
    fail "apt-get absent — deb closure requires an apt-based host (runtime needs Ubuntu 26.04 / glibc>=2.43)" \
         "on other distros install system qgis>=4.2 or use a Debian container"
  command -v python3 >/dev/null || fail "python3 absent" "install python3 to resolve signed apt SHA256 metadata"
fi

# --- seed package list -------------------------------------------------------
if [ ! -f "$PKGFILE" ]; then
  cat > "$PKGFILE" <<'EOF'
# Seed packages for the vendored QGIS runtime (qgis.org repo or distro).
# The full dependency closure is resolved from apt metadata at fetch time.
# Pin to 4.2.x-compatible suites: the committed lock is Ubuntu 26.04 resolute (glibc >= 2.43).
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
  # Windows 检出（autocrlf）的锁文件行尾带 \r——hash 字段会挂上 \r 打破
  # 格式断言伪红；统一剥掉（锁在索引/CI（LF）语义不变）。
  URIS=$(tr -d '\r' < "$LOCK")
fi

COUNT=$(printf '%s\n' "$URIS" | wc -l)
TOTAL_KB=$(printf '%s\n' "$URIS" | awk '{s+=$3} END {printf "%d", s/1048576}')
echo "  .. $COUNT packages, ~${TOTAL_KB} MiB"

if [ "$PRINT_ONLY" = "1" ]; then
  if [ "$UPDATE_LOCK" = 1 ]; then
    mv "$LOCK.tmp" "$LOCK"
    date -u +%Y%m%dT%H%M%SZ > "$SNAPSHOT_FILE"
  fi
  # 冒烟门语义：顺带校验每条锁目格式合法（与下载路径同一断言），零下载。
  printf '%s\n' "$URIS" | while read -r quoted name size hash; do
    [[ "$quoted" == \'http*\' && "$name" =~ ^[A-Za-z0-9][A-Za-z0-9.+_%:~-]*\.deb$ &&
       "$size" =~ ^[0-9]+$ && "$hash" =~ ^SHA256:[a-f0-9]{64}$ ]] || \
      fail "invalid lock entry: $quoted $name $size $hash" "refresh $LOCK with --update-lock"
  done
  if [ "$CHECK_URLS" = 1 ]; then
    # 锁腐烂冒烟：只做 HEAD 探测（不下载、不校验内容——内容由下载路径的
    # SHA256 断言兜底）。pool 404 时探 snapshot 回退源。
    missing=0
    while read -r quoted name size hash; do
      url=${quoted#\'}; url=${url%\'}
      curl -fsIL --max-time 60 "$url" >/dev/null 2>&1 && continue
      snap=$(snapshot_url_for "$url")
      if [ -n "$snap" ] && curl -fsIL --max-time 60 "$snap" >/dev/null 2>&1; then
        echo "  .. pool miss, snapshot ok: $name"
        continue
      fi
      echo "  !! unreachable: $name ($url)"
      missing=$((missing + 1))
    done <<< "$URIS"
    [ "$missing" = 0 ] || fail "$missing locked archive(s) unreachable" "refresh $LOCK with --update-lock"
    echo "  OK all $COUNT locked archives reachable"
    exit 0
  fi
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
  if [ ! -f "$CACHE/$name" ]; then
    echo "  .. fetch $name"
    if ! curl -fsSL "$url" -o "$CACHE/$name.part"; then
      rm -f "$CACHE/$name.part"
      # 只对 Ubuntu 官方 pool 回退 snapshot（qgis.org 等第三方源没有快照服务）。
      snap=$(snapshot_url_for "$url")
      [ -n "$snap" ] || fail "cannot download $name from $url" "refresh $LOCK with --update-lock"
      echo "     pool miss — retry from snapshot $DEB_SNAPSHOT"
      curl -fsSL "$snap" -o "$CACHE/$name.part" || {
        rm -f "$CACHE/$name.part"
        fail "cannot download $name (pool and snapshot $DEB_SNAPSHOT)" \
             "refresh $LOCK with --update-lock on Ubuntu 26.04, or set PALEO_DEB_SNAPSHOT"
      }
    fi
    mv "$CACHE/$name.part" "$CACHE/$name"
  fi
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

# Debian 库在解包式部署下有两类 NEEDED 断点：
# 1) RUNPATH 硬编码系统绝对路径（libpulse → /usr/lib/.../pulseaudio，
#    vendored 树落空 → CI 实锤 pa_cstrerror undefined）：patchelf 改
#    $ORIGIN/pulseaudio；
# 2) alternatives 布局——blas/lapack 等本体在 <ma>/<subdir>/，顶层同名
#    符号链接由 update-alternatives/postinst 建，解包没有这步：为
#    <libdir>/<subdir>/lib*.so.N 在父目录补相对符号链接，运行期
#    LD_LIBRARY_PATH 才能命中（链接期由 CMake 侧 -rpath-link 覆盖）。
#    只补缺失项、按序取先（确定性、幂等）；限带版本号的 lib*.so.N
#    避免把插件类 unversioned .so 摊到顶层。
for ma in "$PREFIX"/usr/lib/*-linux-gnu; do
  [ -d "$ma/pulseaudio" ] || continue
  if command -v patchelf >/dev/null; then
    find "$ma" -maxdepth 1 -name 'libpulse*.so*' -type f -exec \
      patchelf --set-rpath '$ORIGIN/pulseaudio' {} +
  else
    echo "  !! patchelf absent — libpulse RUNPATH 未修（运行期靠下面符号链接兜底）"
  fi
done
#    逐 libdir（usr/lib 与 <ma>）只农场其直下子目录的 .so——SONAME 符号
#    链接（-type l，如 blas/libblas.so.3 → .so.3.12.1）也必须抬上来，
#    NEEDED 记的是 SONAME。
for libdir in "$PREFIX/usr/lib" "$PREFIX"/usr/lib/*-linux-gnu; do
  [ -d "$libdir" ] || continue
  find "$libdir" -mindepth 2 -maxdepth 2 \( -type f -o -type l \) -name 'lib*.so*' | \
  sort | while read -r lib; do
    b=$(basename "$lib"); sub=$(basename "$(dirname "$lib")")
    case "$sub" in *-linux-gnu) continue;; esac   # multiarch 目录不是 alternatives 子目录
    # -e 对悬空符号链接返回假——deb 可能自带顶层链接，-L 一并认作已存在
    [ -e "$libdir/$b" ] || [ -L "$libdir/$b" ] || ln -s "$sub/$b" "$libdir/$b"
  done
done

# Record the resolved closure for reproducibility/audit.
if [ "$UPDATE_LOCK" = 1 ]; then
  mv "$LOCK.tmp" "$LOCK"
  date -u +%Y%m%dT%H%M%SZ > "$SNAPSHOT_FILE" # snapshot 回退源的时间点（随锁提交）
fi
echo "  OK vendored $COUNT debs -> $PREFIX (lock: vendor/deb-closure.lock)"
echo "  NOTE set QGIS_PREFIX_PATH=$PREFIX/usr and LD_LIBRARY_PATH=$PREFIX/usr/lib/<multiarch>:$PREFIX/usr/lib (./paleo-dev does this)"
