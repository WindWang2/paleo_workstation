#!/usr/bin/env python3
"""OSGeo4W 包集钉版刷新（#231）。

manifest.json 的 deps.osgeo4w 里 packages（直接包）+ closure（闭包全量）
必须是 OSGeo4W live 索引的显式快照：任何上游发版不再静默流进 Windows
CI/本机 bootstrap——漂移会被 paleo-dev.ps1 的装后校验打红，刷新 =
重跑本工具并提交（有意变更，同 vendor/deb-closure.lock 的口径）。

用法：
  python3 tools/pin_osgeo4w.py                  # 拉 live 索引，重写 manifest.json
  python3 tools/pin_osgeo4w.py --setup-ini PATH # 离线（测试/审计用同一份索引）
  python3 tools/pin_osgeo4w.py --selftest

机制：
  1. 解析 setup.ini 的 @-记录（version / requires）；
  2. 从 DIRECT_PACKAGES 出发沿 requires 解析全闭包；
  3. 只写 deps.osgeo4w 的 packages/closure 两个键，其余键原样保留。
"""
import json
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MANIFEST = REPO / "vendor" / "manifest.json"
SETUP_INI_URL = "https://download.osgeo.org/osgeo4w/v2/x86_64/setup.ini"

# 直接包清单（intent）。「qgis-devel-deps」已从索引下架，setup 对未知包名
# 只告警不装东西（CI 实锤），不再列入。增删直接包 = 有意变更，改这里。
DIRECT_PACKAGES = [
    "qgis", "qgis-devel", "qt6-devel", "qt6-oci", "gdal-devel", "proj-devel",
    "geos-devel", "sqlite3-devel", "qscintilla-qt6-devel", "zlib-devel",
]


def parse_setup_ini(text):
    """@-格式 → {包名: {字段: 值}}。"""
    pkgs = {}
    cur = None
    for line in text.splitlines():
        if line.startswith("@ "):
            cur = line[2:].strip()
            pkgs[cur] = {}
            continue
        if cur is None or not line.strip() or line.startswith("#"):
            continue
        if ":" in line:
            key, _, value = line.partition(":")
            pkgs[cur][key.strip()] = value.strip()
    return pkgs


def resolve_closure(pkgs, direct):
    """direct 出发沿 requires 的全闭包 {名: version}；缺包即报错（钉版不容缺件）。"""
    closure = {}
    stack = list(direct)
    while stack:
        name = stack.pop()
        if name in closure:
            continue
        rec = pkgs.get(name)
        if rec is None or "version" not in rec:
            raise SystemExit(f"FAIL 索引里找不到包 {name}（requires 断链或包下架）——"
                             "先修 DIRECT_PACKAGES 再跑")
        closure[name] = rec["version"]
        stack.extend(r for r in rec.get("requires", "").split() if r)
    return closure


def refresh(setup_ini_text, manifest_path=MANIFEST):
    pkgs = parse_setup_ini(setup_ini_text)
    missing = [d for d in DIRECT_PACKAGES if d not in pkgs]
    if missing:
        raise SystemExit(f"FAIL 直接包不在索引里: {missing}")
    closure = resolve_closure(pkgs, DIRECT_PACKAGES)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    osgeo = manifest["deps"]["osgeo4w"]
    osgeo["packages"] = {d: pkgs[d]["version"] for d in DIRECT_PACKAGES}
    osgeo["closure"] = {k: closure[k] for k in sorted(closure)}
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"pin_osgeo4w: 直接包 {len(DIRECT_PACKAGES)} / 闭包 {len(closure)} "
          f"-> {manifest_path}")
    return closure


def selftest():
    fixture = """arch: x86_64

@ base
version: 1.0-1

@ liba
version: 2.0-1
requires: base

@ app
version: 3.0-1
requires: liba libb

@ libb
version: 4.0-1
requires: base
"""
    pkgs = parse_setup_ini(fixture)
    assert pkgs["app"]["requires"] == "liba libb"
    closure = resolve_closure(pkgs, ["app"])
    assert closure == {"app": "3.0-1", "liba": "2.0-1", "libb": "4.0-1",
                       "base": "1.0-1"}, closure
    try:
        resolve_closure(pkgs, ["ghost"])
    except SystemExit:
        pass
    else:
        raise AssertionError("缺包必须报错")
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        mpath = Path(tmp) / "manifest.json"
        mpath.write_text(json.dumps({
            "schema": 1,
            "deps": {"osgeo4w": {
                "platform": "windows-x64", "site": "https://example.invalid",
                "installer_sha256": "0" * 64,
                "packages": {"app": "0-0"},
                "closure": {"app": "0-0"},
            }}}), encoding="utf-8")
        saved = DIRECT_PACKAGES[:]
        try:
            DIRECT_PACKAGES[:] = ["app"]
            refresh(fixture, mpath)
        finally:
            DIRECT_PACKAGES[:] = saved
        merged = json.loads(mpath.read_text(encoding="utf-8"))
        osgeo = merged["deps"]["osgeo4w"]
        assert osgeo["site"] == "https://example.invalid", "其余键必须原样保留"
        assert osgeo["packages"] == {"app": "3.0-1"}
        assert osgeo["closure"] == closure
    print("pin_osgeo4w selftest: PASS")
    return 0


def main():
    args = sys.argv[1:]
    if "--selftest" in args:
        return selftest()
    offline = None
    if "--setup-ini" in args:
        i = args.index("--setup-ini")
        try:
            offline = args[i + 1]
        except IndexError:
            print("FAIL --setup-ini 需要路径参数", file=sys.stderr)
            return 2
    if offline:
        text = Path(offline).read_text(encoding="utf-8", errors="replace")
    else:
        with urllib.request.urlopen(SETUP_INI_URL, timeout=60) as resp:
            text = resp.read().decode("utf-8", "replace")
    refresh(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
