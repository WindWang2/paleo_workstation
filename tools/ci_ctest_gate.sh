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
# #501：串行重跑会覆盖 LastTestsFailed.log，先把首轮失败名单留住——重跑通过
# 的项即「本轮 flaky 放行集合」，必须写台账（此前只有一行 warning，无从复审）。
first_failed_log="$build_dir/Testing/Temporary/FirstRunFailed.log"
cp -f "$build_dir/Testing/Temporary/LastTestsFailed.log" "$first_failed_log" 2>/dev/null || true
# #136：perf 标签（墙钟预算）不重跑——确定性超限串行重跑只会再烧 200 s 后
# 照样红；perf 在独立的非阻断 linux-perf job 里跑（tools/ci_perf.sh）。
# #397：串行重跑也走 ./paleo-dev test——裸 ctest 丢失 vendored 环境注入
#（QGIS_PREFIX_PATH/deb 库路径/GDAL_DRIVER_PATH），重跑裁决与第一轮
# 不同条件：加载 flake 变硬红、栅格类静默丢驱动。
./paleo-dev test -j1 --rerun-failed -LE perf
rerun_rc=$?

# #501：放行必须留痕——逐项 annotation（PR 评审面可见）+ step summary +
# 构建目录台账。首轮红、重跑绿 = flaky，同一测试反复入账应升级为硬红。
if [ "$rerun_rc" -eq 0 ] && [ -s "$first_failed_log" ]; then
  mapfile -t released < <(sed -E 's/^[0-9]+://' "$first_failed_log" | tr -d '\r' | sed '/^$/d')
  if [ "${#released[@]}" -gt 0 ]; then
    {
      echo "=== $(date -u +%Y-%m-%dT%H:%M:%SZ) run=${GITHUB_RUN_ID:-local} ==="
      printf '%s\n' "${released[@]}"
    } >> "$build_dir/Testing/flaky-ledger.txt"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
      {
        echo "## flaky 直跑放行（#501，${#released[@]} 项）"
        echo ""
        echo "首轮失败、串行重跑通过——已放行但**须复审**（同一测试反复入账应升级为硬红）："
        echo ""
        printf -- '- `%s`\n' "${released[@]}"
      } >> "$GITHUB_STEP_SUMMARY"
    fi
    for t in "${released[@]}"; do
      echo "::warning title=flaky 放行 (${#released[@]} 项)::$t 首轮失败、重跑通过——已记入 flaky 台账"
    done
    echo "flaky 台账已追加 ${#released[@]} 项：$build_dir/Testing/flaky-ledger.txt"
  fi
fi
exit "$rerun_rc"
