# Frozen single-factor numeric reference

The product runtime is C++. These scripts only import the numeric modules of
`WWX9/haiyou-visualization` at `27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f` and
write point-query goldens. They do not open a window or create a layer.

The checkout used on this machine is read-only:

```text
/home/kevin/projects/_refs/haiyou-visualization
```

Parent packages are registered empty so `drawing/__init__.py` and
`drawing/single_factor/__init__.py` are not executed. NumPy, SciPy, and Shapely
are imported for real.

## Environment

Use a venv outside the repository. Do not change the system interpreter.

```bash
/usr/bin/python3.14 -m venv /tmp/sf-ref-venv
/tmp/sf-ref-venv/bin/pip install --only-binary=:all: \
  'numpy==2.5.3' 'scipy==1.18.1' 'shapely==2.1.2'
```

`requirements.lock` records those wheels and their sha256. `numpy==2.2.6` has
no CPython 3.14 wheel and does not build on GCC 16, so it is not the lock.

## Generate

From the worktree root:

```bash
/tmp/sf-ref-venv/bin/python tools/reference/singlefactor/generate_reference.py \
  --reference /home/kevin/projects/_refs/haiyou-visualization/Drawing \
  --out tests/fixtures/singlefactor/kernel_parity.json \
  --manifest tools/reference/singlefactor/reference_manifest.json
```

## Compare

`compare_results.py` reads two JSON documents and exits 1 on a mask or numeric
miss. It does not write either file.

```bash
/tmp/sf-ref-venv/bin/python tools/reference/singlefactor/compare_results.py \
  tests/fixtures/singlefactor/kernel_parity.json \
  /path/to/candidate.json
```

Double point queries use `maxAbsError <= 1e-9 * S` and `RMSE <= 1e-10 * S`,
where `S = max(1, max(abs(finite reference values)))`. The masks must match
before those tolerances are applied. Hard-barrier cases are absent here; they
follow Paleo `grid_connectivity_v1` and are covered by `tst_singlefactor_kernel`.
