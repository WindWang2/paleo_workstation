#!/usr/bin/env python3
"""Read-only audit of the competition-data layout; no inference or input filling.

Requires openpyxl. Coverage is a GR/MD/lithology screening, not model accuracy
or a replacement for the application's model-specific readiness check.
"""
import argparse
import bisect
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
import statistics
from datetime import datetime, timezone


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def las_profile(path):
    curves, depths, values, annotations = [], [], [], []
    section, null, step = '', -9999.99, None
    with path.open(encoding='utf-8-sig', errors='replace') as stream:
        for line in stream:
            text = line.strip()
            if not text:
                continue
            if text.startswith('#'):
                if 'PALEO_SYNTHETIC_INPUT' in text:
                    annotations.append(text)
                continue
            if text.startswith('~'):
                section = text[1:2].upper()
                continue
            if section == 'C':
                curves.append(text.split('.', 1)[0].strip().upper())
            elif section == 'W':
                match = re.match(r'(NULL|STEP)\s*\.\S*\s+([-+\d.eE]+)', text, re.I)
                if match:
                    if match[1].upper() == 'NULL':
                        null = float(match[2])
                    else:
                        step = float(match[2])
            elif section == 'A':
                parts = text.split()
                if not curves or 'GR' not in curves:
                    continue
                try:
                    depth, value = float(parts[0]), float(parts[curves.index('GR')])
                except (ValueError, IndexError):
                    continue
                if math.isfinite(depth) and depth != null:
                    depths.append(depth)
                    # 与生产曲线缺值口径一致；0保留为数值，另行统计，不能静默当缺测。
                    usable = math.isfinite(value) and abs(value-null) > 1e-4 and value not in (
                        -99999, -999.25, -9999, -999) and -99990 < value < 1e29
                    values.append(value if usable else None)
    pairs = sorted(zip(depths, values))
    depths, values = [p[0] for p in pairs], [p[1] for p in pairs]
    increments = [b - a for a, b in zip(depths, depths[1:]) if b > a]
    median = statistics.median(increments) if increments else None
    public = dict(path=str(path), sha256=digest(path), curves=curves,
                  samples=len(depths), valid_gr=sum(v is not None for v in values),
                  positive_gr=sum(v is not None and v > 0 for v in values),
                  zero_gr=sum(v == 0 for v in values),
                  header_step=step, actual_median_step=median,
                  step_disagreement=step is not None and median is not None and abs(abs(step) - median) > 1e-4,
                  source_annotations=annotations,
                  provenance='explicit_synthetic' if annotations else 'undeclared')
    return public, depths, values


def cuttings_profile(path):
    import openpyxl
    book = openpyxl.load_workbook(path, read_only=True, data_only=True)
    intervals, sources = [], collections.Counter()
    for sheet in book:
        rows = sheet.iter_rows(values_only=True)
        for _ in range(15):
            header = next(rows, ())
            names = [str(v or '').strip() for v in header]
            top = next((i for i, n in enumerate(names) if '顶深' in n), None)
            base = next((i for i, n in enumerate(names) if '底深' in n), None)
            lith = next((i for i, n in enumerate(names) if n in ('岩性', '岩石定名', '岩性定名')), None)
            if None not in (top, base, lith):
                break
        else:
            continue
        source = next((i for i, n in enumerate(names) if n in ('数据来源', '来源', 'source')), None)
        for row in rows:
            try:
                a, b = float(row[top]), float(row[base])
                name = str(row[lith] or '').strip()
            except (ValueError, TypeError, IndexError):
                continue
            if not name or not math.isfinite(a + b) or b <= a:
                continue
            origin = str(row[source] or '').strip() if source is not None and source < len(row) else ''
            sources[origin or '未声明'] += 1
            intervals.append((a, b, origin))
        break  # 第一份可识别录井表；不把说明页当独立数据。
    book.close()
    return dict(path=str(path), sha256=digest(path), intervals=len(intervals),
                source_rows=dict(sources)), intervals


def merged_length(intervals, top, base):
    spans = sorted((max(top, a), min(base, b)) for a, b, *_ in intervals if b > top and a < base)
    total, end = 0.0, top
    for a, b in spans:
        total += max(0, b - max(end, a))
        end = max(end, b)
    return total


def screen(row, depths, values, intervals, step, window):
    out = {'well': row['well_name'], 'horizon': row['horizon'], 'factor_quality': row.get('quality', '')}
    try:
        top, base = float(row['top_md']), float(row['base_md'])
        if not math.isfinite(top + base) or base <= top:
            raise ValueError()
    except (KeyError, ValueError):
        return dict(out, ready=False, reason='缺少有效MD层界')
    start, stop = bisect.bisect_left(depths, top), bisect.bisect_left(depths, base)
    run = longest = valid = 0
    previous = None
    for d, v in zip(depths[start:stop], values[start:stop]):
        supported = v is not None and any(a <= d < b for a, b, _ in intervals)
        continuous = previous is None or (step and 0 < d - previous <= step * 1.5)
        run = (run + 1 if continuous else 1) if supported else 0
        valid += v is not None
        longest = max(longest, run)
        previous = d
    coverage = merged_length(intervals, top, base) / (base - top)
    synthetic = [iv for iv in intervals if '合成' in iv[2] or 'synthetic' in iv[2].lower()]
    return dict(out, top_md=top, base_md=base, gr_samples=stop-start, valid_gr=valid,
                longest_supported_run=longest, lithology_coverage=round(coverage, 6),
                synthetic_lithology_coverage=round(merged_length(synthetic, top, base)/(base-top), 6),
                ready=longest >= window,
                reason='' if longest >= window else f'连续GR+岩性样点少于{window}')


def archived_predictions(root, schema):
    path = root / '_旧项目工作区/artifacts/layers/well-log-attributes.gpkg'
    if not path.exists():
        return {}
    connection = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)
    groups = connection.execute('SELECT predicted_facies, COUNT(*), COUNT(DISTINCT well_name) FROM attributes '
        'WHERE predicted_facies IS NOT NULL AND TRIM(predicted_facies) != \'\' GROUP BY predicted_facies').fetchall()
    candidates = []
    # 只接受带显式 m 单位的文件名深度；文件名没有声明 MD/TVD，故仅列候选。
    for image in sorted((root/'井').rglob('*')):
        if image.suffix.lower() not in ('.jpg', '.jpeg', '.tif', '.png'):
            continue
        match = re.match(r'(A\d+)[,，](\d+(?:\.\d+)?)m', image.name, re.I)
        if not match:
            continue
        well, depth = match[1].upper(), float(match[2])
        matches = connection.execute('SELECT DISTINCT horizon, predicted_facies FROM attributes '
            'WHERE well_name=? AND top_md<=? AND base_md>? AND predicted_facies IS NOT NULL '
            'AND TRIM(predicted_facies)!=\'\'', (well, depth, depth)).fetchall()
        for horizon, label in matches:
            candidates.append(dict(well=well, filename_depth_m=depth, horizon=horizon,
                predicted_microfacies=label, image=str(image),
                status='候选关联；须核实照片深度基准和沉积学解释，尚未验证微相'))
    connection.close()
    names = {str(leaf.get('microfacies', leaf.get('name', ''))) for leaf in schema}
    return dict(path=str(path), sha256=digest(path), support_candidates=candidates, classes=[dict(label=label, intervals=count,
        wells=wells, recognized_in_default=label in names) for label, count, wells in groups],
        interpretation='历史预测输出；井段数不是独立标注样本数，也不代表地质验证准确率')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--window', type=int, default=128)
    parser.add_argument('--baseline-logs', type=Path, help='可选：补齐前LAS目录，只读逐深度比对')
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    if args.window < 1 or output == root or root in output.parents:
        parser.error('window必须为正数；output须在数据目录之外，保证只读核验')
    output.mkdir(parents=True, exist_ok=True)
    factors_path = root / '单因素/全部井分层单因素.csv'
    with factors_path.open(encoding='utf-8-sig', newline='') as stream:
        factors = list(csv.DictReader(stream))
    horizons = {p.stem for p in (root/'层位').glob('*.dat')}
    wells, screens = [], []
    for directory in sorted((root/'井').iterdir(), key=lambda p: p.name):
        if not re.fullmatch(r'A\d+', directory.name) or not directory.is_dir():
            continue
        logs = sorted(directory.rglob('*.las'))
        tables = sorted(directory.glob('*岩屑录井*.xlsx'))
        if not logs or not tables:
            wells.append(dict(well=directory.name, error='缺少LAS或岩屑表'))
            continue
        preferred = directory / f'{directory.name}_log.las'
        log, depths, values = las_profile(preferred if preferred.exists() else logs[0])
        if args.baseline_logs:
            baseline = args.baseline_logs / f'{directory.name}_log.las'
            if baseline.exists():
                prior, prior_depths, prior_values = las_profile(baseline)
                original = {round(d, 6): v for d, v in zip(prior_depths, prior_values) if v is not None}
                matched = sum(v is not None and round(d, 6) in original and
                    math.isclose(v, original[round(d, 6)], rel_tol=1e-6, abs_tol=1e-4)
                    for d, v in zip(depths, values))
                log['before_fill'] = dict(profile=prior, unchanged_valid_gr_samples=matched,
                    unmatched_or_changed_gr_samples=log['valid_gr']-matched,
                    semantics='补齐前备份对照；值未变化不等同独立地质解释或微相真值')
        cuttings, intervals = cuttings_profile(tables[0])
        wells.append(dict(well=directory.name, log=log, cuttings=cuttings,
                          duplicate_las_candidates=[str(p) for p in logs if str(p) != log['path']]))
        for horizon in sorted(horizons):
            matches = [r for r in factors if r['well_name'] == directory.name and r['horizon'] == horizon]
            row = matches[0] if len(matches) == 1 else dict(well_name=directory.name, horizon=horizon)
            screens.append(screen(row, depths, values, intervals, log['actual_median_step'], args.window))
    schema_path = root/'相分类/默认相分类.json'
    schema = json.loads(schema_path.read_text(encoding='utf-8-sig'))
    report = dict(audited_at=datetime.now(timezone.utc).isoformat(), root=str(root), window=args.window,
        method='只读GR/MD/岩性连续窗口筛查；不调用预测服务，不补数据，不等同模型专项验收或独立地质验证',
        factors=dict(path=str(factors_path), sha256=digest(factors_path)),
        default_schema=dict(path=str(schema_path), sha256=digest(schema_path), classes=len(schema)),
        wells=wells, screening=screens, archived_predictions=archived_predictions(root, schema))
    (output/'audit.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    fields = list(dict.fromkeys(k for row in screens for k in row))
    with (output/'well_horizon_coverage.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader(); writer.writerows(screens)
    candidates = report['archived_predictions'].get('support_candidates', [])
    if candidates:
        with (output/'microfacies_support_candidates.csv').open('w', encoding='utf-8-sig', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(candidates[0])); writer.writeheader(); writer.writerows(candidates)
    ready = sum(r['ready'] for r in screens)
    synthetic_wells = [w['well'] for w in wells if w.get('log', {}).get('source_annotations')]
    mismatch = [w['well'] for w in wells if w.get('log', {}).get('step_disagreement')]
    classes = report['archived_predictions'].get('classes', [])
    text = ['# 测井微相数据支撑核验', '', report['method'], '',
      f'- 核验 {len(wells)} 口井、{len(horizons)} 个编图层位，共 {len(screens)} 个井层组合。',
      f'- 以 {args.window} 点窗口筛查：{ready} 个具备连续GR及岩性输入，{len(screens)-ready} 个未满足。',
      f'- {len(synthetic_wells)} 口井LAS含显式合成输入标记：{", ".join(synthetic_wells)}。未标记也不能据此认定全实测。',
      f'- LAS头STEP与数据体中位步长不一致：{", ".join(mismatch) or "无"}。',
      f'- 默认分类 {len(schema)} 类；历史井属性表出现 {len(classes)} 类预测词面。', '',
      '| 预测微相 | 历史井段数 | 涉及井数 | 默认词表识别 | 照片候选关联数 | 独立地质验证 |',
      '|---|---:|---:|---|---:|---|']
    text += [f'| {c["label"]} | {c["intervals"]} | {c["wells"]} | {"是" if c["recognized_in_default"] else "待映射"} | '
             f'{sum(r["predicted_microfacies"] == c["label"] for r in candidates)} | 未建立逐段对照 |' for c in classes]
    text += ['', '照片候选仅按文件名中显式带m单位的深度匹配，尚须确认MD/TVD基准并人工解释。'
             '0不代表没有岩心、薄片或粒度资料；没有明确井深关联的资料不计入此列。']
    text += ['', '## 未满足窗口筛查的井层', '', '| 井 | 层位 | 原因 |', '|---|---|---|']
    text += [f'| {r["well"]} | {r["horizon"]} | {r["reason"]} |' for r in screens if not r['ready']]
    if args.baseline_logs:
        text += ['', '## 补齐前备份对照', '',
                 '| 井 | 备份正GR点 | 当前非空GR点 | 原深度原值保留点 | 新增或变化点 |',
                 '|---|---:|---:|---:|---:|']
        for well in wells:
            log = well.get('log', {}); before = log.get('before_fill')
            if before:
                text.append(f'| {well["well"]} | {before["profile"]["positive_gr"]} | {log["valid_gr"]} | '
                    f'{before["unchanged_valid_gr_samples"]} | {before["unmatched_or_changed_gr_samples"]} |')
        text += ['', '正GR点用于暴露备份中的大量0值（详见JSON的zero_gr），不把0自动删除。'
                 '“新增或变化”包含插值、替代、重采样或延伸；仅比较GR深度和值，不据此推断岩性或微相正确。']
    text += ['', '## 验收范围与可选验证', '',
      '连续窗口合格只说明存在可提交输入。GR插值、邻井移植、GR推导岩性和补齐分层不能算作独立实测证据。',
      '本工具核验类别可识别、输入不足提示和来源追溯；不要求另行人工微相标签。'
      '若以后需要独立验证，可按井层隔离合成段，关联有井号、MD井段、解释者和依据的人工标签，'
      '并与岩心、薄片、粒度或原始录井证据逐段对应。现有图片数量不能代替该对应关系。',
      '可选验证可采用独立井/井组留出集，计算逐类召回率、精确率与混淆矩阵；本次不承诺地质准确率。',
      '真实服务模型清单需在软件内检查其完整类别、曲线要求和窗口；本次离线筛查没有访问服务或重新推理。',
      '详细来源、SHA-256和井层缺项见 audit.json 与 well_horizon_coverage.csv。']
    (output/'核验报告.md').write_text('\n'.join(text)+'\n', encoding='utf-8')
    print(json.dumps(dict(wells=len(wells), combinations=len(screens), input_window_ready=ready,
                         explicitly_synthetic_las=len(synthetic_wells), historical_classes=classes,
                         step_disagreement=mismatch, output=str(output)), ensure_ascii=False))


if __name__ == '__main__':
    main()
