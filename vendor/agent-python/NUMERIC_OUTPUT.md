# Host-encoded numeric output

This contract belongs to the new typed-output capability. Existing
`python.generated_compute` and its REPORT-only v1 operation/restorer keep their
original contracts. Installing a runtime does not grant either capability.

The host freezes a `paleo.python-table-declaration.v1` before execution:

```json
{"contract":"paleo.python-table-declaration.v1","maxRows":8192,"columns":[{"id":"source_row","label":"Physical source row","unit":"1","dtype":"<i8","role":"source_row"},{"id":"value","label":"Derived value","unit":"m","dtype":"<f8","role":"value"}]}
```

There are 1–16 columns and 1–8192 rows. Column IDs are fixed ASCII identifiers
`[A-Za-z][A-Za-z0-9_]{0,63}`. Labels and units remain bounded metadata; neither is
inserted into CSV headers. The first column is either a positive, strictly
increasing `source_row` integer axis (`<i8`, unit `1`) or an explicit finite
physical `axis` (`<f8`, declared unit). Other columns have role `value`, with
`<f8` or `|b1` values. Free-text value columns and inferred units are unavailable.

## Wire format

The child writes one bounded stdout stream, with no file paths or MIME claims:

1. Ten bytes `PALEOPYO2\n`.
2. A uint32 little-endian header length, 1–16384 bytes.
3. Strict UTF-8 JSON header, rejecting duplicate keys and excess structure.
4. The REPORT bytes, followed immediately by the TABLE bytes, with no tail.

The header has exactly `contract` (`paleo.python-output-frame.v2`) and `ports`.
The array has exactly these two entries in this order:

```json
{"contract":"paleo.python-output-frame.v2","ports":[{"id":"REPORT","encoding":"utf8-json","size":123,"sha256":"64 lowercase hex characters"},{"id":"TABLE","encoding":"paleo.numeric-columns.v1","size":456,"sha256":"64 lowercase hex characters","rows":3,"columns":[]}]}
```

`columns` must equal the host-frozen declaration exactly, including type, role,
unit and label. Both sizes must match the complete remaining stream; SHA-256 is
recomputed over each actual payload. These child-provided hashes check framing
consistency; authorization and code/input/runtime provenance come from the host.
REPORT is at most 256 KiB and uses the existing generated-report **body**
`{contract,metrics,tables,notes}`. It is validated with the operation's explicit
input contract and then bound to host-authenticated provenance. A frame parser
cannot independently approve a report or accept child-supplied provenance.

TABLE is column-major. Each column contains `rows * itemsize` bytes, then
`ceil(rows / 8)` null-mask bytes; the next column follows without padding.
Integers and floating point use little endian, eight bytes each. Booleans are
one byte, exactly zero or one. A set mask bit (`1 << (row % 8)`) means missing;
unused high bits in the last mask byte must be zero. Missing cell bytes must be
all zero, axes cannot be missing, and NaN/Inf are rejected. There is no implicit
alignment, conversion, join, sort or resampling. A one-based integer axis is
kept as int64 throughout, including values above the exact-double range.

The entire frame is at most 12 MiB, additionally bounded by the selected host
execution policy. TABLE payload is at most 2 MiB; the shape-derived exact length
is checked before reading cells or allocating CSV. These limits are ceilings,
not guaranteed simultaneous capacities.

## Accepted artifacts

The host writes CSV with fixed IDs as the header, locale-independent finite
numbers, exact decimal int64, literal `true`/`false` and empty fields for null.
It uses CRLF records and at most 4 MiB. Arbitrary labels, text cells and formulas
are never interpreted as CSV values. Table types, labels, units and missing-value
semantics stay in host-owned artifact metadata. Encoding identity is
`host-csv.numeric.v1`.

All ports must pass before any accepted port is returned. The worker then writes
only to the existing private output set; prepare/compute do not register formal
results. Group publication, receipts, SHA validation, cancellation, retention,
restart reconciliation and authenticated result pagination use the existing
operation and Catalog path. A report or a table alone cannot complete the group.
Generated values retain the claim
`agent_generated_values_not_independently_recomputed`.

This file describes the O1 implementation contract; availability is established
by the batch's actual build and native execution records. It does not enable
Shell, package installation, network, child disk writes or a general file API.
