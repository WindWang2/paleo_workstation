#!/usr/bin/env python3
# LibreOffice vendored pin 双平台一致性门禁（方向 94）。
#
# 用法：
#   check_lo_pin.py [--root PATH]
#   check_lo_pin.py --selftest
#   未知参数一律 exit 2（与 check_layering.py / check_qgis_versions.py 同约定
# ——参数被静默忽略即假绿）。
#
# 校验 vendor/manifest.json 的 deps.libreoffice（Linux deb tarball）与
# deps.libreoffice_win（Windows MSI，方向 94 新增）：
#   1. 两块齐全：url / archive.sha256（64 hex）/ archive.size_bytes（正整数）
#      / layout.root / layout.soffice / version；
#   2. 跨平台版本对齐：两块 version 完全一致（跨平台转换血缘可追溯的前提，
#      PROVENANCE 的 converterVersion 才有单一口径）；
#   3. url 与 version 口径自洽：URL 内嵌版本（去第 4 位微版本）是 pin 版本的
#      前缀，且 linux 块指向 deb/x86_64 tar.gz、win 块指向 win/x86_64 msi；
#   4. layout.soffice 与 resolver 期望一致：linux program/soffice、
#      windows program/soffice.exe（src/io/dataimport_document.cpp 探测序）；
#   5. fetch-libreoffice.sh 确实引用 libreoffice_win pin（防脚本/manifest 漂移
#      ——脚本解错块或 manifest 改名都会静默退回 PATH 兜底）。
# 任一不满足 exit 1。
import json
import re
import sys
import shutil
import tempfile
from pathlib import Path

# Windows 裸跑 ctest 时控制台可能是 cp1252：中文报告行直接 UnicodeEncodeError
# 假红（CI 的 paleo-dev.ps1 test 已设 PYTHONUTF8=1，这里兜底手动场景）。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")

MANIFEST = "vendor/manifest.json"
FETCH_SH = "vendor/fetch-libreoffice.sh"


def fail(msg: str) -> int:
    print(f"FAIL check_lo_pin: {msg}")
    print("       fix: 修正 vendor/manifest.json 对应 pin（或 fetch 脚本引用）后重跑")
    return 1


def check_block(root: Path, key: str, expect_sub: str, expect_ext: str,
                expect_soffice: str, errs: list) -> dict:
    deps = json.loads((root / MANIFEST).read_text(encoding="utf-8"))["deps"]
    block = deps.get(key)
    if not isinstance(block, dict):
        errs.append(f"deps.{key} 缺失")
        return {}
    url = block.get("url", "")
    sha = (block.get("archive") or {}).get("sha256", "")
    size = (block.get("archive") or {}).get("size_bytes")
    layout = block.get("layout") or {}
    version = str(block.get("version", ""))
    if not re.fullmatch(r"[0-9]+(\.[0-9]+)+", version):
        errs.append(f"deps.{key}.version 非版本串: {version!r}")
    if not url.startswith("https://"):
        errs.append(f"deps.{key}.url 非 https: {url!r}")
    if expect_sub not in url:
        errs.append(f"deps.{key}.url 平台段异常（应含 {expect_sub}）: {url!r}")
    if not url.lower().endswith(expect_ext):
        errs.append(f"deps.{key}.url 后缀应为 {expect_ext}: {url!r}")
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        errs.append(f"deps.{key}.archive.sha256 非 64 位 hex: {sha!r}")
    if not isinstance(size, int) or size <= 0:
        errs.append(f"deps.{key}.archive.size_bytes 非正整数: {size!r}")
    if layout.get("root") != "vendor/libreoffice":
        errs.append(f"deps.{key}.layout.root 应为 vendor/libreoffice: {layout.get('root')!r}")
    if layout.get("soffice") != expect_soffice:
        errs.append(f"deps.{key}.layout.soffice 应为 {expect_soffice}: {layout.get('soffice')!r}")
    # URL 内嵌版本（如 26.2.6）应是 pin 版本（26.2.6.3）的前缀段。
    m = re.search(r"(\d+\.\d+\.\d+)", url.split("/")[-1])
    if m and not version.startswith(m.group(1)):
        errs.append(f"deps.{key} 版本 {version} 与 url 内嵌版本 {m.group(1)} 不一致")
    return block


def run(root: Path) -> int:
    errs: list = []
    linux = check_block(root, "libreoffice", "/deb/x86_64/", ".tar.gz",
                        "program/soffice", errs)
    win = check_block(root, "libreoffice_win", "/win/x86_64/", ".msi",
                      "program/soffice.exe", errs)
    if linux and win:
        if linux.get("version") != win.get("version"):
            errs.append(
                f"跨平台版本漂移: libreoffice={linux.get('version')!r} "
                f"vs libreoffice_win={win.get('version')!r}")
    sh = (root / FETCH_SH).read_text(encoding="utf-8")
    if "libreoffice_win" not in sh:
        errs.append(f"{FETCH_SH} 未引用 libreoffice_win pin（Windows 段漂移？）")
    if errs:
        for e in errs:
            print(f"  - {e}")
        return fail("; ".join(errs[:3]) + ("…" if len(errs) > 3 else ""))
    print("OK check_lo_pin: libreoffice/libreoffice_win pin 齐全且版本对齐 "
          f"({linux.get('version')})")
    return 0


def selftest() -> int:
    """合成树：五种漂移形态必须逐个打红，绿形态必须过。"""
    base = json.loads((Path(__file__).resolve().parents[1] / MANIFEST)
                      .read_text(encoding="utf-8"))
    base_sh = (Path(__file__).resolve().parents[1] / FETCH_SH
               ).read_text(encoding="utf-8")

    def mutate(fn):
        deps = json.loads(json.dumps(base["deps"]))
        fn(deps)
        return {**base, "deps": deps}

    cases = []
    d = dict(base["deps"]); d.pop("libreoffice_win"); cases.append(({"deps": d}, "win 块缺失"))
    def drop_sha(deps): deps["libreoffice_win"]["archive"]["sha256"] = "deadbeef"
    cases.append((mutate(drop_sha), "sha 漂移"))
    def ver_drift(deps): deps["libreoffice_win"]["version"] = "24.8.4.2"
    cases.append((mutate(ver_drift), "版本漂移"))
    def bad_layout(deps): deps["libreoffice_win"]["layout"]["soffice"] = "soffice.exe"
    cases.append((mutate(bad_layout), "layout 漂移"))
    def bad_url(deps): deps["libreoffice"]["url"] = deps["libreoffice"]["url"].replace("/deb/", "/rpm/")
    cases.append((mutate(bad_url), "url 平台段漂移"))

    failures = 0
    for i, (manifest, name) in enumerate(cases):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "vendor").mkdir()
            (root / MANIFEST).write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            (root / FETCH_SH).write_text(base_sh, encoding="utf-8")
            code = run(root)
            if code == 0:
                print(f"  selftest[{i}] {name}: 假绿！")
                failures += 1
            else:
                print(f"  selftest[{i}] {name}: 如期红 ✓")

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "vendor").mkdir()
        (root / MANIFEST).write_text(json.dumps(base, indent=2), encoding="utf-8")
        (root / FETCH_SH).write_text(base_sh, encoding="utf-8")
        if run(root) != 0:
            print("  selftest[绿形态]: 假红！")
            failures += 1
        else:
            print("  selftest[绿形态]: 如期绿 ✓")

    if failures:
        print(f"FAIL check_lo_pin selftest: {failures} 个用例不符")
        return 1
    print("OK check_lo_pin selftest")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        if len(args) != 1:
            return 2
        return selftest()
    if args and args[0] == "--root":
        if len(args) != 2:
            return 2
        return run(Path(args[1]))
    if args:
        return 2
    return run(Path(__file__).resolve().parents[1])


if __name__ == "__main__":
    sys.exit(main())
