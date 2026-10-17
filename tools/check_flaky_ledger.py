#!/usr/bin/env python3
"""flaky 台账统计器（#501 绿灯通胀防回归：串行重跑放行必须有台账）。

背景：tools/ci_ctest_gate.sh（Linux）/paleo-dev.ps1（Windows）对非崩溃类
失败做一次串行/直跑复核，通过即放行。旧口径只打 ::warning——同一测试
反复靠重跑放行不会被累计、不会被升级。现在每次「重跑通过」放行都会
append 一条 JSON 行到 tools/flaky-ledger.jsonl（已 gitignore，本地台账），
本工具按测试名聚合命中次数：

  - 命中 >= --threshold（默认 3）→ flaky 嫌疑：默认档打 ::warning::（issue
    建议的警告档起步，不打断门禁）；--strict 升级为 exit 1（连续命中 N 次
    即硬红，供后续接线）。
  - --window-days 只统计近 N 天记录（默认不限）；--since 可选过滤。
  - 台账缺失/为空 → 提示并 exit 0（没有放行记录不是问题）。
  - 坏行（非法 JSON）计数提示但不打断统计——台账是 append-only 运行产物，
    不因个别坏行废掉全部数据；坏行占比过高（>50%）才 exit 2（树损坏）。

用法：
  python3 tools/check_flaky_ledger.py [--ledger PATH] [--threshold N]
         [--window-days D] [--strict] [--selftest]
清理：台账是本地证据，按周人工复审（issue #501 建议）；确认修复后可删行。
"""
import argparse
import datetime
import json
import sys
from collections import Counter
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")

REPO = Path(__file__).resolve().parents[1]
DEFAULT_LEDGER = REPO / "tools" / "flaky-ledger.jsonl"


def load_records(path):
    """→ (records, n_bad)；records = [{test, utc, ...}]。"""
    records, n_bad = [], 0
    p = Path(path)
    if not p.is_file():
        return records, n_bad
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
            if isinstance(rec, dict) and rec.get("test"):
                records.append(rec)
            else:
                n_bad += 1
        except ValueError:
            n_bad += 1
    return records, n_bad


def parse_utc(s):
    try:
        return datetime.datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except ValueError:
        return None


def summarize(records, threshold, window_days=None, now=None):
    """→ (suspects, per_test)；suspects = [(test, n)] 命中 >= threshold。"""
    now = now or datetime.datetime.now(datetime.timezone.utc)
    if window_days is not None:
        horizon = now - datetime.timedelta(days=window_days)
        records = [r for r in records
                   if (t := parse_utc(r.get("utc"))) is not None and t >= horizon]
    per_test = Counter(r["test"] for r in records)
    suspects = sorted((t, n) for t, n in per_test.items() if n >= threshold)
    return suspects, per_test


def run(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ledger", default=str(DEFAULT_LEDGER))
    ap.add_argument("--threshold", type=int, default=3,
                    help="同一测试命中次数达到该值即列为 flaky 嫌疑（默认 3）")
    ap.add_argument("--window-days", type=int, default=None,
                    help="只统计近 N 天记录（默认不限）")
    ap.add_argument("--strict", action="store_true",
                    help="flaky 嫌疑升级为 exit 1（默认只打 ::warning::）")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.threshold < 1:
        print("FAIL --threshold 必须 >= 1", file=sys.stderr)
        return 2
    records, n_bad = load_records(a.ledger)
    if not records and not n_bad:
        print(f"flaky 台账：{a.ledger} 无记录（没有串行重跑放行——正常）")
        return 0
    if n_bad > len(records):
        print(f"FAIL 台账坏行过多（{n_bad} 坏 / {len(records)} 好）——树损坏，人工检查",
              file=sys.stderr)
        return 2
    suspects, per_test = summarize(records, a.threshold, a.window_days)
    window = f"（近 {a.window_days} 天）" if a.window_days else ""
    print(f"flaky 台账：{len(records)} 条放行记录{window}，涉及 {len(per_test)} 个测试"
          + (f"，坏行 {n_bad} 条已跳过" if n_bad else ""))
    for test, n in sorted(per_test.items(), key=lambda kv: -kv[1]):
        mark = " ← flaky 嫌疑" if (test, n) in suspects else ""
        print(f"  {n:3d} 次  {test}{mark}")
    for test, n in suspects:
        print(f"::warning::{test} 靠串行重跑放行已 {n} 次（>= {a.threshold}）——"
              f"偶发竞态候选，按 #501 复审（修复或转间歇性登记），不应长期靠重跑绿灯")
    if suspects and a.strict:
        print(f"FAIL --strict：{len(suspects)} 个测试达到 flaky 阈值（"
              f"{', '.join(t for t, _ in suspects)}）")
        return 1
    return 0


# ---------------------------------------------------------------- selftest --
def selftest():
    import tempfile
    failures = []

    def write(lines):
        tmp = Path(tempfile.mkdtemp(prefix="flaky-")) / "ledger.jsonl"
        tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return tmp

    def rc_of(args, ledger):
        import contextlib, io as _io
        with contextlib.redirect_stdout(_io.StringIO()):
            return run(["--ledger", str(ledger)] + args)

    rec = lambda t, d: json.dumps({"test": t, "utc": d, "event": "serial-rerun-pass"})
    ledger_empty = write([])
    if rc_of([], ledger_empty) != 0:
        failures.append("空台账应绿")
    ledger_ok = write([rec("tst_a", "2026-10-01T00:00:00Z") for _ in range(2)]
                      + [rec("tst_b", "2026-10-02T00:00:00Z")])
    if rc_of([], ledger_ok) != 0:
        failures.append("低于阈值应绿（默认警告档）")
    ledger_hot = write([rec("tst_flaky", "2026-10-01T00:00:00Z") for _ in range(3)])
    if rc_of([], ledger_hot) != 0:
        failures.append("达到阈值默认档仍应绿（警告档起步，#501）")
    if rc_of(["--strict"], ledger_hot) != 1:
        failures.append("达到阈值 --strict 必须红")
    if rc_of(["--threshold", "5"], ledger_hot) != 0:
        failures.append("调高阈值后应回落绿")
    # 时间窗：老记录出窗后不再计数。
    if rc_of(["--window-days", "1"], ledger_hot) != 0:
        failures.append("全部出窗应绿")
    if rc_of(["--window-days", "1", "--strict"], ledger_hot) != 0:
        failures.append("全部出窗 --strict 也应绿")
    # 坏行容忍 + 占比过高判损坏。
    ledger_bad = write(["not json"] + [rec("tst_a", "2026-10-01T00:00:00Z")] * 3)
    if rc_of([], ledger_bad) != 0:
        failures.append("个别坏行应容忍（默认档仍按数据统计）")
    ledger_rot = write(["not json", "{bad"] * 2)
    if rc_of([], ledger_rot) != 2:
        failures.append("坏行占比过高必须 exit 2（树损坏）")
    # 阈值参数校验。
    if rc_of(["--threshold", "0"], ledger_ok) != 2:
        failures.append("--threshold 0 必须 exit 2")

    if failures:
        for f in failures:
            print("SELFTEST FAIL", f)
        return 1
    print("check_flaky_ledger selftest: PASS（9 场景：空/阈值/警告档/strict/"
          "时间窗/坏行容忍/树损坏/参数校验）")
    return 0


if __name__ == "__main__":
    sys.exit(run())
