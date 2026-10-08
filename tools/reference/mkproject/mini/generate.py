#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mkproject 微型合成数据集生成器（方向 78 夹具工厂化）。

生成 tools/reference/mkproject/mini/ 下的全部夹具文件 + manifest.json：
  - 3 口井（A1/A2/A3）井位 SpreadsheetML 工作簿（.xml——readWorkbook 原生
    支持，manifest 里 convert=well_head 走竞赛同款转换）；
  - 分层两源：tops.dat（位置约定 井名 层名 MD）+ tops_header.dat（'#' 头
    驱动列序，列序故意打乱——覆盖 sniffTopsColumnMap）；
  - LAS×4（A3 第二份——多文件井面），每件 400 采样 × DEPT/GR/DT/RHOB/NPHI；
  - 微型 SEG-Y（subprocess 复用 tools/make_segy_fixture.py——道头约定单一
    真源）：4 IL × 5 XL × 64 采样，inline 10-13 × xline 100-104；
  - 岩心 JPG×3 + 薄片 JPG×1（四张真异内容合法 JPEG 色板——SHA 互异不触发
    导入 dedup、GDAL 严格解码零告警；深度锚在文件名）；
  - 岩屑 CSV×3（顶深/底深/岩性/描述表头方言）；
  - 参考文档 docx×1（复制 testdata/project_area/mini_report.docx）；
  - 外委工作簿 wg1_well.xml×1（复制 tests/fixtures/outsource/wg1_well.xml，
    manifest 里 linkExternal 外链口径）。

确定性：LCG 种子固定，输出逐字节可复现（重跑 = 幂等覆盖）。目录名沿用竞赛
关键词（岩心资料/岩屑录井数据/薄片岩矿鉴定）——井附件角色按目录段词表判。

体量约束：全目录 ≤2MB，mkproject 全链 ctest 秒级（见方向 78 任务书 Oracle 5）。

用法：
  python3 tools/reference/mkproject/mini/generate.py   # 就地重生成
"""

from __future__ import annotations

import base64
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path

MINI_DIR = Path(__file__).resolve().parent
REPO_ROOT = MINI_DIR.parents[3]

# ---- 确定性伪随机（与 C++ PerfFixtures::lcgNext 同族的 32 位 LCG）----
_LCG_STATE = 20261009


def lcg_unit() -> float:
    global _LCG_STATE
    _LCG_STATE = (1103515245 * _LCG_STATE + 12345) & 0xFFFFFFFF
    return _LCG_STATE / float(0x100000000)


def lcg_range(lo: float, hi: float) -> float:
    return lo + (hi - lo) * lcg_unit()


# ---- 工区几何：局部网格 x∈[1000,1100] y∈[5000,5100]（测网内），TD ~1820m ----
WELLS = [
    # name, x, y, KB(海拔m), TD(m)
    ("A1", 1010.0, 5020.0, 1048.5, 1820.0),
    ("A2", 1050.0, 5060.0, 1052.3, 1845.0),
    ("A3", 1080.0, 5040.0, 1046.9, 1802.5),
]
FORMATION_TOPS = ["石盒子组", "山西组", "太原组"]

# ---- 地理配准（manifest georeference 节；与 project.paleo 同形状）----
ANCHOR_LON, ANCHOR_LAT = 108.0, 36.0
M_PER_LON, M_PER_LAT = 90000.0, 111000.0
G_A, G_B, G_TE, G_TN = 1.0, 0.0, 0.0, 0.0
# A3 实测纬度微扰 3.0m——让「配准残差 ≤ 阈值」断言有物理意义。
A3_LAT_PERTURB_M = 3.0


def geo_forward(x: float, y: float) -> tuple[float, float]:
    e = G_A * x - G_B * y + G_TE
    n = G_B * x + G_A * y + G_TN
    return ANCHOR_LON + e / M_PER_LON, ANCHOR_LAT + n / M_PER_LAT


def control_points() -> list[dict]:
    cps = []
    for name, x, y, _kb, _td in WELLS:
        lon, lat = geo_forward(x, y)
        if name == "A3":
            lat += A3_LAT_PERTURB_M / M_PER_LAT
        dlon = (lon - geo_forward(x, y)[0]) * M_PER_LON
        dlat = (lat - geo_forward(x, y)[1]) * M_PER_LAT
        residual = math.hypot(dlon, dlat)
        cps.append({"well": name, "x": x, "y": y, "lon": lon, "lat": lat,
                    "residualM": round(residual, 6)})
    return cps


# ---- 合法最小 JPEG ×3（16×16 纯色三色板，System.Drawing 一次性产的
# 基线件；真异内容 = SHA 互异（dedup 面无需尾缀技巧，GDAL 严格解码零告警）。
JPEGS = {
    1: "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAQABADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD1Oiiivyc/TT//2Q==",
    2: "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAQABADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD2Ciiivzw8Q//Z",
    3: "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAQABADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD0Wiiiug+wP//Z",
    4: "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAQABADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDoKKKK/Sz5o//Z",
}


def write_jpeg(path: Path, tag: int) -> None:
    path.write_bytes(base64.b64decode(JPEGS[((tag - 1) % len(JPEGS)) + 1]))


# ---- 井位 SpreadsheetML 工作簿（.xml；readWorkbook 原生面）----
def write_wellhead_xml(path: Path) -> None:
    ss = ['<?xml version="1.0" encoding="UTF-8"?>',
          '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet"',
          ' xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">',
          ' <Worksheet ss:Name="井位坐标">',
          '  <Table>',
          '   <Row>']
    for h in ("井号", "井口横坐标X", "井口纵坐标Y", "补心海拔", "完钻井深"):
        ss.append(f'    <Cell><Data ss:Type="String">{h}</Data></Cell>')
    ss.append("   </Row>")
    for name, x, y, kb, td in WELLS:
        ss.append("   <Row>")
        ss.append(f'    <Cell><Data ss:Type="String">{name}</Data></Cell>')
        for v in (x, y, kb, td):
            ss.append(f'    <Cell><Data ss:Type="Number">{v}</Data></Cell>')
        ss.append("   </Row>")
    ss += ['  </Table>', ' </Worksheet>', '</Workbook>', '']
    path.write_text("\n".join(ss), encoding="utf-8")


# ---- 分层数据（两源：位置约定 + 头驱动列序）----
def write_tops_dat(path: Path) -> None:
    lines = ["# 井名 层名 MD（位置约定）"]
    for wi, (name, _x, _y, _kb, td) in enumerate(WELLS):
        for fi, form in enumerate(FORMATION_TOPS):
            md = 1500.0 + wi * 37.5 + fi * 95.25
            if md < td:
                lines.append(f"{name} {form} {md:.2f}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def write_tops_header_dat(path: Path) -> None:
    # 列序故意打乱（DepthMD 在前）——证明 '#' 头驱动列映射生效。
    lines = ["# topName DepthMD wellName"]
    for wi, (name, _x, _y, _kb, td) in enumerate(WELLS):
        for fi, form in enumerate(FORMATION_TOPS):
            md = 1560.0 + wi * 41.0 + fi * 88.5
            if md < td:
                lines.append(f"{form} {md:.2f} {name}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


# ---- LAS 2.0（DEPT/GR/DT/RHOB/NPHI × 400 采样）----
def write_las(path: Path, well: str, seed_tag: int) -> None:
    start, step, n = 1500.0, 0.5, 400
    stop = start + step * (n - 1)
    L = ["# LAS Format Log File (paleo mkproject mini fixture)",
         "~Version Information",
         "VERS.   2.0:",
         "WRAP.   NO:",
         "~Well",
         f"STRT .m   {start:.3f} :",
         f"STOP .m   {stop:.3f} :",
         f"STEP .m   {step:.3f} :",
         "NULL .  -99999 :",
         "COMP.           : PALEO FIXTURE",
         f"WELL.  {well}  : WELL",
         f"DATE.  01/01/2026 00:00:00  : DATE",
         "~Curve",
         "DEPT .m                   : DEPTH",
         "GR .api                   : GAMMA RAY",
         "DT .us/m                 : SONIC",
         "RHOB .g/cm3              : DENSITY",
         "NPHI .v/v                : NEUTRON",
         "~Parameter",
         "# ====================================",
         "~Ascii"]
    for i in range(n):
        dep = start + step * i
        if (i + seed_tag) % 97 == 0:
            gr = -99999.0  # 埋 NULL 值（对齐 PerfFixtures 夹具口径）
        else:
            gr = lcg_range(40.0, 140.0)
        dt = lcg_range(200.0, 280.0)
        rhob = lcg_range(2.2, 2.7)
        nphi = lcg_range(0.05, 0.35)
        L.append(f" {dep:10.3f} {gr:14.3f} {dt:14.3f} {rhob:14.3f} {nphi:14.3f}")
    L.append("")
    path.write_text("\n".join(L), encoding="utf-8")


# ---- 岩屑 CSV（顶深/底深/岩性/描述 表头方言）----
LITHO_ROWS = [
    ("灰绿色泥岩", "水平层理发育"),
    ("浅灰色细砂岩", "分选中等"),
    ("深灰色粉砂质泥岩", "见黄铁矿"),
    ("灰白色中砂岩", "钙质胶结"),
]


WELL_INDEX = {w[0]: i for i, w in enumerate(WELLS)}


def write_cuttings_csv(path: Path, well: str) -> None:
    # 深度基点按井序偏移：三井字节互异（同字节会被导入侧 SHA dedup 成一件）。
    base = 1500.0 + WELL_INDEX[well] * 12.5
    lines = ["顶深,底深,岩性,描述"]
    for i, (litho, desc) in enumerate(LITHO_ROWS):
        top = base + i * 62.5
        lines.append(f"{top:.2f},{top + 62.5:.2f},{litho},{desc}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def build_segy(path: Path) -> None:
    subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "make_segy_fixture.py"),
         "--out", str(path),
         "--inlines", "4", "--xlines", "5", "--samples", "64",
         "--base-inline", "10", "--base-xline", "100",
         "--x0", "1000", "--dx", "25", "--y0", "5000", "--dy", "25"],
        check=True)


def main() -> int:
    for sub in ("well", "strat", "las", "seis", "2.3岩心资料", "2.4岩屑录井数据",
                "2.6薄片岩矿鉴定", "ref", "outsource"):
        (MINI_DIR / sub).mkdir(parents=True, exist_ok=True)

    write_wellhead_xml(MINI_DIR / "well" / "井位坐标.xml")
    write_tops_dat(MINI_DIR / "strat" / "tops.dat")
    write_tops_header_dat(MINI_DIR / "strat" / "tops_header.dat")

    las_tag = 1
    for name, _x, _y, _kb, _td in WELLS:
        write_las(MINI_DIR / "las" / f"{name}.las", name, las_tag)
        las_tag += 1
    write_las(MINI_DIR / "las" / "A3_rerun.las", "A3", las_tag)  # 多文件井

    build_segy(MINI_DIR / "seis" / "mini.sgy")

    tag = 1
    depths = {"A1": 1610.5, "A2": 1625.0, "A3": 1640.5}
    for name, _x, _y, _kb, _td in WELLS:
        write_jpeg(MINI_DIR / "2.3岩心资料" / f"{name}_{depths[name]:.1f}_岩心.jpg",
                   tag)
        tag += 1
    write_jpeg(MINI_DIR / "2.6薄片岩矿鉴定" / "A3_1701.0_薄片.jpg", tag)

    for name, _x, _y, _kb, _td in WELLS:
        write_cuttings_csv(MINI_DIR / "2.4岩屑录井数据" / f"{name}_岩屑录井.csv",
                           name)

    shutil.copyfile(REPO_ROOT / "testdata" / "project_area" / "mini_report.docx",
                    MINI_DIR / "ref" / "工区说明.docx")
    shutil.copyfile(REPO_ROOT / "tests" / "fixtures" / "outsource" / "wg1_well.xml",
                    MINI_DIR / "outsource" / "wg1_well.xml")

    cps = control_points()
    max_res = max(c["residualM"] for c in cps)
    manifest = {
        "schema": 1,
        "georeference": {
            "kind": "similarity2d",
            "targetCrs": "EPSG:4326",
            "anchor": {"lonDeg": ANCHOR_LON, "latDeg": ANCHOR_LAT,
                       "metersPerDegLon": M_PER_LON, "metersPerDegLat": M_PER_LAT},
            "params": {"a": G_A, "b": G_B, "tE": G_TE, "tN": G_TN},
            "formula": ("E = a*x - b*y + tE ; N = b*x + a*y + tN ; "
                        "lon = lon0 + E/mPerLon ; lat = lat0 + N/mPerLat (meters)"),
            "provenance": (
                "mini 合成数据集生成器直写（tools/reference/mkproject/mini/"
                "generate.py）：恒等相似变换 + A3 纬度微扰 %.1fm——残差断言有"
                "物理意义" % A3_LAT_PERTURB_M),
            "controlPoints": cps,
            "maxResidualM": max_res,
        },
        "imports": [
            {"path": "well/井位坐标.xml", "convert": "well_head"},
            {"path": "strat/tops.dat", "type": "well_stratification"},
            {"path": "strat/tops_header.dat", "type": "well_stratification"},
            {"path": "las/A1.las"},
            {"path": "las/A2.las"},
            {"path": "las/A3.las"},
            {"path": "las/A3_rerun.las"},
            {"path": "seis/mini.sgy"},
            {"path": "2.3岩心资料/A1_1610.5_岩心.jpg"},
            {"path": "2.3岩心资料/A2_1625.0_岩心.jpg"},
            {"path": "2.3岩心资料/A3_1640.5_岩心.jpg"},
            {"path": "2.6薄片岩矿鉴定/A3_1701.0_薄片.jpg"},
            {"path": "2.4岩屑录井数据/A1_岩屑录井.csv"},
            {"path": "2.4岩屑录井数据/A2_岩屑录井.csv"},
            {"path": "2.4岩屑录井数据/A3_岩屑录井.csv"},
            {"path": "ref/工区说明.docx"},
            {"path": "outsource/wg1_well.xml", "type": "outsource_workbook",
             "linkExternal": True},
        ],
        "expect": {
            "wells": 3,
            "entitiesByType": {"well": 3, "seismic_survey": 1, "auxiliary": 2},
            "unresolvedLinks": 0,
            "georeferencedWells": 3,
            "seismicGeometry": {"inlineMin": 10, "inlineMax": 13,
                                "xlineMin": 100, "xlineMax": 104},
        },
    }
    (MINI_DIR / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")

    total = sum(f.stat().st_size for f in MINI_DIR.rglob("*") if f.is_file())
    print(f"mini dataset regenerated under {MINI_DIR} ({total / 1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
