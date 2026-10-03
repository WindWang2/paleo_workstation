#!/usr/bin/env python3
"""End-to-end upstream reference for the structural_idw "current layers" path.

Runs the frozen haiyou-visualization single-factor workflow exactly as its
dialog does in data_mode=current_layers with the default method
``structural_idw`` ("IDW 方向线与打断约束") and the dialog defaults, then the
separate "提取等值线" step with the dialog's adaptive levels. Writes one JSON
golden: accepted wells, skip reasons, the analysis grid (node-centred
linspace axes, rows ascending in y), the frozen valid mask, resolved
constraints and the contour polylines per level.

Deliberate deviations from the frozen code (each is a product decision, not a
formula change):

* ``_first_numeric_factor_value`` is disabled by default. Upstream falls back
  to the first numeric attribute when the chosen value field is empty, which
  turns e.g. OCR_CONF=0 into a fabricated well value. Paleo skips such wells
  and reports them (user decision 2026-10-03). Pass ``--keep-numeric-fallback``
  to reproduce the frozen behaviour verbatim.
* The PyQt6-only raster preview (``_build_current_trend_surface_features``)
  is stubbed. It only renders the finished grid and never feeds back.
* ``resolve_performance_grid_resolution`` reads machine compute settings; the
  reference pins ``max_cells`` to the upstream fallback (200 000) so the
  golden does not depend on the host.

Parent packages are registered empty so ``drawing/__init__.py`` (PyQt6) is not
executed. NumPy, SciPy and Shapely are real imports (see requirements.lock).
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import math
import platform
import sys
import time
import types
from pathlib import Path

REFERENCE_SHA = "27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f"
DEFAULT_REF = Path("/home/kevin/projects/_refs/haiyou-visualization/Drawing")
SCHEMA = "paleo.singlefactor.structural_reference/1"
SOURCE_FILES = (
    "drawing/single_factor/workflow.py",
    "drawing/single_factor/structural_idw.py",
    "drawing/single_factor/field_contours.py",
    "drawing/single_factor/contour_work_field.py",
    "drawing/single_factor/contour_avoidance.py",
    "drawing/single_factor/continuous_metric.py",
    "drawing/single_factor/interpretive_boundary.py",
    "drawing/single_factor/well_clusters.py",
    "drawing/single_factor/surfer_idw.py",
    "drawing/single_factor/fast_grid.py",
    "drawing/single_factor/masks.py",
    "drawing/single_factor/constrained_engine.py",
    "drawing/constraint_semantics.py",
    "drawing/io/shapefile_importer.py",
    "drawing/ui/dialogs/single_factor_dialog.py",
)

# SingleFactorDialog.get_request() for data_mode=current_layers, untouched
# dialog widgets (single_factor_dialog.py L1294-1363 and widget defaults
# L344-600). value_min/value_max = 0/1 because the value-range combo selects
# "ratio_0_1" for ratio fields such as sand_ratio (L1052).
UI_DEFAULTS = dict(
    dataset_root="", source="当前图层", factor_name="sand_ratio", statistic="mean",
    interpolation_method="structural_idw", grid_resolution=339, grid_smoothing_iterations=2,
    contour_level_count=8, zone_count=0, smoothing=0.0, style_preset="constrained_contour",
    facies_order="none", data_mode="current_layers", factor_mode="direct", value_field="sand_ratio",
    numerator_field="", denominator_field="",
    value_min=0.0, value_max=1.0, contour_extraction_method="partitioned_marching_squares",
    contour_level_mode="step", contour_level_start=0.3, contour_level_step=0.1, contour_level_end=0.9,
    contour_levels=None, well_id_field="well_id",
    search_radius=0.0, interpolation_mask_radius=0.0, extend_trend_to_boundary=True,
    min_points=3, max_points=12, power=2.0, global_anisotropy_ratio=12.0, global_anisotropy_angle=15.0,
    enable_barriers=True, enable_directions=True, direction_ratio_default=8.0,
    direction_influence_radius_default=0.0, use_extended_search=True, barrier_blank_cells=0.0,
    barrier_buffer_distance=150.0, contour_stop_buffer_distance=None, barrier_buffer_auto=False,
    barrier_extend_to_boundary=True, barrier_extension_limit=-1.0, interpretive_boundary_strength=0.35,
    barrier_shape_strength=1.0, barrier_shape_radius=0.0, generate_contours=False, show_well_values=True,
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Reference:
    def __init__(self, drawing_root: Path):
        root = drawing_root.resolve()
        if not (root / "drawing/single_factor/structural_idw.py").is_file():
            raise SystemExit(f"reference sources not found under {root}")
        self.root = root
        if str(root) not in sys.path:
            sys.path.insert(0, str(root))
        for name, rel in (("drawing", "drawing"), ("drawing.single_factor", "drawing/single_factor"),
                          ("drawing.io", "drawing/io")):
            existing = sys.modules.get(name)
            if existing is not None and getattr(existing, "__file__", None):
                raise SystemExit(f"{name} already imported from {existing.__file__}")
            module = types.ModuleType(name)
            module.__path__ = [str(root / rel)]
            module.__package__ = name
            sys.modules[name] = module
        from drawing.io.shapefile_importer import ShapefileImporter
        from drawing.single_factor import contour_work_field, fast_grid, field_contours, surfer_idw
        from drawing.single_factor import workflow as wf
        self.ShapefileImporter = ShapefileImporter
        self.wf = wf
        self.surfer_idw = surfer_idw
        self.field_contours = field_contours
        self.contour_work_field = contour_work_field
        self.fast_grid = fast_grid
        self.dialog = self._dialog_helpers()

    def _dialog_helpers(self):
        """Exec the PyQt-free level helpers verbatim from the dialog source."""
        path = self.root / "drawing/ui/dialogs/single_factor_dialog.py"
        tree = ast.parse(path.read_text(encoding="utf-8"))
        keep = {"_nice_number", "suggest_contour_step_and_range", "contour_grid_value_range"}
        body = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in keep]
        if {n.name for n in body} != keep:
            raise SystemExit("dialog level helpers not found")
        ns = {"math": math, "Tuple": tuple}
        exec(compile(ast.Module(body=body, type_ignores=[]), str(path), "exec"), ns)
        return types.SimpleNamespace(**{k: ns[k] for k in keep})

    def load(self, path):
        if path is None:
            return []
        res = self.ShapefileImporter().import_shp(str(path))
        if res is None:
            raise SystemExit(f"upstream shapefile import failed: {path}")
        feats = []
        for item in res["layers"]:
            feats.extend(item["features"])
        return feats

    def dialog_levels(self, grid_z, valid_mask):
        """ContourExtractDialog defaults: adaptive step, target 10, formatted text."""
        d = self.dialog
        lo, hi = d.contour_grid_value_range(grid_z, valid_mask)
        start, end, step = d.suggest_contour_step_and_range(lo, hi, target_levels=10)
        # _apply_adaptive_step: spin decimals from the step, then setValue()
        # rounds each spin to those decimals (QDoubleSpinBox); _generate_levels
        # reads the rounded spin values back.
        decimals = 0
        s = abs(step)
        while s > 0 and s < 1.0 - 1e-12 and decimals < 6:
            s *= 10.0
            decimals += 1
        decimals = max(2, min(6, decimals + 1))
        spin = lambda v: float(f"{v:.{decimals}f}")
        start_s, end_s, step_s = spin(start), spin(end), spin(step)
        if end_s < start_s:
            start_s, end_s = end_s, start_s
        count = int(math.floor(max(0.0, end_s - start_s) / step_s + 1e-10))
        levels = [float(f"{start_s + i * step_s:.{decimals}f}") for i in range(count + 1)]
        return dict(value_range=[lo, hi], start=start, end=end, step=step, decimals=decimals,
                    spin=[start_s, end_s, step_s], levels=levels)

    def run(self, paths, overrides=None, *, keep_numeric_fallback=False, levels=None):
        wf = self.wf
        np = __import__("numpy")
        if not keep_numeric_fallback:
            wf._first_numeric_factor_value = lambda attrs: None
        wf._build_current_trend_surface_features = lambda *a, **k: (wf.DrawLayer(name="trend"), [])
        original_cap = self.fast_grid.resolve_performance_grid_resolution
        self.fast_grid.resolve_performance_grid_resolution = (
            lambda w, h, r, **k: original_cap(w, h, r, max_cells=200_000))
        detour = {}
        original_detour = self.contour_work_field.local_detour_surface

        def record_detour(surface, lv, contours):
            work = original_detour(surface, lv, contours)
            from drawing.constraint_semantics import is_contour_stop_mode
            detour["triggered"] = work is not None
            # Same selection rule as local_detour_surface (index into surface["barriers"]).
            detour["crossed_barriers"] = [
                i for i, b in enumerate(surface.get("barriers", []))
                if b.get("active", True) and is_contour_stop_mode(b.get("block_mode", "full_block"))
                and any(self._crosses(line, b["points"]) for paths_ in contours.values() for line in paths_)]
            detour["info"] = (work or {}).get("contour_work_info")
            return work
        self.contour_work_field.local_detour_surface = record_detour

        inputs = dict(wells=self.load(paths.get("wells")), boundary=self.load(paths.get("boundary")),
                      interpolation_area=self.load(paths.get("interpolation_area")),
                      barriers=self.load(paths.get("barriers")), directions=self.load(paths.get("directions")))
        kwargs = dict(UI_DEFAULTS)
        kwargs.update(overrides or {})
        request = wf.SingleFactorRequest(current_layer_inputs=inputs, **kwargs)
        t0 = time.time()
        result = wf._generate_single_factor_from_current_layers(request)
        t1 = time.time()
        sd = result.surface_data
        level_info = self.dialog_levels(sd["grid_z"], sd.get("valid_mask"))
        chosen = list(levels) if levels is not None else level_info["levels"]
        policy = dict(sd.get("contour_partition") or {})
        base = dict(sd, contour_partition=dict(policy, geometry_policy="field_only")) if policy else sd
        initial = self.field_contours.extract_field_contours(base, tuple(chosen), smooth=True) if policy else None
        contours = self.surfer_idw.extract_global_contours(sd, chosen, smooth=True)
        t2 = time.time()

        def num(v):
            v = float(v)
            return v if math.isfinite(v) else None

        def lines(cmap):
            return {repr(float(k)): [[[float(x), float(y)] for x, y in line] for line in v]
                    for k, v in sorted(cmap.items())}

        fm = sd.get("field_model") or {}
        wells_meta = []
        well_features = inputs["wells"]
        accepted = fm.get("wells") or []
        # Recover ids in the same order _extract_current_wells produced them.
        extracted, skipped = wf._extract_current_wells(
            well_features, wf._extract_current_boundaries(inputs["boundary"]), request)
        for w in extracted:
            wells_meta.append(dict(well_id=w.well_id, x=float(w.x), y=float(w.y), value=float(w.value),
                                   is_control_point=bool(w.is_control_point)))
        if [[m["x"], m["y"], m["value"]] for m in wells_meta] != [list(map(float, a)) for a in accepted]:
            raise SystemExit("well re-extraction does not match the field model order")
        domain = self._domain(inputs)
        pieces = []
        for d in fm.get("directions") or []:
            pieces.append(dict(line_id=d["line_id"], ratio=d["ratio"], influence_radius=d["influence_radius"],
                               core_radius=d["core_radius"], active=d["active"],
                               pieces=self._clip(domain, d["points"])))
        return dict(
            timing=dict(surface_s=t1 - t0, contours_s=t2 - t1),
            request={k: v for k, v in kwargs.items()},
            keep_numeric_fallback=bool(keep_numeric_fallback),
            wells=wells_meta,
            skipped=list(result.skipped_wells),
            sample_count=result.sample_count,
            request_levels=list(result.levels),
            grid=dict(x=[float(v) for v in sd["grid_x"]], y=[float(v) for v in sd["grid_y"]],
                      z=[[num(v) for v in row] for row in np.asarray(sd["grid_z"], float)],
                      valid_mask=np.asarray(sd["valid_mask"], bool).astype(int).tolist()),
            value_min=sd.get("value_min"), value_max=sd.get("value_max"),
            field_model=dict(cluster_span=fm.get("cluster_span"), search_radius=fm.get("search_radius"),
                             min_points=fm.get("min_points"), max_points=fm.get("max_points"),
                             power=fm.get("power"), directions=pieces,
                             interpretive_boundaries=fm.get("interpretive_boundaries") or []),
            barriers=sd.get("barriers") or [],
            barrier_buffer_distance=sd.get("barrier_buffer_distance"),
            contour_stop_buffer_distance=sd.get("contour_stop_buffer_distance"),
            contour_partition=policy or None,
            diagnostics={k: v for k, v in (sd.get("diagnostics") or {}).items()
                         if isinstance(v, (bool, int, float, str))},
            levels=level_info,
            levels_used=chosen,
            initial_contours=lines(initial) if initial is not None else None,
            local_detour=detour,
            contours=lines(contours),
        )

    @staticmethod
    def _crosses(line, points):
        from shapely.geometry import LineString
        return LineString(line).crosses(LineString(points))

    def _domain(self, inputs):
        from shapely.geometry import Polygon
        from shapely.ops import unary_union
        bounds = self.wf._extract_current_boundaries(inputs["boundary"])
        return unary_union([Polygon(b.exterior, b.holes) for b in bounds])

    @staticmethod
    def _clip(domain, points):
        from shapely.geometry import LineString
        geom = LineString(points).intersection(domain)
        parts = [geom] if geom.geom_type == "LineString" else [g for g in getattr(geom, "geoms", ())
                                                               if g.geom_type == "LineString"]
        return [[[float(x), float(y)] for x, y in p.coords] for p in parts if p.length > 1e-10]


def provenance(ref: Reference):
    import numpy, scipy, shapely
    return dict(schema=SCHEMA, referenceRevision=REFERENCE_SHA, referenceRoot=str(ref.root),
                adapter="tools/reference/singlefactor/run_structural_reference.py",
                python=sys.version.split()[0], platform=platform.platform(),
                packages=dict(numpy=numpy.__version__, scipy=scipy.__version__, shapely=shapely.__version__,
                              geos=shapely.geos_version_string),
                sources=[dict(path=p, sha256=_sha256(ref.root / p)) for p in SOURCE_FILES])


def package_paths(package: Path):
    d03 = package / "03_点位与成图边界"
    return dict(wells=d03 / "外委恩平一二段_井位标注点.shp", boundary=d03 / "恩平一二段_成图范围.shp",
                barriers=d03 / "打断线_解释性分区草案.shp", directions=d03 / "方向线_解释草案.shp")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--reference", type=Path, default=DEFAULT_REF)
    parser.add_argument("--package", type=Path, help="extracted 恩平一二段_汇报资料整合包 directory")
    parser.add_argument("--wells", type=Path)
    parser.add_argument("--boundary", type=Path)
    parser.add_argument("--barriers", type=Path)
    parser.add_argument("--directions", type=Path)
    parser.add_argument("--interpolation-area", type=Path)
    parser.add_argument("--override", action="append", default=[],
                        help="request override key=json, e.g. grid_resolution=120")
    parser.add_argument("--levels", help="comma-separated contour levels (default: dialog adaptive)")
    parser.add_argument("--keep-numeric-fallback", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    ref = Reference(args.reference)
    paths = package_paths(args.package) if args.package else {}
    for key in ("wells", "boundary", "barriers", "directions", "interpolation_area"):
        value = getattr(args, key)
        if value is not None:
            paths[key] = value
    if not paths.get("wells") or not paths.get("boundary"):
        raise SystemExit("need --package or --wells/--boundary")
    overrides = {}
    for item in args.override:
        key, _, value = item.partition("=")
        overrides[key] = json.loads(value)
    levels = [float(v) for v in args.levels.split(",")] if args.levels else None
    data = ref.run(paths, overrides, keep_numeric_fallback=args.keep_numeric_fallback, levels=levels)
    out = dict(provenance(ref), inputs={k: str(v) for k, v in paths.items()}, **data)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(out, ensure_ascii=False), encoding="utf-8")
    counts = {k: len(v) for k, v in data["contours"].items()}
    print(f"wells={len(data['wells'])} skipped={len(data['skipped'])} grid={len(data['grid']['x'])}x"
          f"{len(data['grid']['y'])} levels={data['levels_used']} contours={counts} "
          f"detour={data['local_detour'].get('triggered')} t={data['timing']}")


if __name__ == "__main__":
    main()
