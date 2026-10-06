#!/usr/bin/env python3
# 层：测试壳
"""
Automated offscreen verification runner for Milestone 4 (R4: Runtime Dynamic Locale Loading).

Verifies:
1. ./build/paleo --help exits with 0 and prints usage options with zero leaked crash flags.
2. ./build/paleo -h exits with 0 and prints usage options with zero leaked crash flags.
3. Subsequent launch in clean XDG_DATA_HOME does not falsely report abnormal exit.
4. CLI --lang zh_CN loads Chinese translation catalog.
5. CLI --lang en_US retains unmodified built-in English.
6. PALEO_LOCALE=zh_CN loads Chinese translation catalog.
7. Precedence: CLI --lang en_US overrides PALEO_LOCALE=zh_CN.
8. PALEO_LOCALE=en_US retains English.
9. CLI --locale zh_CN alias works.
10. CLI --lang=zh_CN equals format works.
11. CLI --lang zh-CN hyphen format works.
12. CLI --lang zh_Hans script format works.
13. CLI --lang zh_CN.UTF-8 codeset normalization works.
14. CLI fallback on unsupported locale (fr_FR) to English.
15. CLI fallback on invalid subtag (zh_bogus) to English.
16. CLI fallback on bogus subtag (zh_bogus_nonsense) to English.
17. CLI fallback on numeric subtag (zh_123) to English.
18. CLI fallback on command injection attempt (zh_CN; reboot) to English.
19. CLI fallback on trailing flag without value (--lang) to English.
20. In-process core views translation resolution (DataPreviewTabs, SeismicSectionDockWidget, CompositionWorkflow).
"""

import os
import sys
import subprocess
import tempfile

def run_cmd(name, args, env_overrides, expected_patterns, unexpected_patterns=None, check_crash_flags=False):
    with tempfile.TemporaryDirectory() as default_xdg:
        env = os.environ.copy()
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["PALEO_STARTUP_EXIT_AFTER_FRAME"] = "1"
        if "XDG_DATA_HOME" not in env_overrides:
            env["XDG_DATA_HOME"] = default_xdg
        env.update(env_overrides)
        xdg_data = env["XDG_DATA_HOME"]

        cmd = ["./build/paleo"] + args
        print(f"--> [TEST] {name}: {' '.join(cmd)}")
        try:
            res = subprocess.run(
                cmd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=25
            )
        except subprocess.TimeoutExpired:
            print(f"    FAILED: Command timed out after 25s")
            return False

        if res.returncode != 0:
            print(f"    FAILED (exit code {res.returncode}):\n{res.stdout}")
            return False

        for pat in expected_patterns:
            if pat not in res.stdout:
                print(f"    FAILED (missing expected pattern '{pat}'):\n{res.stdout}")
                return False

        if unexpected_patterns:
            for pat in unexpected_patterns:
                if pat in res.stdout:
                    print(f"    FAILED (found unexpected pattern '{pat}'):\n{res.stdout}")
                    return False

        if check_crash_flags:
            crash_dir = os.path.join(xdg_data, "paleo", "paleo_workbench", "crash")
            if os.path.exists(crash_dir):
                lingering = [f for f in os.listdir(crash_dir) if f.startswith(".running")]
                if lingering:
                    print(f"    FAILED: Lingering crash flags detected in {crash_dir}: {lingering}")
                    return False

        print("    PASSED")
        return True

def verify_help_no_crash_leak():
    print("--> [TEST] Verifying --help and -h do not leak crash flags in isolated XDG_DATA_HOME:")
    with tempfile.TemporaryDirectory() as xdg:
        env = os.environ.copy()
        env["XDG_DATA_HOME"] = xdg
        # 1. Test --help
        res = subprocess.run(["./build/paleo", "--help"], env=env, capture_output=True, text=True, timeout=10)
        if res.returncode != 0:
            print(f"    FAILED: ./build/paleo --help exited with {res.returncode}")
            return False
        crash_dir = os.path.join(xdg, "paleo", "paleo_workbench", "crash")
        if os.path.exists(crash_dir):
            leaked = [f for f in os.listdir(crash_dir) if f.startswith(".running")]
            if leaked:
                print(f"    FAILED: Found lingering crash flag files after --help: {leaked}")
                return False

        # 2. Test -h
        res = subprocess.run(["./build/paleo", "-h"], env=env, capture_output=True, text=True, timeout=10)
        if res.returncode != 0:
            print(f"    FAILED: ./build/paleo -h exited with {res.returncode}")
            return False
        if os.path.exists(crash_dir):
            leaked = [f for f in os.listdir(crash_dir) if f.startswith(".running")]
            if leaked:
                print(f"    FAILED: Found lingering crash flag files after -h: {leaked}")
                return False

        # 3. Subsequent normal startup in same XDG_DATA_HOME must NOT detect dirty exit
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["PALEO_STARTUP_EXIT_AFTER_FRAME"] = "1"
        res = subprocess.run(["./build/paleo"], env=env, capture_output=True, text=True, timeout=25)
        combined_output = res.stdout + res.stderr
        if "上次异常退出" in combined_output or "previousDirtyExit" in combined_output:
            print("    FAILED: Subsequent launch falsely detected abnormal dirty exit")
            return False
    print("    PASSED (Zero crash flags leaked on --help / -h, subsequent launch clean)")
    return True

def verify_core_views_translations():
    print("--> [TEST] Core views translation lookup via QTranslator:")
    try:
        from PyQt6.QtCore import QCoreApplication, QTranslator
    except ImportError:
        print("    PyQt6 not available, skipping in-process QTranslator check")
        return True

    app = QCoreApplication([])
    trans = QTranslator()
    qm_path = "build/translations/paleo_zh_CN.qm"
    if not os.path.exists(qm_path):
        print(f"    FAILED: Translation catalog '{qm_path}' not found")
        return False

    if not trans.load(qm_path):
        print(f"    FAILED: Could not load translation catalog '{qm_path}'")
        return False

    # 1. Verify English fallback without translator
    test_cases = [
        ("DataPreviewTabs", "KB", "补心 (KB)"),
        ("DataPreviewTabs", "TD", "完钻深度 (TD)"),
        ("DataPreviewTabs", "BottomX", "井底 X"),
        ("DataPreviewTabs", "BottomY", "井底 Y"),
        ("DataPreviewTabs", "WellType", "井型"),
        ("SeismicSectionDockWidget", "INLINE (189-192)", "主测线 (189-192)"),
        ("SeismicSectionDockWidget", "CROSSLINE (193-196)", "联络测线 (193-196)"),
        ("SeismicSectionDockWidget", "field record (9-12)", "野外记录号 (9-12)"),
        ("SeismicSectionDockWidget", "CDP ensemble (21-24)", "CDP 道集号 (21-24)"),
        ("SeismicSectionDockWidget", "CDP X (73-76)", "CDP 坐标 X (73-76)"),
        ("SeismicSectionDockWidget", "CDP Y (77-80)", "CDP 坐标 Y (77-80)"),
        ("CompositionWorkflow", "composition workflow is not bound to services", "综合编图工作流未绑定服务"),
        ("CompositionWorkflow", "facies fusion returned no output path", "相融合未返回输出路径"),
        ("CompositionWorkflow", "no raster layer supplied for facies polygons", "未提供用于生成相多边形的栅格图层"),
    ]

    for ctx, src, _ in test_cases:
        fallback = app.translate(ctx, src)
        if fallback != src:
            print(f"    FAILED: Expected English fallback '{src}', got '{fallback}'")
            return False

    # 2. Install translator and verify Chinese translations
    app.installTranslator(trans)
    for ctx, src, expected in test_cases:
        actual = app.translate(ctx, src)
        if actual != expected:
            print(f"    FAILED: [{ctx}] '{src}' -> got '{actual}', expected '{expected}'")
            return False
        print(f"    ✓ [{ctx}] '{src}' -> '{actual}'")

    print("    PASSED (All core views translated accurately)")
    return True

def main():
    if not os.path.exists("./build/paleo"):
        print("Error: ./build/paleo does not exist. Run cmake --build build -j8 first.")
        sys.exit(1)

    tests = [
        ("CLI --help leaves zero crash flags", ["--help"], {}, ["Usage: paleo", "--lang", "--locale", "--prefix", "--help"], None, True),
        ("CLI -h leaves zero crash flags", ["-h"], {}, ["Usage: paleo", "--lang", "--locale", "--prefix", "--help"], None, True),
        ("CLI --lang zh_CN", ["--lang", "zh_CN"], {}, ["Loaded Chinese translation catalog", "via CLI flag --lang"]),
        ("CLI --lang en_US", ["--lang", "en_US"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("ENV PALEO_LOCALE=zh_CN", [], {"PALEO_LOCALE": "zh_CN"}, ["Loaded Chinese translation catalog", "via environment variable PALEO_LOCALE"]),
        ("Precedence: CLI --lang en_US overrides PALEO_LOCALE=zh_CN", ["--lang", "en_US"], {"PALEO_LOCALE": "zh_CN"}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("ENV PALEO_LOCALE=en_US", [], {"PALEO_LOCALE": "en_US"}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("CLI --locale zh_CN alias", ["--locale", "zh_CN"], {}, ["Loaded Chinese translation catalog"]),
        ("CLI --lang=zh_CN format", ["--lang=zh_CN"], {}, ["Loaded Chinese translation catalog"]),
        ("CLI --lang zh-CN hyphen normalization", ["--lang", "zh-CN"], {}, ["Loaded Chinese translation catalog"]),
        ("CLI --lang zh_Hans script variation", ["--lang", "zh_Hans"], {}, ["Loaded Chinese translation catalog"]),
        ("CLI --lang zh_CN.UTF-8 codeset normalization", ["--lang", "zh_CN.UTF-8"], {}, ["Loaded Chinese translation catalog"]),
        ("CLI fallback on unsupported locale (fr_FR)", ["--lang", "fr_FR"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("CLI fallback on bogus locale (zh_bogus)", ["--lang", "zh_bogus"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("CLI fallback on bogus locale (zh_bogus_nonsense)", ["--lang", "zh_bogus_nonsense"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("CLI fallback on numeric subtag (zh_123)", ["--lang", "zh_123"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("CLI fallback on command injection attempt", ["--lang", "zh_CN; reboot"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
        ("CLI fallback on trailing flag without value", ["--lang"], {}, ["using built-in English (en_US)"], ["Loaded Chinese translation catalog"]),
    ]

    passed_count = 0
    total_tests = len(tests) + 2  # CLI tests + core views + verify_help_no_crash_leak

    all_passed = True
    for item in tests:
        name = item[0]
        args = item[1]
        env_vars = item[2]
        expected = item[3]
        unexp = item[4] if len(item) > 4 else None
        check_crash = item[5] if len(item) > 5 else False
        if run_cmd(name, args, env_vars, expected, unexp, check_crash):
            passed_count += 1
        else:
            all_passed = False

    if verify_core_views_translations():
        passed_count += 1
    else:
        all_passed = False

    if verify_help_no_crash_leak():
        passed_count += 1
    else:
        all_passed = False

    if all_passed:
        print("\n=======================================================")
        print(f"All Milestone 4 offscreen verification tests PASSED! ({passed_count}/{total_tests})")
        print("=======================================================")
        sys.exit(0)
    else:
        print(f"\nSome verification tests FAILED. ({passed_count}/{total_tests} passed)")
        sys.exit(1)

if __name__ == "__main__":
    main()
