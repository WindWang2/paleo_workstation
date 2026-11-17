# Host PNG codec for output frame v3

This is the bounded data codec used by the separate
[O2 capability](../../docs/PYTHON_GENERATED_OUTPUTS.md). Platform execution
acceptance is recorded independently; registration alone does not certify that
an untested platform can run the profile. O1 frame v2 and its validator retain
their original semantics.

The host freezes a table declaration, an image declaration, an encoder identity
and the output policy before execution. The image declaration has exactly these
fields:

```json
{"contract":"paleo.python-image-declaration.v1","width":3,"height":2,"format":"rgba8"}
```

Both dimensions are integers in 1..1024. Pixels are row-major, tightly packed
straight-alpha R,G,B,A bytes. This is a nonspatial report image; it declares no
coordinate system, geographic extent or accepted geological interpretation.

The stdout frame is `PALEOPYO3\n` (ten bytes), a uint32 little-endian header length,
1..16384 UTF-8 JSON header bytes, then the three exact payload segments in order:

```json
{"contract":"paleo.python-output-frame.v3","ports":[
  {"id":"REPORT","encoding":"utf8-json","size":123,"sha256":"<lowercase SHA-256>"},
  {"id":"TABLE","encoding":"paleo.numeric-columns.v1","size":34,"sha256":"<SHA>","rows":2,"columns":["<frozen table columns>"]},
  {"id":"IMAGE","encoding":"rgba8","size":24,"sha256":"<SHA>","width":3,"height":2}
]}
```

The column array above is illustrative; real entries use the unchanged O1 column
objects. REPORT is at most 256 KiB, TABLE at most 2 MiB, and IMAGE must have exactly
`width * height * 4` bytes. All sizes and dimensions are checked before the image
is created. The total frame obeys both the 12-MiB protocol ceiling and the frozen
process output budget. Extra/trailing bytes, repeated or reordered ports,
duplicate JSON fields, substituted shapes/types and bad hashes fail the group.
Child paths, filenames, MIME claims, PNG/SVG/HTML/PDF or other encoded images are
not accepted. The child performs no file I/O for delivery.

`validateImageOutput` authenticates every segment and then reuses the O1 numeric
validator, including the caller's actual frozen REPORT validator. It repacks only
the REPORT/TABLE subset as a bounded v2 frame; it does not broaden v2 acceptance.
The host wraps the authenticated RGBA data with the const-data QImage overload,
making a bounded alignment copy only when the variable-length frame places it
at an unaligned address. QImageWriter encodes PNG into a QBuffer whose writes and
seeks reject growth beyond 4 MiB or the remaining frozen group output budget,
whichever is smaller. Both the raw frame and the final REPORT/CSV/PNG byte sum
obey the 12-MiB/frozen policy ceiling. No decoder is used in production. Maximum image
dimensions do not guarantee that every incompressible image fits this PNG limit.

`pngEncoderIdentity()` records the host encoder contract, Qt compile/runtime
versions, pixel format and fixed encoder settings. A compute/resume request with
a different frozen encoder identity fails instead of silently re-encoding with
new settings. Publication of an already authenticated output set consumes the
stored PNG; it must not require the current encoder or rerun the original code.
The current Qt PNG compression option is `100` on Qt's 0..100 scale. This maps
to libpng level 9; passing `9` to Qt instead maps to level 0 in the
[pinned Qt 6.11.2 handler](https://github.com/qt/qtbase/blob/v6.11.2/src/gui/image/qpnghandler.cpp#L953).
The former reviewed `9` identity remains valid for publication of stored bytes;
compute/resume rejects it as encoder drift. Neither setting relaxes the PNG cap.
Image metadata records the frozen declaration, raw RGBA SHA and encoder identity.
The worker also adds table/image declarations and encoder provenance to its
host-authored REPORT and rechecks the final 256-KiB REPORT limit.

The codec reserves host memory separately from child RSS: retained v3 frames,
v2 repack/numeric copies, CSV, REPORT DOM/identity repacking, RGBA alignment/image
conversion copies, PNG buffer/final bytes, conservative Qt/libpng scratch and
small metadata. `imageOutputHostReservation()` can reserve against the 12-MiB
frame ceiling before starting the child. It is a conservative staging budget,
not a measured or enforced limit on the entire application's RSS. The codec
rechecks this reservation against the frozen policy using the captured frame.

Any malformed port, cancellation, budget failure or encoder error returns empty
REPORT, CSV, PNG, metadata and summary together. Only complete successful groups
may reach the existing private output set and Catalog publication chain. Pixel
and numeric values remain agent generated and are not independently recomputed
by the host.

The QtCore/Test/Gui fixture `tst_pythonimageoutput` covers actual PNG encoding and
pixel round trips, typed CSV/REPORT reuse, malformed framing/ports/hashes,
dimensions/overflow rejection before business validation, encoder drift,
host/output budgets, cancellation and a real incompressible PNG limit failure.
