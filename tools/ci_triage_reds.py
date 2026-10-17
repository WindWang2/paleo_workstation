#!/usr/bin/env python3
# CI 红项分诊（#497/#475/#484-P2）。
#
# 用法：
#   ci_triage_reds.py --jobs jobs.json --logs linux=DIR --logs windows=DIR ...
#   ci_triage_reds.py --validate
#   ci_triage_reds.py --selftest
#   未知参数一律 exit 2（与 check_layering.py 同约定——参数被静默忽略即假绿）。
#
# 背景（#497）：master 连续 10+ 次 push 全红仍持续合入，绿灯失去门禁语义。
# 与其让「全红」吞掉一切信号，不如把红拆成两类：
#   已知红项（docs/ci/known-reds.json 登记、带 issue 链接与复审期限）
#     → ::warning 降级呈现，摘要链接 issue；
#   未知红项 → ::error，本脚本 exit 1（triage job 红 = 有新红，需分诊）。
# 已知红项只许「只缩不涨」：golden 类预算超基线即红（#475）；测试转绿即提示
# 收缩（STALE）；复审到期（review_due）不再降级——到期要么续期要么转正式修复。
#
# 数据面：
#   --jobs   GitHub Actions `gh api .../actions/runs/<id>/jobs` 输出。continue-on-error
#            job（linux-perf/linux-asan）的真实 conclusion 只有这里可见（needs
#            上下文里它们恒 success）——#484 P2「观察档静默腐烂」的轮询点。
#   --logs   LANE=DIR，DIR 是该 lane 上传的 build/Testing artifact 解包目录：
#            - **/LastTestsFailed.log   最终失败测试（串行重跑后仍红 = 真红）
#            - **/FirstRunFailed.log    首轮失败（重跑洗白 = flaky，只汇总）
#            - **/Test.xml              ctest XML（Status="failed" 的并集口径）
#            - 任意 *.txt/*.log         GOLDEN-SUMMARY / GOLDEN-DIFF / 「N/48 抽样点
#              超容差」明细行（#475 golden 分诊明细，tests/tst_wellcomposite_visual
#              输出）。
#
# 语义：
#   gating lane（lint/linux/windows）：
#     job 红 + 无测试级红 => 构建/安装/门禁级红，不可分诊（job scope 条目除外）
#     => 未知红 exit 1；已知红 warning 降级。
#   observation lane（linux-perf/linux-asan，continue-on-error by design）：
#     红 = ::warning + 摘要（追踪不阻断——#484 P2 状态追踪；从未跑进编译步的
#     asan 前科靠这里每轮点名）。
#   exit：0 = 分诊通过（只有已知红/观察红）；1 = 存在未知红/预算超涨/复审到期；
#         2 = 名单或参数坏（配置错不能静默变绿）。

import io
import json
import os
import re
import sys
import tempfile
import contextlib

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REDS = os.path.join(REPO, "docs", "ci", "known-reds.json")

ENTRY_FIELDS = ("id", "scope", "match", "issue", "reason", "owner", "added", "review_due")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
BUDGET_METRICS = {"golden_points_over"}  # 只缩不涨的计数预算（#475）
LAST_FAILED_RE = re.compile(r"^\s*\d+\s*:\s*(.+?)\s*$")
GOLDEN_LINE_RE = re.compile(r"GOLDEN-(SUMMARY|DIFF)\b.*|^\s*\d+/\d+\s*抽样点超容差.*$")
# 两种形态：结构行 `GOLDEN-SUMMARY over=N/M`（tests/tst_wellcomposite_visual 输出）
# 与 QtTest 失败消息 `N/M 抽样点超容差`。
GOLDEN_OVER_RE = re.compile(
    r"GOLDEN-SUMMARY\s+over=\s*(\d+)\s*/\s*(\d+)"
    r"|(\d+)\s*/\s*(\d+)\s*抽样点超容差")


def fail2(msg):
    print(f"FAIL {msg}", file=sys.stderr)
    return 2


def load_reds(path=None):
    """读 + 校验 docs/ci/known-reds.json；结构坏 exit 2（哑名单不能静默降级）。"""
    path = path or DEFAULT_REDS
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        return None, fail2(f"known-reds 名单不可用：{path}: {exc}")
    try:
        lanes = data["lanes"]
        gating = [str(x) for x in lanes["gating"]]
        observation = [str(x) for x in lanes["observation"]]
        raw_entries = data["entries"]
        assert isinstance(raw_entries, list)
    except (KeyError, TypeError, AssertionError):
        return None, fail2(f"known-reds 名单缺 lanes.gating/lanes.observation/entries：{path}")
    all_lanes = gating + observation
    if not gating:
        return None, fail2("known-reds lanes.gating 为空——必过车道清单缺失")
    if len(set(all_lanes)) != len(all_lanes):
        return None, fail2("known-reds lanes 有重复车道名")
    entries = {}
    for e in raw_entries:
        if not isinstance(e, dict):
            return None, fail2(f"known-reds 条目非对象：{e!r}")
        for k in ENTRY_FIELDS:
            if not str(e.get(k, "")).strip():
                return None, fail2(f"known-reds 条目缺必填字段 {k}：{e!r}")
        if e["scope"] not in ("test", "job"):
            return None, fail2(f"known-reds 条目 scope 必须 test|job：{e!r}")
        if not NAME_RE.match(str(e["match"])):
            return None, fail2(f"known-reds match 非法（测试/作业名，禁通配）：{e['match']!r}")
        if not (isinstance(e["issue"], int) and e["issue"] > 0):
            return None, fail2(f"known-reds issue 必须是正整数（issue 链接是降级的强制代价）：{e!r}")
        for d in ("added", "review_due"):
            if not DATE_RE.match(str(e[d])):
                return None, fail2(f"known-reds {d} 必须 YYYY-MM-DD：{e!r}")
        if str(e["review_due"]) < str(e["added"]):
            return None, fail2(f"known-reds review_due 早于 added：{e!r}")
        lanes_of_entry = e.get("lanes", list(all_lanes))
        if not isinstance(lanes_of_entry, list) or not lanes_of_entry \
                or any(l not in all_lanes for l in lanes_of_entry):
            return None, fail2(f"known-reds lanes 引用未知车道：{e!r}")
        budget = e.get("budget")
        if budget is not None:
            try:
                assert budget["metric"] in BUDGET_METRICS
                assert isinstance(budget["value"], int) and budget["value"] >= 0
            except (KeyError, TypeError, AssertionError):
                return None, fail2(f"known-reds budget 非法（metric ∈ "
                                   f"{sorted(BUDGET_METRICS)}, value ≥ 0）：{e!r}")
        if e["match"] in entries:
            return None, fail2(f"known-reds match 重复：{e['match']}")
        entries[e["match"]] = dict(e, lanes=lanes_of_entry)
    return {"gating": gating, "observation": observation, "entries": entries}, None


# ---------------- 数据面解析 ----------------

def _walk_files(root):
    for dirpath, _dirs, files in os.walk(root):
        for f in sorted(files):
            yield os.path.join(dirpath, f)


def parse_last_failed(root):
    """**/LastTestsFailed.log → set(测试名)。格式：每行 `<#> : <name>`。"""
    names = set()
    for p in _walk_files(root):
        if os.path.basename(p) != "LastTestsFailed.log":
            continue
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    m = LAST_FAILED_RE.match(line)
                    if m:
                        names.add(m.group(1))
        except OSError:
            continue
    return names


def parse_test_xml(root):
    """**/Test.xml 里 Status="failed" 的 <Test><Name>（与 LastTestsFailed 并集）。"""
    names = set()
    import xml.etree.ElementTree as ET
    for p in _walk_files(root):
        if os.path.basename(p) != "Test.xml":
            continue
        try:
            tree = ET.parse(p)
        except (ET.ParseError, OSError):
            continue
        for el in tree.iter("Test"):
            if el.get("Status") == "failed":
                name_el = el.find("Name")
                if name_el is not None and name_el.text:
                    names.add(name_el.text.strip())
    return names


def parse_first_failed(root):
    """**/FirstRunFailed.log（ci_ctest_gate.sh 留存的首轮失败）→ flaky 候选。"""
    names = set()
    for p in _walk_files(root):
        if os.path.basename(p) == "FirstRunFailed.log":
            try:
                with open(p, encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        m = LAST_FAILED_RE.match(line)
                        if m:
                            names.add(m.group(1))
            except OSError:
                continue
    return names


def collect_golden_details(root):
    """golden 明细行（GOLDEN-SUMMARY/GOLDEN-DIFF/「N/N 抽样点超容差」）→ [行]。"""
    lines = []
    for p in _walk_files(root):
        if not p.endswith((".txt", ".log")) or os.path.getsize(p) > 20 * 1024 * 1024:
            continue
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    if GOLDEN_LINE_RE.search(line):
                        lines.append(line.strip()[:400])
        except OSError:
            continue
    return lines


def golden_over_count(test_name, details):
    """golden 预算核数：该测试明细行里最大的 over=N/M（取最坏值）。无明细 → None。

    归属：带测试名的行（QtTest 失败消息）只认该测试；GOLDEN-SUMMARY 结构行
    目前仅 tst_wellcomposite_visual 一处输出，全局计入（多 golden 测试出现时
    须给结构行补测试名再收紧归属）。
    """
    best = None
    for line in details:
        if "GOLDEN-SUMMARY" not in line and test_name not in line:
            continue
        m = GOLDEN_OVER_RE.search(line)
        if not m:
            continue
        num = m.group(1) or m.group(3)
        if num and (best is None or int(num) > best):
            best = int(num)
    return best


def parse_jobs(path):
    """gh api jobs 输出 → {job名: conclusion}（job 名即 lane 名，本仓无 name: 覆写）。

    解析失败返回 None（调用方 exit 2）——空映射不能当「全绿」静默放行。
    """
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        return {str(j["name"]): j.get("conclusion") for j in data.get("jobs", [])}
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"FAIL jobs 解析失败：{exc}", file=sys.stderr)
        return None


# ---------------- 分诊 ----------------

def issue_link(entry):
    return f"#{entry['issue']}"


def classify(reds, jobs, lane_logs, today=None, out=None, summary_fh=None):
    """分诊主流程。返回 exit code（0/1）。

    jobs: {lane: conclusion or None}；lane_logs: {lane: 日志根目录 or None}。
    两遍走：先解析全部日志（测试红集/flaky/golden 明细），再按 job 结论分诊——
    golden 明细与 STALE 判定不依赖车道迭代顺序。
    """
    out = out or sys.stdout
    today = today or __import__("datetime").date.today().isoformat()
    gating, observation = reds["gating"], reds["observation"]
    entries = reds["entries"]
    all_lanes = gating + observation
    rc = 0
    hit_matches = set()
    known_warn, unknown_err, obs_warn, flaky_names, golden_details = [], [], [], set(), []
    lane_failed = {}

    # ---- 第一遍：日志解析（绿 lane 也解析——flaky/STALE/golden 明细照收）----
    for lane in all_lanes:
        logs = lane_logs.get(lane)
        if not (logs and os.path.isdir(logs)):
            continue
        failed = parse_last_failed(logs) | parse_test_xml(logs)
        lane_failed[lane] = failed
        flaky_names |= parse_first_failed(logs) - failed
        golden_details += collect_golden_details(logs)

    # ---- 第二遍：按 job 结论分诊 ----
    for lane in all_lanes:
        conclusion = jobs.get(lane)
        lane_obs = lane in observation
        failed_tests = lane_failed.get(lane, set())
        if conclusion not in ("failure",):
            if conclusion in ("cancelled", "skipped"):
                print(f"NOTE {lane}: conclusion={conclusion}，本轮不参与分诊", file=out)
            continue
        if lane_obs:
            print(f"::warning::观察档 {lane} 本轮红（continue-on-error，不阻断）——"
                  f"#484 状态追踪：连续红须在 docs/ci/known-reds.json 登记或修复", file=out)
        if not failed_tests:
            entry = entries.get(lane)
            if entry and entry.get("scope") == "job" and lane in entry.get("lanes", all_lanes):
                hit_matches.add(lane)
                print(f"::warning title=已知红项 ({lane})::{lane} job 级红已登记"
                      f"（{issue_link(entry)}：{entry['reason']}）", file=out)
                known_warn.append((lane, lane, entry))
            elif lane_obs:
                print(f"::warning::观察档 {lane} job 红（无测试级明细——构建/安装/环境级），"
                      f"须分诊登记", file=out)
                obs_warn.append((lane, "<job>", None))
            else:
                print(f"::error title=未知红项 ({lane})::{lane} job 红（无测试级明细——"
                      f"构建/安装/门禁级红不可分诊，须修复后在 docs/ci/known-reds.json 登记）",
                      file=out)
                unknown_err.append((lane, "<job>"))
                rc = 1
            continue
        for test in sorted(failed_tests):
            entry = entries.get(test)
            if entry and (entry.get("scope") != "test"
                          or lane not in entry.get("lanes", all_lanes)):
                entry = None
            if not entry:
                if lane_obs:
                    print(f"::warning::观察档 {lane} 未登记红：{test}（不阻断，"
                          f"连续红须登记/修复）", file=out)
                    obs_warn.append((lane, test, None))
                else:
                    print(f"::error title=未知红项 ({lane})::{test} 红——"
                          f"不在 docs/ci/known-reds.json，须修复或显式登记（PR 改名单）",
                          file=out)
                    unknown_err.append((lane, test))
                    rc = 1
                continue
            hit_matches.add(test)
            if str(entry["review_due"]) < today:
                print(f"::error title=已知红项复审到期 ({lane})::{test} 的登记"
                      f"（{issue_link(entry)}）review_due={entry['review_due']} 已过期——"
                      f"降级失效按未知红处理：要么续期（改 review_due + 补证据）要么转正式修复",
                      file=out)
                unknown_err.append((lane, test))
                rc = 1
                continue
            budget = entry.get("budget")
            over = None
            if budget and budget["metric"] == "golden_points_over":
                over = golden_over_count(test, golden_details)
                if over is not None and over > budget["value"]:
                    print(f"::error title=已知红项超预算 ({lane})::{test} golden 超差点数 "
                          f"{over} > 登记 {budget['value']}——只缩不涨（#475）："
                          f"回归恶化须修复，不许改大预算", file=out)
                    unknown_err.append((lane, test))
                    rc = 1
                    continue
            msg = (f"{test}（{lane}）红已登记 {issue_link(entry)}：{entry['reason']}")
            if budget and over is not None and over < budget["value"]:
                msg += f"；本轮 {over} < 登记 {budget['value']}——可收缩预算"
                print(f"NOTE {test} golden 预算可收缩：{over} < {budget['value']}"
                      f"（known-reds 只缩不涨，请同步收紧）", file=out)
            print(f"::warning title=已知红项 ({lane})::{msg}", file=out)
            known_warn.append((lane, test, entry))

    # STALE：lane 日志已解析、登记项本轮未命中（测试转绿或 job 绿）→ 名单可收缩。
    parsed_lanes = set(lane_failed)
    for match, entry in sorted(entries.items()):
        if match in hit_matches:
            continue
        if not (set(entry.get("lanes", all_lanes)) & parsed_lanes):
            continue
        print(f"NOTE known-reds 可收缩：{match} 本轮未红（登记车道日志已解析）——"
              f"测试转绿即删条目（{issue_link(entry)}）", file=out)

    # ---- 汇总（stdout + 可选 $GITHUB_STEP_SUMMARY）----
    print(f"\n分诊汇总：已知红 {len(known_warn)}（降级 warning）｜未知红 {len(unknown_err)}"
          f"（{'error/exit 1' if unknown_err else '无'}）｜观察档红 {len(obs_warn)}"
          f"（不阻断）｜flaky 重跑洗白 {len(flaky_names)}", file=out)
    for lane, test in sorted(unknown_err):
        print(f"  未知红：{lane} / {test}", file=out)
    for line in golden_details[:20]:
        print(f"  golden 明细：{line}", file=out)
    if summary_fh is not None:
        w = summary_fh.write
        w("## CI 红项分诊（#497/#475）\n\n")
        w(f"- 已知红项：{len(known_warn)}（降级 warning，见上）\n")
        w(f"- **未知红项：{len(unknown_err)}**（triage 红的本因）\n")
        w(f"- 观察档红（linux-perf/linux-asan）：{len(obs_warn)}\n")
        w(f"- flaky（首轮红、串行重跑绿）：{len(flaky_names)}\n\n")
        if unknown_err:
            w("### 未知红项清单\n\n" + "".join(f"- `{l}` / `{t}`\n" for l, t in sorted(unknown_err)))
        if golden_details:
            w("\n### golden 容差明细（#475）\n\n```\n"
              + "\n".join(golden_details[:20]) + "\n```\n")
        w("\n名单：`docs/ci/known-reds.json`（变更须走 PR，diff 会以 notice 点名）。\n")
    return rc


def selftest():
    import datetime
    failures = 0

    def run_case(name, reds_obj, jobs, lane_logs, expect_rc, must_contain=(), today=None):
        nonlocal failures
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            real_logs = {}
            for lane, files in (lane_logs or {}).items():
                d = os.path.join(tmp, lane)
                os.makedirs(d, exist_ok=True)
                for fname, content in files.items():
                    fpath = os.path.join(d, fname)
                    os.makedirs(os.path.dirname(fpath), exist_ok=True)
                    with open(fpath, "w", encoding="utf-8") as fh:
                        fh.write(content)
                real_logs[lane] = d
            rc = classify(reds_obj, jobs, real_logs, today=today or "2026-10-17", out=buf)
            text = buf.getvalue()
            if rc != expect_rc:
                print(f"SELFTEST FAIL {name}: rc={rc} 期望 {expect_rc}\n{text}")
                failures += 1
                return
            for needle in must_contain:
                if needle not in text:
                    print(f"SELFTEST FAIL {name}: 输出缺标记 {needle!r}\n{text}")
                    failures += 1

    entry_known = {"id": "wc-golden", "scope": "test", "match": "tst_wellcomposite_visual",
                   "issue": 475, "reason": "17/48 超容差", "owner": "WindWang2",
                   "added": "2026-10-17", "review_due": "2027-01-16",
                   "budget": {"metric": "golden_points_over", "value": 17}}
    entry_obs = {"id": "cache-las", "scope": "test", "match": "tst_cache_las",
                 "issue": 475, "reason": "比率门 host 相关", "owner": "WindWang2",
                 "added": "2026-10-17", "review_due": "2027-01-16",
                 "lanes": ["linux-perf"]}
    entry_job = {"id": "job-demo", "scope": "job", "match": "lint",
                 "issue": 999, "reason": "演示 job 级登记", "owner": "x",
                 "added": "2026-10-17", "review_due": "2027-01-16"}
    base_reds = {"gating": ["lint", "linux", "windows"],
                 "observation": ["linux-perf", "linux-asan"],
                 "entries": {}}
    last_failed = {"Temporary/LastTestsFailed.log": "3 : tst_wellcomposite_visual\n"}
    golden_txt = {"Temporary/LastTest.log":
                  "FAIL! : tst_wellcomposite_visual::testGoldenImageSampling"
                  " 'diffs <= 4' returned FALSE. (17/48 抽样点超容差)\n"}

    def reds_with(*entries):
        r = json.loads(json.dumps(base_reds))
        r["entries"] = {e["match"]: e for e in entries}
        return r

    # 1) 未知红（gating lane 测试红、无登记）→ exit 1
    run_case("unknown-red", base_reds, {"linux": "failure"},
             {"linux": last_failed}, 1, ("未知红项", "tst_wellcomposite_visual"))
    # 2) 已知红 → warning 降级，exit 0
    run_case("known-red", reds_with(entry_known), {"linux": "failure"},
             {"linux": last_failed}, 0, ("已知红项", "#475"))
    # 3) 预算超涨（18 > 17）→ exit 1（#475 只缩不涨）
    grown = {"Temporary/LastTest.log":
             "GOLDEN-SUMMARY over=18/48\nGOLDEN-DIFF gx=1 gy=2\n"}
    run_case("budget-grew", reds_with(entry_known), {"linux": "failure"},
             {"linux": {**last_failed, **grown}}, 1, ("超预算", "18 > 登记 17"))
    # 4) 预算收缩（5 < 17）→ exit 0 + NOTE
    shrunk = {"Temporary/LastTest.log": "GOLDEN-SUMMARY over=5/48\n"}
    run_case("budget-shrunk", reds_with(entry_known), {"linux": "failure"},
             {"linux": {**last_failed, **shrunk}}, 0, ("可收缩", "5 < 17"))
    # 5) 观察档未登记红 → 不阻断 exit 0
    run_case("obs-unknown", base_reds, {"linux-perf": "failure"},
             {"linux-perf": {"Temporary/LastTestsFailed.log": "9 : tst_singlefactor_perf\n"}},
             0, ("观察档", "tst_singlefactor_perf"))
    # 6) 观察档已登记红 → exit 0
    run_case("obs-known", reds_with(entry_obs), {"linux-perf": "failure"},
             {"linux-perf": {"Temporary/LastTestsFailed.log": "9 : tst_cache_las\n"}},
             0, ("观察档", "tst_cache_las"))
    # 7) gating job 级红（无测试明细）→ exit 1；job scope 登记可降级
    run_case("job-red-unknown", base_reds, {"lint": "failure"}, {"lint": {}}, 1, ("job 红", "不可分诊"))
    run_case("job-red-known", reds_with(entry_job), {"lint": "failure"}, {"lint": {}},
             0, ("job 级红已登记", "#999"))
    # 8) Test.xml 口径（LastTestsFailed 缺失时兜底）
    xml_logs = {"33001099/Test.xml":
                '<Site><Testing><Test Status="failed"><Name>tst_wellcomposite_visual</Name>'
                "</Test></Testing></Site>\n"}
    run_case("xml-source", base_reds, {"windows": "failure"},
             {"windows": xml_logs}, 1, ("未知红项", "tst_wellcomposite_visual"))
    # 9) 复审到期 → 降级失效 exit 1
    overdue = dict(entry_known, review_due="2026-01-01")
    run_case("overdue", reds_with(overdue), {"linux": "failure"},
             {"linux": last_failed}, 1, ("复审到期", "review_due=2026-01-01"))
    # 10) STALE：登记项未红 + 日志已解析 → NOTE 可收缩；flaky 汇总
    run_case("stale", reds_with(entry_known), {"linux": "success"},
             {"linux": {"Temporary/LastTestsFailed.log": "",
                        "Temporary/FirstRunFailed.log": "7 : tst_flaky_one\n"}},
             0, ("可收缩", "flaky"))
    # 11) cancelled lane 不参与分诊
    run_case("cancelled", base_reds, {"linux": "cancelled"}, {"linux": last_failed},
             0, ("不参与分诊",))

    # ---- 名单校验夹具（坏结构必须 exit 2）----
    with tempfile.TemporaryDirectory() as tmp:
        bad = os.path.join(tmp, "bad.json")
        for content in (
            "not json",
            '{"lanes": {"gating": [], "observation": []}, "entries": []}',
            '{"lanes": {"gating": ["linux"], "observation": []}, "entries": '
            '[{"id": "x", "scope": "test", "match": "t", "issue": "#475", "reason": "r", '
            '"owner": "o", "added": "2026-01-01", "review_due": "2026-02-01"}]}',
            '{"lanes": {"gating": ["linux"], "observation": []}, "entries": '
            '[{"id": "x", "scope": "test", "match": "t", "issue": 475, "reason": "r", '
            '"owner": "o", "added": "2026-13-01", "review_due": "2026-02-01"}]}',
        ):
            with open(bad, "w", encoding="utf-8") as fh:
                fh.write(content)
            _reds, rc = load_reds(bad)
            if rc != 2:
                print(f"SELFTEST FAIL reds-validate: 坏名单必须 exit 2：{content[:40]}…")
                failures += 1
        good = os.path.join(tmp, "good.json")
        with open(good, "w", encoding="utf-8") as fh:
            json.dump({"lanes": {"gating": ["linux"], "observation": ["linux-perf"]},
                       "entries": [entry_known]}, fh)
        reds, rc = load_reds(good)
        if rc is not None or "tst_wellcomposite_visual" not in reds["entries"]:
            print("SELFTEST FAIL reds-validate: 合法名单应载入成功")
            failures += 1
        # 仓库名单本体必须合法（本文件随 triage job / ctest validate 双跑）
        repo_reds, rc = load_reds(DEFAULT_REDS)
        if rc is not None:
            print("SELFTEST FAIL reds-validate: 仓库 docs/ci/known-reds.json 结构非法")
            failures += 1
        else:
            dup = set(repo_reds["gating"]) & set(repo_reds["observation"])
            if dup:
                print(f"SELFTEST FAIL reds-validate: 车道双登记 {dup}")
                failures += 1

    # ---- jobs 解析夹具 ----
    with tempfile.TemporaryDirectory() as tmp:
        jp = os.path.join(tmp, "jobs.json")
        with open(jp, "w", encoding="utf-8") as fh:
            json.dump({"jobs": [{"name": "linux", "conclusion": "failure"},
                                {"name": "linux-perf", "conclusion": "failure"}]}, fh)
        if parse_jobs(jp) != {"linux": "failure", "linux-perf": "failure"}:
            print("SELFTEST FAIL parse_jobs: 结论映射不符")
            failures += 1
        if parse_jobs(os.path.join(tmp, "missing.json")) is not None:
            print("SELFTEST FAIL parse_jobs: 解析失败应返回 None（exit 2 通道）")
            failures += 1

    if failures:
        print(f"selftest：{failures} 处夹具失败")
        return 1
    print("selftest 通过：11 分诊夹具 + 4 名单校验夹具 + 2 jobs 解析夹具全部命中预期。")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    reds_path = None
    jobs_path = None
    lane_logs = {}
    mode = None
    it = iter(argv)
    for a in it:
        if a == "--selftest":
            return selftest()
        if a == "--validate":
            mode = "validate"
        elif a == "--reds":
            reds_path = next(it, None)
            if not reds_path:
                return fail2("--reds 需要路径参数")
        elif a == "--jobs":
            jobs_path = next(it, None)
            if not jobs_path:
                return fail2("--jobs 需要路径参数")
        elif a == "--logs":
            pair = next(it, None)
            if not pair or "=" not in pair:
                return fail2("--logs 需要 LANE=DIR 形式参数")
            lane, _, dirp = pair.partition("=")
            lane_logs[lane] = dirp
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            return fail2(f"未知参数：{a}")
    reds, rc = load_reds(reds_path)
    if rc is not None:
        return rc
    if mode == "validate":
        print(f"known-reds 名单合法：{len(reds['entries'])} 条登记"
              f"（gating {reds['gating']} / observation {reds['observation']}）")
        return 0
    if jobs_path is None:
        return fail2("分诊需要 --jobs（gh api …/actions/runs/<id>/jobs 输出）")
    unknown_lanes = set(lane_logs) - set(reds["gating"]) - set(reds["observation"])
    if unknown_lanes:
        return fail2(f"--logs 车道未在名单 lanes 登记：{sorted(unknown_lanes)}")
    jobs = parse_jobs(jobs_path)
    if jobs is None:
        return 2
    summary_fh = None
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        summary_fh = open(summary_path, "a", encoding="utf-8")
    try:
        return classify(reds, jobs, lane_logs, summary_fh=summary_fh)
    finally:
        if summary_fh:
            summary_fh.close()


if __name__ == "__main__":
    sys.exit(main())
