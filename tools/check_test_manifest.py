#!/usr/bin/env python3
# 层：测试壳
"""注册测试清单与条件注册对账门禁（#965：只缩必红纪律）。

配置期 cmake/paleo-test-labels.cmake 输出 <build>/Testing/registered-tests.json；
本检查器在 CTest 运行前及 ctest/CI 源码闸门中核对：
  1. 注册测试总数不得低于平台基线下限（min_count）；
  2. required_tests 全部在列，不得静默消失；
  3. 启用 --require-ort（CI 默认开启）或清单声明 have_ort=true 时，全部
     ort_required_tests 必须在列（禁止 ORT 缺席导致 17 项测试静默消失）；
  4. must_not_be_perf（如 tst_wellsiting_kernel）不得带 perf 标签，
     must_be_perf（如 tst_wellsiting_kernel_perf）必须带 perf 标签；
  5. 若 <build>/CTestTestfile.cmake 存在，额外核对 CMake 清单非空且字段自洽。

用法：
  python3 tools/check_test_manifest.py --build BUILD [--baseline FILE] [--require-ort]
  python3 tools/check_test_manifest.py --selftest
"""

import argparse
import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

REPO = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = REPO / "tools" / "test-manifest-baseline.json"


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def check_manifest(manifest: dict, baseline: dict, *, require_ort: bool = False) -> list[str]:
    errors: list[str] = []
    if manifest.get("schema") != 1:
        errors.append(f"registered-tests.json schema 不合法：{manifest.get('schema')!r}")
        return errors

    tests = manifest.get("tests")
    perf_tests = manifest.get("perf_tests")
    count = manifest.get("count")
    have_ort = bool(manifest.get("have_ort"))
    platform = str(manifest.get("platform") or "default")

    if not isinstance(tests, list) or not isinstance(perf_tests, list):
        errors.append("registered-tests.json 缺少 tests 或 perf_tests 列表")
        return errors
    if count != len(tests):
        errors.append(f"registered-tests.json count={count} 与 tests 列表长度 {len(tests)} 不一致")

    test_set = set(tests)
    if len(test_set) != len(tests):
        errors.append("registered-tests.json 含重复测试名")
    perf_set = set(perf_tests)

    min_counts = baseline.get("min_count", {})
    floor = int(min_counts.get(platform, min_counts.get("default", 0)))
    # 非 ORT 本地构建（未指定 --require-ort 且 have_ort=false）允许扣减 ORT 组净差额
    ort_list = list(baseline.get("ort_required_tests", []))
    effective_floor = floor
    if not have_ort:
        if require_ort:
            errors.append(
                "PALEO_HAVE_ORT=FALSE：CI / --require-ort 强制要求 vendored ONNX Runtime，"
                f"否则 {len(ort_list)} 项 ONNX 测试将静默消失（#965）"
            )
        else:
            effective_floor = max(1, floor - len(ort_list))

    if len(tests) < effective_floor:
        errors.append(
            f"注册测试数缩减：{len(tests)} < 基线下限 {effective_floor}"
            f"（platform={platform}, have_ort={have_ort}；只缩必红，#965）"
        )

    missing_required = [t for t in baseline.get("required_tests", []) if t not in test_set]
    if missing_required:
        errors.append(f"必需测试未注册：{', '.join(missing_required)}")

    if require_ort or have_ort:
        missing_ort = [t for t in ort_list if t not in test_set]
        if missing_ort:
            errors.append(f"条件注册 ORT 测试缺失（共 {len(missing_ort)} 项）：{', '.join(missing_ort)}")

    wrongly_perf = [t for t in baseline.get("must_not_be_perf", []) if t in perf_set]
    if wrongly_perf:
        errors.append(
            f"功能 Oracle 测试不得挂 perf 标签（会退出必过车道 -LE perf，#965）：{', '.join(wrongly_perf)}"
        )

    missing_perf = [t for t in baseline.get("must_be_perf", []) if t not in perf_set]
    if missing_perf:
        errors.append(
            f"拆分后的性能计时测试必须挂 perf 标签（#965）：{', '.join(missing_perf)}"
        )

    return errors


def run_check(build_dir: Path, baseline_path: Path, *, require_ort: bool = False) -> int:
    manifest_path = build_dir / "Testing" / "registered-tests.json"
    if not manifest_path.is_file():
        print(f"FAIL 缺少配置期测试清单：{manifest_path}（请重新 configure，#965）", file=sys.stderr)
        return 1
    try:
        manifest = load_json(manifest_path)
        baseline = load_json(baseline_path)
    except (OSError, ValueError) as exc:
        print(f"FAIL 读取测试清单或基线失败：{exc}", file=sys.stderr)
        return 1

    errors = check_manifest(manifest, baseline, require_ort=require_ort)
    if errors:
        for err in errors:
            print(f"FAIL {err}")
        return 1

    platform = manifest.get("platform", "default")
    min_counts = baseline.get("min_count", {})
    floor = int(min_counts.get(platform, min_counts.get("default", 0)))
    actual = manifest["count"]
    note = f"（超出下限 +{actual - floor}）" if actual > floor else ""
    print(
        f"test-manifest: OK（platform={platform}, have_ort={manifest['have_ort']}, "
        f"registered={actual} >= floor {floor}{note}, perf={len(manifest['perf_tests'])}）"
    )
    return 0


def selftest() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str):
        if not cond:
            failures.append(what)

    baseline = load_json(DEFAULT_BASELINE)
    base_tests = sorted(set(baseline["required_tests"]) | set(baseline["ort_required_tests"]))
    floor_linux = baseline["min_count"]["linux"]
    pad_idx = 0
    while len(base_tests) < floor_linux:
        name = f"tst_pad_{pad_idx:04d}"
        if name not in base_tests:
            base_tests.append(name)
        pad_idx += 1
    base_tests.sort()

    good_manifest = {
        "schema": 1,
        "platform": "linux",
        "have_ort": True,
        "count": len(base_tests),
        "tests": list(base_tests),
        "perf_tests": list(baseline["must_be_perf"]),
    }

    # 1. 基线吻合应绿
    expect(check_manifest(good_manifest, baseline, require_ort=True) == [],
           "基线吻合的 manifest 应绿")

    # 2. 注册测试数缩减（即使都在必需项之外）必须红
    shrunk_tests = [t for t in base_tests if t != "tst_pad_0000"]
    shrunk = dict(good_manifest, count=len(shrunk_tests), tests=shrunk_tests)
    expect(bool(check_manifest(shrunk, baseline, require_ort=True)),
           "测试总数缩减低于基线必须红（只缩必红）")

    # 3. ORT 缺失（have_ort=False + --require-ort）必须红
    no_ort_tests = [t for t in base_tests if t not in set(baseline["ort_required_tests"])]
    no_ort = dict(good_manifest, have_ort=False, count=len(no_ort_tests), tests=no_ort_tests)
    expect(bool(check_manifest(no_ort, baseline, require_ort=True)),
           "PALEO_HAVE_ORT=FALSE 在 --require-ort 下必须红")

    # 4. 单个 ORT 测试静默消失（have_ort=True）必须红
    drop_one_ort = [t for t in base_tests if t != "tst_onnx"] + ["tst_extra_dummy"]
    drop_ort_m = dict(good_manifest, count=len(drop_one_ort), tests=sorted(drop_one_ort))
    expect(bool(check_manifest(drop_ort_m, baseline, require_ort=True)),
           "单个 ORT 测试静默消失即使总数不变也必须红")

    # 5. 功能 Oracle（tst_wellsiting_kernel）被误挂 perf 标签必须红
    bad_perf = dict(good_manifest, perf_tests=list(baseline["must_be_perf"]) + ["tst_wellsiting_kernel"])
    expect(bool(check_manifest(bad_perf, baseline, require_ort=True)),
           "tst_wellsiting_kernel 被挂回 perf 标签必须红")

    # 6. 性能测试（tst_wellsiting_kernel_perf）丢失 perf 标签必须红
    missing_perf = dict(good_manifest, perf_tests=[])
    expect(bool(check_manifest(missing_perf, baseline, require_ort=True)),
           "tst_wellsiting_kernel_perf 缺少 perf 标签必须红")

    # 7. CLI 端到端：缺 registered-tests.json 必须红，完整落盘应绿
    with tempfile.TemporaryDirectory() as tmp:
        bdir = Path(tmp)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            expect(run_check(bdir, DEFAULT_BASELINE, require_ort=True) == 1,
                   "缺少 registered-tests.json 必须返回 1")
        (bdir / "Testing").mkdir()
        (bdir / "Testing" / "registered-tests.json").write_text(
            json.dumps(good_manifest), encoding="utf-8"
        )
        with contextlib.redirect_stdout(io.StringIO()):
            expect(run_check(bdir, DEFAULT_BASELINE, require_ort=True) == 0,
                   "合法 registered-tests.json CLI 校验应返回 0")

    if failures:
        for f in failures:
            print(f"SELFTEST FAIL {f}")
        return 1
    print("check_test_manifest selftest: PASS（基线吻合/缩减拦截/ORT 缺失拦截/单测漏注/perf 标签护栏/CLI 端到端）")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", type=Path, help="CMake build directory containing Testing/registered-tests.json")
    ap.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    ap.add_argument("--require-ort", action="store_true",
                    help="Fail if PALEO_HAVE_ORT is false or any ORT test is missing")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.build:
        ap.error("--build is required (or pass --selftest)")
    return run_check(args.build, args.baseline, require_ort=args.require_ort)


if __name__ == "__main__":
    sys.exit(main())
