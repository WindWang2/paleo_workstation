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

The output is a few KB — small enough to commit under testdata/ (the real
volume is 966 MB and must never enter the repository).

Usage:
  python3 tools/make_segy_fixture.py --out testdata/segy/synthetic_4x5.sgy
  python3 tools/make_segy_fixture.py --out /tmp/other.sgy --inlines 8 --xlines 12
"""

import argparse
import struct


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


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output .sgy path")
    ap.add_argument("--inlines", type=int, default=4)
    ap.add_argument("--xlines", type=int, default=5)
    ap.add_argument("--base-inline", type=int, default=10)
    ap.add_argument("--base-xline", type=int, default=100)
    ap.add_argument("--samples", type=int, default=64)
    ap.add_argument("--dt", type=int, default=2000, help="sample interval, microseconds")
    ap.add_argument("--delay-ms", type=int, default=100)
    ap.add_argument("--x0", type=float, default=1000.0)
    ap.add_argument("--dx", type=float, default=25.0)
    ap.add_argument("--y0", type=float, default=5000.0)
    ap.add_argument("--dy", type=float, default=25.0)
    args = ap.parse_args()

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
