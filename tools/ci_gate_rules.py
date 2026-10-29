#!/usr/bin/env python3
# 层：测试壳
"""Windows / Linux 测试门禁共享规则读取器（#760）。

单一真源是 tools/ci-gate-rules.json；本工具把它渲染成两端脚本可直接用的形态：

  ci_gate_rules.py --regex             → crash 判定正则（| 连接的 ERE）
  ci_gate_rules.py --result-line-regex → ctest 结果行正则（限定扫描范围）
  ci_gate_rules.py --policy KEY        → 单条放行策略说明
  ci_gate_rules.py --check-consistency → 两端脚本内联回落副本与 JSON 一致？
  ci_gate_rules.py --selftest          → 合成夹具正反向验证

背景（#760）：Windows 与 Linux 的门禁语义此前各自演化，四处不一致——
①超时判红（Windows 认 ***Timeout 为崩溃类，Linux 不认）；
②豁免放行（Windows 用 check_env_redset --emit-unexpected 过滤重跑集，Linux 全量重跑）；
③日志扫描范围（Windows 扫整份日志，Linux 只扫 ctest 结果行）；
④超时重跑（同上①的连带差异）。①②③④的收敛规则集中在本 JSON，
两端脚本只读不写；内联副本仅作 python 不可用时的回落，漂移即被
--check-consistency 打红。
未知参数一律 exit 2（与 check_layering.py 同约定——参数被静默忽略即假绿）。
"""
import argparse
import json
import re
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


def inline_copies():
    """→ {文件: 内联 crash 回落正则 or None}（找不到内联副本 = 漂移，无法回落）。

    取**最后一个非空**副本：ps1 里先有 `$crashPattern = ''` 占位、reader 失败后才
    赋内联字面量，最后那个非空的就是真回落副本。
    """
    out = {}
    for path in (GATE_SH, DEV_PS1):
        text = path.read_text(encoding="utf-8", errors="replace")
        hits = [h for h in INLINE_RE.findall(text) if h]
        out[path.name] = hits[-1] if hits else None
    return out


def check_consistency():
    rules = load_rules()
    want = crash_regex(rules)
    rc = 0
    copies = inline_copies()
    for name, got in copies.items():
        if got is None:
            print(f"FAIL {name}: 找不到内联 crash 回落副本（python 不可用时无路可退）")
            rc = 1
        elif got != want:
            print(f"FAIL {name}: 内联 crash 副本与 ci-gate-rules.json 不一致\n"
                  f"  json : {want}\n  inline: {got}")
            rc = 1
        else:
            print(f"  OK   {name}: 内联 crash 副本与 JSON 一致")
    # 结果行口径也必须落到两端：任一端不从共享规则取即漂移。
    for path in (GATE_SH, DEV_PS1):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "--result-line-regex" not in text:
            print(f"FAIL {path.name}: 未从共享规则取结果行口径（--result-line-regex）——"
                  f"扫描范围会重新漂移")
            rc = 1
        else:
            print(f"  OK   {path.name}: 引用共享结果行口径")
    return rc


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

    # 合成 rules：markers 顺序/内容变化必须如实反映到渲染结果。
    with tempfile.TemporaryDirectory() as t:
        p = Path(t) / "r.json"
        p.write_text(json.dumps({"crash_markers": ["A|B", "C"], "result_line_regex": "X"}),
                     encoding="utf-8")
        r = json.load(open(p, encoding="utf-8"))
        expect(crash_regex(r) == "A\\|B|C", "marker 含正则元字符必须被转义")
        expect("\\ " not in crash_regex(r), "空格不得被转义（POSIX ERE 未定义行为）")

    rc = check_consistency()
    if rc != 0:
        failures.append("--check-consistency 在合成校验下必须绿（两端内联副本应一致）")
    if failures:
        for f in failures:
            print("SELFTEST FAIL", f)
        return 1
    print("ci_gate_rules selftest: PASS（渲染/转义/Timeout 入库/两端一致性）")
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
