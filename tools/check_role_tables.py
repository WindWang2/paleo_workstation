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

口径：只比对 key 集合（不比中文名/挂接域——两表职责不同，字段级对齐是
另一回事）。解析用正则提取两类登记形态：
  catalogroles.cpp: add("role", "group", order, ...)
  roleregistry.cpp: add(QStringLiteral("role"), ...) 与手工
                   t.append(RoleDef{QStringLiteral("role"), ...})

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
RE_CATALOG = re.compile(r'add\("([a-z_]+)"\s*,')
# add(QStringLiteral("role"), QStringLiteral("well"), ...)
# t.append(RoleDef{QStringLiteral("role"), ...
RE_REGISTRY = re.compile(
    r'(?:add\(|t\.append\(RoleDef\{)QStringLiteral\("([a-z_]+)"\)')


def extract(text: str, pattern) -> set:
    return set(pattern.findall(text))


def check_pair(catalog_text: str, registry_text: str, label_a: str,
               label_b: str):
    keys_a = extract(catalog_text, RE_CATALOG)
    keys_b = extract(registry_text, RE_REGISTRY)
    problems = []
    for missing in sorted(keys_a - keys_b):
        problems.append(
            f"{label_a} 有而 {label_b} 无：{missing}——链接词表侧漏登，"
            f"装载含该角色链接的工程会刷「未知角色」")
    for extra in sorted(keys_b - keys_a):
        problems.append(
            f"{label_b} 有而 {label_a} 无：{extra}——UI 树面/分组侧漏登，"
            f"该角色在树面上无名无组")
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
        print("角色双表一致：catalogroles 与 roleregistry 的 key 集合相等。")
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
         'add(QStringLiteral("a"), z); add(QStringLiteral("b"), w);',
         True, "两表一致应绿"),
        ('add("a", "well", 0, x); add("reference", "misc", 30, y);',
         'add(QStringLiteral("a"), z);',
         False, "catalogroles 多 reference 应红"),
        ('add("a", "well", 0, x);',
         'add(QStringLiteral("a"), z); t.append(RoleDef{QStringLiteral("boundary"), ...});',
         False, "roleregistry 手工 append 多 boundary 应红"),
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
    print("check_role_tables selftest 通过：3 夹具全部命中预期。")
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
