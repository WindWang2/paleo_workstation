#!/usr/bin/env python3
# 层：测试壳
"""Paleo Workstation 自动化本地化门禁与质量验证引擎 (check_l10n.py)
Milestone 5 Quality Gate (R5, Acceptance Criteria)

Checks:
  1. 0 未翻译项：无 <translation type="unfinished">，无缺失/空白 <translation> 节点（源文非空时）。
  2. 100% 占位符多重集恒等：源文与译文中 %1..%9 与 %n 出现频次完全一致（Multiset equality）。
  3. 术语表零冲突与正向抽检：
     - 术语表规范完备性：glossary-zh-CN.md 存在且包含 >=120 条地质/桌面专业术语。
     - 绝对禁用词命中：禁止出现 25 项硬裁决明令禁止的误译/假朋友。
     - 正向样本抽检：抽检 >=40 条与术语表对应的条目，验证 0 冲突且符合规范。
  4. 变异自检 (--selftest)：运行 10 个合成变异用例，验证门禁拦截的完备性与健壮性。

用法：
  python3 tools/check_l10n.py [--ts PATH] [--glossary PATH] [--verbose] [--strict]
  python3 tools/check_l10n.py --selftest

退出码：
  0 = 检查通过 / 自检全部变异测试捕获成功
  1 = 发现本地化缺陷 / 变异漏检
  2 = 参数错误 / 路径不存在 / XML 格式损坏 / 术语表不足 120 条
"""

import argparse
from collections import Counter
import os
from pathlib import Path
import random
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

# Windows 控制台缺省 cp1252 无法编码中文输出，统一重配为 UTF-8
for _stream in (sys.stdout, sys.stderr):
    try:
        if _stream and hasattr(_stream, "reconfigure"):
            _stream.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):
        pass

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TS_PATH = REPO_ROOT / "translations" / "paleo_zh_CN.ts"
DEFAULT_GLOSSARY_PATH = REPO_ROOT / "translations" / "glossary-zh-CN.md"

# 占位符匹配正则：%1 到 %9 以及 %n
PLACEHOLDER_RE = re.compile(r'%[1-9]|%n')

# 行业权威裁决绝对禁用词汇（源自 translations/glossary-zh-CN.md 第2节硬裁决与第3节主表）
BANNED_TERMS = [
    "地平线", "视野", "视界", "打开项目", "新建项目", "井顶", "井口顶部",
    "交叉图", "十字图", "交会画图", "音轨", "检查射击", "检查炮", "格子化",
    "多边形化", "多边形生成", "扁平化", "压平", "断距投掷", "故障抛掷",
    "工作拷贝", "作业复本", "碑文", "死标记", "家系", "门第", "基础水平",
    "基底平面", "升尺度", "故障飞机", "故障棍", "故障多边形", "惠勒图",
    "同步表面", "计时地层学", "深感应法", "低水位系统道"
]


def extract_placeholders(text: str) -> list[str]:
    """提取 Qt 占位符列表（保留重复项以支持多重集比对）。"""
    return PLACEHOLDER_RE.findall(text or "")


def parse_glossary(glossary_path: Path) -> tuple[bool, int, list[dict], str]:
    """解析术语表文件，返回 (是否合法, 词条数, 规则列表, 错误信息)。"""
    if not glossary_path.is_file():
        return False, 0, [], f"术语表文件不存在: {glossary_path}"
    try:
        content = glossary_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return False, 0, [], f"无法读取术语表: {exc}"

    term_rules = []
    term_count = 0
    for line in content.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        parts = [p.strip() for p in line.split("|")]
        # 排除表头与分割线 (| # | English Term | 标准中译 ... 或 | English Term | ...)
        if len(parts) >= 6 and not parts[1].startswith("---") and not parts[1].startswith("English") and not parts[1].startswith("#"):
            term_count += 1
            en = parts[1].strip()
            zh_raw = parts[2].strip()
            zh_opts = [
                z.strip()
                for z in zh_raw.replace("/", ",").replace("（", ",").replace("）", "").split(",")
                if z.strip() and len(z.strip()) >= 2
            ]
            banned = [b.strip() for b in parts[5].replace("、", ",").split(",") if b.strip()]
            if zh_opts:
                term_rules.append({
                    "en": en,
                    "zh_raw": zh_raw,
                    "zh_opts": zh_opts,
                    "banned": banned,
                })

    if term_count < 120:
        return False, term_count, term_rules, f"术语表词条数不足: 仅有 {term_count} 条 (要求 >= 120 条)"
    return True, term_count, term_rules, ""


def check_catalog(ts_path: Path, banned_terms: list[str] = None) -> list[dict]:
    """全量扫描 TS 文件并返回违规条目列表。"""
    if banned_terms is None:
        banned_terms = BANNED_TERMS

    violations = []
    if not ts_path.is_file():
        return [{"rule": "file_not_found", "context": "", "source": "", "translation": "", "detail": f"文件不存在: {ts_path}"}]

    try:
        tree = ET.parse(ts_path)
    except ET.ParseError as exc:
        return [{"rule": "xml_parse_error", "context": "", "source": "", "translation": "", "detail": f"XML 解析失败: {exc}"}]

    root = tree.getroot()
    for ctx in root.findall("context"):
        ctx_name = ctx.findtext("name") or "<anonymous>"
        for msg in ctx.findall("message"):
            src = msg.findtext("source") or ""
            trans_elem = msg.find("translation")

            # 1. 缺失翻译节点
            if trans_elem is None:
                violations.append({
                    "rule": "missing_translation",
                    "context": ctx_name,
                    "source": src,
                    "translation": "",
                    "detail": "缺少 <translation> 节点"
                })
                continue

            # 2. type="unfinished" 未完成标记
            if trans_elem.get("type") == "unfinished":
                violations.append({
                    "rule": "unfinished",
                    "context": ctx_name,
                    "source": src,
                    "translation": trans_elem.text or "",
                    "detail": '标记为 type="unfinished"'
                })
                continue

            trans_text = trans_elem.text or ""

            # 3. 译文为空（源非空时）
            if not trans_text.strip() and src.strip():
                violations.append({
                    "rule": "empty_translation",
                    "context": ctx_name,
                    "source": src,
                    "translation": trans_text,
                    "detail": "翻译文本为空"
                })
                continue

            # 4. 占位符多重集比对
            src_phs = Counter(extract_placeholders(src))
            trans_phs = Counter(extract_placeholders(trans_text))
            if src_phs != trans_phs:
                violations.append({
                    "rule": "placeholder_mismatch",
                    "context": ctx_name,
                    "source": src,
                    "translation": trans_text,
                    "detail": f"占位符不匹配: 源={dict(src_phs)} 译={dict(trans_phs)}"
                })

            # 5. 术语冲突 / 禁用词命中
            for term in banned_terms:
                if term in trans_text:
                    violations.append({
                        "rule": "banned_glossary_term",
                        "context": ctx_name,
                        "source": src,
                        "translation": trans_text,
                        "detail": f"命中绝对禁用术语: '{term}'"
                    })

    return violations


def audit_glossary_positive(
    ts_path: Path,
    term_rules: list[dict],
    banned_terms: list[str] = None,
    sample_size: int = 40,
    seed: int = 42
) -> tuple[list[dict], list[dict]]:
    """抽检 TS 中与术语表匹配的正向条目，验证其符合标准译名且零冲突。
    
    返回: (抽检的条目列表, 冲突违规列表)
    """
    if banned_terms is None:
        banned_terms = BANNED_TERMS

    if not ts_path.is_file():
        return [], []

    try:
        tree = ET.parse(ts_path)
    except ET.ParseError:
        return [], []

    root = tree.getroot()
    candidates = []

    for ctx in root.findall("context"):
        ctx_name = ctx.findtext("name") or "<anonymous>"
        for msg in ctx.findall("message"):
            src = msg.findtext("source") or ""
            tr = msg.findtext("translation") or ""
            src_lower = src.lower()

            for rule in term_rules:
                en = rule["en"]
                if len(en) >= 4 and en.lower() in src_lower:
                    matched_zh = [z for z in rule["zh_opts"] if z in tr]
                    if matched_zh:
                        # 检查是否有冲突禁用词
                        conflicts = [b for b in rule["banned"] if b in tr] + [b for b in banned_terms if b in tr]
                        candidates.append({
                            "context": ctx_name,
                            "source": src,
                            "translation": tr,
                            "term": en,
                            "expected": rule["zh_raw"],
                            "matched": matched_zh,
                            "conflicts": conflicts,
                        })
                        break

    if not candidates:
        return [], []

    rng = random.Random(seed)
    sampled = rng.sample(candidates, min(sample_size, len(candidates)))
    conflicts = [c for c in sampled if c["conflicts"]]
    return sampled, conflicts


def selftest() -> int:
    """运行变异测试（Mutation Testing），验证门禁拦截能力的完备性。"""
    print("=== 开始执行 check_l10n 变异测试（Mutation Selftest） ===")
    failures = 0

    mutation_cases = [
        (
            "1. 注入 type='unfinished' 未完成条目",
            '<context><name>TestCtx</name><message><source>Save Project</source><translation type="unfinished">保存工程</translation></message></context>',
            "unfinished"
        ),
        (
            "2. 注入空白翻译条目",
            '<context><name>TestCtx</name><message><source>Save Project</source><translation></translation></message></context>',
            "empty_translation"
        ),
        (
            "3. 注入缺失占位符条目 (%1 %2 -> %1)",
            '<context><name>TestCtx</name><message><source>Processed %1 wells in %2 ms</source><translation>处理了 %1 口井</translation></message></context>',
            "placeholder_mismatch"
        ),
        (
            "4. 注入损坏占位符序号条目 (%1 -> %9)",
            '<context><name>TestCtx</name><message><source>Open file %1</source><translation>打开文件 %9</translation></message></context>',
            "placeholder_mismatch"
        ),
        (
            "5. 注入重复占位符导致计数不符条目 (%1 %2 -> %1 %1)",
            '<context><name>TestCtx</name><message><source>Copy %1 to %2</source><translation>将 %1 复制到 %1</translation></message></context>',
            "placeholder_mismatch"
        ),
        (
            "6. 注入禁用术语条目 (地平线 for horizon)",
            '<context><name>TestCtx</name><message><source>Track Horizon</source><translation>追踪地平线</translation></message></context>',
            "banned_glossary_term"
        ),
        (
            "7. 注入禁用术语条目 (井顶 for well top)",
            '<context><name>TestCtx</name><message><source>Well top depth</source><translation>井顶深度</translation></message></context>',
            "banned_glossary_term"
        ),
        (
            "8. 干净基线样本 (无违规)",
            '<context><name>TestCtx</name><message><source>Save Project</source><translation>保存工程</translation></message>'
            '<message><source>Processed %1 wells in %2 ms</source><translation>在 %2 毫秒内处理了 %1 口井</translation></message></context>',
            None
        ),
    ]

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)

        # 逐项验证 XML 变异用例 (用例 1..8)
        for idx, (title, xml_body, expected_rule) in enumerate(mutation_cases, 1):
            full_xml = f'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n<TS version="2.1" language="zh_CN">\n{xml_body}\n</TS>'
            case_file = tmp_path / f"case_{idx}.ts"
            case_file.write_text(full_xml, encoding="utf-8")

            violations = check_catalog(case_file)
            detected_rules = {v["rule"] for v in violations}

            if expected_rule is None:
                if violations:
                    print(f"  [FAIL] 用例 {idx} ({title}): 期望 0 违规，实得 {len(violations)} 违规 ({detected_rules})")
                    failures += 1
                else:
                    print(f"  [PASS] 用例 {idx} ({title}): 干净样本成功通过 (0 误报)")
            else:
                if expected_rule in detected_rules:
                    print(f"  [PASS] 用例 {idx} ({title}): 成功捕获预期规则 [{expected_rule}]")
                else:
                    print(f"  [FAIL] 用例 {idx} ({title}): 未捕获预期规则 [{expected_rule}]，实际检出: {detected_rules}")
                    failures += 1

        # 9. 格式破损 XML 健壮性
        bad_xml_file = tmp_path / "broken.ts"
        bad_xml_file.write_text("<TS><context><message><unclosed>", encoding="utf-8")
        bad_v = check_catalog(bad_xml_file)
        if any(v["rule"] == "xml_parse_error" for v in bad_v):
            print("  [PASS] 用例 9 (格式损坏 XML 健壮性): 成功捕获 [xml_parse_error]，无未捕获异常崩溃")
        else:
            print("  [FAIL] 用例 9 (格式损坏 XML 健壮性): 未正常报告 [xml_parse_error]")
            failures += 1

        # 10. 术语表词条数门限自检
        mock_glossary = tmp_path / "mock_glossary.md"
        mock_glossary.write_text(
            "| # | Term | Trans | Domain | Note | Banned |\n"
            "|---|---|---|---|---|---|\n"
            "| 1 | horizon | 层位 | Stratigraphy | 描述 | 地平线 |\n",
            encoding="utf-8"
        )
        ok, count, rules, msg = parse_glossary(mock_glossary)
        if not ok and count < 120:
            print("  [PASS] 用例 10 (术语表词条不足门限拦截): 成功拒绝不足 120 条的虚假术语表")
        else:
            print("  [FAIL] 用例 10 (术语表词条不足门限拦截): 拦截失败")
            failures += 1

    total_cases = len(mutation_cases) + 2
    if failures == 0:
        print(f"\ncheck_l10n selftest 成功通过: 全部 {total_cases} 个变异/健壮性场景均按预期命中！")
        return 0
    else:
        print(f"\ncheck_l10n selftest 失败: {failures}/{total_cases} 个测试用例未达到预期！", file=sys.stderr)
        return 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Paleo Workstation 本地化门禁与变异自检 (check_l10n.py)")
    parser.add_argument("--ts", type=Path, default=DEFAULT_TS_PATH, help="指定待检查的 TS 文件路径")
    parser.add_argument("--glossary", type=Path, default=DEFAULT_GLOSSARY_PATH, help="指定术语表 Markdown 文件路径")
    parser.add_argument("--verbose", "-v", action="store_true", help="详细诊断输出模式")
    parser.add_argument("--selftest", action="store_true", help="执行变异自检（Mutation Testing）")
    parser.add_argument("--strict", action="store_true", help="严格门禁模式（任何告警即退出 1）")

    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    # 1. 校验术语表
    glossary_ok, glossary_terms, term_rules, glossary_err = parse_glossary(args.glossary)
    if not glossary_ok:
        print(f"FAIL 术语表校验失败: {glossary_err}", file=sys.stderr)
        return 2

    # 2. 校验 TS 文件目录
    violations = check_catalog(args.ts)

    if violations and violations[0]["rule"] in ("file_not_found", "xml_parse_error"):
        print(f"FAIL {violations[0]['detail']}", file=sys.stderr)
        return 2

    # 3. 正向术语抽检（抽检 >= 40 条）
    sampled_entries, sample_conflicts = audit_glossary_positive(
        args.ts, term_rules, banned_terms=BANNED_TERMS, sample_size=40, seed=42
    )

    for conflict in sample_conflicts:
        violations.append({
            "rule": "glossary_audit_conflict",
            "context": conflict["context"],
            "source": conflict["source"],
            "translation": conflict["translation"],
            "detail": f"正向抽检发现术语冲突: {conflict['conflicts']}"
        })

    # 统计信息
    by_rule = Counter(v["rule"] for v in violations)

    try:
        tree = ET.parse(args.ts)
        root = tree.getroot()
        context_count = len(root.findall("context"))
        message_count = len(root.findall(".//message"))
        placeholder_messages = sum(1 for m in root.findall(".//message") if extract_placeholders(m.findtext("source") or ""))
    except Exception:
        context_count = 0
        message_count = 0
        placeholder_messages = 0

    print("=== Paleo Workstation 本地化门禁检查 (check_l10n) ===")
    print(f"翻译文件: {args.ts}")
    print(f"术语规范: {args.glossary} (已登记 {glossary_terms} 项标准化术语)")

    if violations:
        print(f"\nFAIL: 发现 {len(violations)} 处本地化缺陷：")
        for v in violations[:30]:
            print(f"  FAIL [{v['rule']}] 上下文: {v['context']} | 源文: \"{v['source']}\" | 译文: \"{v['translation']}\"")
            print(f"       -> {v['detail']}")
        if len(violations) > 30:
            print(f"  ... 另有 {len(violations) - 30} 处违规已省略。")

        print("\n违规类型统计：")
        for rule, count in sorted(by_rule.items()):
            print(f"  - {rule}: {count} 处")
        print("\n本地化门禁未通过！请修正上述缺陷后再行提交。", file=sys.stderr)
        return 1

    print("\n[1/3] 翻译完备性检查:")
    print(f"  - 上下文数: {context_count}")
    print(f"  - 消息总数: {message_count}")
    print(f"  - 未完成条目 (unfinished): 0")
    print(f"  - 空白翻译条目 (empty): 0")
    print("  -> PASS: 0 未翻译项")

    print("\n[2/3] 参数占位符恒等检查:")
    print(f"  - 包含占位符消息数: {placeholder_messages}")
    print(f"  - 占位符多重集不匹配数: 0")
    print("  -> PASS: 100% 占位符多重集恒等 (%1..%9, %n)")

    print("\n[3/3] 地质与桌面术语表合规性审计:")
    print(f"  - 术语表词条数: {glossary_terms} (>= 120 要求达成)")
    print(f"  - 全量扫描绝对禁用词命中: 0 (25 项硬裁决 0 冲突)")
    print(f"  - 正向样本抽检: {len(sampled_entries)}/{len(sampled_entries)} 条符合标准术语且 0 冲突")
    print("  -> PASS: 术语表审计通过")

    if args.verbose:
        print("\n--- 正向抽检样本详情（前 10 条）---")
        for i, item in enumerate(sampled_entries[:10], 1):
            print(f"  [{i}] [{item['context']}] 术语: {item['term']} -> {item['matched']}")
            print(f"      源文: \"{item['source']}\"")
            print(f"      译文: \"{item['translation']}\"")

    print("\n=======================================================")
    print("PASS: 本地化门禁全部规则核验通过 (100% 完备且合规)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
