# Controlled numeric table input

`paleo.python_compute_table` / `python.numeric_table_compute` consumes one
operator-confirmed SDK `numeric_table.v1` projection. It accepts an exact
`SourceRef`, decimal `table_id`, `projection_revision`, ordered `column_ids`,
decimal zero-based `first_row` within the projection and `row_count` (0–8192).
There are at most 15 value columns plus the physical row axis. Paths, SQL,
runtime roots, inferred types/units and caller-supplied Catalog parent versions
are not accepted. A projection declaration is an explicit SDK write/CAS operation;
this read path never upgrades or migrates the model.

`paleo.query_project` topic `numeric_table_projection`, exact `source_ref` and
`table_id` returns the declared types, units, null rules, range, revision and a
ready-to-select `input_ref`. It is a bounded metadata point lookup: no numeric
arrays, automatic declaration or name fallback. The entire reply must fit
`max_bytes` (2048–65536, default 8192); an oversized reply fails without a partial
projection. The caller must already know the exact source/table ID.

The host resolves the file source to an actual Catalog asset/version with a full
SHA-256 and matching `areaModelSourceId`. A source table is not itself a Catalog
version: the actual file source version is the parent. Missing mapping, a quick
fingerprint or an inactive/different SDK source is rejected. The parsed model
can remain usable when the original file is offline; these reads authenticate
the stored source identity, observed SDK read cut and frozen bytes, without
claiming a fresh full scan of that offline file.

## Input and report contracts

The transport remains `paleo.python-analysis-frame.v1`: a four-byte little-endian
unsigned JSON length, at most 64 KiB JSON, then exact binary payload bytes. Its
body contract is independently versioned as
`paleo.python-numeric-table-input.v1`; it is not the old curve-input contract.
`shape = [rows, valueColumns]` excludes the separate axis.

Payload order is the signed little-endian int64 physical source row axis, then
each selected column's contiguous values and its separate uint8 null mask.
Float64 uses `<f8`; Boolean uses `u1` with values 0/1. A mask value of 1 means
null, whose value bytes are zero. All offsets and byte lengths are declared.
Physical row numbers never pass through a floating-point conversion. Units and
null rules come from the confirmed projection; no MD axis, alignment, join,
resampling or unit conversion is inferred.

The SDK read cut records the actual model identity/generation/head, table,
projection revision and source SHA observed in a SQLite read transaction. Its
boundary is `observed_sqlite_read_snapshot_not_history_lease`: it cannot reopen
historical SQL arrays. `code.py`, `request.json` and `data.bin` are frozen in the
existing immutable InputBundle; their authenticated bytes support later
execution and publication. Read metrics distinguish delivered SDK text/typed
bytes and reserved host buffers from physical disk I/O or process RSS. Static
limits and the global execution permit precede payload allocation/SDK reads;
SDK reads use a combined 8 MiB metadata/cell-text budget and bounded fields.
No failed read yields a partial numeric bundle.

Only the existing REPORT output envelope is accepted: `paleo.python-output.v1`
with `REPORT` body `paleo.python-generated-report.v1`. Source/ReadCut/code/runtime
provenance is added by the host. The script's numerical claims are not separately
recomputed by the product. CSV, PNG and multi-artifact outputs are later batches.

## Environment, recovery and validation

This capability requires generated-code permission and a real strong-backend
proof. Trusted-only permission is insufficient. Production wiring uses the
managed environment registry and freezes its historical identity envelope around
`paleo.python-numeric-table-operation.v1`. It uses integrity-only runtime
verification followed by the existing strong backend; no ordinary interpreter
or trusted-template fallback is introduced. Existing platform/resource limits
and unavailable states remain in force.

Prepare, compute-only, private authenticated report pages, diagnostics and
separate authorized publication use the existing TaskService/operation/Catalog
chain. A new Host can publish a completed report under its approved historical
environment identity without rereading the live projection or rerunning Python;
revocation still blocks new publication. Original Generated v1 plans keep their
own strict input/report/restorer contracts and the pre-environment-envelope
compatibility path.

`tst_pythonnumerictable`, `tst_pythonnumerictablewiring` and
`tst_pythonnumerictableoperations` exercise bounded SDK input, source mappings,
query and managed-admission boundaries without an executable fixture. Configure
`PALEO_TEST_PYTHON_NUMERIC_TABLE_NATIVE=ON` to register
`tst_pythonnumerictable_native`; it requires explicit
`PALEO_TEST_AGENT_PYTHON_ROOT` and `PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256` at run
time. Missing fixtures/backend proofs fail, with no skips. Its real business
paths cover managed SDK-table compute/pages, historical-policy Host restart,
publication once/revocation and old Generated v1 recovery. A source-only test
definition is not an execution result; final evidence belongs in the batch's
progress record.
