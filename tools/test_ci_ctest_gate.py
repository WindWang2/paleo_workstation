#!/usr/bin/env python3
"""Run the CI wrapper against a minimal CTest project, without QGIS."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from check_env_redset import results_from_junit

ROOT = Path(__file__).resolve().parent.parent
FILES = (
    "paleo-dev",
    "tools/ci_ctest_gate.sh",
    "tools/ci_gate_rules.py",
    "tools/ci-gate-rules.json",
    "tools/check_env_redset.py",
    "tools/env_redset.json",
    "tools/documented_redset.json",
    "tools/check_flaky_ledger.py",
    "tools/check_test_manifest.py",
    "tools/test-manifest-baseline.json",
    "vendor/deb-closure.lock",
)


def run_gate(root):
    env = os.environ.copy()
    for key in ("PALEO_FLAKY_STRICT", "PALEO_REDSET_STALE", "GITHUB_STEP_SUMMARY",
                "BUILD_DIR", "QGIS_PREFIX_PATH"):
        env.pop(key, None)
    return subprocess.run(
        ["bash", "tools/ci_ctest_gate.sh", "-j4", "-LE", "perf"],
        cwd=root, env=env, text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=45,
    )


def main():
    with tempfile.TemporaryDirectory(prefix="paleo-ctest-gate-") as temp:
        root = Path(temp)
        build = root / "build"
        build.mkdir()
        for file in FILES:
            target = root / file
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((ROOT / file).read_text(encoding="utf-8"), encoding="utf-8")
        prefix = root / "vendor" / "prefix"
        (prefix / "usr").mkdir(parents=True)
        lock = (root / "vendor" / "deb-closure.lock").read_bytes()
        (prefix / ".paleo-deb-closure.sha256").write_text(
            hashlib.sha256(lock).hexdigest() + "\n", encoding="ascii")
        (root / "paleo-dev").chmod(0o755)
        once = root / "fail-once.sh"
        once.write_text("#!/bin/sh\nif [ ! -e started ]; then touch started; exit 1; fi\n",
                        encoding="utf-8")
        once.chmod(0o755)
        testfile = build / "CTestTestfile.cmake"
        junit = build / "Testing" / "ctest-junit.xml"
        true_cmd, false_cmd = shutil.which("true"), shutil.which("false")

        def register(early_failure):
            commands = [true_cmd] * 130
            if early_failure:
                commands[1] = false_cmd
            commands[129] = once
            testfile.write_text("".join(
                f'add_test(sample_{index:03d} "{command}")\n'
                for index, command in enumerate(commands, start=1)
            ), encoding="utf-8")

        register(early_failure=True)
        mixed = run_gate(root)
        assert mixed.returncode != 0, mixed.stdout
        assert junit.is_file(), mixed.stdout
        assert results_from_junit(junit) == {"sample_002": "fail", "sample_130": "pass"}, mixed.stdout

        (build / "started").unlink()
        register(early_failure=False)
        flaky = run_gate(root)
        assert flaky.returncode == 0, flaky.stdout
        assert "sample_130" in (root / "tools" / "flaky-ledger.jsonl").read_text(encoding="utf-8")

        testfile.write_text('add_test(broken "true"\n', encoding="utf-8")
        broken = run_gate(root)
        assert broken.returncode != 0, broken.stdout
        assert not junit.exists() or "sample_130" not in junit.read_text(encoding="utf-8"), broken.stdout
        empty_report = '<testsuite tests="0" failures="0"/>'
        (root / "paleo-dev").write_text(
            f"#!/bin/sh\nprintf '%s\\n' '{empty_report}' > build/Testing/ctest-junit.xml\n",
            encoding="utf-8",
        )
        empty = run_gate(root)
        assert junit.read_text(encoding="utf-8").strip() == empty_report, empty.stdout
        assert empty.returncode != 0, empty.stdout

        # #965：配置期注册测试清单缩减时，ci_ctest_gate 必须在跑 CTest 前硬失败。
        manifest_path = build / "Testing" / "registered-tests.json"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(
            '{"schema": 1, "platform": "linux", "have_ort": false, "count": 1, "tests": ["only_one"], "perf_tests": []}\n',
            encoding="utf-8",
        )
        shrunk = run_gate(root)
        assert shrunk.returncode != 0, shrunk.stdout
        assert "注册测试清单对账失败" in shrunk.stdout, shrunk.stdout
        manifest_path.unlink()

        # #965：documented_redset 仅放行命中 expect_fail 且无崩溃的失败，失败形态漂移必须判红。
        (root / "paleo-dev").write_text((ROOT / "paleo-dev").read_text(encoding="utf-8"), encoding="utf-8")
        (root / "paleo-dev").chmod(0o755)
        doc_script = root / "doc-red.sh"
        qtest_out = build / "Testing" / "qtest-first-run" / "tst_wellcomposite_visual.txt"
        doc_script.write_text(
            "#!/bin/sh\n"
            f"mkdir -p '{qtest_out.parent}'\n"
            f"cat > '{qtest_out}' <<'EOF'\n"
            "FAIL!  : WellCompositeVisualTests::testGoldenImageSampling() 'diffs <= 4' returned FALSE. (17/48 抽样点超容差)\n"
            "Totals: 1 passed, 1 failed, 0 skipped, 0 blacklisted, 5ms\n"
            "EOF\n"
            "exit 1\n",
            encoding="utf-8",
        )
        doc_script.chmod(0o755)
        testfile.write_text(f'add_test(tst_wellcomposite_visual "{doc_script}")\n', encoding="utf-8")
        doc_ok = run_gate(root)
        assert doc_ok.returncode == 0, doc_ok.stdout

        # 改变失败形态（非预期断言）→ 必须拒收并判红。
        doc_script.write_text(
            "#!/bin/sh\n"
            f"mkdir -p '{qtest_out.parent}'\n"
            f"cat > '{qtest_out}' <<'EOF'\n"
            "FAIL!  : WellCompositeVisualTests::testExportPdf() Compared values are not the same\n"
            "Totals: 1 passed, 1 failed, 0 skipped, 0 blacklisted, 5ms\n"
            "EOF\n"
            "exit 1\n",
            encoding="utf-8",
        )
        doc_drift = run_gate(root)
        assert doc_drift.returncode != 0, doc_drift.stdout

        print("CI CTest gate: mixed failures, flaky rerun, failed discovery, empty JUnit, test manifest, and documented redset verified")


if __name__ == "__main__":
    main()
