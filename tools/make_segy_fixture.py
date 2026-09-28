#!/usr/bin/env python3
"""Synthetic minimal SEG-Y fixture generator (wave3/model-hardening).

Produces a legal SEG-Y shaped like the real project-area volume — i.e. the
conventions SegyReader::open() freezes on (see src/io/segyreader.cpp, plan §2):

  * 3200-byte ASCII textual header + 400-byte binary header, no extended headers
  * format code 5 (IEEE fp32), fixed sample interval / sample count
  * inline word at trace-header offset 188 is CONSTANTLY 0 (project convention)
    -> the reader falls back to ordinal indexing: inline = first-trace field
       record (offset 8) + trace_index / traces_per_inline
  * crossline = the CDP word at offset 20, repeating identically on every line
  * traces per inline declared in the binary header (bytes 13-14, offset 12)
  * survey corners come from Source X/Y at offsets 72/76 with scalar at 70;
    within a line x is non-decreasing with CDP, across lines y is
    non-decreasing with inline (regular grid -> exact corners)
  * trace numbers (TRACL, offset 0) are consecutive from 1

The default output is a few KB — small enough to commit under testdata/ (the
real volume is 966 MB and must never enter the repository).

`--mb N` (wave/seismic-engine-deep 主线7 性能闸门) synthesizes a production-
shaped volume of at least N MiB with the SAME ordinal header convention
(mirrors the 966 MB real survey), for latency-budget tests. First run takes
~30-60 s in pure Python (numpy, when present, makes it near-instant); the
file is cached by the caller (build dir), never committed.

Usage:
  python3 tools/make_segy_fixture.py --out testdata/segy/synthetic_4x5.sgy
  python3 tools/make_segy_fixture.py --out /tmp/other.sgy --inlines 8 --xlines 12
  python3 tools/make_segy_fixture.py --out build/seismic_perf/big.sgy --mb 220
"""

import argparse
import struct
import sys


def build(args) -> bytes:
    inlines = args.inlines
    xlines = args.xlines
    samples = args.samples
    dt_us = args.dt
    x0, dx = args.x0, args.dx
    y0, dy = args.y0, args.dy

    out = bytearray()

    # --- textual header: 3200 bytes ASCII, padded with spaces ---
    banner = (
        b"C 1 SYNTHETIC MINIMAL SEG-Y FIXTURE (tools/make_segy_fixture.py)\n"
        b"C 2 inline word @188 == 0 (ordinal indexing), CDP @20 == crossline,\n"
        b"C 3 corners from Source X/Y @72/@76, field record @8 == inline base.\n"
    )
    assert len(banner) <= 3200
    out += banner + b" " * (3200 - len(banner))

    # --- binary header: 400 bytes ---
    bin_hdr = bytearray(400)
    struct.pack_into(">i", bin_hdr, 4, 1001)          # line number (informational)
    struct.pack_into(">h", bin_hdr, 12, xlines)       # bytes 13-14: traces per inline
    struct.pack_into(">h", bin_hdr, 16, dt_us)        # sample interval, microseconds
    struct.pack_into(">h", bin_hdr, 20, samples)      # samples per trace
    struct.pack_into(">h", bin_hdr, 24, 5)            # format code: IEEE fp32
    struct.pack_into(">h", bin_hdr, 300, 0x0100)      # SEG-Y rev 1
    struct.pack_into(">h", bin_hdr, 304, 0)           # no extended headers
    out += bin_hdr

    # --- traces ---
    tracl = 0
    for i in range(inlines):                          # i -> inline base+i
        for j in range(xlines):                       # j -> xline base+j
            tracl += 1
            hdr = bytearray(240)
            struct.pack_into(">i", hdr, 0, tracl)            # TRACL: consecutive
            struct.pack_into(">i", hdr, 8, args.base_inline + i)  # field record = inline
            struct.pack_into(">i", hdr, 20, args.base_xline + j)  # CDP = crossline
            struct.pack_into(">h", hdr, 70, 1)               # coordinate scalar
            struct.pack_into(">i", hdr, 72, int(x0 + j * dx))  # Source X
            struct.pack_into(">i", hdr, 76, int(y0 + i * dy))  # Source Y
            struct.pack_into(">h", hdr, 108, args.delay_ms)   # delay recording time
            struct.pack_into(">h", hdr, 114, samples)         # ns (per trace)
            struct.pack_into(">h", hdr, 116, dt_us)           # dt (per trace)
            # offset 188 stays 0 — the project convention this fixture exists for
            out += hdr

            for k in range(samples):
                # Deterministic, easily recomputed amplitude: distinguishable by
                # (inline, xline, sample) without any float ambiguity.
                value = float((i + 1) * 100 + j) + k / 100.0
                out += struct.pack(">f", value)

    return bytes(out)


def trace_samples_bytes(base_value: float, samples: int) -> bytes:
    """IEEE fp32 big-endian sample block for one trace (value = base + k/100)."""
    try:
        import numpy as np

        block = base_value + np.arange(samples, dtype=np.float32) / np.float32(100.0)
        return block.astype(">f4").tobytes()
    except ImportError:
        import array

        # MSB-first float32 == big-endian on little-endian hosts after byteswap
        vals = array.array("f", [base_value + k / 100.0 for k in range(samples)])
        if vals.itemsize != 4:
            raise SystemExit("unexpected float width: %d" % vals.itemsize)
        import sys

        if sys.byteorder == "little":
            vals.byteswap()
        return vals.tobytes()


def build_big(args) -> bytes:
    """≥ --mb MiB production-shaped volume (same ordinal conventions).

    生产形状默认：1024 样点 × 221 crossline × dt 2ms（966MB 真工区同量级的
    道长/线宽比例）；inlines 由目标体积反推。显式传 --samples/--xlines 可覆盖。
    """
    samples = args.samples if args.samples_given else 1024
    xlines = args.xlines if args.xlines_given else 221
    bytes_per_trace = 240 + samples * 4
    target_bytes = int(args.mb * 1024 * 1024)
    total_traces = (target_bytes + bytes_per_trace - 1) // bytes_per_trace
    inlines = max(1, total_traces // xlines + 1)      # 保证 ≥ 目标体积

    # 每 inline 一块流式写（大体积下避免整卷驻留内存）
    out_path = args.out
    with open(out_path, "wb") as f:
        banner = (
            b"C 1 SYNTHETIC PERF SEG-Y FIXTURE (tools/make_segy_fixture.py --mb)\n"
            b"C 2 inline word @188 == 0 (ordinal indexing), CDP @20 == crossline,\n"
            b"C 3 corners from Source X/Y @72/@76, field record @8 == inline base.\n"
        )
        f.write(banner + b" " * (3200 - len(banner)))
        bin_hdr = bytearray(400)
        struct.pack_into(">i", bin_hdr, 4, 1001)
        struct.pack_into(">h", bin_hdr, 12, xlines)
        struct.pack_into(">h", bin_hdr, 16, args.dt)
        struct.pack_into(">h", bin_hdr, 20, samples)
        struct.pack_into(">h", bin_hdr, 24, 5)
        struct.pack_into(">h", bin_hdr, 300, 0x0100)
        struct.pack_into(">h", bin_hdr, 304, 0)
        f.write(bin_hdr)

        tracl = 0
        # 每 inline 的 xline 采样块只与 (i, j) 有关：逐线预生成本线用不到的
        # 冗余——仍按道写，保持与 KB 级夹具逐字节同构的道头约定。
        for i in range(inlines):
            for j in range(xlines):
                tracl += 1
                hdr = bytearray(240)
                struct.pack_into(">i", hdr, 0, tracl)
                struct.pack_into(">i", hdr, 8, args.base_inline + i)
                struct.pack_into(">i", hdr, 20, args.base_xline + j)
                struct.pack_into(">h", hdr, 70, 1)
                struct.pack_into(">i", hdr, 72, int(args.x0 + j * args.dx))
                struct.pack_into(">i", hdr, 76, int(args.y0 + i * args.dy))
                struct.pack_into(">h", hdr, 108, args.delay_ms)
                struct.pack_into(">h", hdr, 114, samples)
                struct.pack_into(">h", hdr, 116, args.dt)
                f.write(hdr)
                f.write(trace_samples_bytes(float((i + 1) * 100 + j), samples))

    import os

    size = os.path.getsize(out_path)
    expected = 3600 + inlines * xlines * bytes_per_trace
    assert size == expected, (size, expected)
    print(f"wrote {out_path}: {size} bytes ({size / 1048576.0:.1f} MiB), "
          f"inlines {args.base_inline}..{args.base_inline + inlines - 1} ({inlines}), "
          f"xlines {args.base_xline}..{args.base_xline + xlines - 1} ({xlines}), "
          f"samples {samples}")
    return b""  # big path 已直接落盘


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output .sgy path")
    ap.add_argument("--inlines", type=int, default=4)
    ap.add_argument("--xlines", type=int, default=5,
                    help="crosslines per inline (--mb 模式默认 221，生产形状)")
    ap.add_argument("--base-inline", type=int, default=10)
    ap.add_argument("--base-xline", type=int, default=100)
    ap.add_argument("--samples", type=int, default=64,
                    help="samples per trace (--mb 模式默认 1024，生产形状)")
    ap.add_argument("--dt", type=int, default=2000, help="sample interval, microseconds")
    ap.add_argument("--delay-ms", type=int, default=100)
    ap.add_argument("--x0", type=float, default=1000.0)
    ap.add_argument("--dx", type=float, default=25.0)
    ap.add_argument("--y0", type=float, default=5000.0)
    ap.add_argument("--dy", type=float, default=25.0)
    ap.add_argument("--mb", type=float, default=0.0,
                    help="synthesize >= MB MiB production-shaped volume "
                         "(overrides --inlines; keeps ordinal conventions)")
    args = ap.parse_args()
    # --mb 模式的生产形状默认：区分「用户显式传参」与「小夹具默认」
    args.samples_given = "--samples" in sys.argv
    args.xlines_given = "--xlines" in sys.argv

    if args.mb > 0:
        build_big(args)
        return

    data = build(args)
    with open(args.out, "wb") as f:
        f.write(data)

    expected = 3600 + args.inlines * args.xlines * (240 + args.samples * 4)
    assert len(data) == expected, (len(data), expected)
    print(f"wrote {args.out}: {len(data)} bytes, "
          f"inlines {args.base_inline}..{args.base_inline + args.inlines - 1}, "
          f"xlines {args.base_xline}..{args.base_xline + args.xlines - 1}")


if __name__ == "__main__":
    main()
