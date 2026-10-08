#!/usr/bin/env python3
"""环境债红名单检查器（方向 81 防回归）。

按 tools/env_redset.json 给一次 ctest 结果分类：
  EXEMPT     名单内 status=exempt、路线匹配、且 QtTest 输出里每条 FAIL! 都命中
             expect_fail —— 放行（门槛不动，只认指定的预算型断言）。
  REGRESSED  名单内 status=fixed 的项又红了 —— 已修环境债回归。
  UNEXPECTED 名单外的红，或 exempt 项红得不对（别的断言 / 崩溃 / 无输出 /
             路线不在 routes）。
  STALE      exempt 项本轮通过 —— 名单可删此条（--strict 下按失败算）。
退出码：0 = 无 REGRESSED/UNEXPECTED（--strict 另要求无 STALE），1 = 有。

用法：
  python3 tools/check_env_redset.py --build BUILD [--route localdeps]
         [--junit BUILD/Testing/ctest-junit.xml] [--emit-unexpected FILE] [--strict]
  python3 tools/check_env_redset.py --validate     # 名单结构 + 测试名在 CMake 中注册
  python3 tools/check_env_redset.py --selftest
  python3 tools/check_env_redset.py --build RUN2 --compare-build RUN1 --route localdeps
    # 每个 RUN 保存自己的 Testing/ctest-junit.xml 与 qtest-first-run/；
    # 双遍范围相同、16 项齐全、至少 10 个 fixed 项连续通过，无未豁免红。
失败集来源：--junit（全量通过/失败/跳过）优先，否则
BUILD/Testing/Temporary/LastTestsFailed.log；QtTest 输出取
BUILD/Testing/qtest-first-run/<test>.txt（add_paleo_test 的 -o 落点）。
"""
import argparse
import contextlib
import io
import json
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_LIST = REPO / "tools" / "env_redset.json"
STATUSES = ("fixed", "exempt")
FAIL_LINE = re.compile(r"^FAIL!\s+:\s+(.*)$")
CRASH = re.compile(r"Received signal|Caught unhandled exception|QFATAL|\*\*\*Exception|ASSERT failure", re.I)
UNSAFE_CTEST = re.compile(r"timeout|segfault|exception|aborted|killed|bad command|not run", re.I)


class Fail(Exception):
    pass


def load_list(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    errors = []
    cats = data.get("categories", {})
    seen = set()
    for i, e in enumerate(data.get("entries", [])):
        where = f"entries[{i}] {e.get('test', '?')}"
        if not e.get("test"):
            errors.append(f"{where}: 缺 test")
        if e.get("test") in seen:
            errors.append(f"{where}: 重复")
        seen.add(e.get("test"))
        if e.get("category") not in cats:
            errors.append(f"{where}: category {e.get('category')!r} 不在 categories")
        if e.get("status") not in STATUSES:
            errors.append(f"{where}: status 必须是 {STATUSES}")
        if e.get("status") == "fixed" and not e.get("fix"):
            errors.append(f"{where}: fixed 项须写 fix")
        if e.get("status") == "exempt":
            if not e.get("routes") or not e.get("reason"):
                errors.append(f"{where}: exempt 项须写 routes 与 reason")
            pats = e.get("expect_fail") or []
            if not pats:
                errors.append(f"{where}: exempt 项须写 expect_fail（不许无条件豁免）")
            for p in pats:
                try:
                    rx = re.compile(p)
                except re.error as ex:
                    errors.append(f"{where}: expect_fail 正则非法 {p!r}: {ex}")
                    continue
                if rx.search("") is not None:
                    errors.append(f"{where}: expect_fail {p!r} 匹配空串——等于无条件豁免")
    if errors:
        raise Fail("名单结构错误：\n  " + "\n  ".join(errors))
    return {e["test"]: e for e in data["entries"]}


def results_from_junit(path):
    """→ {test: 'pass'|'fail'|'unsafe'|'skip'}，崩溃/超时不得套预算豁免。"""
    out = {}
    for tc in ET.parse(path).getroot().iter("testcase"):
        name = tc.get("name")
        status = (tc.get("status") or "").lower()
        if tc.find("failure") is not None or tc.find("error") is not None or status == "fail":
            failures = [*tc.findall("failure"), *tc.findall("error")]
            detail = " ".join(str(f.attrib) + " " + (f.text or "") for f in failures)
            out[name] = "unsafe" if UNSAFE_CTEST.search(detail) else "fail"
        elif tc.find("skipped") is not None or status in ("notrun", "disabled"):
            out[name] = "skip"
        else:
            out[name] = "pass"
    if not out:
        raise Fail(f"JUnit 没有测试结果：{path}")
    return out


def results_from_lastfailed(build):
    log = Path(build) / "Testing" / "Temporary" / "LastTestsFailed.log"
    out = {}
    if log.is_file():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if ":" in line:
                out[line.split(":", 1)[1].strip()] = "fail"
    return out


def qtest_fail_lines(build, test):
    """→ (FAIL! 消息列表, 输出是否完整)。无输出/崩溃标记 → 不完整。"""
    f = Path(build) / "Testing" / "qtest-first-run" / f"{test}.txt"
    if not f.is_file():
        return [], False
    text = f.read_text(encoding="utf-8", errors="replace")
    fails = [m.group(1) for line in text.splitlines() if (m := FAIL_LINE.match(line))]
    complete = "Totals:" in text and not CRASH.search(text)
    return fails, complete


def classify(entries, results, build, route):
    """→ [(kind, test, detail)]，kind ∈ EXEMPT/REGRESSED/UNEXPECTED/STALE"""
    report = []
    for test, state in sorted(results.items()):
        e = entries.get(test)
        if state in ("fail", "unsafe"):
            if e is None:
                report.append(("UNEXPECTED", test, "不在环境债名单"))
            elif e["status"] == "fixed":
                report.append(("REGRESSED", test, f"[{e['category']}] 已修：{e['fix']}"))
            elif route not in e["routes"]:
                report.append(("UNEXPECTED", test,
                               f"[{e['category']}] 豁免只限路线 {e['routes']}，本轮路线 {route!r}"))
            elif state == "unsafe":
                report.append(("UNEXPECTED", test, "CTest 超时/崩溃/启动失败，禁止预算豁免"))
            else:
                fails, complete = qtest_fail_lines(build, test)
                pats = [re.compile(p) for p in e["expect_fail"]]
                odd = [m for m in fails if not any(p.search(m) for p in pats)]
                if not complete or not fails:
                    report.append(("UNEXPECTED", test,
                                   f"[{e['category']}] QtTest 输出缺失/不完整/崩溃——豁免只认预算断言"))
                elif odd:
                    report.append(("UNEXPECTED", test,
                                   f"[{e['category']}] 非预期断言：{odd[0][:160]}"))
                else:
                    report.append(("EXEMPT", test, f"[{e['category']}] {e['reason']}"))
        elif state == "pass" and e is not None and e["status"] == "exempt":
            report.append(("STALE", test, f"[{e['category']}] 本轮通过——名单可删此条"))
    return report


def validate_registered(entries):
    """名单里的测试名必须在 CMake 中注册（add_paleo_test(X / add_test(NAME X）。"""
    text = ""
    for f in [REPO / "CMakeLists.txt", *sorted((REPO / "cmake").glob("*.cmake"))]:
        text += f.read_text(encoding="utf-8", errors="replace")
    missing = [t for t in entries
               if not re.search(r"(add_paleo_test\(\s*|add_test\(\s*NAME\s+)" + re.escape(t) + r"[\s)]", text)]
    if missing:
        raise Fail(f"名单里的测试未在 CMake 注册：{missing}")


def compare_runs(entries, first, second):
    """只证明结果收敛；实际平台/依赖路线仍须在 ledger 记录，不能以 Linux 代 Windows。"""
    if set(first) != set(second):
        raise Fail("双遍测试范围不同，不能证明全量稳定")
    missing = set(entries) - set(first)
    if missing:
        raise Fail(f"双遍缺环境债测试（过滤 perf/只跑子集不能验收）：{sorted(missing)}")
    stable_green = [name for name, e in entries.items() if e["status"] == "fixed" and
                    first[name] == second[name] == "pass"]
    if len(stable_green) < 10:
        raise Fail(f"双遍固定项连续绿 {len(stable_green)} < 10：{stable_green}")
    for name, e in entries.items():
        if e["status"] == "fixed" and (first[name] != "pass" or second[name] != "pass"):
            raise Fail(f"已修项 {name} 未连续通过：{first[name]} / {second[name]}")
    return stable_green


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", default=str(DEFAULT_LIST))
    ap.add_argument("--build")
    ap.add_argument("--junit")
    ap.add_argument("--compare-build")
    ap.add_argument("--route", default="unknown")
    ap.add_argument("--emit-unexpected")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    entries = load_list(a.list)
    if a.validate:
        validate_registered(entries)
        print(f"env_redset: OK（{len(entries)} 项，"
              f"{sum(e['status'] == 'exempt' for e in entries.values())} 项豁免）")
        return 0
    if not a.build:
        raise Fail("--build 必填（或 --validate / --selftest）")
    junit = Path(a.junit) if a.junit else Path(a.build) / "Testing" / "ctest-junit.xml"
    if junit.is_file():
        results = results_from_junit(junit)
    else:
        failed_log = Path(a.build) / "Testing" / "Temporary" / "LastTestsFailed.log"
        if not failed_log.is_file():
            raise Fail("没有本轮 JUnit 或 LastTestsFailed.log，不能证明红集合")
        results = results_from_lastfailed(a.build)
    report = classify(entries, results, a.build, a.route)
    n_fail = sum(s in ("fail", "unsafe") for s in results.values())
    if a.compare_build:
        previous = results_from_junit(Path(a.compare_build) / "Testing" / "ctest-junit.xml")
        stable_green = compare_runs(entries, previous, results)
        report += classify(entries, previous, a.compare_build, a.route)
        n_fail += sum(s in ("fail", "unsafe") for s in previous.values())
        print(f"双遍连续绿：{len(stable_green)} 个已修项（需 ledger 确认实际平台与全量范围）")
    for kind, test, detail in report:
        print(f"{kind:<10} {test}: {detail}")
    bad = [t for k, t, _ in report if k in ("REGRESSED", "UNEXPECTED") or (a.strict and k == "STALE")]
    if a.emit_unexpected:
        Path(a.emit_unexpected).write_text("".join(t + "\n" for t in bad), encoding="utf-8")
    print(f"env_redset: {n_fail} 红 → {sum(k == 'EXEMPT' for k, _, _ in report)} 豁免 / "
          f"{len(bad)} 不放行（路线 {a.route}）")
    return 1 if bad else 0


def main(argv=None):
    try:
        return run(sys.argv[1:] if argv is None else argv)
    except Fail as e:
        print(f"FAIL {e}", file=sys.stderr)
        return 1


# ---------------------------------------------------------------- selftest --
def selftest():
    budget = "FAIL!  : CacheLasTests::secondOpen() 'diskMs < 0.5 * coldMs' returned FALSE. " \
             "(disk phase median 1.857ms >= 0.5×cold parse median 2.996ms)\n   Loc: [x.cpp(141)]\n"
    other = "FAIL!  : CacheLasTests::secondOpen() Compared values are not the same\n"
    totals = "Totals: 3 passed, 1 failed, 0 skipped, 0 blacklisted, 12ms\n"
    lst = {
        "schema": 1,
        "categories": {"perf-host": "x", "srsdb": "y"},
        "entries": [
            {"test": "tst_cache_las", "category": "perf-host", "status": "exempt", "routes": ["localdeps"],
             "reason": "r", "expect_fail": [r"(disk phase|memory load) median [0-9.]+ms >= 0\.5×cold parse median"]},
            {"test": "tst_runtime", "category": "srsdb", "status": "fixed", "fix": "f"},
        ],
    }

    def scenario(results, qtest, route="localdeps", strict=False, entries_json=None):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "list.json").write_text(json.dumps(entries_json or lst, ensure_ascii=False), encoding="utf-8")
            (tmp / "Testing" / "qtest-first-run").mkdir(parents=True)
            for name, text in qtest.items():
                (tmp / "Testing" / "qtest-first-run" / f"{name}.txt").write_text(text, encoding="utf-8")
            suite = ET.Element("testsuite")
            for name, st in results.items():
                tc = ET.SubElement(suite, "testcase", name=name, status="fail" if st in ("fail", "unsafe") else "run")
                if st in ("fail", "unsafe"):
                    ET.SubElement(tc, "failure", message="Timeout" if st == "unsafe" else "Failed")
                if st == "skip":
                    tc.set("status", "notrun")
                    ET.SubElement(tc, "skipped")
            ET.ElementTree(suite).write(tmp / "junit.xml", encoding="utf-8")
            emit = tmp / "unexpected.txt"
            argv = ["--list", str(tmp / "list.json"), "--build", str(tmp), "--junit", str(tmp / "junit.xml"),
                    "--route", route, "--emit-unexpected", str(emit)] + (["--strict"] if strict else [])
            with contextlib.redirect_stdout(io.StringIO()):
                rc = run(argv)
            return rc, emit.read_text(encoding="utf-8").split()

    cases = [
        # (说明, results, qtest, route, strict, 期望 rc, 期望不放行集合)
        ("预算断言红+路线匹配 → 豁免", {"tst_cache_las": "fail", "tst_a": "pass"},
         {"tst_cache_las": budget + totals}, "localdeps", False, 0, []),
        ("同红换路线（CI/vendored）→ 不放行", {"tst_cache_las": "fail"},
         {"tst_cache_las": budget + totals}, "vendored", False, 1, ["tst_cache_las"]),
        ("豁免项红在别的断言 → 不放行", {"tst_cache_las": "fail"},
         {"tst_cache_las": budget + other + totals}, "localdeps", False, 1, ["tst_cache_las"]),
        ("豁免项崩溃（无 Totals）→ 不放行", {"tst_cache_las": "fail"},
         {"tst_cache_las": budget}, "localdeps", False, 1, ["tst_cache_las"]),
        ("豁免项无 QtTest 输出 → 不放行", {"tst_cache_las": "fail"}, {}, "localdeps", False, 1, ["tst_cache_las"]),
        ("CTest 超时 + 上轮完整预算输出 → 不放行", {"tst_cache_las": "unsafe"},
         {"tst_cache_las": budget + totals}, "localdeps", False, 1, ["tst_cache_las"]),
        ("已修项再红 → REGRESSED", {"tst_runtime": "fail"}, {}, "localdeps", False, 1, ["tst_runtime"]),
        ("名单外红 → UNEXPECTED", {"tst_new": "fail"}, {}, "localdeps", False, 1, ["tst_new"]),
        ("豁免项转绿 → STALE（非 strict 放行）", {"tst_cache_las": "pass"}, {}, "localdeps", False, 0, []),
        ("豁免项转绿 → STALE（strict 不放行）", {"tst_cache_las": "pass"}, {}, "localdeps", True, 1, ["tst_cache_las"]),
        ("跳过不算红", {"tst_new": "skip"}, {}, "localdeps", False, 0, []),
    ]
    failures = []
    real_entries = load_list(DEFAULT_LIST)
    green = {name: "pass" for name in real_entries}
    assert len(compare_runs(real_entries, green, green)) == 13
    for first, second in ((green, {**green, "new_test": "pass"}),
                          ({name: st for name, st in green.items() if name != "tst_aiassist_perf"},
                           {name: st for name, st in green.items() if name != "tst_aiassist_perf"}),
                          (green, {**green, "tst_runtime": "skip"})):
        try:
            compare_runs(real_entries, first, second)
            failures.append("双遍范围/缺项/跳过突变被漏放")
        except Fail:
            pass
    with tempfile.TemporaryDirectory() as tmp:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            if main(["--build", tmp]) != 1:
                failures.append("无结果文件必须拒绝")
        empty = Path(tmp) / "empty.xml"
        empty.write_text("<testsuite />", encoding="utf-8")
        try:
            results_from_junit(empty)
            failures.append("空 JUnit 必须拒绝")
        except Fail:
            pass
    for desc, results, qtest, route, strict, want_rc, want_bad in cases:
        rc, bad = scenario(results, qtest, route, strict)
        if rc != want_rc or sorted(bad) != sorted(want_bad):
            failures.append(f"{desc}: rc={rc} bad={bad}（期望 rc={want_rc} bad={want_bad}）")
    # 结构校验：无条件豁免（空/匹配空串的 expect_fail）与缺 routes 必须拒绝。
    for mutate, desc in ((lambda e: e.update(expect_fail=[]), "空 expect_fail"),
                         (lambda e: e.update(expect_fail=[".*"]), "expect_fail='.*'"),
                         (lambda e: e.pop("routes"), "缺 routes"),
                         (lambda e: e.update(status="ignored"), "非法 status")):
        bad_list = json.loads(json.dumps(lst))
        mutate(bad_list["entries"][0])
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "l.json"
            p.write_text(json.dumps(bad_list), encoding="utf-8")
            try:
                load_list(p)
                failures.append(f"结构校验漏放：{desc}")
            except Fail:
                pass
    # 仓库真名单必须结构合法且全部已注册；豁免正则须命中各测试源码里的
    # 预算断言文案（样本按 QVERIFY2 消息格式手写，测试文案改了这里会红）。
    samples = {
        "tst_cache_las": ["(disk phase median 1.857ms >= 0.5×cold parse median 2.996ms)",
                          "(memory load median 1.700ms >= 0.5×cold parse median 2.996ms)"],
        "tst_singlefactor_perf": ["(S median 5321 ms)", "(cancel max 1203 ms)", "(cell ratio 9000 / 1200)"],
        "tst_startup_trace": ["(启动比率 qgis_init_share_max 中位 0.2441 > 门 0.2200（基线×2.5，3 轮样本 0.24/0.25/0.24）)"],
    }
    try:
        real = load_list(DEFAULT_LIST)
        validate_registered(real)
        for test, msgs in samples.items():
            pats = [re.compile(p) for p in real[test]["expect_fail"]]
            for m in msgs:
                if not any(p.search(m) for p in pats):
                    failures.append(f"仓库名单 {test} 的 expect_fail 不命中样本 {m!r}")
            if any(p.search("Compared values are not the same") for p in pats):
                failures.append(f"仓库名单 {test} 的 expect_fail 过宽（命中 QCOMPARE 失败）")
        for test in samples:
            src = (REPO / "tests" / f"{test}.cpp").read_text(encoding="utf-8")
            for frag in {"tst_cache_las": ["0.5×cold parse median"], "tst_singlefactor_perf": ["S median", "cell ratio"],
                         "tst_startup_trace": ["启动比率", "中位", "> 门"]}[test]:
                if frag not in src:
                    failures.append(f"{test}.cpp 已无断言文案 {frag!r}——同步 env_redset.json 的 expect_fail")
    except Fail as e:
        failures.append(f"仓库名单：{e}")
    if failures:
        for f in failures:
            print("SELFTEST FAIL", f)
        return 1
    print(f"check_env_redset selftest: PASS（{len(cases)} 场景 + 4 结构突变）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
