#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
便携包打包器 —— 产出可在其他 Windows 电脑上直接双击运行的目录/压缩包。

设计要点
--------
1. **依赖闭包靠实测**：从产物 exe 出发做 BFS，逐层 dumpbin 解析 import 表，
   递归收集传递依赖。只收实际用到的 DLL，不做「整目录复制」——
   deps/Library/bin 有 400+ 个 dll，整拷会让包体膨胀数倍且混入版本冲突。
2. **Qt 6.11.2 双源**：主体模块（Core/Gui/Widgets/…）来自 conda deps
   （Library/bin），Qt6Pdf* 官方 6.11.2 扩展前缀补位（构建即如此链接，
   见 paleo-dev.ps1 Enter-LocalDepsEnvironment）。同名 DLL 以 deps 优先。
3. **Qt 插件按需**：platforms/qwindows.dll 是 GUI 必需；imageformats/
   iconengines/styles/sqldrivers 按名单收，调试变体（*d.dll）排除。
   插件必须与运行时 Qt 同源（conda），官方前缀的插件不进包。
4. **QGIS 前缀布局**：包根即 QGIS prefix——resources/ 整目录随包
   （srs.db/qgis.db/符号库/3D 模型，运行时自检硬依赖），
   启动器设 QGIS_PREFIX_PATH=%~dp0。provider 内建在 qgis_core.dll，
   本机 prefix 无 plugins 目录也可用，包内同构。
5. **GDAL/PROJ 数据**：share/gdal + share/proj 随包，启动器设
   GDAL_DATA / PROJ_LIB / PROJ_DATA——缺省时 GDAL 找不到 tms json、
   PROJ 报 CRS 无大地基准（paleo-dev.ps1 同款口径）。
6. **禁止污染**：只读源目录，全部写进 build/portable/。
7. **--verify**：打包后用净环境（PATH 只含包目录 + System32）跑包内
   paleo_selfcheck.exe，模拟新机器首启；GUI 冒烟用 paleo.exe --help。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from collections import deque
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---- 源位置（可用 --deps/--qgis-prefix/--qt-pdf 覆盖）-------------------------
DEPS = Path(r"C:\Users\wangj.KEVIN\paleo-qgis-deps")
DEPS_BIN = DEPS / "Library" / "bin"
QGIS_PREFIX = Path(r"C:\Users\wangj.KEVIN\paleo-qgis-prefix")
QT_PDF_BIN = Path(r"C:\deps\Qt\6.11.2\msvc2022_64\bin")
ORT_LIB = REPO_ROOT / "vendor" / "onnxruntime" / "lib"

# 搜索根：收集 DLL 时按优先级查找（同名 DLL 前面的根胜出——
# conda deps 的 Qt 6.11.2 必须压过官方前缀，避免混 ABI）。
SEARCH_ROOTS = [
    DEPS_BIN,
    QGIS_PREFIX / "bin",
    QT_PDF_BIN,
    ORT_LIB,
    Path(r"C:\Windows\System32"),
]

# 产品级 exe（要进包的）；测试 exe 不进包
PRODUCT_EXES = [
    "paleo.exe",                  # 主应用（GUI）
    "paleo_mkproject.exe",         # 工程脚手架
    "paleo_seismic_demo.exe",      # 地震展示 demo
    "paleo_selfcheck.exe",         # 自检（新机排障首选）
]

# 延迟加载 / 运行时按名装载的模块：dumpbin 静态 import 表扫不到。
# find_package 面里 paleo.exe 未静态 import 的：SerialPort/Positioning/
# UiTools/Concurrent；onnxruntime.dll 会延迟拉 providers_shared。
EXTRA_DLLS = [
    "Qt6SerialPort.dll", "Qt6Positioning.dll", "Qt6UiTools.dll",
    "Qt6Concurrent.dll", "Qt6DBus.dll",
    "onnxruntime_providers_shared.dll",
]

# 系统 DLL 白名单（目标机 Windows 10/11 自带，不进包）
SYSTEM_DLL = re.compile(
    r"^(api-ms-win-|ext-ms-win-|kernel32|kernelbase|ntdll|user32|gdi32|"
    r"advapi32|shell32|ole32|oleaut32|comdlg32|comctl32|shlwapi|msvcrt|"
    r"rpcrt4|winmm|ws2_32|crypt32|bcrypt|ncrypt|secur32|wintrust|"
    r"imm32|version|setupapi|psapi|imagehlp|dbghelp|winhttp|urlmon|"
    r"dwmapi|uxtheme|uxcore|shcore|propsys|oleacc|uiautomationcore|"
    r"wininet|mswsock|normaliz|userenv|wtsapi32|netapi32|"
    r"powrprof|jobapi|memapi|cfgmgr|wevtapi|sechost|taskschd|tasksch)",
    re.I,
)

# 明确排除（Python 运行时非 GUI 必需且体积大；paleo 的 python 面向
# QProcess 外部解释器，缺省时相关功能优雅降级，不阻塞主程序）
EXCLUDE_DLL = {
    "python3.dll", "python314.dll", "python.exe", "pythonw.exe",
}


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def find_dumpbin() -> Path | None:
    base = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC")
    if not base.is_dir():
        return None
    for v in sorted(base.iterdir(), reverse=True):
        p = v / "bin" / "Hostx64" / "x64" / "dumpbin.exe"
        if p.is_file():
            return p
    return None


def dumpbin_imports(exe: Path, dumpbin: Path) -> set:
    r = run([str(dumpbin), "/nologo", "/dependents", str(exe)])
    if r.returncode != 0:
        return set()
    return set(m.group(1) for m in re.finditer(r"^\s{4}(\S+\.dll)\s*$", r.stdout, re.M))


def resolve(name: str, roots) -> Path | None:
    for r in roots:
        c = r / name
        if c.is_file():
            return c
    return None


def collect_dll_closure(entry_exes: list[Path], dumpbin: Path) -> tuple[dict, list]:
    """BFS 收集 DLL 闭包。返回 (dll名 -> 源路径, 缺失清单)。

    凡只在 C:\\Windows\\System32 命中的依赖视为 Windows 自带（10/11 均随 OS
    发行），不进包——拷系统 DLL 进包既膨胀又会在目标机上以 exe 目录优先级
    遮蔽系统版本，属反模式。VC 运行库（msvcp140/vcruntime140*）在 conda
    bin 先命中，仍正常随包。"""
    system32 = Path(r"C:\Windows\System32").resolve()
    roots = [r for r in SEARCH_ROOTS if r.is_dir()]
    seen: dict[str, Path] = {}
    missing: list[str] = []
    queue = deque()

    for e in entry_exes:
        for d in dumpbin_imports(e, dumpbin):
            queue.append(d)

    while queue:
        name = queue.popleft()
        lname = name.lower()
        if lname in {s.lower() for s in seen}:
            continue
        if SYSTEM_DLL.match(name) or lname in EXCLUDE_DLL:
            continue
        src = resolve(name, roots)
        if not src:
            missing.append(name)
            continue
        if src.resolve().parent == system32:
            seen[name] = src  # 占位防重扫；复制阶段按 System32 跳过
            continue
        seen[name] = src
        for d in dumpbin_imports(src, dumpbin):
            if d.lower() not in {s.lower() for s in seen}:
                queue.append(d)
    return seen, missing


def copy_qt_plugins(dest_root: Path) -> list[str]:
    """收 Qt 插件。源必须是 Qt 6.11.2 布局——conda 的 qt6-main 把插件放在
    Library/lib/qt6/plugins（Library/plugins 是 Qt5 遗留，元数据 5.15，
    混进包会在目标机上以「incompatible Qt library」拒载）。"""
    copied = []
    plugin_root = DEPS / "Library" / "lib" / "qt6" / "plugins"
    if not plugin_root.is_dir():
        return copied
    keep = {
        "platforms": {"qwindows.dll", "qoffscreen.dll"},
        "styles": {"qmodernwindowsstyle.dll"},
        "iconengines": {"qsvgicon.dll"},
        "sqldrivers": {"qsqlite.dll"},
        "imageformats": None,  # 前缀名单，见下
    }
    img_prefixes = ("qjpeg", "qgif", "qico", "qsvg", "qwebp", "qtiff", "qwbmp", "qtga")
    for sub, names in keep.items():
        d = plugin_root / sub
        if not d.is_dir():
            continue
        for p in sorted(d.glob("*.dll")):
            lname = p.name.lower()
            if lname.endswith("d.dll"):          # 调试变体不进生产包
                continue
            if names is not None and lname not in names:
                continue
            if names is None and not any(lname.startswith(k) for k in img_prefixes):
                continue
            out = dest_root / "plugins" / sub
            out.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, out / p.name)
            copied.append(f"plugins/{sub}/{p.name}")
    return copied


def dir_size(p: Path) -> str:
    tot = sum(f.stat().st_size for f in p.rglob("*") if f.is_file())
    for unit, div in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if tot >= div:
            return f"{tot/div:.1f} {unit}"
    return f"{tot} B"


LAUNCHER_BAT = (
    "@echo off\r\n"
    "chcp 65001 >nul\r\n"
    "setlocal\r\n"
    "cd /d \"%~dp0\"\r\n"
    "rem -- Qt / QGIS / GDAL / PROJ 资源定位（全部指向包内）--\r\n"
    "set \"QT_PLUGIN_PATH=%~dp0plugins\"\r\n"
    "set \"QGIS_PREFIX_PATH=%~dp0\"\r\n"
    "set \"GDAL_DATA=%~dp0share\\gdal\"\r\n"
    "set \"PROJ_LIB=%~dp0share\\proj\"\r\n"
    "set \"PROJ_DATA=%~dp0share\\proj\"\r\n"
    "rem WebEngine：绕系统代理直连本机服务；conda 构建子进程关沙箱/GPU 防断言崩溃；\r\n"
    "rem Qt Quick 后端钉 OpenGL 与主窗口（QGIS 画布）合成 API 一致。\r\n"
    "set \"QTWEBENGINE_CHROMIUM_FLAGS=--proxy-server=direct:// --no-sandbox --disable-gpu\"\r\n"
    "set \"QSG_RHI_BACKEND=opengl\"\r\n"
    "set \"PATH=%~dp0;%PATH%\"\r\n"
    "start \"\" \"%~dp0paleo.exe\" %*\r\n"
)

README_TXT = (
    "paleo_workstation 便携包\r\n"
    "========================\r\n\r\n"
    "双击「启动-paleo.bat」运行主程序。\r\n"
    "双击 paleo_seismic_demo.exe 运行地震展示 demo。\r\n"
    "遇到启动/运行问题时，先双击 paleo_selfcheck.exe 查看环境自检。\r\n\r\n"
    "目录说明：\r\n"
    "  *.exe            产品可执行文件\r\n"
    "  *.dll            Qt 6.11.2 / QGIS 4.2 / GDAL / PROJ / onnxruntime 运行时闭包\r\n"
    "  plugins/         Qt 平台/图像/图标/样式/SQLite 驱动插件（与 exe 同目录的 Qt 配套）\r\n"
    "  resources/       QGIS 资源（srs.db、符号库、3D 模型）——包根即 QGIS prefix\r\n"
    "  share/gdal,proj  GDAL/PROJ 数据目录（坐标系与栅格驱动元数据）\r\n"
    "  resources-paleo/ 应用内样式、文档、示例\r\n"
    "  office-editor/   Office 原件编辑页（OnlyOffice WASM；AGPL——见 office-editor-NOTICE.txt）\r\n"
    "  testdata/        示例工程与地质夹具（可删）\r\n\r\n"
    "系统要求：Windows 10/11 x64。无需安装 Python、Qt 或 QGIS。\r\n"
    "Python 脚本台面等可选功能需另装 Python（PALEO_PYTHON 指向解释器）。\r\n"
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True, help="构建目录（含产物 exe）")
    ap.add_argument("--out", default=None, help="输出目录，默认 <build>/portable")
    ap.add_argument("--zip", action="store_true", help="额外产出 zip")
    ap.add_argument("--verify", action="store_true",
                    help="净环境跑包内 selfcheck + paleo.exe 冒烟（模拟新机）")
    args = ap.parse_args()

    build = Path(args.build_dir).resolve()
    out = Path(args.out).resolve() if args.out else build / "portable"
    src_root = build.parent

    dumpbin = find_dumpbin()
    if not dumpbin:
        sys.exit("找不到 dumpbin（需要 VS2022 Build Tools）")

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    # 1) 产品 exe
    print("[1/7] 收集产品 exe …")
    exes = []
    for name in PRODUCT_EXES:
        p = build / name
        if p.is_file():
            shutil.copy2(p, out / name)
            exes.append(p)
            print(f"      + {name}  ({p.stat().st_size/1e6:.1f} MB)")
        else:
            print(f"      ! 缺失 {name}")

    # 2) DLL 闭包
    print("[2/7] 解析 DLL 传递依赖闭包（dumpbin BFS）…")
    closure, missing = collect_dll_closure(exes, dumpbin)
    print(f"      收集 {len(closure)} 个 DLL；未解析 {len(missing)} 个")
    for m in sorted(missing)[:20]:
        print(f"        - {m}")

    print("[3/7] 复制 DLL …")
    system32 = Path(r"C:\Windows\System32").resolve()
    skipped_sys = 0
    for name, src in sorted(closure.items()):
        if src.resolve().parent == system32:
            skipped_sys += 1
            continue
        shutil.copy2(src, out / name)
    # 延迟加载补充（按搜索根解析，避免硬编码目录）
    for name in EXTRA_DLLS:
        if (out / name).is_file():
            continue
        src = resolve(name, SEARCH_ROOTS)
        if src:
            shutil.copy2(src, out / name)
            print(f"      + {name}  (延迟加载)")
    print(f"      DLL 就绪（{dir_size(out)}；跳过 {skipped_sys} 个 System32 系统库）")

    # WebEngine 运行件——闭包只收 DLL；进程 exe/pak/icudtl/语言包须显式落位。
    # 注意 conda 版 WebEngine 按运行时打印的搜索路径找 <exe目录>/share/qt6/
    # resources（conda 布局，非官方 Qt 的 <exe>/resources 布局）：
    #   share/qt6/resources/{icudtl.dat, qtwebengine_resources*.pak}
    #   share/qt6/translations/qtwebengine_locales/
    #   QtWebEngineProcess.exe 与 DLL 同目录（root）。
    we = DEPS / "Library" / "lib" / "qt6" / "QtWebEngineProcess.exe"
    if we.is_file():
        shutil.copy2(we, out / "QtWebEngineProcess.exe")
        res = DEPS / "Library" / "share" / "qt6" / "resources"
        we_out = out / "share" / "qt6" / "resources"
        we_out.mkdir(parents=True, exist_ok=True)
        n_pak = 0
        for p in list(res.glob("icudtl.dat")) + list(res.glob("v8_context_snapshot.bin")) \
                 + list(res.glob("qtwebengine_resources*.pak")):
            if p.name.endswith(".debug.pak"):
                continue
            shutil.copy2(p, we_out / p.name)
            n_pak += 1
        loc = DEPS / "Library" / "share" / "qt6" / "translations" / "qtwebengine_locales"
        if loc.is_dir():
            shutil.copytree(loc, out / "share" / "qt6" / "translations" / "qtwebengine_locales",
                            dirs_exist_ok=True)
        print(f"      + WebEngine 运行件（process + {n_pak} resources + locales → share/qt6）")

    # 4) Qt 插件（Qt 6.11.2 conda 布局，与运行时 Qt 同 ABI）
    print("[4/7] 复制 Qt 插件 …")
    for c in copy_qt_plugins(out):
        print(f"      + {c}")

    # 5) QGIS 前缀资源（包根即 prefix）
    print("[5/7] 复制 QGIS 运行时资源 …")
    qres = QGIS_PREFIX / "resources"
    if qres.is_dir():
        shutil.copytree(qres, out / "resources", dirs_exist_ok=True)
        print(f"      resources/  {dir_size(out / 'resources')}")
    else:
        print(f"      ! 缺失 {qres}")

    # 6) GDAL/PROJ 数据 + 应用资源 + Office 编辑页
    print("[6/7] 复制 GDAL/PROJ 数据与应用资源 …")
    for s, name in ((DEPS / "Library" / "share" / "gdal", "share/gdal"),
                    (DEPS / "Library" / "share" / "proj", "share/proj")):
        if s.is_dir():
            shutil.copytree(s, out / name, dirs_exist_ok=True)
            print(f"      {name}  {dir_size(out / name)}")
    for d in ("resources", "testdata"):
        s = src_root / d
        if s.is_dir():
            target = "resources-paleo" if d == "resources" else d
            shutil.copytree(s, out / target, dirs_exist_ok=True)
            print(f"      {target}/  {dir_size(out / target)}")
    # Office 原件编辑页（ranuts/document，OnlyOffice WASM 静态页）——应用按
    # 「同目录 office-editor/」解析（vendor/fetch-ranuts-document.sh 产物）。
    office = REPO_ROOT / "vendor" / "ranuts-document"
    if office.is_dir() and (office / "index.html").is_file():
        shutil.copytree(office, out / "office-editor", dirs_exist_ok=True)
        print(f"      office-editor/  {dir_size(out / 'office-editor')}")
        (out / "office-editor-NOTICE.txt").write_text(
            "office-editor/ 是 ranuts/document（github.com/ranuts/document，"
            "tag v0.0.6，commit 82cb9f68617383c1b3a00865a5880069cbdf2d92）的"
            "构建产物，含 ONLYOFFICE 的 AGPL-3.0 派生作品。\r\n"
            "对外分发本包时须按 AGPL-3.0 提供对应源码（上述仓库该提交即为源），\r\n"
            "并保留编辑页中的 ONLYOFFICE 标识。\r\n", encoding="utf-8")
    else:
        print("      ! vendor/ranuts-document 未安装——Office 预览将不可用"
              "（先运行 vendor/fetch-ranuts-document.sh 再打包）")

    # 7) 启动器 + 说明
    print("[7/7] 生成启动器与说明 …")
    (out / "启动-paleo.bat").write_text(LAUNCHER_BAT, encoding="utf-8")
    (out / "使用说明.txt").write_text(README_TXT, encoding="utf-8")

    print(f"\n完成。输出目录：{out}")
    print(f"总体积：{dir_size(out)}")

    if args.zip:
        zp = build / "paleo-portable.zip"
        print(f"\n压缩中 → {zp.name} …")
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for f in out.rglob("*"):
                if f.is_file():
                    z.write(f, f.relative_to(out.parent))
        print(f"      {zp.stat().st_size/1e6:.1f} MB")

    if args.verify:
        verify(out)


def verify(out: Path):
    """净环境（PATH 只含包 + System32）跑包内自检与 GUI 冒烟——
    本机装的 conda Qt / QGIS 前缀全部从 PATH 里剥掉，模拟全新机器。"""
    print("\n=== 验证：净环境 selfcheck（真实 windows 平台）===")
    env = {
        "PATH": str(out) + os.pathsep + r"C:\Windows\System32",
        "QGIS_PREFIX_PATH": str(out),
        "QT_PLUGIN_PATH": str(out / "plugins"),
        "GDAL_DATA": str(out / "share" / "gdal"),
        "PROJ_LIB": str(out / "share" / "proj"),
        "PROJ_DATA": str(out / "share" / "proj"),
        "SystemRoot": r"C:\Windows",
        "TEMP": str(out / "verify-tmp"), "TMP": str(out / "verify-tmp"),
    }
    (out / "verify-tmp").mkdir(exist_ok=True)
    r = run([str(out / "paleo_selfcheck.exe")], env=env, cwd=str(out),
            timeout=300)
    tail = "\n".join(r.stdout.strip().splitlines()[-12:])
    print(tail)
    if r.returncode != 0:
        print(f"!! selfcheck 退出码 {r.returncode}")

    print("\n=== 验证：paleo.exe 启动冒烟（offscreen，存活 8s 即过）===")
    try:
        p = subprocess.Popen([str(out / "paleo.exe")], env=env,
                             cwd=str(out), stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        time.sleep(8)
        alive = p.poll() is None
        p.kill()
        o, _ = p.communicate(timeout=10)
        print((o or "").strip()[-800:])
        print("paleo.exe 存活 8s ✓" if alive else f"!! paleo.exe 提前退出，退出码 {p.returncode}")
    except Exception as e:
        print(f"!! 启动失败: {e}")


if __name__ == "__main__":
    main()
