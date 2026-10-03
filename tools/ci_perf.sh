#!/usr/bin/env bash
# CI 非阻断 perf job（#136 / #81 余项）：只编 perf 标签测试，串行跑，产出计时 artifact。
#
# 必过门禁（linux job）用 -LE perf 排除墙钟预算类测试——它们对 runner 负载
# 敏感（tst_singlefactor_perf L 档 16.7 s vs 15 s 预算），确定性超限也不该
# 拖红合并门禁、更不该被 ci_ctest_gate.sh 再串行重跑 200 s。这里单独跑：
# 阈值一律不动，结果（JUnit + 原始日志）上传做趋势跟踪。
#
# 用法：tools/ci_perf.sh            （需已 configure 过 $BUILD_DIR，默认 build）
set -uo pipefail
build_dir="${BUILD_DIR:-build}"
out_dir="${PERF_OUT_DIR:-$build_dir/perf-results}"
mkdir -p "$out_dir"

# 1) perf 标签测试清单（configure 期即可得，无需先编译）
mapfile -t tests < <(ctest --test-dir "$build_dir" -N -L perf |
                     sed -nE 's/^ *Test +#[0-9]+: +([^ ]+).*$/\1/p')
if [ "${#tests[@]}" -eq 0 ]; then
  echo "::warning::没有 perf 标签测试（ctest -L perf -N 为空）"
  exit 0
fi
echo "perf tests (${#tests[@]}): ${tests[*]}"

# 2) 只编这些目标（同名可执行文件；非目标的 ctest 项跳过）
targets=()
# Ninja 的 `--target help` 不列可执行目标，优先用 `ninja -t targets all`
if [ -f "$build_dir/build.ninja" ] && command -v ninja >/dev/null; then
  all_targets="$(ninja -C "$build_dir" -t targets all 2>/dev/null)"
else
  all_targets="$(cmake --build "$build_dir" --target help 2>/dev/null)"
fi
for t in "${tests[@]}"; do
  if grep -qE "(^|[[:space:]])${t}(:|[[:space:]]|$)" <<<"$all_targets"; then
    targets+=("$t")
  fi
done
echo "build targets (${#targets[@]}): ${targets[*]}"
if [ "${#targets[@]}" -gt 0 ]; then
  cmake --build "$build_dir" --target "${targets[@]}" || exit 1
fi

# 3) 串行跑（计时类不并行），JUnit 落盘做趋势
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
  ./paleo-dev test -L perf -j1 --output-junit "$PWD/$out_dir/perf-junit.xml" 2>&1 |
  tee "$out_dir/perf-ctest.log"
rc=${PIPESTATUS[0]}
# 计时摘要（ctest 结果行）单独一份，便于肉眼对比
grep -E "Test +#[0-9]+:" "$out_dir/perf-ctest.log" > "$out_dir/perf-summary.txt" || true
exit "$rc"
