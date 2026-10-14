# Acceptance task matrix

IMPLEMENTED is not DONE. DONE requires meaningful tests and independent review.

| Task | Owner | State | Acceptance |
|---|---|---|---|
| T0 source/PR/architecture audit | root + read-only audits | DONE | receipts in baseline.md; build validation tracked separately |
| T1 compatible semantic records/states/events/quality | root | TESTED | old v1, malformed, duplicate, conflict, revision tests |
| T2 pipeline propagation and registration rollback | workflow agent | IMPLEMENTED | actual inputs/priorities/params, factor evidence, no orphan |
| T3 stable feature identity and typed selection | linkage agent | IN_PROGRESS | export/reload/split/merge, single hub compatibility, non-edit locate |
| T4 persistent manual review/geometry changes | root | IN_PROGRESS | append history, local operator, stale revision rejection, undo/save |
| T5 local evidence graph + three-way UI integration | root | IN_PROGRESS | source/evidence/map navigation, bounded/cancelled queries |
| T6 publish immutable snapshot/integrity | workflow agent + root sequential | IMPLEMENTED | save binding, missing sources, published old evidence unchanged |
| T7 full integration/failure/performance fixtures | root | TODO | 10k/100k real measurements, no asserted unmeasured timings |
| T8 independent read-only actual diff review | audit_model agent | IN_PROGRESS | P0/P1 fixed/reviewed and P2 addressed |
| T9 final validation/docs/example/push/PR | root | TODO | local checks, no CI wait, attach PR, no merge |
