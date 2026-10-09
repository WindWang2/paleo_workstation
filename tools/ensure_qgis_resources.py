#!/usr/bin/env python3
"""QGIS prefix 资源闭包补齐（方向 81 srs.db 族）。

问题：本机 QGIS 前缀（Windows localdeps 的 ~/paleo-qgis-prefix 只有
bin/include/lib）与 deb 闭包解包前缀（fetch-deps.sh 只 dpkg-deb -x、不跑
postinst）都没有 resources/srs.db——tst_runtime / boot 的
QgsApplication::srsDatabaseFilePath() 断言必红，CRS 查表退化。

来源（同源不手抄）：与方向 71 的 deb 闭包同一把锁 vendor/deb-closure.lock——
QGIS 资源由其中的 qgis-providers-common（qgis.db / srs-template.db /
symbology-style.xml / customization.xml）与 qgis-common（其余 resources/**）
两个架构无关（_all）包提供；URL / 大小 / SHA-256 全取自锁文件，校验不过即拒。
srs.db 按 Debian postinst 同义生成：cp srs-template.db srs.db（不跑 crssync
——与官方包装后、触发器同步前的状态一致）。

用法：
  python3 tools/ensure_qgis_resources.py --prefix DIR [--layout auto|win|deb]
         [--cache DIR] [--lock FILE] [--check] [--force]
  python3 tools/ensure_qgis_resources.py --selftest

  --layout win : 资源落 <prefix>/resources（QGIS Windows 前缀布局，
                 pkgDataPath = prefix）
  --layout deb : 资源落 <prefix>/share/qgis/resources（/usr 布局）
  --layout auto: <prefix>/share/qgis 存在 → deb，否则 win
  --check      : 只检查 srs.db 是否就位（0=齐备，1=缺），不下载不写
  已有 resources/srs-template.db 而缺 srs.db（deb 闭包解包前缀）时只补 srs.db，
  不联网。写入后在资源目录落 .paleo-resources.json（来源包 + SHA-256 + 文件清单）。
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from urllib.parse import urlsplit
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = REPO / "vendor" / "deb-closure.lock"
DEFAULT_CACHE = REPO / "vendor" / "cache" / "debs"
# 提供 usr/share/qgis/resources/** 的锁内包（架构无关 _all 包，Windows 前缀可用）。
RESOURCE_PACKAGES = ("qgis-providers-common", "qgis-common")
RES_PREFIX = "usr/share/qgis/resources/"
MANIFEST_NAME = ".paleo-resources.json"


class Fail(Exception):
    pass


def parse_lock(text):
    """deb-closure.lock → [{url, file, size, sha256, package}]。

    行格式（apt-get --print-uris 口径）：'URL' 文件名 大小 SHA256:十六进制
    """
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^'([^']+)'\s+(\S+)\s+(\d+)\s+SHA256:([0-9a-fA-F]{64})$", line)
        if not m:
            continue
        url, fname, size, sha = m.groups()
        out.append({"url": url, "file": fname, "size": int(size), "sha256": sha.lower(),
                    "package": fname.split("_", 1)[0]})
    return out


def resource_entries(lock_text):
    entries = {e["package"]: e for e in parse_lock(lock_text)}
    missing = [p for p in RESOURCE_PACKAGES if p not in entries]
    if missing:
        raise Fail(f"deb-closure.lock 里找不到资源包 {missing}——锁与 RESOURCE_PACKAGES 不一致")
    return [entries[p] for p in RESOURCE_PACKAGES]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot_url_for(url, snapshot):
    """同 fetch-deps.sh：只给 Ubuntu 官方 pool 加锁时快照回退。"""
    parsed = urlsplit(url)
    if (parsed.scheme not in ("http", "https") or
            parsed.netloc not in ("archive.ubuntu.com", "security.ubuntu.com") or
            not parsed.path.startswith("/ubuntu/pool/")):
        return None
    stamp = os.environ.get("PALEO_DEB_SNAPSHOT")
    if not stamp:
        stamp = Path(snapshot).read_text(encoding="utf-8").strip() if Path(snapshot).is_file() else "20260926T000000Z"
    if not re.fullmatch(r"\d{8}T\d{6}Z", stamp):
        raise Fail(f"非法 deb snapshot 时间戳：{stamp!r}")
    return f"https://snapshot.ubuntu.com/ubuntu/{stamp}/pool/{parsed.path[len('/ubuntu/pool/'):]}"


def fetch(entry, cache, snapshot=DEFAULT_LOCK.with_suffix(".snapshot")):
    """取 .deb 到 cache（已缓存且校验通过则复用），返回路径；大小/SHA-256 不符即拒。"""
    cache.mkdir(parents=True, exist_ok=True)
    dest = cache / entry["file"]
    if dest.exists() and dest.stat().st_size == entry["size"] and sha256_of(dest) == entry["sha256"]:
        return dest
    tmp = dest.with_suffix(dest.suffix + ".part")
    urls = [entry["url"]]
    fallback = snapshot_url_for(entry["url"], snapshot)
    if fallback:
        urls.append(fallback)
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=120) as resp, open(tmp, "wb") as f:
                shutil.copyfileobj(resp, f)
            break
        except OSError as e:
            tmp.unlink(missing_ok=True)
            if url == urls[-1]:
                raise Fail(f"下载失败 {url}: {e}——可先在 Linux 上 ./vendor/fetch-deps.sh 取档，"
                           f"或把 {entry['file']} 放进 {cache}") from e
    size, sha = tmp.stat().st_size, sha256_of(tmp)
    if size != entry["size"] or sha != entry["sha256"]:
        tmp.unlink(missing_ok=True)
        raise Fail(f"{entry['file']} 校验不符（size {size} vs {entry['size']}，"
                   f"sha256 {sha} vs {entry['sha256']}）——拒绝使用")
    os.replace(tmp, dest)
    return dest


def ar_members(data):
    """最小 ar(1) 解析：{成员名: bytes}（.deb 外壳）。"""
    if not data.startswith(b"!<arch>\n"):
        raise Fail("不是 ar 归档（.deb 外壳）")
    pos, out = 8, {}
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode("ascii", "replace").strip().rstrip("/")
        size = int(hdr[48:58].decode("ascii").strip())
        pos += 60
        out[name] = data[pos:pos + size]
        pos += size + (size & 1)
    return out


def decompress_zstd(blob):
    try:  # Python 3.14+ 标准库
        from compression import zstd  # type: ignore
        return zstd.decompress(blob)
    except ImportError:
        pass
    try:
        import zstandard  # type: ignore
        return zstandard.ZstdDecompressor().decompressobj().decompress(blob)
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if exe:
        return subprocess.run([exe, "-dc"], input=blob, stdout=subprocess.PIPE, check=True).stdout
    raise Fail("data.tar.zst 需要 zstd 解压：Python≥3.14、`pip install zstandard` 或 PATH 上的 zstd 任一")


def data_tar(deb_bytes):
    members = ar_members(deb_bytes)
    for name, blob in members.items():
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            return tarfile.open(fileobj=io.BytesIO(decompress_zstd(blob)), mode="r:")
        return tarfile.open(fileobj=io.BytesIO(blob), mode="r:*")  # gz/xz/bz2/无压缩
    raise Fail("deb 里没有 data.tar.*")


def extract_resources(deb_path, res_dir):
    """把 usr/share/qgis/resources/** 解到 res_dir，返回写入的相对路径列表。"""
    written = []
    with data_tar(Path(deb_path).read_bytes()) as tf:
        for m in tf.getmembers():
            name = m.name[2:] if m.name.startswith("./") else m.name
            if not name.startswith(RES_PREFIX) or not (m.isfile() or m.isdir()):
                continue
            rel = name[len(RES_PREFIX):]
            if not rel or rel.startswith("/") or ".." in Path(rel).parts:
                continue
            target = res_dir / rel
            if m.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(m) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            written.append(rel)
    return written


def resources_dir(prefix, layout):
    prefix = Path(prefix)
    if layout == "auto":
        layout = "deb" if (prefix / "share" / "qgis").is_dir() else "win"
    return prefix / "resources" if layout == "win" else prefix / "share" / "qgis" / "resources"


def make_srs_db(res_dir):
    template = res_dir / "srs-template.db"
    if not template.is_file():
        raise Fail(f"缺 {template}——资源包未解出或锁内包不含 srs-template.db")
    shutil.copyfile(template, res_dir / "srs.db")


def ensure(prefix, layout="auto", cache=DEFAULT_CACHE, lock=DEFAULT_LOCK, force=False, log=print):
    res = resources_dir(prefix, layout)
    srs = res / "srs.db"
    if srs.is_file() and not force:
        log(f"ensure_qgis_resources: OK {srs}（已就位，跳过）")
        return res
    record = {"generated_by": "tools/ensure_qgis_resources.py",
              "resources_dir": Path(os.path.relpath(res, prefix)).as_posix(),
              "srs_db": "cp srs-template.db srs.db（Debian postinst 同义；未跑 crssync）"}
    if (res / "srs-template.db").is_file() and not force:
        # deb 闭包解包前缀：资源已在，只缺 postinst 生成的 srs.db——不联网。
        make_srs_db(res)
        record["source"] = "existing srs-template.db（deb 闭包解包前缀，补 postinst 步）"
        record["files"] = ["srs.db"]
    else:
        entries = resource_entries(Path(lock).read_text(encoding="utf-8"))
        res.mkdir(parents=True, exist_ok=True)
        files = []
        for e in entries:
            deb = fetch(e, Path(cache), Path(lock).with_suffix(".snapshot"))
            files += extract_resources(deb, res)
        make_srs_db(res)
        files.append("srs.db")
        record["source"] = {"lock": os.path.relpath(lock, REPO) if Path(lock).is_relative_to(REPO)
                            else str(lock),
                            "packages": [{k: e[k] for k in ("package", "file", "url", "sha256")}
                                         for e in entries]}
        record["files"] = sorted(set(files))
    (res / MANIFEST_NAME).write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n",
                                     encoding="utf-8")
    log(f"ensure_qgis_resources: 写入 {srs}（清单 {res / MANIFEST_NAME}，{len(record['files'])} 文件）")
    return res


# ---------------------------------------------------------------- selftest --
def _ar(members):
    out = bytearray(b"!<arch>\n")
    for name, blob in members:
        hdr = f"{name:<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(blob):<10}`\n".encode("ascii")
        assert len(hdr) == 60
        out += hdr + blob
        if len(blob) & 1:
            out += b"\n"
    return bytes(out)


def _zstd_compress(blob):
    """合成 zst 夹具用；本机无任何 zstd 实现时返回 None（该分支跳过）。"""
    try:
        from compression import zstd  # type: ignore
        return zstd.compress(blob)
    except ImportError:
        pass
    try:
        import zstandard  # type: ignore
        return zstandard.ZstdCompressor().compress(blob)
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if exe:
        return subprocess.run([exe, "-c", "-q"], input=blob, stdout=subprocess.PIPE, check=True).stdout
    return None


def _deb(files, zst=False):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w" if zst else "w:gz") as tf:
        for name, data in files.items():
            info = tarfile.TarInfo("./" + name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    if zst:
        packed = _zstd_compress(buf.getvalue())
        if packed is None:
            return None
        return _ar([("debian-binary", b"2.0\n"), ("control.tar.zst", b""), ("data.tar.zst", packed)])
    return _ar([("debian-binary", b"2.0\n"), ("control.tar.gz", b""), ("data.tar.gz", buf.getvalue())])


def selftest():
    from unittest.mock import patch
    from urllib.error import HTTPError
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        cache = tmp / "cache"          # 本地缓存（起始为空：首轮必走「下载」）
        mirror = tmp / "mirror"        # 假远端（file:// URL，走真实下载 + 校验路径）
        mirror.mkdir()
        debs = {
            "qgis-providers-common": _deb({RES_PREFIX + "srs-template.db": b"SQLite format 3\0tpl",
                                           RES_PREFIX + "qgis.db": b"qgisdb",
                                           "usr/share/doc/x": b"ignored"}),
            "qgis-common": _deb({RES_PREFIX + "data/world_map.gpkg": b"gpkg",
                                 RES_PREFIX + "../../../../evil": b"x"}),
        }
        lock_lines = []
        for pkg, blob in debs.items():
            fname = f"{pkg}_1%3a4.2.3+44resolute_all.deb"
            (mirror / fname).write_bytes(blob)
            sha = hashlib.sha256(blob).hexdigest()
            lock_lines.append(f"'{(mirror / fname).as_uri()}' {fname} {len(blob)} SHA256:{sha}")
        lock_lines.append("'https://example.invalid/libfoo_1_amd64.deb' libfoo_1_amd64.deb 1 SHA256:" + "0" * 64)
        lock = tmp / "deb-closure.lock"
        lock.write_text("\n".join(lock_lines) + "\n", encoding="utf-8")
        quiet = lambda *_: None

        # pool 被安全更新删档时仍取同一锁时快照，并继续检验摘要。
        snap = tmp / "deb-closure.snapshot"
        snap.write_text("20260926T000000Z\n", encoding="utf-8")
        entry = resource_entries(lock.read_text(encoding="utf-8"))[0]
        entry["url"] = "https://archive.ubuntu.com/ubuntu/pool/main/q/qgis/" + entry["file"]
        with patch.dict(os.environ, {"PALEO_DEB_SNAPSHOT": "20260926T000000Z"}):
            fallback = snapshot_url_for(entry["url"], snap)
            assert fallback.startswith("https://snapshot.ubuntu.com/ubuntu/20260926T000000Z/pool/")
            assert snapshot_url_for("https://qgis.org/ubuntu/pool/pkg.deb", snap) is None
            assert snapshot_url_for("https://archive.ubuntu.com.evil/ubuntu/pool/pkg.deb", snap) is None
            blob = debs[entry["package"]]
            with patch.object(urllib.request, "urlopen", side_effect=[
                    HTTPError(entry["url"], 404, "gone", {}, None), io.BytesIO(blob)]) as get:
                assert fetch(entry, tmp / "snapcache", snap).read_bytes() == blob
                assert [call.args[0] for call in get.call_args_list] == [entry["url"], fallback]
            with patch.object(urllib.request, "urlopen", side_effect=[
                    HTTPError(entry["url"], 404, "gone", {}, None), io.BytesIO(b"bad snapshot")]):
                try:
                    fetch(entry, tmp / "badsnapcache", snap)
                except Fail as e:
                    assert "校验不符" in str(e), e
                else:
                    raise AssertionError("快照摘要不符必须拒绝")

        # 1) Windows 布局全量补齐：资源 + srs.db + 清单；越界路径被拒。
        prefix = tmp / "winprefix"
        res = ensure(prefix, "win", cache, lock, log=quiet)
        assert res == prefix / "resources", res
        assert (res / "srs.db").read_bytes() == b"SQLite format 3\0tpl"
        assert (res / "qgis.db").is_file() and (res / "data" / "world_map.gpkg").is_file()
        assert not (tmp / "evil").exists() and not any(tmp.rglob("evil")), "路径穿越必须拒绝"
        man = json.loads((res / MANIFEST_NAME).read_text(encoding="utf-8"))
        assert [p["package"] for p in man["source"]["packages"]] == list(RESOURCE_PACKAGES)
        assert "srs.db" in man["files"] and "data/world_map.gpkg" in man["files"], man["files"]
        assert sorted(p.name for p in cache.iterdir()) == sorted(
            f"{k}_1%3a4.2.3+44resolute_all.deb" for k in debs), "下载后应落缓存"
        # 2) 幂等：已齐备 → 不动（哨兵内容保留）。
        (res / "srs.db").write_bytes(b"sentinel")
        ensure(prefix, "win", cache, lock, log=quiet)
        assert (res / "srs.db").read_bytes() == b"sentinel", "已齐备时不得重写 srs.db"
        # 3) 远端包被篡改（缓存为空、必须重下）→ 校验拒绝，不写 srs.db、不留缓存。
        bad = tmp / "badprefix"
        fname = "qgis-providers-common_1%3a4.2.3+44resolute_all.deb"
        good = (mirror / fname).read_bytes()
        (mirror / fname).write_bytes(good[:-1] + bytes([good[-1] ^ 0xFF]))
        cache2 = tmp / "cache2"
        try:
            ensure(bad, "win", cache2, lock, log=quiet)
        except Fail as e:
            assert "校验不符" in str(e), e
        else:
            raise AssertionError("SHA-256 不符必须拒绝")
        assert not (bad / "resources" / "srs.db").exists()
        assert not (cache2 / fname).exists(), "坏包不得进缓存"
        # 3b) 缓存里的坏包不被复用：远端恢复后自动重下覆盖。
        (cache / fname).write_bytes(b"corrupt")
        (mirror / fname).write_bytes(good)
        ensure(tmp / "healprefix", "win", cache, lock, log=quiet)
        assert (cache / fname).read_bytes() == good
        # 4) deb 闭包解包前缀：已有 srs-template.db、缺 srs.db → 只补 srs.db，不碰锁/网络。
        debprefix = tmp / "usr"
        (debprefix / "share" / "qgis" / "resources").mkdir(parents=True)
        (debprefix / "share" / "qgis" / "resources" / "srs-template.db").write_bytes(b"tpl2")
        res2 = ensure(debprefix, "auto", tmp / "nocache", tmp / "nolock", log=quiet)
        assert res2 == debprefix / "share" / "qgis" / "resources"
        assert (res2 / "srs.db").read_bytes() == b"tpl2"
        # 5) --check 语义 + 锁缺资源包即报错。
        assert main(["--prefix", str(prefix), "--layout", "win", "--check"]) == 0
        assert main(["--prefix", str(tmp / "empty"), "--layout", "win", "--check"]) == 1
        try:
            resource_entries(lock_lines[-1] + "\n")
        except Fail:
            pass
        else:
            raise AssertionError("锁缺资源包必须报错")
        # 6) data.tar.zst（真实 Ubuntu deb 的压缩形态）：有 zstd 实现即验解包。
        zdeb = _deb({RES_PREFIX + "srs-template.db": b"zst-tpl"}, zst=True)
        if zdeb is None:
            print("ensure_qgis_resources selftest: zst 分支跳过（本机无 zstd 实现）")
        else:
            (tmp / "z.deb").write_bytes(zdeb)
            zres = tmp / "zres"
            assert extract_resources(tmp / "z.deb", zres) == ["srs-template.db"]
            assert (zres / "srs-template.db").read_bytes() == b"zst-tpl"
    print("ensure_qgis_resources selftest: PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prefix")
    ap.add_argument("--layout", choices=("auto", "win", "deb"), default="auto")
    ap.add_argument("--cache", default=str(DEFAULT_CACHE))
    ap.add_argument("--lock", default=str(DEFAULT_LOCK))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.prefix:
        ap.error("--prefix 必填")
    if a.check:
        srs = resources_dir(a.prefix, a.layout) / "srs.db"
        ok = srs.is_file()
        print(f"ensure_qgis_resources: {'OK' if ok else 'MISSING'} {srs}")
        return 0 if ok else 1
    try:
        ensure(a.prefix, a.layout, Path(a.cache), Path(a.lock), a.force)
    except Fail as e:
        print(f"FAIL {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
