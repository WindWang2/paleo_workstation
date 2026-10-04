#!/usr/bin/env python3
"""方向41 外委格式夹具生成器（纯标准库，确定性）。

生成：
  tests/fixtures/outsource/coordinates.xlsx   —— 坐标统计表（井号/X/Y，含坏行）
  tests/fixtures/outsource/wg1_well.xml       —— SpreadsheetML 2003 井数据（含坏行）
  tests/fixtures/outsource/not_a_workbook.xml —— 非工作簿 XML（分类器不得当数据）
  tests/fixtures/sfpkg/demo.sfpkg             —— 完整字段包（NaN nodata）
  tests/fixtures/sfpkg/bad_checksum.sfpkg     —— surface.npz 被改写 → 校验必须失败
  tests/fixtures/sfpkg/pickle_forbidden.sfpkg —— has_pickle=true → 必须拒绝
  tests/fixtures/sfpkg/missing_grid_z.sfpkg   —— 缺 grid_z → 必须失败

格式定义来源（只读参考，固定 SHA）：
  WWX9/haiyou-visualization @ 27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f
    Drawing/drawing/facies_workflow/sfpkg.py
    Drawing/drawing/single_factor/workflow.py

用法：
  python tools/reference/singlefactor/make_outsource_fixtures.py [--check]
  --check 只校验磁盘上的夹具与重新生成的字节一致（不做写入）。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTSOURCE_DIR = ROOT / "tests" / "fixtures" / "outsource"
SFPKG_DIR = ROOT / "tests" / "fixtures" / "sfpkg"


# ---------------------------------------------------------------- NPY / NPZ


def npy_bytes(descr: str, shape, data: bytes, fortran_order: bool = False) -> bytes:
    """最小 NPY v1 写入器（numpy 头部按 64 字节对齐，读回兼容）。"""
    shape_text = "(" + ", ".join(str(int(d)) for d in shape) + ("," if len(shape) == 1 else "") + ")"
    header = "{'descr': '%s', 'fortran_order': %s, 'shape': %s, }" % (
        descr,
        "True" if fortran_order else "False",
        shape_text,
    )
    prefix_len = 6 + 2 + 2
    pad = (-(prefix_len + len(header) + 1)) % 64
    header = header + " " * pad + "\n"
    head = b"\x93NUMPY" + bytes([1, 0]) + struct.pack("<H", len(header)) + header.encode("ascii")
    return head + data


def npy_f8(values, shape) -> bytes:
    return npy_bytes("<f8", shape, struct.pack("<%dd" % len(values), *values))


def npy_i4(values, shape) -> bytes:
    return npy_bytes("<i4", shape, struct.pack("<%di" % len(values), *values))


def npy_u1(values, shape) -> bytes:
    return npy_bytes("|u1", shape, bytes(int(v) & 0xFF for v in values))


def zip_bytes(entries) -> bytes:
    """确定性 ZIP（固定时间戳与权限位），夹具字节可复现（--check 依赖）。"""
    import io

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, payload in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 4, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, payload)
    return buffer.getvalue()


# ---------------------------------------------------------------- SFPKG


def surface_values():
    """包内数组的值域定义（读回期望值以这里为准）。"""
    rows, cols = 17, 21
    grid_z = []
    for i in range(rows):
        for j in range(cols):
            grid_z.append(0.4 + 0.1 * math.sin(i * 0.7 + j * 0.3))
    grid_z[0] = float("nan")  # 上游用例：左上角 nodata
    grid_x = [100.0 + 5.0 * j for j in range(cols)]
    grid_y = [10.0 + 5.0 * i for i in range(rows)]
    valid = [0 if math.isnan(v) else 1 for v in grid_z]
    boundary = list(valid)
    for j in range(cols):
        boundary[(rows - 1) * cols + j] = 0  # 上游用例：最后一行不在边界内
    coverage = [1 if v else 0 for v in valid]
    region = [1 if v else 0 for v in valid]
    trend = [0.35 for _ in grid_z]
    return {
        "grid_z": ("<f8", grid_z, (rows, cols)),
        "grid_x": ("<f8", grid_x, (cols,)),
        "grid_y": ("<f8", grid_y, (rows,)),
        "valid_mask": ("|u1", valid, (rows, cols)),
        "boundary_mask": ("|u1", boundary, (rows, cols)),
        "coverage_status": ("|u1", coverage, (rows, cols)),
        "region_ids": ("<i4", region, (rows, cols)),
        "well_region_ids": ("<i4", [1, 1, 1], (3,)),
        "source_trend_grid": ("<f8", trend, (rows, cols)),
    }


NPY_WRITERS = {"<f8": npy_f8, "<i4": npy_i4, "|u1": npy_u1}


def npz_bytes(drop_grid_z: bool = False, shift_grid_z: bool = False) -> bytes:
    entries = []
    for name, (kind, values, shape) in surface_values().items():
        if drop_grid_z and name == "grid_z":
            continue
        payload = list(values)
        if shift_grid_z and name == "grid_z":
            payload = [value if math.isnan(value) else value + 1.0 for value in payload]
        entries.append((name + ".npy", NPY_WRITERS[kind](payload, shape)))
    return zip_bytes(entries)


def manifest_json(has_pickle: bool = False) -> bytes:
    manifest = {
        "format": "sfpkg",
        "version": "1.0",
        "factor_name": "砂地比",
        "horizon": "T1",
        "method": "idw",
        "method_params": {"power": 2, "anisotropy_ratio": 1.0},
        "value_min": 0.3,
        "value_max": 0.5,
        "value_source": "analysis_grid",
        "barriers": [{"id": "f1", "type": "break_line"}],
        "directions": [{"id": "d1", "type": "direction_line", "ratio": 8}],
        "field_model": {"surface_kind": "analysis"},
        "buffer_transition": {"width": 20.0},
        "contour_partition": {"version": "17", "geometry_policy": "local_interpretive_detour"},
        "partition_extensions": [{"id": "e1"}],
        "barrier_shape_parameters": {"taper": 0.5},
        "barrier_buffer_distance": 30.0,
        "contour_stop_buffer_distance": 12.0,
        "barrier_value_policy": "preserve",
        "analysis_value_policy": "original_interpolation",
        "coverage_info": {"mode": "well_supported", "wells": 3},
        "boundaries": [{"id": "b1"}],
        "partition_barriers": [{"id": "p1"}],
        "partition_complete": True,
        "partition_version": "17",
        "interpolation_model": "local_direction_idw",
        "global_anisotropy_ratio": 1.0,
        "global_anisotropy_angle": 0.0,
        "levels": [0.2, 0.4, 0.6],
        "crs_wkt": 'PROJCS["test",GEOGCS["test",DATUM["test",SPHEROID["test",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["central_meridian",111],UNIT["metre",1]]',
        "data_source": "外委夹具",
        "created_at": "2026-10-04T00:00:00Z",
        "grid_shape": [17, 21],
        "has_pickle": has_pickle,
    }
    return json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")


def sfpkg_bytes(has_pickle: bool = False, drop_grid_z: bool = False, tamper_npz: bool = False) -> bytes:
    # 校验和口径与上游一致：checksum 覆盖真实写入的 manifest 与 npz 字节。
    # tamper_npz 让包内 npz 与 checksum 记录不一致（读取必须拒绝）。
    checksum_npz = npz_bytes(drop_grid_z=drop_grid_z)
    npz = npz_bytes(drop_grid_z=drop_grid_z, shift_grid_z=True) if tamper_npz else checksum_npz
    manifest_bytes = manifest_json(has_pickle)
    checksum = json.dumps(
        {
            "surface_npz_sha256": hashlib.sha256(checksum_npz).hexdigest(),
            "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "algorithm": "sha256",
        },
        ensure_ascii=False,
        indent=2,
        sort_keys=True,
    ).encode("utf-8")
    return zip_bytes(
        [
            ("manifest.json", manifest_bytes),
            ("surface.npz", npz),
            ("checksum.json", checksum),
        ]
    )


# ---------------------------------------------------------------- XLSX


def xlsx_column(index: int) -> str:
    letters = ""
    while index > 0:
        index, rem = divmod(index - 1, 26)
        letters = chr(ord("A") + rem) + letters
    return letters


def xlsx_string_cell(ref: str, shared_index: int) -> str:
    return '<c r="%s" t="s"><v>%d</v></c>' % (ref, shared_index)


def xlsx_number_cell(ref: str, value: str) -> str:
    return '<c r="%s"><v>%s</v></c>' % (ref, value)


def xlsx_sheet(rows) -> bytes:
    """rows: 每行是 [(列号, 类型, 值)]，类型 's'=共享字符串索引 / 'n'=原样数字文本。"""
    parts = [
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
        '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>',
    ]
    for row_index, row in enumerate(rows, start=1):
        parts.append('<row r="%d">' % row_index)
        for column, kind, value in row:
            ref = "%s%d" % (xlsx_column(column), row_index)
            parts.append(
                xlsx_string_cell(ref, value) if kind == "s" else xlsx_number_cell(ref, value)
            )
        parts.append("</row>")
    parts.append("</sheetData></worksheet>")
    return "".join(parts).encode("utf-8")


def coordinates_xlsx() -> bytes:
    # 共享字符串表：索引即出现顺序
    strings = ["井号", "X", "Y", "备注", "A1", "A2", "A3", "A4", "坐标", "1,200.75"]
    index = {text: i for i, text in enumerate(strings)}
    coordinate_rows = [
        [(1, "s", index["井号"]), (2, "s", index["X"]), (3, "s", index["Y"]), (4, "s", index["备注"])],
        [(1, "s", index["A1"]), (2, "n", "100.5"), (3, "n", "200.25"), (4, "s", index["备注"])],
        # 千分位逗号写成字符串格：parseNumericCell 口径（去逗号后 1200.75）
        [(1, "s", index["A2"]), (2, "s", index["1,200.75"]), (3, "n", "300.5"), (4, "s", index["备注"])],
        # 坏行 1：井号为空
        [(2, "n", "400"), (3, "n", "500")],
        # 坏行 2：X 非数值（共享字符串）
        [(1, "s", index["A3"]), (2, "s", index["备注"]), (3, "n", "600")],
        # 坏行 3：井号重复
        [(1, "s", index["A1"]), (2, "n", "111"), (3, "n", "222")],
        # 正常行：备注留空
        [(1, "s", index["A4"]), (2, "n", "700.5"), (3, "n", "800.25")],
    ]
    other_rows = [[(1, "s", index["备注"])], [(1, "s", index["坐标"])]]
    shared = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="%d" uniqueCount="%d">'
        % (len(strings), len(strings))
        + "".join("<si><t>%s</t></si>" % text for text in strings)
        + "</sst>"
    ).encode("utf-8")
    workbook = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
        'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
        "<sheets>"
        '<sheet name="坐标" sheetId="1" r:id="rId1"/>'
        '<sheet name="说明" sheetId="2" r:id="rId2"/>'
        "</sheets></workbook>"
    ).encode("utf-8")
    rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
        '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>'
        '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
        "</Relationships>"
    ).encode("utf-8")
    content_types = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
        "</Types>"
    ).encode("utf-8")
    root_rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
        "</Relationships>"
    ).encode("utf-8")
    return zip_bytes(
        [
            ("[Content_Types].xml", content_types),
            ("_rels/.rels", root_rels),
            ("xl/workbook.xml", workbook),
            ("xl/_rels/workbook.xml.rels", rels),
            ("xl/sharedStrings.xml", shared),
            ("xl/worksheets/sheet1.xml", xlsx_sheet(coordinate_rows)),
            ("xl/worksheets/sheet2.xml", xlsx_sheet(other_rows)),
        ]
    )


# ---------------------------------------------------------------- SpreadsheetML 2003


def cell(text: str, index=None, merge: int = 0) -> str:
    attrs = ""
    if index is not None:
        attrs += ' ss:Index="%d"' % index
    if merge:
        attrs += ' ss:MergeAcross="%d"' % merge
    return "<Cell%s><Data>%s</Data></Cell>" % (attrs, text)


def spreadsheetml_well() -> bytes:
    rows_a = [
        "<Row>" + cell("深度") + cell("GR") + cell("孔隙度") + "</Row>",
        "<Row>" + cell("1000") + cell("45") + cell("0.12") + "</Row>",
        "<Row>" + cell("1005") + cell("50") + cell("0.15") + "</Row>",
    ]
    rows_b = [
        "<Row>" + cell("层号") + cell("顶深") + cell("底深") + cell("厚度") + "</Row>",
        "<Row>" + cell("T1") + cell("1000") + cell("1050") + cell("50") + "</Row>",
        # ss:Index="4"：厚度在第 4 列，第 2/3 列留空（空洞补齐 + 空值报因）
        "<Row>" + cell("T2") + cell("60", index=4) + "</Row>",
        # 坏值行：底深「abc」、厚度「-」——数值字段读面必须逐条报因
        "<Row>" + cell("T3") + cell("1200") + cell("abc") + cell("-") + "</Row>",
        # 坏行：ss:Index 非整数 → 整行跳过并列因
        '<Row ss:Index="abc">' + cell("T4") + cell("1300") + cell("1350") + cell("50") + "</Row>",
    ]
    xml = (
        '<?xml version="1.0"?>\n'
        '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" '
        'xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">\n'
        '<Worksheet ss:Name="测井曲线"><Table>\n' + "\n".join(rows_a) + "\n</Table></Worksheet>\n"
        '<Worksheet ss:Name="地层单位道"><Table>\n' + "\n".join(rows_b) + "\n</Table></Worksheet>\n"
        "</Workbook>\n"
    )
    return xml.encode("utf-8")


NOT_A_WORKBOOK = (
    '<?xml version="1.0"?>\n<WellHead><Well><Name>A1</Name></Well></WellHead>\n'
).encode("utf-8")


# ---------------------------------------------------------------- 主流程


def expected_files():
    return {
        OUTSOURCE_DIR / "coordinates.xlsx": coordinates_xlsx(),
        OUTSOURCE_DIR / "wg1_well.xml": spreadsheetml_well(),
        OUTSOURCE_DIR / "not_a_workbook.xml": NOT_A_WORKBOOK,
        SFPKG_DIR / "demo.sfpkg": sfpkg_bytes(),
        SFPKG_DIR / "bad_checksum.sfpkg": sfpkg_bytes(tamper_npz=True),
        SFPKG_DIR / "pickle_forbidden.sfpkg": sfpkg_bytes(has_pickle=True),
        SFPKG_DIR / "missing_grid_z.sfpkg": sfpkg_bytes(drop_grid_z=True),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="只校验字节一致，不写盘")
    args = parser.parse_args()

    failures = 0
    for path, payload in expected_files().items():
        if args.check:
            if not path.exists():
                print("MISSING %s" % path.relative_to(ROOT))
                failures += 1
                continue
            if path.read_bytes() != payload:
                print("DIFF    %s" % path.relative_to(ROOT))
                failures += 1
            else:
                print("OK      %s" % path.relative_to(ROOT))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        print("WROTE   %s (%d bytes)" % (path.relative_to(ROOT), len(payload)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
