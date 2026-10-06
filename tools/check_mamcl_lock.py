#!/usr/bin/env python3
"""MAMCL 依赖锁文件门禁（#142）。

校验 vendor/mamcl/<zip 基名>.requirements.lock：
  1. 每个 zip 都有同名锁文件；
  2. 锁内每个包都是 `name==version` 精确钉版本，且至少一个 --hash=sha256；
  3. zip 内 requirements.txt 的每个顶层依赖（剔除不在 PyPI 的 openzgy）都出现在锁里；
  4. cmake/extra-mamcl.cmake 登记的 SHA-256 与 zip 实际哈希一致。
零网络；`--selftest` 跑内置夹具。
"""
import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME_RE = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)")


def norm(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_lock(text):
    """返回 {包名: (版本, 哈希数)}；格式错误抛 ValueError。"""
    pins = {}
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("--hash="):
            if current is None or not line.startswith("--hash=sha256:"):
                raise ValueError(f"孤立或非 sha256 的哈希行：{line}")
            ver, n = pins[current]
            pins[current] = (ver, n + 1)
            continue
        spec = line.split(";")[0].rstrip("\\").strip()
        m = re.match(r"^([A-Za-z0-9][A-Za-z0-9._-]*)==([^\s\\]+)$", spec)
        if not m:
            raise ValueError(f"未精确钉版本：{line}")
        current = norm(m.group(1))
        pins[current] = (m.group(2), 0)
    for name, (_, n) in pins.items():
        if n == 0:
            raise ValueError(f"{name} 缺 --hash")
    return pins


def top_level(req_text):
    out = []
    for raw in req_text.replace("\r", "").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = NAME_RE.match(line)
        if m and norm(m.group(1)) != "openzgy":
            out.append(norm(m.group(1)))
    return out


def check(root):
    errors = []
    vdir = os.path.join(root, "vendor", "mamcl")
    cmake = open(os.path.join(root, "cmake", "extra-mamcl.cmake"), encoding="utf-8").read()
    zips = sorted(f for f in os.listdir(vdir) if f.endswith(".zip"))
    if not zips:
        errors.append("vendor/mamcl 下没有程序包")
    for z in zips:
        base = z[:-4]
        zpath = os.path.join(vdir, z)
        lock = os.path.join(vdir, base + ".requirements.lock")
        if not os.path.exists(lock):
            errors.append(f"{z}: 缺锁文件 {os.path.basename(lock)}")
            continue
        try:
            pins = parse_lock(open(lock, encoding="utf-8").read())
        except ValueError as e:
            errors.append(f"{os.path.basename(lock)}: {e}")
            continue
        with zipfile.ZipFile(zpath) as zf:
            reqs = [n for n in zf.namelist() if n.endswith("requirements.txt")]
            if not reqs:
                errors.append(f"{z}: zip 内无 requirements.txt")
                continue
            text = zf.read(sorted(reqs, key=len)[0]).decode("utf-8", "replace")
        for name in top_level(text):
            if name not in pins:
                errors.append(f"{os.path.basename(lock)}: 缺顶层依赖 {name}")
        digest = hashlib.sha256(open(zpath, "rb").read()).hexdigest()
        # #237：凡 zip 必须在 cmake 登记基名 + SHA——未登记本身即违规，
        # 不得短路掉哈希核对。
        if base not in cmake:
            errors.append(f"{z}: 未在 cmake/extra-mamcl.cmake 登记（凡程序包必须登记 SHA-256）")
        elif digest not in cmake:
            errors.append(f"cmake/extra-mamcl.cmake 登记的 SHA-256 与 {z} 实际 {digest} 不符")
    return errors


def selftest():
    good = "a==1.0 \\\n    --hash=sha256:" + "0" * 64 + "\nb==2 ; sys_platform == 'linux' \\\n    --hash=sha256:" + "1" * 64 + "\n"
    assert set(parse_lock(good)) == {"a", "b"}
    for bad in ("a>=1.0\n", "a==1.0\n", "a==1 \\\n    --hash=md5:00\n"):
        try:
            parse_lock(bad)
        except ValueError:
            continue
        raise AssertionError(f"应拒绝：{bad!r}")
    assert top_level("# c\nnumpy>=1\r\nopenzgy\nKmeans_Pytorch>=0.3\n") == ["numpy", "kmeans-pytorch"]
    # #237：未在 cmake 登记的 zip 必须报错（旧实现短路跳过哈希核对）。
    import tempfile
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "vendor", "mamcl"))
        os.makedirs(os.path.join(root, "cmake"))
        open(os.path.join(root, "cmake", "extra-mamcl.cmake"), "w", encoding="utf-8").write("# empty\n")
        with zipfile.ZipFile(os.path.join(root, "vendor", "mamcl", "rogue.zip"), "w") as zf:
            zf.writestr("rogue/requirements.txt", "a>=1\n")
        open(os.path.join(root, "vendor", "mamcl", "rogue.requirements.lock"), "w",
             encoding="utf-8").write("a==1.0 \\\n    --hash=sha256:" + "0" * 64 + "\n")
        errs = check(root)
        assert any("未在 cmake/extra-mamcl.cmake 登记" in e for e in errs), errs
    print("check_mamcl_lock selftest: PASS")
    return 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
    errors = check(ROOT)
    for e in errors:
        print("ERROR", e)
    if errors:
        return 1
    print("check_mamcl_lock: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
