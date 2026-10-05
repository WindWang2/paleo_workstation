#!/usr/bin/env python3
"""方向50确定性夹具；仅标准库。生成真实大端 SEG-Y/LAS/井表/OOXML。

python3 tools/reference/make_io_robustness_fixtures.py [--out DIR]
每个 SEG-Y 含 2 inline × 2 crossline × 16 样点，有限样点=(道号+1)*16+s。
IEEE 坏样点为 qNaN/sNaN/+Inf/-Inf；IBM 包含转换溢出。文件清单带 SHA256。
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile
import tempfile


def segy(fmt=5, bad=False, offsets=(188, 192)):
    header = bytearray(b" " * 3200 + b"\0" * 400)
    for offset, value in ((3212, 2), (3216, 2000), (3220, 16), (3224, fmt)):
        struct.pack_into(">h", header, offset, value)
    for tr in range(4):
        th = bytearray(240)
        for offset, value in ((0, tr + 1), (8, 100 + tr // 2), (20, 200 + tr % 2),
                              (72, tr % 2 * 20), (76, tr // 2 * 40),
                              (offsets[0], 100 + tr // 2), (offsets[1], 200 + tr % 2)):
            struct.pack_into(">i", th, offset, value)
        for offset, value in ((70, 1), (114, 16), (116, 2000)):
            struct.pack_into(">h", th, offset, value)
        header.extend(th)
        for s in range(16):
            value = (tr + 1) * 16 + s
            if fmt == 5:
                bits = struct.unpack(">I", struct.pack(">f", value))[0]
                if bad and tr == 0 and s in (3, 5, 8, 10):
                    bits = {3: 0x7FC00001, 5: 0x7F800001, 8: 0x7F800000, 10: 0xFF800000}[s]
            else:
                exponent = 64
                mantissa = float(value)
                while mantissa >= 1:
                    mantissa /= 16
                    exponent += 1
                bits = (exponent << 24) | int(mantissa * (1 << 24))
                if bad and tr == 0 and s == 3:
                    bits = 0x7FFFFFFF  # IBM 最大数量级，超出 IEEE float
            header.extend(struct.pack(">I", bits))
    return bytes(header)


def workbook(path, sheet):
    parts = {
        "[Content_Types].xml": '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>',
        "_rels/.rels": '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>',
        "xl/workbook.xml": '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="坐标" sheetId="1" r:id="rId1"/></sheets></workbook>',
        "xl/_rels/workbook.xml.rels": '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>',
        "xl/worksheets/sheet1.xml": '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>' + sheet + '</sheetData></worksheet>',
    }
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, content in sorted(parts.items()):
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.create_system = 3  # Windows/Linux 一致的 ZIP 头
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content.encode("utf-8"))


def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    good = segy()
    files = {
        "good.sgy": good,
        "nonfinite_ieee.sgy": segy(bad=True),
        "nonfinite_ibm.sgy": segy(fmt=1, bad=True),
        "custom_words.sgy": segy(offsets=(180, 184)),
        "swapped_words.sgy": segy(offsets=(192, 188)),
        "truncated_samples.sgy": good[:-1],
        "truncated_trace_header.sgy": good[:3600 + 3 * 304 + 120],
        "truncated_binary_header.sgy": good[:3599],
        "tops_nulls.tsv": b"# WellName\tName\tMD\tX\tY\tZ\tTVD\tTime(ms)\nW1\tGOOD\t100\t10\t20\t-50\t90\t80\nW1\tSMI\t-99999\nW1\tLAS\t-999.25\nW1\tEMPTY\t\t10\t20\t-50\t90\t80\nW1\tTEXT\tbroken\nW1\tMISSING\nW1\tOTHER\t-9999\n",
        "td_nulls.tsv": b"# Well : W1\n100\t-100\t100\t100\n-999.25\t-150\t150\t150\n-99999\t-150\t150\t150\n\t-150\t150\t150\n200\t-200\t200\t200\n",
    }
    bad_tail = bytearray(files["truncated_samples.sgy"])
    struct.pack_into(">h", bad_tail, 3600 + 3 * 304 + 114, -1)
    files["truncated_bad_ns.sgy"] = bytes(bad_tail)
    for name, unit in (("m_upper", "M"), ("meter", "METER"), ("meters", "METERS"),
                       ("m_lower", "m"), ("ft", "ft"), ("metre", "METRE"),
                       ("feet", "FEET"), ("unknown", "UNKNOWN")):
        files["unit_" + name + ".las"] = ("~V\nVERS. 2.0 : version\nWRAP. NO : wrap\n~W\nWELL. W1 : well\nNULL. -999.25 : null\n~C\nDEPT." + unit + " : depth\nGR.API : gamma\n~A\n100 10\n200 20\n").encode()
    for name, content in files.items():
        (out / name).write_bytes(content)
    headers = '<row r="1"><c r="A1" t="inlineStr"><is><t>井号</t></is></c><c r="B1" t="inlineStr"><is><t>X</t></is></c><c r="C1" t="inlineStr"><is><t>Y</t></is></c></row>'
    rows = '<row r="2"><c r="A2" t="inlineStr"><is><t>W1</t></is></c><c r="B2"><v>123.5</v></c><c r="C2"><v>-45.25</v></c></row>'
    rows += '<row r="3"><c r="A3" t="inlineStr"><is><t>W2</t></is></c><c r="C3"><v>23</v></c></row>'
    rows += '<row r="4"><c r="A4" t="inlineStr"><is><t>W3</t></is></c><c r="B4"><v>NaN</v></c><c r="C4"><v>23</v></c></row>'
    rows += '<row r="5"><c r="A5" t="inlineStr"><is><t>W1</t></is></c><c r="B5"><v>999</v></c><c r="C5"><v>999</v></c></row>'
    workbook(out / "coordinates_edges.xlsx", headers + rows)
    workbook(out / "invalid_ref.xlsx", headers + '<row r="2"><c r="XFE2"><v>10</v></c></row>')
    workbook(out / "malformed_refs.xlsx", headers + '<row r="2"><c r="A2" t="inlineStr"><is><t>W1</t></is></c><c r="B2junk"><v>123</v></c><c r="C0"><v>456</v></c></row>')
    workbook(out / "unordered_cells.xlsx", headers + '<row r="2"><c r="C2"><v>456</v></c><c r="A2" t="inlineStr"><is><t>W1</t></is></c><c r="B2"><v>123</v></c></row>')
    workbook(out / "physical_rows.xlsx", headers.replace('r="1"', 'r="7"').replace('A1', 'A7').replace('B1', 'B7').replace('C1', 'C7') + '<row r="100"><c r="A100" t="inlineStr"><is><t>W1</t></is></c><c r="C100"><v>456</v></c></row>')
    workbook(out / "duplicate_cells.xlsx", headers + '<row r="2"><c r="A2" t="inlineStr"><is><t>W1</t></is></c><c r="B2"><v>123</v></c><c r="B2"><v>999</v></c><c r="C2"><v>456</v></c></row>')
    workbook(out / "formula_error.xlsx", headers + '<row r="2"><c r="A2" t="inlineStr"><is><t>W1</t></is></c><c r="B2" t="e"><v>#DIV/0!</v></c><c r="C2"><v>456</v></c></row>')
    workbook(out / "shared_strings_missing.xlsx", headers + '<row r="2"><c r="A2" t="s"><v>0</v></c><c r="B2"><v>123</v></c><c r="C2"><v>456</v></c></row>')
    workbook(out / "implicit_row.xlsx", headers + '<row><c r="C100"><v>456</v></c><c r="A100" t="inlineStr"><is><t>W1</t></is></c><c r="B100"><v>123</v></c></row>')
    (out / "physical_intervals.xml").write_text('<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet"><Worksheet ss:Name="层段"><Table><Row ss:Index="7"><Cell><Data ss:Type="String">层号</Data></Cell><Cell><Data ss:Type="String">厚度</Data></Cell></Row><Row ss:Index="100"><Cell><Data ss:Type="String">T1</Data></Cell><Cell><Data ss:Type="Number">123</Data></Cell></Row></Table></Worksheet></Workbook>', encoding="utf-8")
    manifest = {p.name: {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(out.iterdir()) if p.name != "manifest.json" and p.is_file()}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[2] / "tests/fixtures/io_robustness")
    parser.add_argument("--check", action="store_true", help="在临时目录重生成并逐字节对照，不改写夹具")
    args = parser.parse_args()
    if not args.check:
        generate(args.out)
        return
    with tempfile.TemporaryDirectory(prefix="paleo-io-fixtures-") as directory:
        expected = Path(directory)
        generate(expected)
        names = {p.name for p in expected.iterdir()}
        actual = {p.name for p in args.out.iterdir() if p.is_file()}
        if names != actual:
            parser.error(f"fixture files differ: missing={sorted(names - actual)}, extra={sorted(actual - names)}")
        changed = [name for name in sorted(names) if (expected / name).read_bytes() != (args.out / name).read_bytes()]
        if changed:
            parser.error(f"fixture bytes differ: {changed}")
        print(f"IO fixture matrix: {len(names) - 1} files match byte-for-byte")


if __name__ == "__main__":
    main()
