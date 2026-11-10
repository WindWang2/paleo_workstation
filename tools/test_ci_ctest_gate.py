#!/usr/bin/env python3
"""Run the CI wrapper against a minimal CTest project, without QGIS."""

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
)


def run_gate(root):
    env = os.environ.copy()
    for key in ("PALEO_FLAKY_STRICT", "PALEO_REDSET_STALE", "GITHUB_STEP_SUMMARY"):
        env.pop(key, None)
    return subprocess.run(
        ["bash", "tools/ci_ctest_gate.sh", "-j4", "-LE", "perf"],
        cwd=root, env=env, text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=45,
    )


def main():
    with tempfile.TemporaryDirectory(prefix="paleo-ctest-gate-") as temp:
        root = Path(temp)
        (root / "tools").mkdir()
        build = root / "build"
        build.mkdir()
        for file in FILES:
            target = root / file
            target.write_text((ROOT / file).read_text(encoding="utf-8"), encoding="utf-8")
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
        print("CI CTest gate: mixed failures, flaky rerun, failed discovery, and empty JUnit verified")


if __name__ == "__main__":
    main()
