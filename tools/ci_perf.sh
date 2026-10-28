#!/usr/bin/env bash
# CI 非阻断 perf job（#136 / #81 余项）：只编 perf 标签测试，串行跑，产出计时 artifact。
#
# 必过门禁（linux job）用 -LE perf 排除墙钟预算类测试——它们对 runner 负载
# 敏感（tst_singlefactor_perf L 档 16.7 s vs 15 s 预算），确定性超限也不该
# 拖红合并门禁、更不该被 ci_ctest_gate.sh 再串行重跑 200 s。这里单独跑：
# 阈值一律不动，结果（JUnit + 原始日志）上传做趋势跟踪。
#
# goal/perf-maintenance-20261027 证据链收口（区分「没测试/没执行/没产物」
# 与「通过」，不再统一记成功）：
#   - ctest -N 发现失败或 0 个 perf 测试 → ::error + 非零退出（configure
#     正常的树恒有 perf 测试，空集=注册/发现漂移，绿了反而丢趋势数据）；
#   - 构建步显式 --parallel（默认 8，PALEO_JOBS 可调）——不限制时 Ninja
#     缺省吃满核数，违反本仓 -j8 纪律；
#   - JUnit 统一取 paleo-dev test 落盘的 Testing/ctest-junit.xml 拷贝出
#     artifact 位（此前经 "$@" 二次传 --output-junit，与 paleo-dev 自带
#     的同名 flag 叠加，哪个生效取决于参数序，产物路径不可靠）；跑完
#     校验 junit/summary 产物存在且非空——「跑绿但没产物」判红。
#
# 用法：tools/ci_perf.sh            （需已 configure 过 $BUILD_DIR，默认 build）
set -uo pipefail
build_dir="${BUILD_DIR:-build}"
out_dir="${PERF_OUT_DIR:-$build_dir/perf-results}"
mkdir -p "$out_dir"

# 1) perf 标签测试清单（configure 期即可得，无需先编译）。
#    先取完整输出并核对 ctest -N 自身退出码：失败（build 目录未配置/
#    损坏）不能因空清单而假绿。
list_out="$(ctest --test-dir "$build_dir" -N -L perf 2>&1)"
list_rc=$?
if [ "$list_rc" -ne 0 ]; then
  echo "::error::ctest -N -L perf 发现失败（rc=$list_rc）——build 目录未配置或损坏："
  echo "$list_out"
  exit 1
fi
mapfile -t tests < <(sed -nE 's/^ *Test +#[0-9]+: +([^ ]+).*$/\1/p' <<<"$list_out")
if [ "${#tests[@]}" -eq 0 ]; then
  echo "::error::没有 perf 标签测试（ctest -L perf -N 为空）——正常 configure 的树"
  echo "恒有 perf 测试注册，空集说明注册/发现漂移；按绿处理会静默丢失全部趋势数据。"
  echo "$list_out"
  exit 1
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
  # AGENTS.md -j8 纪律：不显式限并行时 Ninja 缺省吃满核数
  cmake --build "$build_dir" --target "${targets[@]}" --parallel "${PALEO_JOBS:-8}" || exit 1
fi

# 3) 串行跑（计时类不并行）。JUnit 由 paleo-dev test 统一落
#    $BUILD_DIR/Testing/ctest-junit.xml（这里不再经 "$@" 叠加第二个
#    --output-junit），跑完拷到 artifact 位并校验。
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
  ./paleo-dev test -L perf -j1 2>&1 |
  tee "$out_dir/perf-ctest.log"
rc=${PIPESTATUS[0]}
# 计时摘要（ctest 结果行）单独一份，便于肉眼对比
grep -E "Test +#[0-9]+:" "$out_dir/perf-ctest.log" > "$out_dir/perf-summary.txt" || true
junit_src="$build_dir/Testing/ctest-junit.xml"
junit_dst="$out_dir/perf-junit.xml"
if [ -f "$junit_src" ]; then
  cp "$junit_src" "$junit_dst"
else
  echo "::error::paleo-dev test 未落盘 JUnit（$junit_src 缺失）——perf 证据链断链"
  [ "$rc" -ne 0 ] || rc=1
fi
if [ "$rc" -eq 0 ]; then
  # 跑绿但产物缺失/为空 = 假绿：趋势 artifact 上传的是空目录
  if [ ! -s "$junit_dst" ]; then
    echo "::error::perf JUnit 产物缺失或为空（$junit_dst）——结果不可追溯，判红"
    rc=1
  fi
  if [ ! -s "$out_dir/perf-summary.txt" ]; then
    echo "::error::perf summary 产物缺失或为空——结果不可肉眼比对，判红"
    rc=1
  fi
fi
exit "$rc"
