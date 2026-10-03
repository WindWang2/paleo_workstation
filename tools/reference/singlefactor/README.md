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

## End-to-end structural_idw reference (current-layers path)

`run_structural_reference.py` runs the whole upstream current-layers workflow
(`_generate_single_factor_from_current_layers` with the dialog's default method
`structural_idw` and dialog defaults, then the separate contour step with the
dialog's adaptive levels) and writes one JSON golden: accepted wells, skip
reasons, the node-centred analysis grid, the valid mask, resolved constraints,
the field-only contours and the final (local-detour) contours. Its docstring
lists the three deliberate deviations (no first-numeric-column fallback, the
PyQt-only preview stub, the pinned grid-cell budget).

The contour stage depends on GEOS noding/polygonize/triangulation order. The
shapely wheel bundles GEOS 3.13.1; Paleo links GEOS 3.15.0 (system and
superbuild). Between the two GEOS versions the upstream contours themselves move
by up to ~0.3 % of a grid step, so the structural goldens are generated with
shapely built from source against GEOS 3.15.0:

```bash
/usr/bin/python3.14 -m venv /tmp/sf-ref-venv-geos315
/tmp/sf-ref-venv-geos315/bin/pip install --only-binary=:all: 'numpy==2.5.3' 'scipy==1.18.1'
# shapely-2.1.2.tar.gz sha256 2ed4ecb28320a433db18a5bf029986aa8afcfd740745e78847e330d5d94922a9
GEOS_CONFIG=/usr/bin/geos-config /tmp/sf-ref-venv-geos315/bin/pip install --no-binary shapely 'shapely==2.1.2'
```

Synthetic fixture (invented data, committed):

```bash
PY=/tmp/sf-ref-venv-geos315/bin/python
D=tests/fixtures/singlefactor/structural_synthetic
$PY tools/reference/singlefactor/make_structural_synthetic.py --out $D
$PY tools/reference/singlefactor/run_structural_reference.py \
  --wells $D/wells.shp --boundary $D/boundary.shp \
  --directions $D/directions.shp --barriers $D/barriers.shp \
  --override grid_resolution=120 \
  --out tests/fixtures/singlefactor/structural_synthetic_golden.json
```

Real Enping EP1-2 package (outsourced data — never commit the package or its
golden; tests read `PALEO_ENPING_PACKAGE` / `PALEO_ENPING_GOLDEN` and skip when
unset):

```bash
$PY tools/reference/singlefactor/run_structural_reference.py \
  --package ~/.cache/paleo/enping/恩平一二段_汇报资料整合包 \
  --out ~/.cache/paleo/enping/enping_golden.json
```

`compare_structural.py golden.json candidate.json` compares a C++ dump in the
same schema: grid masks must match and `maxAbs <= 1e-9*S`, `RMSE <= 1e-10*S`;
wells must match exactly; contours must match per level in count and
open/closed state, with the worst one-to-one symmetric Hausdorff distance
`<= 1e-6 * grid step` by default (`--contour-tol` overrides).
