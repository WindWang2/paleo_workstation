#!/usr/bin/env python3
# 层：测试壳
"""Windows / Linux 测试门禁共享规则读取器（#760 / #957）。

单一真源是 tools/ci-gate-rules.json；本工具把它渲染成两端脚本可直接用的形态：

  ci_gate_rules.py --regex             → crash 判定正则（| 连接的 ERE）
  ci_gate_rules.py --result-line-regex → ctest 结果行正则（限定扫描范围）
  ci_gate_rules.py --policy KEY        → 单条放行策略说明
  ci_gate_rules.py --check-consistency → 两端脚本内联回落副本与门禁护栏一致？
  ci_gate_rules.py --selftest          → 合成夹具正反向验证

背景（#760 / #957）：Windows 与 Linux 的门禁语义此前各自演化——
①超时判红（Windows 认 ***Timeout 为崩溃类，Linux 不认）；
②豁免放行（Windows 用 check_env_redset --emit-unexpected 过滤重跑集，Linux 全量重跑）；
③日志扫描范围（Windows 扫整份日志，Linux 只扫 ctest 结果行）；
④超时重跑（同上①的连带差异）；
⑤空测试集防护（两端均须带 --no-tests=error + 'No tests were found' 日志兜底）；
⑥flaky strict 传播（两端调用 check_flaky_ledger.py --strict 后均须检查退出码并判红）。
未知参数一律 exit 2（与 check_layering.py 同约定——参数被静默忽略即假绿）。
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO = Path(__file__).resolve().parents[1]
RULES = REPO / "tools" / "ci-gate-rules.json"
GATE_SH = REPO / "tools" / "ci_ctest_gate.sh"
DEV_PS1 = REPO / "paleo-dev.ps1"

# 两端脚本里内联回落副本的字面形态（--check-consistency 从这里抽）。
# bash 侧叫 crash_re、PowerShell 侧叫 $crashPattern（crash 后无下划线）。
INLINE_RE = re.compile(r"""crash_?(?:re|Pattern)\s*=\s*'(.*?)'""", re.S)


def load_rules():
    with open(RULES, encoding="utf-8") as fh:
        return json.load(fh)


def crash_regex(rules):
    """markers → ERE。只转义正则元字符（`*` 必须转义，`***Exception` 才是字面量），
    空格保持原样——`\\ ` 在 POSIX ERE 里是未定义行为，GNU grep 认但别依赖。"""
    def esc(s):
        return "".join("\\" + c if c in ".^$*+?()[]{}|\\" else c for c in s)
    return "|".join(esc(m) for m in rules["crash_markers"])


def inline_copies_from_texts(gate_sh_text, dev_ps1_text):
    out = {}
    for name, text in (("ci_ctest_gate.sh", gate_sh_text), ("paleo-dev.ps1", dev_ps1_text)):
        hits = [h for h in INLINE_RE.findall(text) if h]
        out[name] = hits[-1] if hits else None
    return out


def inline_copies():
    """→ {文件: 内联 crash 回落正则 or None}（找不到内联副本 = 漂移，无法回落）。"""
    return inline_copies_from_texts(
        GATE_SH.read_text(encoding="utf-8", errors="replace"),
        DEV_PS1.read_text(encoding="utf-8", errors="replace"),
    )


def check_script_guards(gate_sh_text, dev_ps1_text, rules):
    """检查 Linux/Windows 两端门禁脚本的共享不变量，返回错误消息列表。"""
    errors = []
    want = crash_regex(rules)
    copies = inline_copies_from_texts(gate_sh_text, dev_ps1_text)
    for name, got in copies.items():
        if got is None:
            errors.append(f"FAIL {name}: 找不到内联 crash 回落副本（python 不可用时无路可退）")
        elif got != want:
            errors.append(
                f"FAIL {name}: 内联 crash 副本与 ci-gate-rules.json 不一致\n"
                f"  json : {want}\n  inline: {got}"
            )

    no_tests_flag = rules.get("no_tests_flag", "--no-tests=error")
    no_tests_marker = rules.get("no_tests_marker", "No tests were found")

    for name, text in (("ci_ctest_gate.sh", gate_sh_text), ("paleo-dev.ps1", dev_ps1_text)):
        if "--result-line-regex" not in text:
            errors.append(
                f"FAIL {name}: 未从共享规则取结果行口径（--result-line-regex）——扫描范围会重新漂移"
            )
        if no_tests_flag not in text:
            errors.append(
                f"FAIL {name}: 缺少 {no_tests_flag}（#957：空测试选择必须判红）"
            )
        if no_tests_marker not in text:
            errors.append(
                f"FAIL {name}: 缺少 '{no_tests_marker}' 日志扫描兜底（#957）"
            )

    # #957：flaky strict 非零退出不得被吞掉（两端对称校验）
    if not re.search(r"if\s+!\s+python3\s+\"\$script_dir/check_flaky_ledger\.py\"", gate_sh_text):
        errors.append("FAIL ci_ctest_gate.sh: check_flaky_ledger.py 未在 if ! 分支检查退出码（#957）")

    # paleo-dev.ps1 必须定义并调用 Invoke-FlakyLedgerCheck（或检查 check_flaky_ledger 后的 $LASTEXITCODE 并 throw）
    ps_fn_match = re.search(
        r"function\s+Invoke-FlakyLedgerCheck\b(.*?)^\}",
        dev_ps1_text,
        re.M | re.S,
    )
    if not ps_fn_match:
        errors.append("FAIL paleo-dev.ps1: 缺少 Invoke-FlakyLedgerCheck 门禁函数（#957）")
    else:
        body = ps_fn_match.group(1)
        if "check_flaky_ledger.py" not in body:
            errors.append("FAIL paleo-dev.ps1: Invoke-FlakyLedgerCheck 未调用 check_flaky_ledger.py")
        if "$LASTEXITCODE" not in body:
            errors.append("FAIL paleo-dev.ps1: Invoke-FlakyLedgerCheck 未检查 $LASTEXITCODE（原生命令非零不会进 catch，#957）")
        if not re.search(r"if\s*\(\s*\$flakyExit\s*-ne\s*0\s*\)\s*\{.*?throw\b", body, re.S):
            errors.append("FAIL paleo-dev.ps1: Invoke-FlakyLedgerCheck 在 $flakyExit -ne 0 的 strict 分支未 throw 判红（#957）")
    if not re.search(r"^\s*Invoke-FlakyLedgerCheck\b", dev_ps1_text, re.M):
        errors.append("FAIL paleo-dev.ps1: test verb 未调用 Invoke-FlakyLedgerCheck（#957）")
    if not re.search(r"ctest\b[^\n]*--no-tests=error", dev_ps1_text):
        errors.append("FAIL paleo-dev.ps1: ctest 调用行缺少 --no-tests=error（#957）")
    return errors


def check_consistency():
    rules = load_rules()
    gate_sh_text = GATE_SH.read_text(encoding="utf-8", errors="replace")
    dev_ps1_text = DEV_PS1.read_text(encoding="utf-8", errors="replace")
    errors = check_script_guards(gate_sh_text, dev_ps1_text, rules)
    if errors:
        for err in errors:
            print(err)
        return 1
    for name in ("ci_ctest_gate.sh", "paleo-dev.ps1"):
        print(f"  OK   {name}: 内联 crash 副本与 JSON 一致")
        print(f"  OK   {name}: 引用共享结果行口径 + 空测试集双防护 + flaky strict 退出码传播")
    return 0


def selftest():
    import tempfile
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    rules = load_rules()
    want = crash_regex(rules)
    expect("***Timeout" in rules["crash_markers"],
           "***Timeout 必须在共享崩溃 markers 里（#760 ①）")
    expect(want.startswith(crash_regex({"crash_markers": ["***Exception"]})),
           "Exception 应排在首位")
    expect("|".join(
        "".join("\\" + c if c in ".^$*+?()[]{}|\\" else c for c in m)
        for m in rules["crash_markers"]) == want,
           "crash_regex 必须按 markers 顺序渲染")
    expect(bool(rules["result_line_regex"]), "result_line_regex 不能为空（#760 ③）")
    expect(rules.get("no_tests_flag") == "--no-tests=error", "no_tests_flag 应为 --no-tests=error（#957）")
    expect(rules.get("no_tests_marker") == "No tests were found", "no_tests_marker 应为 No tests were found（#957）")

    # 合成 rules：markers 顺序/内容变化必须如实反映到渲染结果。
    with tempfile.TemporaryDirectory() as t:
        p = Path(t) / "r.json"
        p.write_text(json.dumps({"crash_markers": ["A|B", "C"], "result_line_regex": "X"}),
                     encoding="utf-8")
        with open(p, encoding="utf-8") as fh:
            r = json.load(fh)
        expect(crash_regex(r) == "A\\|B|C", "marker 含正则元字符必须被转义")
        expect("\\ " not in crash_regex(r), "空格不得被转义（POSIX ERE 未定义行为）")

    # #957 静态门禁突变测试：拆除任一防护必须被 check_script_guards 抓到。
    gate_sh_text = GATE_SH.read_text(encoding="utf-8", errors="replace")
    dev_ps1_text = DEV_PS1.read_text(encoding="utf-8", errors="replace")
    expect(check_script_guards(gate_sh_text, dev_ps1_text, rules) == [],
           "真树脚本必须通过 check_script_guards")
    expect(bool(check_script_guards(gate_sh_text, dev_ps1_text.replace("--no-tests=error", ""), rules)),
           "ps1 拆除 --no-tests=error 必须被检出（#957）")
    expect(bool(check_script_guards(gate_sh_text, dev_ps1_text.replace("No tests were found", ""), rules)),
           "ps1 拆除 'No tests were found' 兜底必须被检出（#957）")
    expect(bool(check_script_guards(gate_sh_text, dev_ps1_text.replace("$flakyExit = $LASTEXITCODE", ""), rules)),
           "ps1 拆除 flaky $LASTEXITCODE 检查必须被检出（#957）")
    expect(bool(check_script_guards(gate_sh_text, re.sub(r"throw\s+\(\"flaky.*?\)", "# swallowed", dev_ps1_text), rules)),
           "ps1 吞掉 flaky strict throw 必须被检出（#957）")

    # #957 逻辑回归 1：三条 flaky 记录 + --strict 必红，警告档仍绿。
    with tempfile.TemporaryDirectory() as t:
        ledger = Path(t) / "flaky.jsonl"
        ledger.write_text(
            "".join(
                json.dumps({"test": "tst_sample", "utc": "2026-11-24T00:00:00Z", "event": "serial-rerun-pass"}) + "\n"
                for _ in range(3)
            ),
            encoding="utf-8",
        )
        checker = REPO / "tools" / "check_flaky_ledger.py"
        strict_run = subprocess.run(
            [sys.executable, str(checker), "--ledger", str(ledger), "--strict"],
            capture_output=True, text=True,
        )
        warn_run = subprocess.run(
            [sys.executable, str(checker), "--ledger", str(ledger)],
            capture_output=True, text=True,
        )
        expect(strict_run.returncode == 1, f"3 条 flaky + --strict 应退出 1，实得 {strict_run.returncode}")
        expect(warn_run.returncode == 0, f"3 条 flaky 警告档应退出 0，实得 {warn_run.returncode}")

    # #957 逻辑回归 2：真实 ctest --no-tests=error -R no_such_test 必非零且命中标记。
    ctest_exe = shutil.which("ctest")
    true_exe = shutil.which("true")
    if ctest_exe and true_exe:
        with tempfile.TemporaryDirectory() as t:
            bdir = Path(t)
            (bdir / "CTestTestfile.cmake").write_text(
                f'add_test(sample_ok "{true_exe}")\n', encoding="utf-8"
            )
            empty_res = subprocess.run(
                [ctest_exe, "--test-dir", str(bdir), "--no-tests=error", "-R", "no_such_test"],
                capture_output=True, text=True,
            )
            expect(empty_res.returncode != 0, "ctest --no-tests=error -R no_such_test 必须非零退出")
            expect("No tests were found" in (empty_res.stdout + empty_res.stderr),
                   "ctest 空选择输出应包含 'No tests were found'")
            ok_res = subprocess.run(
                [ctest_exe, "--test-dir", str(bdir), "--no-tests=error", "-R", "^sample_ok$"],
                capture_output=True, text=True,
            )
            expect(ok_res.returncode == 0, "ctest --no-tests=error 非空测试集必须返回 0")

    rc = check_consistency()
    if rc != 0:
        failures.append("--check-consistency 在真树校验下必须绿（两端内联副本与门禁护栏应一致）")
    if failures:
        for f in failures:
            print("SELFTEST FAIL", f)
        return 1
    print("ci_gate_rules selftest: PASS（渲染/转义/Timeout/空测试集双防护/flaky strict/两端一致性）")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--regex", action="store_true")
    ap.add_argument("--result-line-regex", action="store_true")
    ap.add_argument("--policy")
    ap.add_argument("--check-consistency", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.regex:
        print(crash_regex(load_rules()))
        return 0
    if a.result_line_regex:
        print(load_rules()["result_line_regex"])
        return 0
    if a.policy:
        rules = load_rules()
        if a.policy not in rules.get("policy", {}):
            print(f"FAIL 未知策略键: {a.policy}", file=sys.stderr)
            return 2
        print(rules["policy"][a.policy])
        return 0
    if a.check_consistency:
        return check_consistency()
    ap.print_usage()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
