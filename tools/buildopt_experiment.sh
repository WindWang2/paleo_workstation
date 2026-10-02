#!/usr/bin/env bash
# 构建期优化实验（goal/perf-systematize 簇4）：-O3 / LTO vs 现状 -O2。
# 同一 scratch build 三口径对比：
#   build/      现状 RelWithDebInfo -O2（基线，已存在）
#   build-x-o3  RelWithDebInfo -O3
#   build-x-lto RelWithDebInfo -O2 + -flto=8（链接期优化）
# 基准面：paleo_selfcheck perf --json（数据层基准组）+ 启动分段
# （measure_startup.sh——loader 段是 LTO 的理论收益面）。
# 产出：/tmp/buildopt_results/{o3,lto}/{perf.json,startup.json,startup_report.json}
set -uo pipefail

# 审计 01 L1：不再写死开发机路径——PALEO_ROOT / QGIS_PREFIX 可覆盖，默认取本仓库。
ROOT="${PALEO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
PREFIX="${QGIS_PREFIX:-$ROOT/vendor/superbuild/prefix}"
OUT="/tmp/buildopt_results"
mkdir -p "$OUT/o3" "$OUT/lto"

config_build() { # $1=builddir  $2=extra-cxxflags  $3=extra-linkflags
  cmake -S "$ROOT" -B "$ROOT/$1" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DQGIS_PREFIX="$PREFIX" \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g $2" \
    -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="$3" \
    > "$OUT/$1-configure.log" 2>&1
}

echo "[$(date +%T)] configure o3"
config_build build-x-o3 "-O3" ""
echo "[$(date +%T)] configure lto"
config_build build-x-lto "-flto=8 -ffat-lto-objects" "-flto=8"

for pair in "o3 build-x-o3" "lto build-x-lto"; do
  set -- $pair
  name=$1; dir=$2
  echo "[$(date +%T)] build $name"
  for attempt in 1 2 3; do
    # 本机工具链偶发 ICE/ld 崩：重试续传（BUILDING.md 口径）
    if cmake --build "$ROOT/$dir" -j8 --target paleo_selfcheck paleo \
        > "$OUT/$name-build.log" 2>&1; then
      break
    fi
    echo "[$(date +%T)] $name build attempt $attempt failed — retry"
    sleep 5
  done
  if [ ! -x "$ROOT/$dir/paleo_selfcheck" ] || [ ! -x "$ROOT/$dir/paleo" ]; then
    echo "[$(date +%T)] $name BUILD FAILED（如实记录）"
    tail -5 "$OUT/$name-build.log" > "$OUT/$name-FAILED.txt"
    continue
  fi
  echo "[$(date +%T)] bench $name"
  QGIS_PREFIX_PATH="$PREFIX" "$ROOT/$dir/paleo_selfcheck" perf --json "$OUT/$name/perf.json" \
    > "$OUT/$name/perf-stdout.log" 2>&1 || true
  QGIS_PREFIX_PATH="$PREFIX" QT_QPA_PLATFORM=offscreen \
    "$ROOT/tools/measure_startup.sh" "$ROOT/$dir/paleo" 7 "$OUT/$name/startup.json" \
    > "$OUT/$name/startup_report.json" 2> "$OUT/$name/startup-stderr.log" || true
  cp "$OUT/$name/startup_report.json" "$OUT/$name/startup_report_copy.json" 2>/dev/null || true
done
echo "[$(date +%T)] DONE — 基线（build/）的对照数据由主会话补跑"
