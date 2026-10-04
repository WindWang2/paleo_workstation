#!/usr/bin/env python3
"""Index native before/after screenshots; size checks are not visual approval."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def viewport_change(scale, name, sizes):
    """Only the measured, visually reviewed geometry changes are accepted."""
    factor = 1 if scale == '1x' else 2
    if name in ('12-curves-light.png', '12-curves-dark.png') and sizes == [
            (880 * factor, 560 * factor), (880 * factor, 596 * factor)]:
        return 'Dialog minimum height follows normalized button padding (6px -> spacing.sm); all rows and actions remain visible.'
    expected = [(2550, 1019), (2550, 995)] if factor == 1 else [(5100, 2037), (5100, 1989)]
    if name in ('seismic-3d-viewport-light.png', 'seismic-3d-viewport-dark.png') and sizes == expected:
        return 'Native GL child height follows normalized toolbar padding; its parent window is unchanged. Original framebuffers are archived without resizing.'
    return None


def png_size(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'not a PNG: {path}')
    return struct.unpack('>II', data[16:24])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    archive = args.root / 'docs/visual-polish'
    pairs = []
    failures = []
    for scale in ('1x', '2x'):
        before = archive / 'before' / scale
        after = archive / 'after' / scale
        inventories = []
        for phase in (before, after):
            listed = []
            for metadata in ('capture.json', 'capture-supplemental.json', 'capture-controls.json', 'capture-rows.json'):
                listed += json.loads((phase / metadata).read_text())['images']
            actual = {p.name for p in phase.glob('*.png')}
            if len(listed) != len(set(listed)) or set(listed) != actual or len(actual) != 149:
                failures.append(f'incomplete capture inventory: {phase.relative_to(args.root)}')
            inventories.append(set(listed))
        if inventories[0] != inventories[1]:
            failures.append(f'different capture inventory: {scale}')
        names = sorted({p.name for p in before.glob('*.png')} | {p.name for p in after.glob('*.png')})
        for name in names:
            a, b = before / name, after / name
            if not a.exists() or not b.exists():
                failures.append(f'missing pair: {scale}/{name}')
                continue
            sizes = [png_size(a), png_size(b)]
            reason = viewport_change(scale, name, sizes) if sizes[0] != sizes[1] else None
            if sizes[0] != sizes[1] and not reason:
                failures.append(f'different viewport: {scale}/{name}: {sizes}')
            if name.startswith('seismic-3d-viewport'):
                parent = name.replace('-viewport', '')
                expected_parent = (2550, 1275) if scale == '1x' else (5100, 2550)
                if png_size(before / parent) != expected_parent or png_size(after / parent) != expected_parent:
                    failures.append(f'different native GL parent viewport: {scale}/{parent}')
            pairs.append(dict(name=name, scale=scale, sizes=sizes,
                              reviewedViewportChange=reason,
                              before=a.relative_to(args.root).as_posix(),
                              after=b.relative_to(args.root).as_posix(),
                              beforeSha256=hashlib.sha256(a.read_bytes()).hexdigest(),
                              afterSha256=hashlib.sha256(b.read_bytes()).hexdigest()))
    report = dict(pairs=pairs, failures=failures,
                  limitation='Completeness/viewport checks only; manual visual review recorded in ledger.')
    (archive / 'screenshots.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f'{len(pairs)} pairs; {len(failures)} completeness/viewport failures')
    for failure in failures:
        print(failure)
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
