#!/usr/bin/env python3
# 层：测试壳
"""角色双表一致性护栏（#642）：catalogroles.cpp 与 roleregistry.cpp 的
角色 key 集合必须相等。

背景：仓库里有两套内置角色定义——catalogroles.cpp 的 builtinCatalogRoles()
（UI 树面/分组排序用）与 roleregistry.cpp 的 builtinTable()（链接词表/
准入诊断用）。「新角色两表同加」是既定做法（#630 的 boundary 就补了两张
表），但存量 reference 只在 catalogroles 一侧，导致每次装载含 reference
链接的工程都刷「未知角色: reference」并污染两表口径。本护栏把「两张表的
key 集合相等」机械化，下次漏加直接打红。

口径：比较 (角色, 挂接域)。catalogroles 的第二个参数是 UI 分组，
roleregistry 的第二个参数是实体类型。已写明的跨域复用用别名折到同一分组
（seismic_survey→seismic，study_area/sequence_boundary/planned→misc）。
分组表达不了的实体类型写在 catalogroles 的 `role-entity: <role> <type>` 行，
删掉词表侧对应定义而注释还在，闸门变红。

用法：
  tools/check_role_tables.py [--root PATH]
  tools/check_role_tables.py --selftest
退出码：0 = 无违规；1 = 有违规；2 = 用法/自检失败。
"""

import argparse
import re
import sys
import tempfile
import shutil
from pathlib import Path

if sys.stdout and sys.stdout.encoding and sys.stdout.encoding.lower() not in (
        "utf-8", "utf8"):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

CATALOG_ROLES = "src/catalog/catalogroles.cpp"
ROLE_REGISTRY = "src/catalog/roleregistry.cpp"

# add("role", "group", 30, QStringLiteral("..."))
RE_CATALOG = re.compile(r'add\("([a-z_]+)"\s*,\s*"([a-z_]+)"')
# add(QStringLiteral("role"), QStringLiteral("well"), ...)
RE_REGISTRY_ADD = re.compile(
    r'add\(\s*QStringLiteral\("([a-z_]+)"\)\s*,\s*QStringLiteral\("([a-z_]+)"\)')
# t.append(RoleDef{QStringLiteral("role"), {QStringLiteral("sequence_boundary")}, ...
RE_ROLEDEF = re.compile(
    r'RoleDef\{\s*QStringLiteral\("([a-z_]+)"\)\s*,\s*\{([^}]*)\}')
RE_TYPE = re.compile(r'QStringLiteral\("([a-z_]+)"\)')
# 分组表达不了的实体类型。例：role-entity: reference auxiliary
RE_ENTITY_LINE = re.compile(r'role-entity:\s*([a-z_]+)\s+([a-z_]+)')

# 词表实体类型 → catalogroles 分组。已经写明的跨域复用走这张表，不另开分组。
DOMAIN_ALIAS = {
    "seismic_survey": "seismic",
    "study_area": "misc",
    "sequence_boundary": "misc",
    "planned": "misc",
}


def canon(domain: str) -> str:
    return DOMAIN_ALIAS.get(domain, domain)


def registry_pairs(text: str) -> set:
    pairs = set(RE_REGISTRY_ADD.findall(text))
    for role, body in RE_ROLEDEF.findall(text):
        for entity in RE_TYPE.findall(body):
            pairs.add((role, entity))
    return pairs


def check_pair(catalog_text: str, registry_text: str, label_a: str,
               label_b: str):
    groups = set(RE_CATALOG.findall(catalog_text))
    declared = set(RE_ENTITY_LINE.findall(catalog_text))
    registry = registry_pairs(registry_text)
    problems = []
    for role, entity in sorted(registry):
        if (role, entity) in declared:
            continue
        if (role, canon(entity)) in groups:
            continue
        problems.append(
            f"{label_b} 有而 {label_a} 无：{role}@{entity}——挂接域没写进"
            f"分组或 role-entity 行，删一侧定义时不能靠另一个同 key 蒙混")
    for role, entity in sorted(declared):
        if (role, entity) not in registry:
            problems.append(
                f"{label_a} 写了 {role}@{entity}，{label_b} 没有这条挂接域")
    catalog_roles = {role for role, _group in groups}
    registry_roles = {role for role, _entity in registry}
    for missing in sorted(catalog_roles - registry_roles):
        problems.append(
            f"{label_a} 有而 {label_b} 无：{missing}——链接词表侧漏登")
    for extra in sorted(registry_roles - catalog_roles):
        problems.append(
            f"{label_b} 有而 {label_a} 无：{extra}——UI 树面/分组侧漏登")
    return problems


def run(root: Path) -> int:
    catalog = root / CATALOG_ROLES
    registry = root / ROLE_REGISTRY
    if not catalog.is_file() or not registry.is_file():
        print(f"检查失败：找不到 {catalog} 或 {registry}")
        return 2
    problems = check_pair(catalog.read_text(encoding="utf-8"),
                          registry.read_text(encoding="utf-8"),
                          "catalogroles", "roleregistry")
    if not problems:
        print("角色双表一致：catalogroles 与 roleregistry 的（角色, 挂接域）对齐。")
        return 0
    print(f"角色双表不一致（{len(problems)} 处）：")
    for p in problems:
        print(f"FAIL {p}")
    print("修复手段是补齐缺的角色登记（对齐另一表的 key/中文名），"
          "不是改本检查器。")
    return 1


def selftest() -> int:
    fixtures = [
        # (catalog, registry, expect_ok, 说明)
        ('add("a", "well", 0, x); add("b", "well", 1, y);',
         'add(QStringLiteral("a"), QStringLiteral("well"), z); '
         'add(QStringLiteral("b"), QStringLiteral("well"), w);',
         True, "两表一致应绿"),
        ('add("a", "well", 0, x); add("reference", "misc", 30, y);',
         'add(QStringLiteral("a"), QStringLiteral("well"), z);',
         False, "catalogroles 多 reference 应红"),
        ('add("a", "well", 0, x);',
         'add(QStringLiteral("a"), QStringLiteral("well"), z); '
         't.append(RoleDef{QStringLiteral("boundary"), {QStringLiteral("study_area")}, x});',
         False, "roleregistry 手工 append 多 boundary 应红"),
        ('add("reference", "misc", 30, y);\n// role-entity: reference well\n'
         '// role-entity: reference auxiliary\n',
         'add(QStringLiteral("reference"), QStringLiteral("well"), z);',
         False, "同 key、挂接域不同且一侧缺失应红"),
    ]
    failed = 0
    for catalog, registry, expect_ok, desc in fixtures:
        problems = check_pair(catalog, registry, "A", "B")
        ok = (not problems) if expect_ok else bool(problems)
        if not ok:
            print(f"selftest FAIL：{desc}（得 problems={problems}）")
            failed += 1
        else:
            print(f"selftest PASS：{desc}")
    if failed:
        return 2
    print("check_role_tables selftest 通过：4 夹具全部命中预期。")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="仓库根目录")
    parser.add_argument("--selftest", action="store_true",
                        help="跑合成夹具自检")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    return run(Path(args.root).resolve())


if __name__ == "__main__":
    sys.exit(main())
