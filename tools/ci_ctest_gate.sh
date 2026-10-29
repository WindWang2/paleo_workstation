#!/usr/bin/env bash
# CI 测试门禁（#81）：并行跑一轮；失败时只对「非崩溃」失败串行重跑一次。
#
# 旧策略「失败项串行重跑通过即放行」会把 SEGFAULT / abort 当作负载抖动
# 吞掉（run 36825437562：tst_factorworkflow 并行轮 SegFault，串行轮变
# 断言失败）。崩溃 = 内存/生命周期缺陷，与负载无关，一律直接判红。
#
# #501 绿灯通胀防回归（两处接线）：
#   1) 串行重跑放行记账：重跑通过时把本轮失败项逐条 append 到
#      tools/flaky-ledger.jsonl（gitignore 不入库；CI linux job 经 actions/cache
#      restore/save 跨 run 持久累计），并跑
#      tools/check_flaky_ledger.py 输出聚合——同一测试累计命中 ≥3 次打
#      ::warning::（警告档起步；PALEO_FLAKY_STRICT=1 传 --strict 升红）。
#      放行不再无痕。
#   2) env_redset STALE 接线：重跑前按 tools/env_redset.json 分类本轮红集
#      （--strict 口径）。STALE（exempt 项本轮通过 → 名单可收缩）默认
#      ::warning:: 过渡档；PALEO_REDSET_STALE=strict 升级为 ::error:: 红。
#      REGRESSED/UNEXPECTED 仍走下方既有串行重跑路径裁决。
#
# #760 Windows/Linux 门禁语义对齐：崩溃 markers、扫描范围、豁免放行、超时判红
# 四项规则收敛到 tools/ci-gate-rules.json 单一真源（两端同读；内联副本仅是
# python 不可用时的回落，一致性由 ctest 项 ci_gate_rules 看守）：
#   ① 崩溃 markers 补 ***Timeout（挂死不按负载抖动放行，Windows 侧一直如此）；
#   ② 崩溃判定只扫 ctest 结果行，不扫测试自身输出（Windows 侧此前扫整份日志，
#      测试打印 "Illegal" 字样即假红）；
#   ③ 豁免红不进重跑集、不进 flaky 台账（Windows 侧早已如此，Linux 侧此前全量
#      重跑——环境债洗白混进 #501 的真 flaky 信号）；
#   ④ 超时重跑差异随①②一并消失。
#
# 用法：tools/ci_ctest_gate.sh <ctest 选择参数...>（如 -LE 'core|perf' / -L core -LE perf）
set -uo pipefail

build_dir="${BUILD_DIR:-build}"
log="$(mktemp -t ctest-gate.XXXXXX.log)"
script_dir="$(cd "$(dirname "$0")" && pwd)"
ledger="$script_dir/flaky-ledger.jsonl"

# #230：并行度交给 paleo-dev 的 min(核数, 8) 缺省（AGENTS.md -j8 上限）。
./paleo-dev test "$@" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
# goal/perf-maintenance-20261027：选择器命中 0 个测试时 ctest 退出 0——
# 「没跑任何测试」不等于「通过」（注册/发现漂移会静默变绿）。正常注册的
# 树在车道选择器下非空；空集在此判红。
if grep -q "No tests were found" "$log"; then
  echo "::error::ctest 选中 0 个测试（选择参数：$*）——测试注册或选择器漂移，不是通过"
  exit 1
fi
if [ "$rc" -eq 0 ]; then
  exit 0
fi

# ctest 结果行形如：
#   17/117 Test #17: tst_factorworkflow ....***Exception: SegFault  2.69 sec
#   Test #9: foo ....Subprocess aborted***Exception:   0.10 sec
# #760：崩溃 markers 与「只扫 ctest 结果行」的口径单一真源在
# tools/ci-gate-rules.json，与 Windows paleo-dev.ps1 同读一份（此前两端各自演化：
# Linux 漏 ***Timeout、Windows 扫整份日志）。python3 不可用时退回内联副本；
# 副本与 JSON 的一致性由 ctest 项 ci_gate_rules 每轮看守，防悄悄漂移。
crash_re="$(python3 "$script_dir/ci_gate_rules.py" --regex 2>/dev/null || true)"
if [ -z "$crash_re" ]; then
  crash_re='\*\*\*Exception|\*\*\*Timeout|Subprocess aborted|Child aborted|Subprocess killed|Illegal|SegFault'
fi
result_line_re="$(python3 "$script_dir/ci_gate_rules.py" --result-line-regex 2>/dev/null || true)"
if [ -z "$result_line_re" ]; then
  result_line_re='Test +#[0-9]+:'
fi
if grep -E "$result_line_re" "$log" | grep -E "$crash_re"; then
  echo "::error::并行轮出现崩溃（见上），崩溃不按负载抖动重跑放行"
  exit 1
fi

# ---- #501(2)：env_redset 分类（--strict 口径；只读 junit，不改变重跑裁决）----
junit="$build_dir/Testing/ctest-junit.xml"
redset_out=""
redset_rc=0
redset_ran=0
unexpected_file="$build_dir/Testing/env-redset-unexpected.txt"
if command -v python3 >/dev/null 2>&1 && [ -f "$junit" ]; then
  redset_ran=1
  route="${PALEO_REDSET_ROUTE:-}"
  if [ -z "$route" ]; then
    if [ -d vendor/prefix/usr ] || [ -d vendor/superbuild/prefix ]; then
      route=vendored
    else
      route=localdeps
    fi
  fi
  # #760：--emit-unexpected 把不放行集合（REGRESSED|UNEXPECTED，--strict 下含
  # STALE）落盘，供下方重跑集过滤用（与 paleo-dev.ps1 同口径）。
  rm -f "$unexpected_file"
  redset_out="$(python3 "$script_dir/check_env_redset.py" \
    --build "$build_dir" --junit "$junit" --route "$route" --strict \
    --emit-unexpected "$unexpected_file" 2>&1)"
  redset_rc=$?
  echo "$redset_out"
  if [ "$redset_rc" -ne 0 ]; then
    if printf '%s\n' "$redset_out" | grep -q '^STALE'; then
      if [ "${PALEO_REDSET_STALE:-warn}" = "strict" ]; then
        echo "::error::env_redset STALE——exempt 项本轮通过，名单该收缩（--strict 红）。放行通道关闭，修 tools/env_redset.json"
        exit 1
      fi
      echo "::warning::env_redset STALE——exempt 项本轮通过，名单可收缩（过渡档只警告；PALEO_REDSET_STALE=strict 升红，#501）"
    fi
    if printf '%s\n' "$redset_out" | grep -qE '^(REGRESSED|UNEXPECTED)'; then
      echo "::warning::env_redset 有未豁免红/环境债回归——仍按下方串行重跑裁决（重跑再红即红），分类明细见上"
    fi
  fi
fi

# ---- #501(1)：记账准备——重跑前先留下失败项名单（--rerun-failed 读的就是它）----
failed_names="$(ctest --test-dir "$build_dir" -N --rerun-failed -LE perf 2>/dev/null \
  | sed -nE 's/^ *Test #[0-9]+: *([^ ]+).*/\1/p' | sort -u)"

echo "::warning::并行轮有断言/超时失败，串行重跑失败项一次以排除负载抖动（重跑通过仍留痕——记入 tools/flaky-ledger.jsonl）"
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

# ---- #760：重跑集 = env_redset 不放行集合（与 Windows paleo-dev.ps1 同口径）----
# EXEMPT（已登记环境债）与 DOCUMENTED（#475 文档化红项）按设计分诊放行：既不重跑
# 也不入 flaky 台账。此前 Linux 侧全量重跑——环境债洗白会被记成 flaky（#501），
# 把「已知环境债务」混进「真 flaky」信号；豁免红重跑再红还会让本 gate 判红，
# 豁免在 Linux 侧形同虚设。python/junit 缺位、或检查器非零退出却没给出不放行
# 名单（异常/解析失败）时不过滤——退化到 #760 前的全量重跑，而不是放行。
rerun_names="$failed_names"
if [ "$redset_ran" -eq 1 ] && { [ "$redset_rc" -eq 0 ] || [ -s "$unexpected_file" ]; }; then
  keep_file="$build_dir/Testing/env-redset-keep.txt"
  if [ "$redset_rc" -eq 0 ]; then
    # 无 REGRESSED/UNEXPECTED/STALE → 本轮失败项全部按设计放行
    : > "$keep_file"
  else
    # --strict 会把 STALE 也写进不放行文件；STALE 项本轮是通过的（不在失败集里），
    # 从 keep 集剔除不改变放行语义（同 paleo-dev.ps1 口径）。
    stale_names="$(printf '%s\n' "$redset_out" | sed -nE 's/^STALE[[:space:]]+([^:]+):.*/\1/p' | sort -u)"
    if [ -n "$stale_names" ]; then
      grep -vxF -f <(printf '%s\n' "$stale_names") "$unexpected_file" > "$keep_file" || true
    else
      cp "$unexpected_file" "$keep_file"
    fi
  fi
  if [ -s "$keep_file" ]; then
    rerun_names="$(printf '%s\n' "$failed_names" | grep -xF -f "$keep_file" || true)"
  else
    rerun_names=""
  fi
fi
if [ -z "$rerun_names" ]; then
  echo "env_redset: 本轮失败项均为名单内豁免/文档化放行——不进重跑集、不记 flaky 台账（#760）"
  exit 0
fi
rerun_re="^($(printf '%s\n' "$rerun_names" | paste -sd'|' -))$"
# #136：perf 标签（墙钟预算）不重跑——确定性超限串行重跑只会再烧 200 s 后
# 照样红；perf 在独立的非阻断 linux-perf job 里跑（tools/ci_perf.sh）。
# #397：串行重跑也走 ./paleo-dev test——裸 ctest 丢失 vendored 环境注入
#（QGIS_PREFIX_PATH/deb 库路径/GDAL_DRIVER_PATH），重跑裁决与第一轮
# 不同条件：加载 flake 变硬红、栅格类静默丢驱动。
# #760：用 -R 显式点名（替代 --rerun-failed）——重跑集 = 非豁免的失败项，
# 豁免项既不重跑也不入台账；名单与 --rerun-failed 同源（同一次 -N 抽取）。
./paleo-dev test -j1 -R "$rerun_re" -LE perf
rc2=$?
# #501：放行必须留痕——持久 JSONL 台账（累计命中
# 由 check_flaky_ledger.py 聚合点名）+ 逐项 annotation / step summary /
# 构建目录台账（PR 评审面可见）。首轮红、重跑绿 = flaky，反复入账升硬红。
if [ "$rc2" -eq 0 ] && [ -n "$rerun_names" ]; then
  # #757：ctest 在重跑全通过后**不清** LastTestsFailed.log（最小工程实验证实）——
  # 首轮失败名单会残留成「最终失败集」上传，triage 据此把已洗白的 flaky 误判成
  # 真红，而 flaky 汇总又因它仍在失败集里而漏掉该项。重跑放行即删，让 artifact
  # 反映真实结论（triage 侧另有 FirstRunFailed.log + 最新 tag Test.xml 口径兜底，
  # 见 tools/ci_triage_reds.py 的 E-7）。
  rm -f "$build_dir/Testing/Temporary/LastTestsFailed.log"
  utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  lane="$(printf '%s' "$*" | tr -d '"\\')"
  n=0
  for t in $rerun_names; do
    printf '{"test":"%s","utc":"%s","event":"serial-rerun-pass","lane":"%s"}\n' \
      "$t" "$utc" "$lane" >> "$ledger"
    n=$((n + 1))
  done
  {
    echo "=== ${utc} run=${GITHUB_RUN_ID:-local} ==="
    printf '%s\n' $rerun_names
  } >> "$build_dir/Testing/flaky-ledger.txt"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
      echo "## flaky 直跑放行（#501，${n} 项）"
      echo ""
      echo "首轮失败、串行重跑通过——已放行但**须复审**（同一测试反复入账应升级为硬红）："
      echo ""
      printf -- '- `%s`\n' $rerun_names
    } >> "$GITHUB_STEP_SUMMARY"
  fi
  for t in $rerun_names; do
    echo "::warning title=flaky 放行 (${n} 项)::$t 首轮失败、重跑通过——已记入 flaky 台账"
  done
  echo "::warning::串行重跑放行 $n 项（已逐条记入 tools/flaky-ledger.jsonl；同一测试累计 ≥3 次将被点名）"
  echo "flaky 台账已追加 ${n} 项：$build_dir/Testing/flaky-ledger.txt"
  if command -v python3 >/dev/null 2>&1; then
    flaky_args=""
    if [ "${PALEO_FLAKY_STRICT:-0}" = "1" ]; then flaky_args="--strict"; fi
    # strict 档不得吞掉检查器的非零退出（此前 `|| true` 让 --strict 形同虚设）；
    # 警告档检查器本身只打 ::warning:: 返回 0，行为不变。
    if ! python3 "$script_dir/check_flaky_ledger.py" $flaky_args; then
      if [ -n "$flaky_args" ]; then
        echo "::error::flaky 台账 strict 检查失败（PALEO_FLAKY_STRICT=1）——同一测试累计入账已达阈值"
        rc2=1
      else
        echo "::warning::check_flaky_ledger.py 非零退出（警告档，不阻断）"
      fi
    fi
  fi
fi
exit "$rc2"
