#!/usr/bin/env python3
# QGIS 三路版本口径一致性门禁（方向 71 依赖口径统一）。
#
# 用法：
#   check_qgis_versions.py [--root PATH]
#   check_qgis_versions.py --selftest
#   未知参数一律 exit 2（与 check_layering.py 同约定——参数被静默忽略即假绿）。
#
# 抽取三处 QGIS 版本口径并比对：
#   1. superbuild（源码 vendored 路）：vendor/superbuild/CMakeLists.txt 的
#      QGIS_URL（qgis-<x.y.z>.tar.*）+ qgis ExternalProject 的 URL_HASH 非空
#      + PATCH_COMMAND 引用的补丁文件名版本（patches/qgis-<x.y.z>-*.patch）。
#   2. deb 闭包（Linux CI/加速档）：vendor/deb-closure.lock 的
#      qgis-providers_<epoch>%3a<version>_amd64.deb 条目 → 上游版本
#      （去 epoch、去 +distro 修订后缀，与锁生成方 lock-debs.py 的
#      epoch:version 命名同一口径）。
#   3. OSGeo4W（Windows）：vendor/manifest.json 的 qgis_family → 家族
#      （major.minor；installer 只有包名粒度，精确 pin 不可表达——粒度
#      边界见 manifest notes，此处只要求家族一致）。
#
# 一致性定义（任一不满足即 exit 1）：
#   - superbuild 精确版本 == deb 闭包上游版本；
#   - 两者的 major.minor == manifest qgis_family 家族；
#   - 补丁文件名版本 == superbuild 精确版本（补丁名锁版本是既有纪律）；
#   - QGIS_URL 与 URL_HASH 成对钉死（哈希空/非 64 hex 即红）。
import re
import sys
import tempfile
import shutil
from pathlib import Path

# Windows 裸跑 ctest 时控制台可能是 cp1252：中文报告行直接 UnicodeEncodeError
# 假红（CI 的 paleo-dev.ps1 test 已设 PYTHONUTF8=1，这里兜底手动场景）。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")

SB_CMAKE = "vendor/superbuild/CMakeLists.txt"
LOCK = "vendor/deb-closure.lock"
MANIFEST = "vendor/manifest.json"

VER_RE = r"\d+\.\d+\.\d+"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_superbuild(root: Path):
    """返回 (精确版本, 补丁名版本, URL_HASH)；解析失败抛 RuntimeError。"""
    text = read_text(root / SB_CMAKE)
    m = re.search(r'set\(QGIS_URL\s+"[^"]*qgis-(%s)\.tar[^"]*"' % VER_RE, text)
    if not m:
        raise RuntimeError("%s: QGIS_URL 未钉位或无法解析出 qgis-<x.y.z>.tar 版本" % SB_CMAKE)
    ver = m.group(1)
    idx = text.find("ExternalProject_Add(qgis")
    if idx < 0:
        raise RuntimeError("%s: 找不到 ExternalProject_Add(qgis 块" % SB_CMAKE)
    block = text[idx:]
    h = re.search(r"URL_HASH SHA256=([0-9a-f]{64})", block)
    url_hash = h.group(1) if h else ""
    p = re.search(r"PATCH_FILE=\$\{CMAKE_CURRENT_SOURCE_DIR\}/patches/"
                  r"qgis-(%s)-[A-Za-z0-9._-]+\.patch" % VER_RE, block)
    if not p:
        raise RuntimeError("%s: PATCH_COMMAND 未引用 patches/qgis-<x.y.z>-*.patch" % SB_CMAKE)
    return ver, p.group(1), url_hash


def parse_deb_closure(root: Path):
    """返回 qgis-providers 条目的上游版本（如 4.2.3）；失败抛 RuntimeError。"""
    for line in read_text(root / LOCK).splitlines():
        # 锁行格式：'https://…/qgis-providers_<epoch>%3a<version>_amd64.deb' …
        m = re.search(r"qgis-providers_\d+%3a(" + VER_RE + r")[+_A-Za-z0-9.~]*_amd64\.deb", line)
        if m:
            return m.group(1)
    raise RuntimeError("%s: 找不到 qgis-providers_<epoch>:<version>_amd64.deb 条目" % LOCK)


def parse_manifest_family(root: Path):
    """返回 (家族 major.minor, 原始字段)；失败抛 RuntimeError。"""
    import json
    data = json.loads(read_text(root / MANIFEST))
    raw = data["deps"]["osgeo4w"]["qgis_family"]
    m = re.match(r"^(\d+\.\d+)(\.x)?$", raw)
    if not m:
        raise RuntimeError("%s: qgis_family %r 无法解析（期望形如 '4.2.x'）" % (MANIFEST, raw))
    return m.group(1), raw


def check(root: Path):
    """返回 (exit_code, 报告行列表)。0=一致，1=分裂/解析失败。"""
    lines = []
    try:
        sb_ver, patch_ver, url_hash = parse_superbuild(root)
        deb_ver = parse_deb_closure(root)
        family, family_raw = parse_manifest_family(root)
    except (RuntimeError, KeyError, ValueError) as e:
        return 1, ["FAIL 解析失败: %s" % e]
    lines.append("  superbuild  : %s（URL_HASH %s，补丁名 %s）"
                 % (sb_ver, url_hash[:12] + "…" if url_hash else "缺失!", patch_ver))
    lines.append("  deb 闭包    : %s（qgis-providers 上游版本）" % deb_ver)
    lines.append("  OSGeo4W     : %s（家族 %s，installer 包名粒度）" % (family_raw, family))
    ok = True
    if not url_hash:
        lines.append("FAIL superbuild QGIS URL_HASH 未钉死（URL 与 SHA256 必须成对）")
        ok = False
    if patch_ver != sb_ver:
        lines.append("FAIL 补丁文件名版本 %s != superbuild 版本 %s（补丁名锁版本）" % (patch_ver, sb_ver))
        ok = False
    if deb_ver != sb_ver:
        lines.append("FAIL deb 闭包上游版本 %s != superbuild 版本 %s（方向 71 统一口径）" % (deb_ver, sb_ver))
        ok = False
    fam_sb = ".".join(sb_ver.split(".")[:2])
    fam_deb = ".".join(deb_ver.split(".")[:2])
    if family not in (fam_sb, fam_deb) or fam_sb != fam_deb:
        lines.append("FAIL manifest qgis_family %s 与 superbuild/deb 家族 (%s/%s) 不一致"
                     % (family_raw, fam_sb, fam_deb))
        ok = False
    if ok:
        lines.append("OK 三路 QGIS 版本口径一致：%s（家族 %s）" % (sb_ver, family))
    return (0 if ok else 1), lines


# ---- selftest：合成树验证 通过/各类故意分裂 都能正确判定 --------------------
SELFTEST_SB_OK = """cmake_minimum_required(VERSION 3.28)
set(QGIS_URL "https://download.qgis.org/downloads/qgis-4.2.3.tar.bz2")
ExternalProject_Add(qgis
  URL "${QGIS_URL}"
  URL_HASH SHA256=b044889d895c07d9355512cba76973822830153f6f15feb0d5e00077d3d76e59
  PATCH_COMMAND ${CMAKE_COMMAND}
                -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/qgis-4.2.3-labels-with-layer.patch
)
"""
SELFTEST_LOCK_OK = "'https://qgis.org/debian/pool/main/q/qgis/qgis-providers_4.2.3%2b44resolute_amd64.deb' qgis-providers_1%3a4.2.3+44resolute_amd64.deb 8308866 SHA256:e17dca5f26df96f205c305ca41a30ad459a0761dcdef83ed519dc560d6487be4\n"
SELFTEST_MANIFEST_OK = ('{"schema": 1, "deps": {"osgeo4w": '
                        '{"qgis_family": "4.2.x", "packages": ["qgis"]}}}\n')


def _selftest_write_tree(base: Path, sb: str, lock: str, manifest: str):
    (base / "vendor/superbuild").mkdir(parents=True)
    (base / "vendor/superbuild" / "CMakeLists.txt").write_text(sb, encoding="utf-8")
    (base / "vendor").mkdir(exist_ok=True)
    (base / "vendor" / "deb-closure.lock").write_text(lock, encoding="utf-8")
    (base / "vendor" / "manifest.json").write_text(manifest, encoding="utf-8")


def selftest() -> int:
    cases = [
        ("一致基线", SELFTEST_SB_OK, SELFTEST_LOCK_OK, SELFTEST_MANIFEST_OK, 0),
        ("deb 上游版本分裂", SELFTEST_SB_OK,
         SELFTEST_LOCK_OK.replace("4.2.3", "4.2.4"), SELFTEST_MANIFEST_OK, 1),
        ("manifest 家族分裂", SELFTEST_SB_OK, SELFTEST_LOCK_OK,
         SELFTEST_MANIFEST_OK.replace("4.2.x", "4.3.x"), 1),
        ("补丁名版本分裂", SELFTEST_SB_OK.replace("qgis-4.2.3-labels", "qgis-4.2.2-labels"),
         SELFTEST_LOCK_OK, SELFTEST_MANIFEST_OK, 1),
        ("URL_HASH 缺失", SELFTEST_SB_OK.replace(
            "  URL_HASH SHA256=b044889d895c07d9355512cba76973822830153f6f15feb0d5e00077d3d76e59\n", ""),
         SELFTEST_LOCK_OK, SELFTEST_MANIFEST_OK, 1),
        ("superbuild 版本分裂", SELFTEST_SB_OK.replace("qgis-4.2.3.tar", "qgis-4.2.2.tar"),
         SELFTEST_LOCK_OK, SELFTEST_MANIFEST_OK, 1),
    ]
    base = Path(tempfile.mkdtemp(prefix="qgis-ver-selftest-"))
    failed = 0
    try:
        for name, sb, lock, manifest, want in cases:
            tree = base / name.replace(" ", "_")
            _selftest_write_tree(tree, sb, lock, manifest)
            code, _ = check(tree)
            status = "ok" if code == want else "FAIL"
            if code != want:
                failed += 1
            print("  selftest %-16s -> exit %d (期望 %d) %s" % (name, code, want, status))
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("  selftest %s" % ("OK" if failed == 0 else "FAILED (%d)" % failed))
    return 1 if failed else 0


def main(argv) -> int:
    root = None
    i = 1
    while i < len(argv):
        if argv[i] == "--selftest":
            return selftest()
        if argv[i] == "--root":
            if i + 1 >= len(argv):
                print("FAIL --root 需要一个参数" , file=sys.stderr)
                return 2
            root = Path(argv[i + 1])
            i += 2
            continue
        print("FAIL 未知参数: %s（用法：--root PATH | --selftest）" % argv[i], file=sys.stderr)
        return 2
    if root is None:
        root = Path(__file__).resolve().parent.parent
    code, lines = check(root)
    for line in lines:
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
