# I2 authenticated seismic trace — implementation checkpoint

This batch builds on I1's independent input/report validation and managed
operation identity wrapper. SDK gitlink:
`1362a69e61a663aba2b5024bfa83627af0759354` (SDK PR #23; SDK author separately
validated 11 suites and five source checks). Those SDK results do not establish
the main-repository glue's compilation, actual range-I/O measurements or native
strong execution.

The implementation adds `paleo.python_compute_trace` with an independent seismic
input/operation contract, exact source/volume/ordinal/window references, explicit
time/amplitude semantics, a document-bound reusable authenticated range reader,
bounded metadata point query and shared REPORT/recovery/publication pipeline.
The artifact reader's original profile and old generated/numeric-table contracts
remain intact. Detailed limits and cold/full versus warm/range measurements are
in [`PYTHON_SEISMIC_TRACE_INPUT.md`](../PYTHON_SEISMIC_TRACE_INPUT.md).

Two regular suites and one explicit opt-in native business suite are implemented.
They include real SEG-Y/SDK reads, warm-window proof counters, explicit null and
trace-number-zero semantics, static budgets, unsupported layouts, changed bytes,
no partial bundle, exact formal source mapping, document rebind invalidation,
metadata-only queries and strong managed compute/pages/restart publication.
The native suite requires actual approved runtime/backend proof and never skips
missing execution fixtures.

Compilation, complete application configurations, measured runtime results,
native business acceptance and final rebased source gates are **pending the
coordinated build queue**. Early strict file-budget/layering/i18n checks passed;
they are not final acceptance. I2 is not marked delivered at this checkpoint.
