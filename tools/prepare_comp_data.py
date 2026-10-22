"""Normalize the supplied competition sources and write auditable well factors.

Requires openpyxl, xlsxwriter, numpy, pyproj and segyio. Run without
--apply for inspection. The coordinate table is the authority for well heads;
an assigned EPSG code does not improve the accuracy of the original registration.
"""
import argparse
import csv
import hashlib
import json
import re
import shutil
import sqlite3
import struct
import stat
from datetime import datetime
from pathlib import Path

import numpy as np
import openpyxl
import pyproj
import segyio
import xlsxwriter


CRS = "EPSG:32649"
FIELDS = ["well_name", "horizon", "top_md", "base_md", "log_layer_thickness_md",
          "sand_thick", "sand_ratio", "mud_thick", "coverage_ratio", "sand_beds",
          "max_sand_thick", "source", "quality", "crs"]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for part in iter(lambda: f.read(4 * 1024 * 1024), b""):
            h.update(part)
    return h.hexdigest()


def lithology_kind(name):
    # Mud-rich rocks must be checked before the substring 'sandstone'.
    if any(x in name for x in ("泥岩", "页岩", "泥质粉砂质泥", "shale", "mudstone")):
        return "mud"
    if any(x in name for x in ("砂岩", "sandstone", "siltstone")):
        return "sand"
    if any(x in name for x in ("灰岩", "白云岩", "砾岩", "煤", "凝灰岩")):
        return "other"
    return "unknown"


def interval_factors(top, base, segments):
    """Integrate a disjoint interval partition; gaps/conflicts stay unknown."""
    cuts = {top, base}
    relevant = [(max(top, a), min(base, b), kind) for a, b, kind in segments
                if b > top and a < base and b > a]
    for a, b, _ in relevant:
        cuts.update((a, b))
    covered = sand = mud = longest = current = 0.0
    beds = 0
    conflict = False
    edges = sorted(cuts)
    for a, b in zip(edges, edges[1:]):
        kinds = {k for x, y, k in relevant if x < b - 1e-9 and y > a + 1e-9}
        if len(kinds) != 1 or "unknown" in kinds:
            conflict |= len(kinds) > 1
            current = 0.0
            continue
        kind = kinds.pop()
        covered += b - a
        if kind == "sand":
            beds += current == 0
            current += b - a
            longest = max(longest, current)
            sand += b - a
        else:
            current = 0.0
            if kind == "mud":
                mud += b - a
    complete = covered >= base - top - 1e-6 and not conflict
    return {"log_layer_thickness_md": base - top,
            "sand_thick": sand if complete else None,
            "sand_ratio": sand / (base - top) if complete else None,
            "mud_thick": mud if complete else None,
            "coverage_ratio": covered / (base - top),
            "sand_beds": beds if complete else None,
            "max_sand_thick": longest if complete else None,
            "quality": "完整覆盖（含原表补齐段，非全实测）" if complete else
                       "岩性重叠冲突" if conflict else "岩性覆盖不足，不把空白当泥岩"}


def well_positions(root):
    wb = openpyxl.load_workbook(root / "井坐标.xlsx", data_only=True, read_only=True)
    rows = list(wb.worksheets[0].values)
    result = {str(r[0]): r[1:] for r in rows[1:] if r[0]}
    wb.close()
    if len(result) != 20 or any(not (100000 < v[0] < 900000 and 0 < v[1] < 10000000)
                                for v in result.values()):
        raise ValueError("Expected 20 UTM49N well heads; review source coordinates first")
    return result


def normalize_las(path, well, position):
    raw = path.read_bytes()
    # Preserve the complete curve section byte-for-byte, including precision.
    split = re.search(br"(?im)^~(?:C|CURVE)\b", raw)
    if not split:
        raise ValueError(f"No LAS curve section: {path}")
    header = raw[:split.start()].decode("latin-1")
    x, y = position[:2]
    lon, lat = pyproj.Transformer.from_crs(CRS, 4326, always_xy=True).transform(x, y)
    values = {"X": ("M", x), "Y": ("M", y), "WELL": ("", well),
              "SHORT_WELL_NAME": ("", well), "LONG_WELL_NAME": ("", well),
              "UWI": ("", well), "LONGITUDE": ("DEG", lon), "LATITUDE": ("DEG", lat),
              "HORIZONTAL_COORDINATE_SYSTEM": ("", "EPSG32649")}
    seen = set()
    lines = []
    for line in header.splitlines():
        match = re.match(r"^\s*([A-Za-z_]+)\s*\.", line)
        key = match.group(1).upper() if match else ""
        if key in values:
            unit, value = values[key]
            line = f"{key} .{unit} {value} : normalized from well coordinate table"
            seen.add(key)
        lines.append(line)
    additions = []
    for key in sorted(values.keys() - seen):
        unit, value = values[key]
        additions.append(f"{key} .{unit} {value} : normalized from well coordinate table")
    index = next(i for i, line in enumerate(lines) if line.upper().startswith("~W")) + 1
    lines[index:index] = additions
    path.write_bytes(("\r\n".join(lines) + "\r\n").encode("latin-1") + raw[split.start():])
    assert path.read_bytes().endswith(raw[split.start():])


def normalize_horizon(path):
    text = path.read_text(encoding="utf-8-sig")
    points = {}
    records = []
    for line in text.splitlines():
        if line.strip() and not line.startswith("#"):
            x, y, z, il, xl = line.split()[:5]
            if not (100000 < float(x) < 900000 and 0 < float(y) < 10000000):
                raise ValueError(f"Undeclared local horizon coordinates: {path}")
            points[(int(il), int(xl))] = (float(x), float(y))
            records.append(line)
    headers = []
    for line in text.splitlines():
        if not line.startswith("#"):
            continue
        if line.startswith("# Projection:"):
            line = f"# Projection: {CRS} (WGS 84 / UTM zone 49N)"
        match = re.match(r"# (P[123]):\s*(\d+),\s*(\d+),", line)
        if match:
            label, il, xl = match.groups()
            x, y = points[(int(il), int(xl))]
            line = f"# {label}: {il}, {xl}, {x:.5f}, {y:.5f}"
        headers.append(line)
    path.write_text("\n".join(headers + records) + "\n", encoding="utf-8")
    return {"path": path.name, "points": len(points), "crs": CRS}


def normalize_gpkg(path):
    path.chmod(path.stat().st_mode | stat.S_IWRITE)
    with sqlite3.connect(path) as db:
        row = db.execute("select table_name,column_name,srs_id from gpkg_geometry_columns").fetchone()
        table, column, old_srs = row
        definition = db.execute("select definition from gpkg_spatial_ref_sys where srs_id=?", (old_srs,)).fetchone()[0]
        if not pyproj.CRS.from_wkt(definition).equals(pyproj.CRS(CRS)):
            raise ValueError(f"Unexpected GeoPackage CRS: {path}")
        db.execute("insert or ignore into gpkg_spatial_ref_sys values(?,?,?,?,?,?)",
                   ("WGS 84 / UTM zone 49N", 32649, "EPSG", 32649,
                    pyproj.CRS(CRS).to_wkt("WKT1_GDAL"), "Source registration unchanged"))
        # Existing packages have no RTree triggers. Refuse an unexpected layout.
        if db.execute("select count(*) from sqlite_master where type='trigger' and tbl_name=? and upper(sql) like '%AFTER UPDATE%'", (table,)).fetchone()[0]:
            raise ValueError("Use GDAL to rebuild this GeoPackage with triggers")
        quoted_table = '"' + table.replace('"', '""') + '"'
        quoted_column = '"' + column.replace('"', '""') + '"'
        for fid, value in db.execute(f"select rowid,{quoted_column} from {quoted_table}").fetchall():
            data = bytearray(value)
            struct.pack_into("<i" if data[3] & 1 else ">i", data, 4, 32649)
            db.execute(f"update {quoted_table} set {quoted_column}=? where rowid=?", (data, fid))
        db.execute("update gpkg_geometry_columns set srs_id=32649 where table_name=?", (table,))
        db.execute("update gpkg_contents set srs_id=32649 where table_name=?", (table,))


def collect_factors(root):
    rows = []
    for folder in sorted((root / "井").glob("A*"), key=lambda p: int(p.name[1:])):
        well = folder.name
        litho = next(folder.glob("*岩屑录井数据.xlsx"))
        wb = openpyxl.load_workbook(litho, data_only=True, read_only=True)
        content = list(wb.worksheets[0].values)
        headers = list(content[0])
        a_col, b_col, rock_col = (headers.index(key) for key in ("顶深(m)", "底深(m)", "岩石定名"))
        segments = [(float(r[a_col]), float(r[b_col]), lithology_kind(str(r[rock_col])))
                    for r in content[1:] if isinstance(r[a_col], (int, float))
                    and isinstance(r[b_col], (int, float)) and r[b_col] > r[a_col]]
        wb.close()
        td = np.loadtxt(folder / f"{well}_时深.dat", comments="#", usecols=(2, 3), encoding="utf-8-sig")
        td = td[np.argsort(td[:, 0])]
        wb = openpyxl.load_workbook(folder / f"{well}_分层.xlsx", data_only=True, read_only=True)
        for entry in list(wb.worksheets[0].values)[1:]:
            if len(entry) < 4 or not entry[1] or entry[2] is None or entry[3] is None:
                continue
            a, b = float(entry[2]), float(entry[3])
            if b <= a:
                raise ValueError(f"Invalid formation bounds {well} {entry}")
            if a < td[0, 0] or b > td[-1, 0]:
                rows.append({"well_name": well, "horizon": str(entry[1]), "top_md": None,
                             "base_md": None, **{k: None for k in FIELDS[4:11]},
                             "source": litho.relative_to(root).as_posix(), "crs": CRS,
                             "quality": "时深对照不覆盖层界，未外推；原TVD层界见分层源表"})
                continue
            top, base = np.interp([a, b], td[:, 0], td[:, 1])
            row = {"well_name": well, "horizon": str(entry[1]), "top_md": float(top),
                   "base_md": float(base), "source": litho.relative_to(root).as_posix(), "crs": CRS}
            row.update(interval_factors(top, base, segments))
            rows.append(row)
        wb.close()
    return rows


def write_factors(root, rows):
    all_rows = [(root / "单因素/全部井分层单因素.csv", rows)]
    for well in sorted({r["well_name"] for r in rows}):
        all_rows.append((root / f"井/{well}/{well}_单因素.csv", [r for r in rows if r["well_name"] == well]))
    for path, items in all_rows:
        path.parent.mkdir(exist_ok=True)
        with path.open("w", newline="", encoding="utf-8-sig") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDS)
            writer.writeheader()
            writer.writerows(items)
    book = xlsxwriter.Workbook(root / "单因素/全部井分层单因素.xlsx")
    fmt = book.add_format({"font_name": "Arial", "num_format": "0.000"})
    title = book.add_format({"font_name": "Arial", "bold": True, "bg_color": "#EDF1F5"})
    sheet = book.add_worksheet("分层单因素")
    sheet.write_row(0, 0, FIELDS, title)
    for n, row in enumerate(rows, 1):
        for c, key in enumerate(FIELDS):
            value = row[key]
            if key == "log_layer_thickness_md" and value is not None:
                sheet.write_formula(n, c, f"=D{n+1}-C{n+1}", fmt, value)
            elif key == "sand_ratio" and value is not None:
                sheet.write_formula(n, c, f'=IF(E{n+1}>0,F{n+1}/E{n+1},"")', fmt, value)
            elif value is not None:
                sheet.write(n, c, value, fmt)
    sheet.freeze_panes(1, 2)
    sheet.autofilter(0, 0, len(rows), len(FIELDS) - 1)
    sheet.set_column(0, 13, 20)
    sheet.set_column(11, 12, 55)
    notes = book.add_worksheet("口径与来源")
    for n, line in enumerate([
        "坐标：EPSG:32649；沿用原资料近似配准，并非新实测地理定位。",
        "厚度均为 MD；TVD 层界用各井时深表的 MD/TVD 对照线性内插，禁止外推。",
        "砂厚为层段内砂岩族井段长度并集；粉砂质泥岩属于泥族，泥质粉砂岩属于砂族。",
        "砂地比=砂厚/完整层厚（0–1）；同类重叠去重，跨岩性重叠或覆盖不足留空。",
        "岩屑表包含以前按 GR 生成的补齐井段，结果必须按混合来源解释，不能视为全实测。",
        "源岩性和分层表保留；CSV 为导入交换值，XLSX 的层厚和砂地比公式带已校验缓存。",
        "原件完整备份位置见根目录 坐标与单因素处理记录.json。"]):
        notes.write(n, 0, line)
    notes.set_column(0, 0, 125)
    book.close()


def apply(root, backup, resume=False):
    files = [p for p in root.rglob("*") if p.is_file() and
             not any(x.startswith("_") for x in p.relative_to(root).parts)]
    if backup.exists() and not resume:
        raise ValueError("Backup directory already exists; select a new directory")
    if backup.is_relative_to(root):
        raise ValueError("Backup must be outside the source tree")
    backup.mkdir(parents=True, exist_ok=resume)
    original = json.loads((backup / "sha256.json").read_text(encoding="utf-8")) if resume else {}
    if resume:
        files = [root / relative for relative in original]
    for path in files:
        relative = path.relative_to(root)
        destination = backup / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not resume:
            shutil.copy2(path, destination)
            original[relative.as_posix()] = digest(path)
        if digest(destination) != original[relative.as_posix()]:
            raise IOError(f"Backup verification failed: {path}")
    (backup / "sha256.json").write_text(json.dumps(original, ensure_ascii=False, indent=2), encoding="utf-8")
    positions = well_positions(root)
    rows = collect_factors(root)  # Validate all calculation inputs before editing sources.
    for path in (root / "井").glob("A*/*.las"):
        normalize_las(path, path.parent.name, positions[path.parent.name])
    horizons = [normalize_horizon(p) for p in (root / "层位").glob("*.dat")]
    for path in (root / "古地理图").glob("*.shp"):
        if not pyproj.CRS(path.with_suffix(".prj").read_text()).equals(pyproj.CRS(CRS)):
            raise ValueError(f"Unexpected Shapefile CRS: {path}")
        path.with_suffix(".prj").write_text(pyproj.CRS(CRS).to_wkt("WKT1_ESRI"), encoding="ascii")
    for path in (root / "古地理图").glob("*.gpkg"):
        normalize_gpkg(path)
    for path in (root / "地震").glob("*.sgy"):
        with segyio.open(str(path), "r+", ignore_geometry=True) as f:
            for i in range(f.tracecount):
                f.header[i] = {segyio.TraceField.CoordinateUnits: 1}
            text = bytes(f.text[0])
            text = text[:30 * 80] + b"C31 CRS EPSG:32649 WGS84 UTM49N; CDP_X/CDP_Y METRES".ljust(80) + text[31 * 80:]
            f.text[0] = text
    wb = openpyxl.load_workbook(root / "井坐标.xlsx")
    ws = wb.worksheets[0]
    ws.title = "EPSG32649井位"
    ws.cell(1, 2, "井口横坐标X_EPSG32649(m)")
    ws.cell(1, 3, "井口纵坐标Y_EPSG32649(m)")
    convert = pyproj.Transformer.from_crs(CRS, 4326, always_xy=True)
    for r in range(2, ws.max_row + 1):
        lon, lat = convert.transform(ws.cell(r, 2).value, ws.cell(r, 3).value)
        ws.cell(r, 6, lon)
        ws.cell(r, 7, lat)
        # Historical local coordinates remain explicit history, not import X/Y.
        ws.cell(1, 8, "历史原局部X_不可作当前坐标(m)")
        ws.cell(1, 9, "历史原局部Y_不可作当前坐标(m)")
    wb.save(root / "井坐标.xlsx")
    write_factors(root, rows)
    resources = Path(__file__).resolve().parents[1] / "resources/geology"
    taxonomy = root / "相分类"
    taxonomy.mkdir(exist_ok=True)
    leaves = []
    for name, exported in (("facies-hierarchy.json", "默认相分类.json"),
                           ("facies-templates-extra.json", "扩展相分类模板.json")):
        shutil.copy2(resources / name, taxonomy / exported)
        leaves.extend(json.loads((resources / name).read_text(encoding="utf-8")))
    for leaf in leaves:
        texture = leaf.get("texture")
        if texture:
            destination = taxonomy / texture
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(resources / texture, destination)
    manifest = {"crs": CRS, "registration": "原交付配准近似，不等于实测大地测量",
                "backup": str(backup), "wells": len(positions), "factorRows": len(rows),
                "completeRows": sum(r["sand_ratio"] is not None for r in rows), "horizons": horizons,
                "sourceSha256": original,
                "changed": [p.relative_to(root).as_posix() for p in files
                            if p.exists() and digest(p) != original[p.relative_to(root).as_posix()]]}
    (root / "README_数据说明.md").write_text(
        f"# Paleo 工区源资料\n\n当前所有空间数据采用 **{CRS} / WGS 84 UTM 49N，米**。\n"
        "井坐标表、LAS 井头、地震 CDP 道头、层位 XY 与角点、SHP/GPKG 坐标声明已核对一致。\n"
        "原局部格网曾用近似配准定位；本次统一并不提高原配准的实测精度。\n\n"
        "井/每井目录中的 *_单因素.csv 可自动导入；单因素/ 提供总 CSV 和公式审计 XLSX。\n"
        "按层位统计砂厚、砂地比、层厚、泥厚、砂层数、最大单砂层厚及覆盖率。\n"
        "TVD 层界按时深表转 MD；岩性空缺/冲突留空，绝不按泥岩补零。已有 GR 补齐段如实标注。\n\n"
        "地震目录两个 0 字节 .dat 原件无数据，导入时跳过；参考资料中的旧位置图/加载参数是历史文档，\n"
        "不是当前空间数据，不能作为坐标来源。无配准图片/PDF/粒度照片无空间坐标，不强行配准。\n"
        "_旧项目工作区 与井目录 _备份 是历史归档，不作新建工程输入。\n\n"
        f"修改前的有效源资料完整备份：`{backup}`。逐文件 SHA256 和修改清单见处理记录 JSON。\n",
        encoding="utf-8")
    manifest["postSourceSha256"] = {key: digest(root / key) for key in original}
    manifest["changed"] = [key for key in original if manifest["postSourceSha256"][key] != original[key]]
    manifest["taxonomy"] = {"defaultLeaves": len(json.loads((resources / "facies-hierarchy.json").read_text(encoding="utf-8"))),
                            "allTemplatePaths": len(leaves), "directory": "相分类"}
    manifest["outputs"] = [p.relative_to(root).as_posix() for p in root.rglob("*")
                           if p.is_file() and not any(x.startswith("_") for x in p.relative_to(root).parts)
                           and p.relative_to(root).as_posix() not in original
                           and p.name != "坐标与单因素处理记录.json"]
    (root / "坐标与单因素处理记录.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup", type=Path)
    parser.add_argument("--resume", action="store_true", help="Reuse a complete, SHA-verified backup after a failed run")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    if args.apply:
        backup = args.backup or root.with_name(root.name + "_before_unification_" + datetime.now().strftime("%Y%m%d-%H%M%S"))
        report = apply(root, backup.resolve(), args.resume)
        print(json.dumps({k: v for k, v in report.items() if k not in ("sourceSha256", "changed")}, ensure_ascii=False))
    else:
        rows = collect_factors(root)
        print(json.dumps({"crs": CRS, "wells": len(well_positions(root)), "factorRows": len(rows),
                          "completeRows": sum(r["sand_ratio"] is not None for r in rows)}, ensure_ascii=False))
