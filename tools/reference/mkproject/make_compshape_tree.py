#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""竞赛形状树生成器（方向 78 对拍口径）。

生成鄂尔多斯竞赛数据目录布局的最小可用树（第 9 届竞赛的相对路径一比一，
内容为微型合成数据）：离开真实竞赛数据的机器上，也能对「无 --manifest 的
paleo_mkproject 竞赛路径」做 R0/改造后行为对拍——同树跑两个二进制，stdout
与退出码逐字节一致 = 向后兼容零差异的证据（任务书 Oracle 1）。

xlsx 走 sharedStrings（t="s"）——readWorkbook 的 OOXML 面只认这条（不解析
inlineStr）。分层数据.xlsx 按 per-井 sheet（A1..A20，层位/顶界垂深列）。

树是 throwaway（生成在临时目录，不进仓）；生成器本身进仓保证可复现。

用法：
  python3 tools/reference/mkproject/make_compshape_tree.py --out <dir>
"""

from __future__ import annotations

import argparse
import base64
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
# 复用 mini 生成器的合法最小 JPEG（单一真源，不养第二份基线件）。
sys.path.insert(0, str(REPO_ROOT / "tools" / "reference" / "mkproject" / "mini"))
import generate as mini_generate  # noqa: E402

NW = 20  # 井数（竞赛口径 20 口 A1..A20）

TOPS = ["石盒子组", "山西组", "太原组", "本溪组"]

_MAIN_NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
_REL_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
_PKG_REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"


def col_ref(i: int) -> str:
    s = ""
    while i > 0:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


def write_xlsx(path: Path, sheets: list[tuple[str, list[list[object]]]]) -> None:
    """极简 OOXML 工作簿：文本走 sharedStrings（t="s"），数值裸 <v>。"""
    shared: list[str] = []

    def sid(text: str) -> int:
        if text not in shared:
            shared.append(text)
        return shared.index(text)

    sheet_xmls = []
    for _name, rows in sheets:
        parts = ['<?xml version="1.0" encoding="UTF-8"?>',
                 f'<worksheet xmlns="{_MAIN_NS}">',
                 '<sheetData>']
        for r, row in enumerate(rows, start=1):
            parts.append(f'<row r="{r}">')
            for c, v in enumerate(row, start=1):
                ref = f"{col_ref(c)}{r}"
                if isinstance(v, str):
                    parts.append(
                        f'<c r="{ref}" t="s"><v>{sid(v)}</v></c>')
                else:
                    parts.append(f'<c r="{ref}"><v>{v}</v></c>')
            parts.append('</row>')
        parts += ['</sheetData>', '</worksheet>']
        sheet_xmls.append("".join(parts))

    sheet_tags = "".join(
        f'<sheet name="{name}" sheetId="{i + 1}" r:id="rId{i + 1}"/>'
        for i, (name, _rows) in enumerate(sheets))
    workbook = ('<?xml version="1.0" encoding="UTF-8"?>'
                f'<workbook xmlns="{_MAIN_NS}" xmlns:r="{_REL_NS}">'
                f'<sheets>{sheet_tags}</sheets></workbook>')
    rels = "".join(
        f'<Relationship Id="rId{i + 1}" Type="{_REL_NS}/worksheet" '
        f'Target="worksheets/sheet{i + 1}.xml"/>'
        for i in range(len(sheets)))
    wb_rels = ('<?xml version="1.0" encoding="UTF-8"?>'
               f'<Relationships xmlns="{_PKG_REL_NS}">{rels}'
               '</Relationships>')
    sst_items = "".join(f'<si><t>{s}</t></si>' for s in shared)
    shared_xml = ('<?xml version="1.0" encoding="UTF-8"?>'
                  f'<sst xmlns="{_MAIN_NS}" count="{len(shared)}" '
                  f'uniqueCount="{len(shared)}">{sst_items}</sst>')
    content_types = ('<?xml version="1.0" encoding="UTF-8"?>'
                     '<Types xmlns="http://schemas.openxmlformats.org/package/'
                     '2006/content-types">'
                     '<Default Extension="rels" ContentType="application/vnd.'
                     'openxmlformats-package.relationships+xml"/>'
                     '<Default Extension="xml" ContentType="application/xml"/>'
                     '<Override PartName="/xl/workbook.xml" ContentType='
                     '"application/vnd.openxmlformats-officedocument.'
                     'spreadsheetml.sheet.main+xml"/>'
                     + "".join(
                         f'<Override PartName="/xl/worksheets/sheet{i + 1}.xml" '
                         'ContentType="application/vnd.openxmlformats-'
                         'officedocument.spreadsheetml.worksheet+xml"/>'
                         for i in range(len(sheets)))
                     + '<Override PartName="/xl/sharedStrings.xml" ContentType='
                       '"application/vnd.openxmlformats-officedocument.'
                       'spreadsheetml.sharedStrings+xml"/></Types>')
    root_rels = ('<?xml version="1.0" encoding="UTF-8"?>'
                 f'<Relationships xmlns="{_PKG_REL_NS}">'
                 f'<Relationship Id="rId1" Type="{_OFFICE_DOC_REL}/officeDocument" '
                 'Target="xl/workbook.xml"/></Relationships>')

    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", content_types)
        zf.writestr("_rels/.rels", root_rels)
        zf.writestr("xl/workbook.xml", workbook)
        zf.writestr("xl/_rels/workbook.xml.rels", wb_rels)
        for i, xml in enumerate(sheet_xmls):
            zf.writestr(f"xl/worksheets/sheet{i + 1}.xml", xml)
        zf.writestr("xl/sharedStrings.xml", shared_xml)


_OFFICE_DOC_REL = ("http://schemas.openxmlformats.org/officeDocument/2006/"
                   "relationships")


def write_las(path: Path, well: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    start, step, n = 1500.0, 0.5, 300
    L = ["~Version Information", "VERS.   2.0:", "WRAP.   NO:", "~Well",
         f"STRT .m   {start:.3f} :", f"STOP .m   {start + step * (n - 1):.3f} :",
         f"STEP .m   {step:.3f} :", "NULL .  -99999 :",
         f"WELL.  {well}  : WELL", "~Curve", "DEPT .m : DEPTH",
         "GR .api : GAMMA RAY", "DT .us/m : SONIC", "~Ascii"]
    for i in range(n):
        dep = start + step * i
        L.append(f" {dep:10.3f} {120.0 + (i % 17):14.3f} {240.0 + (i % 23):14.3f}")
    L.append("")
    path.write_text("\n".join(L), encoding="utf-8")


def write_jpg(path: Path, tag: int) -> None:
    # mini 生成器的合法 JPEG 色板轮换（无尾缀——GDAL 严格解码零告警）。
    path.parent.mkdir(parents=True, exist_ok=True)
    plates = mini_generate.JPEGS
    path.write_bytes(base64.b64decode(plates[(tag - 1) % len(plates) + 1]))


def build_segy(path: Path) -> None:
    subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "make_segy_fixture.py"),
         "--out", str(path), "--inlines", "4", "--xlines", "5", "--samples", "32"],
        check=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="输出根目录（throwaway）")
    args = ap.parse_args()
    root = Path(args.out)
    facies = root / "2.沉积相分析-第9届"
    seis = root / "1.地震资料构造解释-第9届"
    load_dir = seis / "01.三维数据、工区加载相关参数及位置图"

    # 井位坐标.xlsx：20 井单表（井号/横坐标/纵坐标/补心/完钻）。
    rows = [["井号", "井口横坐标X", "井口纵坐标Y", "补心海拔", "完钻井深"]]
    for i in range(1, NW + 1):
        rows.append([f"A{i}", 1000.0 + i * 25.0, 5000.0 + i * 40.0,
                     1040.0 + i * 0.5, 1800.0 + i * 2.5])
    write_xlsx(facies / "2.1井位坐标" / "井位坐标.xlsx", [("井位坐标", rows)])

    # 分层数据.xlsx：per-井 sheet（层位/顶界垂深/底界垂深）。
    sheets = [("说明", [["分层数据说明"]])]
    for i in range(1, NW + 1):
        srows = [["层位", "顶界垂深", "底界垂深"]]
        for fi, top in enumerate(TOPS):
            md = 1450.0 + fi * 110.0 + i * 1.5
            srows.append([top, md, md + 100.0])
        sheets.append((f"A{i}", srows))
    write_xlsx(facies / "2.2分层数据" / "分层数据.xlsx", sheets)

    for i in range(1, NW + 1):
        write_las(facies / "2.5测井资料" / f"A{i}_log.las", f"A{i}")
    write_las(seis / "03.测井曲线" / "A3_log.las", "A3")

    # 20kou_tops.dat：'#' 头驱动列序（井名/层位/顶深齐全即按名取列）。
    tops = ["# wellName topName DepthMD"]
    for i in range(1, NW + 1):
        for fi, top in enumerate(TOPS):
            tops.append(f"A{i} {top} {1460.0 + fi * 105.0 + i * 2.0:.2f}")
    tops.append("")
    load_dir.mkdir(parents=True, exist_ok=True)
    (load_dir / "20kou_tops.dat").write_text("\n".join(tops), encoding="utf-8")

    build_segy(load_dir / "200P_seismic.sgy")

    for i, dep in ((1, 1600.0), (2, 1612.5), (3, 1624.0)):
        write_jpg(facies / "2.3岩心资料" / f"A{i}_{dep}_岩心.jpg", i)
    write_jpg(facies / "2.6薄片岩矿鉴定" / "A5_1650.0_薄片.jpg", 10)
    write_jpg(facies / "2.7粒度分析" / "A8_1700.0_粒度.jpg", 11)
    for i in (1, 2, 4):
        csv = ["顶深,底深,岩性,描述", "1500.00,1562.50,灰绿色泥岩,水平层理", ""]
        cut = facies / "2.4岩屑录井数据"
        cut.mkdir(parents=True, exist_ok=True)
        (cut / f"A{i}岩屑录井数据.csv").write_text(
            "\n".join(csv), encoding="utf-8")
    write_jpg(facies / "2.1井位坐标" / "20260722-沉积相平面作图范围-ok.JPG", 20)
    shutil.copyfile(REPO_ROOT / "testdata" / "project_area" / "mini_report.docx",
                    load_dir / "工区参数.docx")
    synth = seis / "05.参考井合成地震记录"
    synth.mkdir(parents=True, exist_ok=True)
    (synth / "A12_合成地震记录.dat").write_text(
        "# 合成地震记录（微型）\nA12 1500.0 2000.5\n", encoding="utf-8")
    aux = root / "辅助-参考测井excel"
    aux.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(REPO_ROOT / "tests" / "fixtures" / "outsource" / "wg1_well.xml",
                    aux / "wg1_well.xml")

    total = sum(f.stat().st_size for f in root.rglob("*") if f.is_file())
    print(json.dumps({"root": str(root), "files": len(list(root.rglob('*'))),
                      "bytes": total}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
