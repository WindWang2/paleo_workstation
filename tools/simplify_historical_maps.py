"""Simplify paired competition SHP/GPKG sources as polygonal coverages.

Requires shapely >= 2.1 (GEOS >= 3.12), numpy and pyshp. Inspection is the
default. --apply requires a new backup directory outside the source dataset.
All features, component/hole counts, attributes and CRS are retained; area_m2
is recalculated. Every staged pair is validated before replacing source files.
"""
import argparse
from contextlib import closing
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import sqlite3
import stat
import struct
import tempfile

import numpy as np
import shapefile
import shapely
from shapely.geometry import mapping, shape


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quoted(name):
    return '"' + name.replace('"', '""') + '"'


def gpkg_geometry(blob):
    if blob[:3] != b"GP\x00":
        raise ValueError("Unsupported GeoPackage geometry header")
    envelope = (blob[3] >> 1) & 7
    size = {0: 0, 1: 32, 2: 48, 3: 48, 4: 64}.get(envelope)
    if size is None:
        raise ValueError("Invalid GeoPackage envelope")
    return shapely.from_wkb(blob[8 + size:])


def gpkg_blob(geometry, srid):
    x0, y0, x1, y1 = geometry.bounds
    return (b"GP\x00\x03" + struct.pack("<i4d", srid, x0, x1, y0, y1)
            + shapely.to_wkb(geometry, byte_order=1, output_dimension=2))


def read_package(path):
    with closing(sqlite3.connect(f"{path.as_uri()}?mode=ro", uri=True)) as db:
        layers = db.execute("select table_name,column_name,srs_id from gpkg_geometry_columns").fetchall()
        if len(layers) != 1:
            raise ValueError("Expected exactly one polygon layer: " + str(path))
        table, column, srid = layers[0]
        rows = db.execute(f"select rowid,{quoted(column)} from {quoted(table)} order by rowid").fetchall()
    return table, column, srid, [r[0] for r in rows], np.array([gpkg_geometry(r[1]) for r in rows], dtype=object)


def topology_counts(geometry):
    parts = list(shapely.get_parts(geometry))
    return len(parts), sum(len(p.interiors) for p in parts)


def simplify(geometries, tolerance):
    if not bool(shapely.is_valid(geometries).all()) or not shapely.coverage_is_valid(geometries):
        raise ValueError("Source is not a valid edge-matched polygon coverage")
    result = shapely.coverage_simplify(geometries, tolerance, simplify_boundary=True)
    if not bool(shapely.is_valid(result).all()) or not shapely.coverage_is_valid(result):
        raise ValueError("Simplification produced invalid coverage")
    for old, new in zip(geometries, result):
        if new.is_empty or topology_counts(old) != topology_counts(new):
            raise ValueError("Simplification changed polygon components or holes")
    drift = shapely.hausdorff_distance(geometries, result)
    area_change = abs(shapely.area(result) - shapely.area(geometries)) / shapely.area(geometries)
    if float(drift.max()) > tolerance * 1.25 or float(area_change.max()) > .0025:
        raise ValueError("Simplification exceeds displacement or per-feature area budget")
    return result, {"features": len(result), "vertices_before": shapely.count_coordinates(geometries),
                    "vertices_after": shapely.count_coordinates(result),
                    "max_displacement_m": float(drift.max()),
                    "max_feature_area_change_pct": float(area_change.max() * 100)}


def write_package(source, destination, geometries):
    shutil.copy2(source, destination)
    destination.chmod(destination.stat().st_mode | stat.S_IWRITE)
    table, column, srid, ids, _ = read_package(source)
    with closing(sqlite3.connect(destination)) as db, db:
        triggers = db.execute("select sql from sqlite_master where type='trigger' and tbl_name=?", (table,)).fetchall()
        if any(re.search(r"\b(?:BEFORE|AFTER|INSTEAD\s+OF)\s+UPDATE\b", r[0] or "", re.I) for r in triggers):
            raise ValueError("Use the GDAL writer for packages with geometry update triggers")
        fields = {r[1] for r in db.execute(f"pragma table_info({quoted(table)})")}
        for identity, geometry in zip(ids, geometries):
            db.execute(f"update {quoted(table)} set {quoted(column)}=? where rowid=?", (gpkg_blob(geometry, srid), identity))
            if "area_m2" in fields:
                db.execute(f"update {quoted(table)} set area_m2=? where rowid=?", (geometry.area, identity))
        x0, y0, x1, y1 = shapely.total_bounds(geometries)
        db.execute("update gpkg_contents set min_x=?,min_y=?,max_x=?,max_y=? where table_name=?", (x0, y0, x1, y1, table))
        db.commit()
        db.execute("VACUUM")
    return destination


def write_shape(source, destination, geometries):
    with shapefile.Reader(str(source), encoding="utf-8") as reader:
        original = np.array([shape(s.__geo_interface__) for s in reader.shapes()], dtype=object)
        if len(original) != len(geometries):
            raise ValueError("SHP/GPKG feature count differs")
        fields = reader.fields[1:]
        area_column = next((i for i, f in enumerate(fields) if f[0] == "area_m2"), None)
        with shapefile.Writer(str(destination), shapeType=reader.shapeType, encoding="utf-8") as writer:
            writer.fields = fields
            for geometry, record in zip(geometries, reader.records()):
                values = list(record)
                if area_column is not None:
                    values[area_column] = geometry.area
                writer.shape(mapping(shapely.orient_polygons(geometry, exterior_cw=True)))
                writer.record(*values)


def validate_pair(original_package, package, original_shape, shape_path):
    table, column, srid, ids, geometries = read_package(package)
    old_table, old_column, old_srid, old_ids, _ = read_package(original_package)
    if (table, column, srid, ids) != (old_table, old_column, old_srid, old_ids):
        raise ValueError("GeoPackage layer, CRS or feature identity changed")
    with closing(sqlite3.connect(original_package)) as old, closing(sqlite3.connect(package)) as new:
        columns = [r[1] for r in old.execute(f"pragma table_info({quoted(table)})") if r[1] not in (column, "area_m2")]
        query = f"select {','.join(quoted(c) for c in columns)} from {quoted(table)} order by rowid"
        if old.execute(query).fetchall() != new.execute(query).fetchall():
            raise ValueError("GeoPackage attributes changed")
    with shapefile.Reader(str(original_shape), encoding="utf-8") as old, shapefile.Reader(str(shape_path), encoding="utf-8") as new:
        if old.fields != new.fields:
            raise ValueError("SHP field definitions changed")
        keep = [i for i, field in enumerate(old.fields[1:]) if field[0] != "area_m2"]
        if [[r[i] for i in keep] for r in old.records()] != [[r[i] for i in keep] for r in new.records()]:
            raise ValueError("SHP attributes changed")
        other = np.array([shape(s.__geo_interface__) for s in new.shapes()], dtype=object)
        if not bool(shapely.equals(geometries, other).all()):
            raise ValueError("SHP/GPKG geometry differs")


def run(root, tolerance, backup=None, resume=False):
    packages = sorted(root.glob("*.gpkg"))
    if not packages:
        raise ValueError("No historical GeoPackages found")
    prepared = []
    for package in packages:
        _, _, srid, _, geometry = read_package(package)
        if srid != 32649:
            raise ValueError("This preparation tool expects EPSG:32649 sources")
        with shapefile.Reader(str(package.with_suffix('.shp')), encoding='utf-8') as reader:
            counterpart = [shape(s.__geo_interface__) for s in reader.shapes()]
        if len(counterpart) != len(geometry) or not bool(shapely.equals(geometry, counterpart).all()):
            raise ValueError("Source SHP/GPKG pairing is inconsistent")
        simple, report = simplify(geometry, tolerance)
        report.update({"file": package.name, "crs": "EPSG:32649", "tolerance_m": tolerance})
        prepared.append((package, simple, report))
    report = {"maps": [r[2] for r in prepared], "backup": str(backup) if backup else None}
    if backup is None:
        return report
    if (backup.exists() and not resume) or backup.is_relative_to(root.parent):
        raise ValueError("Use a NEW backup directory outside the source dataset")
    backup.mkdir(parents=True, exist_ok=resume)
    originals = [p for p in root.iterdir() if p.is_file()]
    hashes = {p.name: digest(p) for p in originals}
    if resume:
        expected = json.loads((backup / "sha256.json").read_text(encoding="utf-8"))
        if expected != hashes:
            raise ValueError("Sources differ from the saved backup; do not resume a partial publication")
    for path in originals:
        if not resume:
            shutil.copy2(path, backup / path.name)
        if digest(backup / path.name) != hashes[path.name]:
            raise IOError("Backup verification failed")
    (backup / "sha256.json").write_text(json.dumps(hashes, indent=2), encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="simplified-", dir=backup) as directory:
        stage = Path(directory)
        replacements = []
        for path, geometry, _ in prepared:
            write_package(path, stage / path.name, geometry)
            write_shape(path.with_suffix('.shp'), stage / path.with_suffix('.shp').name, geometry)
            validate_pair(path, stage / path.name, path.with_suffix('.shp'), stage / path.with_suffix('.shp').name)
            replacements.extend(stage / path.with_suffix(ext).name for ext in ('.gpkg', '.shp', '.shx', '.dbf'))
        replaced = []
        try:
            for path in replacements:
                os.replace(path, root / path.name)
                replaced.append(path.name)
        except OSError:
            for name in reversed(replaced):
                shutil.copy2(backup / name, root / name)
            raise
    report["before_sha256"] = hashes
    report["after_sha256"] = {p.name: digest(p) for p in originals}
    (root / "图件边界简化记录.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--tolerance", type=float, default=30.)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup", type=Path)
    parser.add_argument("--resume", action="store_true", help="Reuse a complete backup only if sources still match it")
    args = parser.parse_args()
    if args.tolerance <= 0 or (args.apply and not args.backup):
        parser.error("Use positive tolerance; --apply requires --backup")
    output = run(args.root.resolve(strict=True), args.tolerance, args.backup.resolve() if args.apply else None, args.resume)
    print(json.dumps({k: v for k, v in output.items() if not k.endswith('sha256')}, ensure_ascii=False))
