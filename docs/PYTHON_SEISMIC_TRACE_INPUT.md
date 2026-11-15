# Controlled authenticated seismic trace input

`paleo.python_compute_trace` / `python.seismic_trace_compute` is an independent
REPORT-only capability. It consumes one exact zero-based file trace ordinal and
a window of 1–8192 samples. Public inputs are a full-SHA `SourceRef`, decimal
`volume_id`, exact SDK `volume_revision`, decimal `trace_index`, integer
`first_sample` / `sample_count`, amplitude unit and explicit `ms` TWT/OWT time
type/reference. There is no path, SQL, caller identity, nearest-trace choice,
spatial slice, resampling, MD substitution or whole-volume algorithm.

The current supported layout is SEG-Y rev1, fixed-length traces, big-endian
IEEE32 amplitudes, nonnegative bounded extended-text-header count, and a binary
header/selected trace sampling axis that agrees with the declared SDK sampling.
Other formats or changed/missing semantics fail explicitly. Trace sequence
number, inline and crossline come from the selected authenticated trace header;
they are not inferred grid coordinates or nearest-key matches.

## Source and metadata identity

The SDK dependency is the authenticated seismic-window contract in
`1362a69e61a663aba2b5024bfa83627af0759354` (SDK PR #23, based on #22).
The host resolves the SourceRef to the actual Catalog asset/version and exact
`areaModelSourceId`. The Catalog version must carry a complete SHA-256. A quick
Catalog/SourceRef fingerprint cannot authorize this capability. The SDK source
registration may retain its quick fingerprint as metadata; it is not promoted
to a full-byte proof. A registered full SHA must agree with the host's anchor.
The actual Catalog file source version is the parent; no volume version is
invented from a `volumes` row.

`paleo.query_project` topic `seismic_trace_descriptor` takes exact `source_ref`
and `volume_id`, returning only bounded sampling declarations, revision, observed
metadata ReadCut and an editable exact `input_ref`. It reads no source/array
bytes, makes no integrity or execution-ready claim, and does not silently fill
unknown units or source references. The complete response must fit `max_bytes`
(2048–65536, default 8192); no partially truncated descriptor is returned. The
caller already needs the exact volume ID. Source-file availability/layout and
full/range integrity are checked during preparation, not inferred from this
metadata-only response.

The descriptor revision hashes the relevant stored sampling, amplitude unit,
source ID and registered fingerprint. It is different from a general
`DataObjectDescriptor` revision. The SDK takes an exact model-head savepoint for
the consumed window; the host records the actual cut and verifies the final
head. This observed SQL cut and authenticated source ranges are not a historical
SQL snapshot lease.

## Bounded authentication and frozen bytes

The existing `AuthenticatedArtifactReader` uses a host-only `SeismicSource`
profile: at most 64 GiB source, 1 MiB authentication chunks, 64 KiB returned page
and the existing 4 MiB total digest budget. The artifact paging profile retains
its 1 GiB / 64 KiB chunk / 24 KiB page limits. Neither profile is selectable by a
tool argument.

A cold source is scanned completely once against the Catalog full SHA while
building its bounded chunk index. Later requests authenticate only the consumed
chunks against that index, including equal-length/equal-mtime changes. This is
bounded-memory streaming, not a promise of window-only cold I/O. The reader is
shared by `DocumentSession` throughout one binding; its scope includes document
identity, document epoch, actual source-version ID and caller scope, without
operation or client request IDs. Closing/rebinding invalidates old instances and
in-flight reads. Different sources/scopes retain distinct bounded entries.

Metrics separately record SDK requested source ranges, actual full-hash bytes,
authenticated chunk/read bytes, digest retention and bounded numeric preparation
reservations. Reader byte counters measure bytes returned by `QFile::read`,
including OS-cached reads; they are not hardware block-I/O telemetry or RSS.
The shared execution permit precedes SDK/source payload I/O and allocation.
Static shape/scope/input/memory budgets and impossible ordinal/window bounds
reject early. SDK metadata/range/working budgets remain bounded. Failed reads,
cancellation, revision drift and failed authentication expose no partial numeric
bundle.

The transport is the existing `paleo.python-analysis-frame.v1`: uint32LE JSON
length, at most 64 KiB JSON, then exact binary bytes. Its independent body is
`paleo.python-seismic-trace-input.v1`, shape `[samples, 1]`: little-endian Float64
time-axis values, Float64 amplitudes, then a separate uint8 mask (`1=null`).
IEEE32 finite amplitudes are represented exactly in Float64; nonfinite samples
are explicit nulls with zero payload. No unit conversion/interpolation occurs.
`code.py`, `request.json` and `data.bin` use the existing immutable InputBundle.

## Execution and recovery

The inner frozen plan `paleo.python-seismic-trace-operation.v1` is wrapped in the
managed environment identity envelope. Generated-code permission and a real
strong-backend proof are mandatory; trusted-only permission or an ordinary
interpreter is not a fallback. Runtime integrity verification, process isolation,
budgets, diagnostics and platform unavailable states retain their existing rules.

The shared generated-report worker validates the explicit seismic input contract
before accepting the existing `paleo.python-output.v1` / REPORT
`paleo.python-generated-report.v1`. Host-owned source/ReadCut/code/runtime evidence
is added to the report. Script numerical claims are not independently recomputed
by the product. CSV/image/typed multi-artifact outputs belong to later versions.

Compute-only, authenticated report pages, new-Host recovery, authorized
publication once and revocation use the existing operation/Catalog chain.
Publication of a completed frozen report can proceed under an approved historical
identity without reopening the original source file or rerunning Python. Old
curve-input and numeric-table contracts/restorers remain unchanged.

`tst_pythonseismictrace` and `tst_pythonseismictracewiring` cover the real SDK,
filesystem range proof, model/Catalog binding and metadata/admission boundaries.
`PALEO_TEST_PYTHON_SEISMIC_TRACE_NATIVE=ON` registers
`tst_pythonseismictrace_native`, requiring explicit locked
`PALEO_TEST_AGENT_PYTHON_ROOT` / `PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256`. Missing
fixtures/backend proof fail with no skip. The native business chain is genuine
managed config → SDK/authenticated source → strong CPython/NumPy → bounded pages
→ new-Host historical publication once. Test definitions alone do not establish
execution; the progress record must cite the final coordinated build results.
