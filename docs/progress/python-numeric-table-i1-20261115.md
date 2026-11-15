# I1 numeric table input — implementation checkpoint

This batch adds an independent numeric-table REPORT capability, bounded declared
projection metadata lookup, typed InputBundle materialization and E2 managed
historical identity/recovery wiring. SDK gitlink:
`f329d93d4ed4b9c60fbc6c92b49ea5953950daf7` (strict UTF-8 fix `8ecc6d2b`).
The SDK contract was validated separately; those results do not establish this
main-repository integration's compilation or business execution.

The contract and limitations are recorded in
[`PYTHON_NUMERIC_TABLE_INPUT.md`](../PYTHON_NUMERIC_TABLE_INPUT.md). Original
Generated v1 remains a separate strict contract and restorer. No new CSV/image
output, arbitrary table/SQL/path runner or ordinary-interpreter execution is
added.

The three regular suites and explicit opt-in native suite are implemented.
The native suite uses actual Catalog mappings, a managed generated-only runtime,
real SDK Float64/Boolean/null input, strong CPython/NumPy, bounded report pages,
new-Host historical publication once and old Generated v1 recovery. Its reusable
fixture supports the later output batches. Required fixture/backend absence is
a failure, never a skip or a synthetic success.

Compilation, both complete application configurations, native positive business
execution, old-contract regressions and final rebased source gates are **pending
the coordinated build queue**. File-budget and strict layering early checks
passed on the implementation snapshot; they do not replace the final gates.
This checkpoint does not mark I1 delivered or platform support expanded.
