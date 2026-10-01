#!/usr/bin/env bash
# PGO 两阶段实验（goal/perf-systematize 簇4）：
#   阶段1 build-x-pgo：-fprofile-generate 编 paleo_selfcheck，跑 perf 组产 .gcda
#   阶段2 同目录重配 -fprofile-use，重建，跑 perf 组对比
set -uo pipefail
ROOT="/home/kevin/projects/paleo_workstation/.worktrees/perf-systematize"
PREFIX="/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix"
OUT="/tmp/buildopt_results/pgo"
mkdir -p "$OUT"
DIR="$ROOT/build-x-pgo"

echo "[$(date +%T)] phase1: profile-generate"
cmake -S "$ROOT" -B "$DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX="$PREFIX" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fprofile-generate" > "$OUT/configure1.log" 2>&1
for a in 1 2 3; do
  cmake --build "$DIR" -j8 --target paleo_selfcheck > "$OUT/build1.log" 2>&1 && break
  echo "[$(date +%T)] build1 attempt $a failed — retry"; sleep 5
done
[ -x "$DIR/paleo_selfcheck" ] || { echo PHASE1_BUILD_FAILED; tail -3 "$OUT/build1.log"; exit 1; }
# 训练：跑 perf 组（数据层热路径）×2 轮
cd "$DIR"
for i in 1 2; do QGIS_PREFIX_PATH="$PREFIX" ./paleo_selfcheck perf >/dev/null 2>&1; done
echo "[$(date +%T)] phase2: profile-use"
cmake -S "$ROOT" -B "$DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX="$PREFIX" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fprofile-use -fprofile-correction" \
  > "$OUT/configure2.log" 2>&1
find "$DIR" -name '*.gcda' -exec cp {} "$OUT/" \; 2>/dev/null || true
# profile-use 编译需 .gcda 在对象树同路径——不清理直接重编
for a in 1 2 3; do
  cmake --build "$DIR" -j8 --target paleo_selfcheck paleo > "$OUT/build2.log" 2>&1 && break
  echo "[$(date +%T)] build2 attempt $a failed — retry"; sleep 5
done
[ -x "$DIR/paleo_selfcheck" ] || { echo PHASE2_BUILD_FAILED; tail -5 "$OUT/build2.log" > "$OUT/FAILED.txt"; exit 1; }
echo "[$(date +%T)] bench pgo"
QGIS_PREFIX_PATH="$PREFIX" "$DIR/paleo_selfcheck" perf --json "$OUT/perf.json" > "$OUT/perf-stdout.log" 2>&1 || true
QGIS_PREFIX_PATH="$PREFIX" QT_QPA_PLATFORM=offscreen "$ROOT/tools/measure_startup.sh" \
  "$DIR/paleo" 5 "$OUT/startup.json" > "$OUT/startup_report.json" 2>&1 || true
echo "[$(date +%T)] PGO DONE"
