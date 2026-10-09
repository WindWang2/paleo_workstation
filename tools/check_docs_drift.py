#!/usr/bin/env python3
# 层：测试壳
"""文档漂移护栏（方向 100）：三项静态检查，防「文档口径再漂移」。

背景：#312 拆完主窗后 UI_LAYER_PLAN 仍写 ~2128 行、AUDIT_ISSUES 头部
「243 CTest suites」无日期绑定、goal-loop-prompts/README 份数与目录实数
曾靠人肉对齐。本护栏把三个「文档声明 vs 仓库实况」的比对机械化：

  ① 任务书份数：goal-loop-prompts/README.md 写明的
     `编号任务书份数：<整数>` vs 目录 `^[0-9]+-.*\\.md$`（不含 README.md）
     实数。**期望值只来自 README 那一行**——脚本不写死任何份数（写死 100
     会在 selftest 的「README=100 且实数=100 应绿」夹具上翻车，写死 94
     会在「README=94 且实数=94 应绿」夹具上翻车）。
  ② 主窗行数落点：docs/UI_LAYER_PLAN.md 中最后一条
     `实测（YYYY-MM-DD）：paleomainwindow.cpp N 行（家族 M TU）`日期戳
     现状行 vs 仓库实况——N 对 `src/ui/paleomainwindow.cpp` 实测行数按
     ±20% 容差（容差口径记 .goal-loop-ledger-docs-drift.md D1），M 对
     `src/ui/paleomainwindow_*.cpp` 家族 TU 数精确相等。历史块
     （autoplan-accepted / Review record）里的旧数字不是比对对象——
     冻结记录不设防，活口径必须带日期戳；无日期戳现状行即红。
  ③ 审计快照计数：AUDIT_ISSUES.md 头部（前 60 行）每处
     `N CTest suites` 计数句必须同句携带 YYYY-MM-DD 日期戳。计数是快照
     事实会自然过期，护栏只强制「计数必带日期」防无日期的「现况」断言。

用法：
  tools/check_docs_drift.py [--root PATH]
  tools/check_docs_drift.py --selftest
退出码：0 = 无违规；1 = 有违规；2 = 用法/自检失败（与 check_layering.py
同约定——参数被静默忽略即假绿）。
"""

import argparse
import re
import sys
import tempfile
import shutil
from pathlib import Path

# Windows 裸跑时控制台可能是 cp1252：中文报告行直接 UnicodeEncodeError
# 假红（与 check_layering.py / check_ci_scripts.py 同款守卫）。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

PROMPTS_DIR = Path("goal-loop-prompts")
README = PROMPTS_DIR / "README.md"
UI_PLAN = Path("docs/UI_LAYER_PLAN.md")
AUDIT = Path("AUDIT_ISSUES.md")
MAINWINDOW = Path("src/ui/paleomainwindow.cpp")
MAINWINDOW_FAMILY_GLOB = "paleomainwindow_*.cpp"

DECLARED_COUNT_RE = re.compile(r"编号任务书份数：\s*(\d+)\s*$")
PROMPT_FILE_RE = re.compile(r"^\d+-.*\.md$")
# ② 的日期戳现状行——UI_LAYER_PLAN W8 段承载（最后一条为准，历史注记
# 里若引用同款句式也自然取更晚者）。
LINE_CLAIM_RE = re.compile(
    r"实测（(\d{4}-\d{2}-\d{2})[^）]*）：paleomainwindow\.cpp\s*(\d+)\s*行"
    r"（家族\s*(\d+)\s*TU）"
)
AUDIT_COUNT_RE = re.compile(r"(\d+)\s*CTest\s+suites")
DATE_RE = re.compile(r"\d{4}-\d{2}-\d{2}")
LINE_TOLERANCE = 0.20  # ±20%
AUDIT_HEAD_LINES = 60


def check_prompts_count(root: Path, problems: list) -> None:
    readme = root / README
    if not readme.is_file():
        problems.append(f"① 缺 {README}——份数声明无从读取")
        return
    declared = None
    for line in readme.read_text(encoding="utf-8").splitlines():
        m = DECLARED_COUNT_RE.search(line)
        if m:
            declared = int(m.group(1))
            break
    if declared is None:
        problems.append(f"① {README} 未写明「编号任务书份数：<整数>」行")
        return
    actual = sum(
        1 for p in (root / PROMPTS_DIR).iterdir()
        if p.is_file() and PROMPT_FILE_RE.match(p.name) and p.name != "README.md"
    )
    if declared != actual:
        problems.append(
            f"① 任务书份数漂移：README 声明 {declared} 份，目录实数 {actual} 份"
            f"（{PROMPTS_DIR}/ 下 ^[0-9]+-.*\\.md$，不含 README.md）——"
            f"改 README 那一行或补/删文件，二者必须一致"
        )


def check_mainwindow_claim(root: Path, problems: list) -> None:
    plan = root / UI_PLAN
    if not plan.is_file():
        problems.append(f"② 缺 {UI_PLAN}")
        return
    text = plan.read_text(encoding="utf-8")
    claims = list(LINE_CLAIM_RE.finditer(text))
    if not claims:
        problems.append(
            f"② {UI_PLAN} 无「实测（日期）：paleomainwindow.cpp N 行（家族 M TU）」"
            f"日期戳现状行——活口径必须有带日期的实测锚点（W8 段承载）"
        )
        return
    date, claimed_lines, claimed_tus = claims[-1].group(1), int(claims[-1].group(2)), int(claims[-1].group(3))
    mw = root / MAINWINDOW
    if not mw.is_file():
        problems.append(f"② 缺 {MAINWINDOW}——实测无从谈起")
        return
    actual_lines = sum(1 for _ in mw.read_text(encoding="utf-8", errors="replace").splitlines())
    lo, hi = actual_lines * (1 - LINE_TOLERANCE), actual_lines * (1 + LINE_TOLERANCE)
    if not (lo <= claimed_lines <= hi):
        problems.append(
            f"② 主窗行数漂移：{UI_PLAN} 日期戳现状行（{date}）写 {claimed_lines} 行，"
            f"实测 {actual_lines} 行，超出 ±{int(LINE_TOLERANCE * 100)}% 容差"
            f"（[{lo:.0f}, {hi:.0f}]）——刷新 W8 段的实测行并更新日期戳"
        )
    family = sorted((root / MAINWINDOW.parent).glob(MAINWINDOW_FAMILY_GLOB))
    actual_tus = len(family)
    if claimed_tus != actual_tus:
        problems.append(
            f"② 主窗家族 TU 数漂移：现状行（{date}）写 {claimed_tus} TU，"
            f"src/ui/{MAINWINDOW_FAMILY_GLOB} 实数 {actual_tus} TU"
            f"（{', '.join(p.name for p in family)}）——W8 家族表与现状行同步刷新"
        )


def check_audit_stamp(root: Path, problems: list) -> None:
    audit = root / AUDIT
    if not audit.is_file():
        problems.append(f"③ 缺 {AUDIT}")
        return
    header = audit.read_text(encoding="utf-8", errors="replace").splitlines()[:AUDIT_HEAD_LINES]
    # 段内换行归一成空格再按句切——计数句可能被 markdown 换行拆开（历史
    # 实例：10-03 note 的「registers 243\nCTest suites」），逐行匹配会漏。
    text = " ".join(header)
    for sentence in re.split(r"(?<=[.。;；])\s+", text):
        m = AUDIT_COUNT_RE.search(sentence)
        if m and not DATE_RE.search(sentence):
            snippet = sentence.strip()[:120]
            problems.append(
                f"③ {AUDIT} 头部计数句无日期戳：「…{snippet}…」——"
                f"CTest 套件数是会过期的快照，计数所在句必须绑定 "
                f"YYYY-MM-DD 实测日期（如「{m.group(1)} CTest suites"
                f"（YYYY-MM-DD 实测）」）；日期在段首标题、计数在另一句"
                f"不算绑定"
            )


def run_checks(root: Path) -> list:
    problems: list = []
    check_prompts_count(root, problems)
    check_mainwindow_claim(root, problems)
    check_audit_stamp(root, problems)
    return problems


# ---- selftest：合成树夹具。绿夹具证明期望值来自 README（写死份数必翻车），
# 红夹具证明三类漂移与缺锚点都能被抓。 ----

def _write_tree(root: Path, declared: int, prompt_files: int,
                claim: str | None, mw_lines: int, family_tus: int,
                audit_count_line: str) -> None:
    prompts = root / PROMPTS_DIR
    prompts.mkdir(parents=True)
    readme = ["# Goal-Loop Prompts", "", f"编号任务书份数：{declared}", ""]
    (prompts / "README.md").write_text("\n".join(readme), encoding="utf-8")
    for i in range(1, prompt_files + 1):
        (prompts / f"{i:02d}-fake-direction.md").write_text("x", encoding="utf-8")
    docs = root / "docs"
    docs.mkdir()
    plan = ["# plan", ""]
    if claim is not None:
        plan.append(claim)
    (docs / "UI_LAYER_PLAN.md").write_text("\n".join(plan), encoding="utf-8")
    ui = root / "src/ui"
    ui.mkdir(parents=True)
    (ui / "paleomainwindow.cpp").write_text("\n" * mw_lines, encoding="utf-8")
    for i in range(family_tus):
        (ui / f"paleomainwindow_d{i}.cpp").write_text("x\n", encoding="utf-8")
    (root / AUDIT).write_text(
        f"# audit\n\n{audit_count_line}\n", encoding="utf-8"
    )


GOOD_CLAIM = "实测（2026-10-10，origin/master f474069b）：paleomainwindow.cpp 900 行（家族 2 TU）"


def selftest() -> int:
    cases = [
        # (name, should_pass, kwargs)
        ("份数一致（README=94 口径）", True,
         dict(declared=94, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2, audit_count_line="2026-10-10 快照：registers 377 CTest suites（2026-10-10 实测）。")),
        ("份数一致（README=100 且实数=100 也应绿——期望值只来自 README）", True,
         dict(declared=100, prompt_files=100, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2, audit_count_line="registers 100 CTest suites（2026-10-10 实测）")),
        ("份数漂移（README 写 100，实数 94——任务书点名的 mutation）", False,
         dict(declared=100, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2, audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("份数反向漂移（README 写 94，实数 95）", False,
         dict(declared=94, prompt_files=95, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2, audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("README 缺声明行", False,
         dict(declared=0, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2, audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("行数超容差（声称 2128 旧口径 vs 实测 900）", False,
         dict(declared=94, prompt_files=94,
              claim="实测（2026-10-10）：paleomainwindow.cpp 2128 行（家族 2 TU）",
              mw_lines=900, family_tus=2,
              audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("行数在 ±20% 容差内（声称 999 vs 实测 900）", True,
         dict(declared=94, prompt_files=94,
              claim="实测（2026-10-10）：paleomainwindow.cpp 999 行（家族 2 TU）",
              mw_lines=900, family_tus=2,
              audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("无日期戳现状行（只有无日期的行数句）", False,
         dict(declared=94, prompt_files=94, claim=None, mw_lines=900,
              family_tus=2, audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("家族 TU 数漂移（声称 21 实数 2）", False,
         dict(declared=94, prompt_files=94,
              claim="实测（2026-10-10）：paleomainwindow.cpp 900 行（家族 21 TU）",
              mw_lines=900, family_tus=2,
              audit_count_line="registers 377 CTest suites（2026-10-10 实测）")),
        ("审计计数无日期戳（243 旧口径形态）", False,
         dict(declared=94, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2,
              audit_count_line="current code uses C++20 and registers 243 CTest suites.")),
        ("审计计数跨行被拆开且句内无日期（AUDIT_ISSUES 实录形态）", False,
         dict(declared=94, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2,
              audit_count_line=(
                  "**2026-10-03 snapshot note:** This is the 2026-10-01 audit record, not a current\n"
                  "open-issue list. Its test counts and severity claims remain historical\n"
                  "evidence; current code uses C++20 and registers 243\n"
                  "CTest suites. The old workflows.cpp has been split."))),
        ("计数句自带日期（更新后目标形态）", True,
         dict(declared=94, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2,
              audit_count_line=(
                  "**snapshot note:** current code uses C++20 and registers 377\n"
                  "CTest suites（2026-10-10 实测；2026-10-03 时点 243，见方向 100 复核）."))),
        ("审计计数在 60 行头部之外不误伤", True,
         dict(declared=94, prompt_files=94, claim=GOOD_CLAIM, mw_lines=900,
              family_tus=2, audit_count_line="x" * 0 + "（该行仅占位）")),
    ]
    failures = 0
    for name, should_pass, kwargs in cases:
        tmp = Path(tempfile.mkdtemp(prefix="docs-drift-selftest-"))
        try:
            if name == "审计计数在 60 行头部之外不误伤":
                # 头部放干净计数句，60 行外放无日期计数句——不应误伤
                head = ["# audit", "", "registers 377 CTest suites（2026-10-10 实测）", ""]
                head += [f"<!-- padding {i} -->" for i in range(60)]
                head += ["", "old note: registers 243 CTest suites", ""]
                _write_tree(tmp, **{**kwargs, "audit_count_line": "（占位）"})
                (tmp / AUDIT).write_text("\n".join(head), encoding="utf-8")
            else:
                if name == "README 缺声明行":
                    _write_tree(tmp, **{k: v for k, v in kwargs.items()})
                    # declared=0 仍会写出声明行，改为抹掉
                    rp = tmp / README
                    rp.write_text("# Goal-Loop Prompts\n\n（无声明行）\n", encoding="utf-8")
                else:
                    _write_tree(tmp, **kwargs)
            problems = run_checks(tmp)
            passed = not problems
            status = "PASS" if passed == should_pass else "FAIL"
            if passed != should_pass:
                failures += 1
                print(f"[{status}] {name}（期望{'绿' if should_pass else '红'}，实得"
                      f"{'绿' if passed else '红'}：{problems}）")
            else:
                print(f"[{status}] {name}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"selftest：{len(cases) - failures}/{len(cases)} 通过")
    return 0 if failures == 0 else 2


def main() -> int:
    parser = argparse.ArgumentParser(description="文档漂移护栏（方向 100）")
    parser.add_argument("--root", default=None, help="仓库根（默认按脚本位置定位）")
    parser.add_argument("--selftest", action="store_true", help="合成夹具自检")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    problems = run_checks(root)
    if problems:
        print(f"docs drift：{len(problems)} 处违规")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("docs drift：三项检查绿（任务书份数 / 主窗行数落点 / 审计快照日期戳）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
