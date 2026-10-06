#!/usr/bin/env bash
# CI 测试门禁（#81）：并行跑一轮；失败时只对「非崩溃」失败串行重跑一次。
#
# 旧策略「失败项串行重跑通过即放行」会把 SEGFAULT / abort 当作负载抖动
# 吞掉（run 36825437562：tst_factorworkflow 并行轮 SegFault，串行轮变
# 断言失败）。崩溃 = 内存/生命周期缺陷，与负载无关，一律直接判红。
#
# 用法：tools/ci_ctest_gate.sh <ctest 选择参数...>（如 -LE 'core|perf' / -L core -LE perf）
set -uo pipefail

build_dir="${BUILD_DIR:-build}"
log="$(mktemp -t ctest-gate.XXXXXX.log)"

# #230：并行度交给 paleo-dev 的 min(核数, 8) 缺省（AGENTS.md -j8 上限）。
./paleo-dev test "$@" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
if [ "$rc" -eq 0 ]; then
  exit 0
fi

# ctest 结果行形如：
#   17/117 Test #17: tst_factorworkflow ....***Exception: SegFault  2.69 sec
#   Test #9: foo ....Subprocess aborted***Exception:   0.10 sec
crash_re='\*\*\*Exception|Subprocess aborted|Child aborted|Subprocess killed|Illegal|SegFault'
if grep -E "Test +#[0-9]+:" "$log" | grep -E "$crash_re"; then
  echo "::error::并行轮出现崩溃（见上），崩溃不按负载抖动重跑放行"
  exit 1
fi

echo "::warning::并行轮有断言/超时失败，串行重跑失败项一次以排除负载抖动（重跑通过仍留痕）"
# 保留第一轮 QtTest 输出；串行 ctest 会覆盖同名 -o 文件。
first_run_dir="$build_dir/Testing/qtest-first-run"
if [ -d "$first_run_dir" ]; then
  rm -rf "$build_dir/Testing/qtest-first-run-initial"
  cp -a "$first_run_dir" "$build_dir/Testing/qtest-first-run-initial"
fi
# #136：perf 标签（墙钟预算）不重跑——确定性超限串行重跑只会再烧 200 s 后
# 照样红；perf 在独立的非阻断 linux-perf job 里跑（tools/ci_perf.sh）。
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
  ctest --test-dir "$build_dir" -j1 --rerun-failed -LE perf --output-on-failure
