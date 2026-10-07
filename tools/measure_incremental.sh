#!/usr/bin/env bash
# 方向70：增量构建基线测量——刷新 PLAN ET14「单文件改动增量 ≤60s」的实测
# 口径（BUILDING.md 原「3 目标 <1min」是 Phase 0 规模数据，当前 425+ .cpp）。
#
# 做三件事：
#   1) 确认 build 目录已全量（无改动增量构建 = no-op，否则提示先全量）；
#   2) touch 一个 app 源文件（默认 src/app/appcontext.cpp，组装根最小扇出
#      样本）后计时跑增量构建；
#   3) 解析 ninja 输出统计：执行步数 / 重编 TU 数 / 链接步数 / 墙钟 ms，
#      连同 TU 总数与比率写入 JSON。
#
# 口径：绝对时长仅记录（跨机不可比），比率（重编 TU / 总 TU、增量步数 /
# 总步数）才是跨机可比的回归口径。JSON 追加写（默认
# build/incremental-baseline.jsonl），趋势自管。
#
# 用法：tools/measure_incremental.sh [build-dir] [源文件] [输出jsonl]
#   build-dir 缺省 build；Windows 下需在 MSVC 环境（vcvars）里跑。
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
SRC="${2:-src/app/appcontext.cpp}"
OUT="${3:-$BUILD_DIR/incremental-baseline.jsonl}"
JOBS="${PALEO_JOBS:-8}"

[ -f "$SRC" ] || { echo "source not found: $SRC" >&2; exit 2; }
[ -f "$BUILD_DIR/build.ninja" ] || { echo "build dir not configured: $BUILD_DIR" >&2; exit 2; }

# ninja 总步数（全图，含 phony）：ET14 比率分母。
total_steps=$(ninja -C "$BUILD_DIR" -t targets all 2>/dev/null | wc -l)
total_tu=$(find src -name '*.cpp' | wc -l)

echo "== preflight: no-op incremental build (must be up to date) =="
cmake --build "$BUILD_DIR" --parallel "$JOBS" > /tmp/paleo-incr-pre.log 2>&1 || {
  cat /tmp/paleo-incr-pre.log >&2; echo "preflight build failed" >&2; exit 2; }
# [0/N] 是 glob 重扫/顺序声明（CONFIGURE_DEPENDS 的 Re-checking），非构建步。
if grep -qE '^\[[1-9][0-9]*/' /tmp/paleo-incr-pre.log; then
  echo "build tree was not up to date — rerun after this settling build" >&2
  exit 2
fi

echo "== touch $SRC + timed incremental build (-j$JOBS) =="
touch "$SRC"
start=$(date +%s%3N)
cmake --build "$BUILD_DIR" --parallel "$JOBS" > /tmp/paleo-incr.log 2>&1 || {
  cat /tmp/paleo-incr.log >&2; echo "incremental build failed" >&2; exit 2; }
wall_ms=$(( $(date +%s%3N) - start ))

steps=$(grep -cE '^\[[1-9][0-9]*/' /tmp/paleo-incr.log || true)
tus=$(grep -cE '^\[[1-9][0-9]*/.*Building CXX object' /tmp/paleo-incr.log || true)
links=$(grep -cE '^\[[1-9][0-9]*/.*(Linking|lib.*\.a)' /tmp/paleo-incr.log || true)

mkdir -p "$(dirname "$OUT")"
printf '{"date":"%s","source":"%s","tu_total":%d,"ninja_targets":%d,"steps_run":%d,"tu_recompiled":%d,"link_steps":%d,"wall_ms":%d,"tu_ratio":%.6f,"step_ratio":%.6f,"jobs":%s,"uname":"%s"}\n' \
  "$(date -Iseconds)" "$SRC" "$total_tu" "$total_steps" "$steps" "$tus" "$links" \
  "$wall_ms" "$(awk "BEGIN{print $tus/$total_tu}")" \
  "$(awk "BEGIN{print $steps/$total_steps}")" \
  "$JOBS" "$(uname -srm | tr ' ' '_')" >> "$OUT"

echo "== done =="
cat "$OUT" | tail -1
echo "ratios: tu $tus/$total_tu, steps $steps/$total_steps (absolute wall ${wall_ms}ms is machine-local, recorded only)"
