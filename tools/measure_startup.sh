#!/usr/bin/env bash
# 启动分段测量（goal/perf-systematize 簇1）。
#
# 用法：
#   tools/measure_startup.sh <paleo 二进制> [轮数=7] [输出 JSON]
#
# 每轮 offscreen 起停真实 paleo 二进制（首帧即退 + PALEO_STARTUP_TRACE
# 落盘），汇总：
#   · 各段耗时中位数（绝对值，机器相关——记档参考，不作门）；
#   · 机器无关份额比率（qgis / 服务装配 / show→首帧 / loader），
#     可直接入 docs/perf/baselines/startup_ratios.json 当门基线。
# 全部数字实测；无拍脑袋预算。exit 0 = 测量完成，非 0 = 有轮次失败。
set -euo pipefail

BIN="${1:?用法: measure_startup.sh <paleo 二进制> [轮数] [out.json]}"
RUNS="${2:-7}"
OUT="${3:-}"

TMPDIR_RUN="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_RUN"' EXIT

export QT_QPA_PLATFORM=offscreen
export PALEO_STARTUP_EXIT_AFTER_FRAME=1

fail=0
for i in $(seq 1 "$RUNS"); do
  trace="$TMPDIR_RUN/run$i.json"
  export PALEO_STARTUP_TRACE="$trace"
  if ! "$BIN" >/dev/null 2>"$TMPDIR_RUN/run$i.err"; then
    echo "run $i: 进程异常退出（exit $?）——stderr 摘要：" >&2
    tail -3 "$TMPDIR_RUN/run$i.err" >&2 || true
    # offscreen 崩溃也可能是脏退出旗标残留（.running）——文件在就继续汇总
    if [ ! -s "$trace" ]; then fail=$((fail+1)); continue; fi
  fi
  if [ ! -s "$trace" ]; then
    echo "run $i: 无 trace 落盘" >&2
    fail=$((fail+1))
  fi
done

python3 - "$TMPDIR_RUN" "$RUNS" "$OUT" <<'PY'
import json, statistics, sys
from pathlib import Path

tmp, runs, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
docs = [json.loads(p.read_text()) for p in sorted(Path(tmp).glob("run*.json")) if p.stat().st_size]
if not docs:
    sys.exit("没有任何有效 trace 轮次")

def at(doc, name):
    for s in doc["segments"]:
        if s["name"] == name:
            return s["at_ms"]
    return None

def dur(doc, a, b):
    x, y = at(doc, a), at(doc, b)
    if x is None or y is None or y < x:
        return None
    return y - x

def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None

total = [at(d, "first_paint") for d in docs]
qgis = [dur(d, "pre_qt_ready", "qgis_app_ready") for d in docs]
svc = [dur(d, "qgis_app_ready", "services_ready") for d in docs]
paint = [dur(d, "window_shown", "first_paint") for d in docs]
loader = [d.get("process_to_main_ms", -1) for d in docs]

def share(num, den):
    pairs = [(n, t) for n, t in zip(num, den) if n is not None and t and t > 0]
    return statistics.median(n / t for n, t in pairs) if pairs else None

report = {
    "runs": len(docs),
    "segments_median_ms": {
        "process_to_main": med(loader),
        "pre_qt": med([dur(d, "main_entry", "pre_qt_ready") for d in docs]),
        "qgis_app_init": med(qgis),
        "service_assembly": med(svc),
        "theme": med([dur(d, "services_ready", "theme_ready") for d in docs]),
        "main_window_build": med([dur(d, "theme_ready", "main_window_ready") for d in docs]),
        "show_to_first_paint": med(paint),
        "main_to_first_frame": med(total),
    },
    # loader 份额分母 = loader + main→首帧（exec→首屏的全链路），与
    # tst_startup_trace::evaluateGates 同口径。
    "share_ratios": {
        "qgis_init_share_max": share(qgis, total),
        "service_assembly_share_max": share(svc, total),
        "show_to_paint_share_max": share(paint, total),
        "loader_share_max": share(loader, [l + t for l, t in zip(loader, total)]),
    },
    "per_run_ratios": [
        {
            "qgis": (dur(d, "pre_qt_ready", "qgis_app_ready") or 0) / max(at(d, "first_paint") or 1, 1),
            "svc": (dur(d, "qgis_app_ready", "services_ready") or 0) / max(at(d, "first_paint") or 1, 1),
            "paint": (dur(d, "window_shown", "first_paint") or 0) / max(at(d, "first_paint") or 1, 1),
            "loader": (d.get("process_to_main_ms", 0) or 0)
                / max(d.get("process_to_main_ms", 0) + (at(d, "first_paint") or 1), 1),
            "total": at(d, "first_paint"),
        }
        for d in docs
    ],
}
print(json.dumps(report, indent=2, ensure_ascii=False))
if out:
    Path(out).write_text(json.dumps(report["share_ratios"], indent=2) + "\n")
    print(f"→ 份额比率已写 {out}", file=sys.stderr)
PY

[ "$fail" -eq 0 ] || { echo "$fail/$RUNS 轮失败" >&2; exit 1; }
