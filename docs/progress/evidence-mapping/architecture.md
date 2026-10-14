# Evidence mapping architecture and engineering decisions

Base: 8e963cd7882db6337121547574fd328549600d79. Target: the user's full evidence/mapping integration goal.
Authorization: user explicitly requires automatic reasonable technical decisions, complete local tests, independent review, one worktree and one PR. No feature scope cuts or extra approval gates.

## Existing foundations

PredictionEvidence v1, facies_evidence GPKG field, MappingWorkbench native undo/edit copies, Catalog SQLite/BatchSave, MapVersionStore, DataGraph and DerivationGraph, SelectionContext and QGIS layer services remain authoritative. New code extends these existing boundaries. No additional database, global selection hub, external graph or GUI framework.

## Contracts

- Generated provenance keeps schema=paleo_evidence_v1, original fields and deterministic parameter fingerprints. Extension records have stable source identities. Missing inputs remain explicit.
- Evidence observations, geological assertions and interpretations have distinct identities. Roles distinguish support, contradiction, constraint and reference from actual computational parents. Multiple roles for one version remain visible.
- facies_evidence stores active associations and unlink tombstones. paleo_interpretation stores identity, status, revision, local actor, reason and append events; data verification is separate from interpretation acceptance.
- New outputs/drafts carry paleo_feature_uid, paleo_parent_uids and geometry digests. Existing historical files are never updated to add identity. Export changes FID without changing business references.
- Geometry changes preserve UID and require review; native split/merge creates new identities and parent lineage. Exact union may establish merge identity; uncertainty is explicit. Undo/redo replays the complete native command without adding audit events.
- SelectionContext owns the primary typed target. Existing naked well IDs stay compatible. Source highlighting uses independent QGIS rubber bands and does not change current edit layer.
- Local graph query receives immutable feature evidence values; this payload, version, horizon, project session and interpretation revision participate in cache equality. Shared catalog graph indexes never contain every map feature.
- Map saving freezes vector view exports, rasters, evidence and PDF at the save boundary; publishing reads those frozen files and verifies SHA. Catalog/file failures remove staging and restore state. Published provenance is immutable.

## Data flow

QGIS native select -> SelectionContext target -> MappingWorkbench featureEvidence values
                                               -> shared three-stage evidence panel
                                               -> DataGraph Query local overlay (bounded async)
Evidence/source or graph node intent -> EvidenceInteraction -> QGIS adapter highlight/locate
Manual edit/review -> domain validation/revision/event -> native QGIS edit buffer -> immutable new version
New derived input -> registrar stage/commit -> v1 provenance + catalog parents -> downstream graph/quality
Saved map -> frozen files + evidence manifest + SHA -> publish verifies frozen bytes

## Ownership / sequence

Root: domain semantic model, catalog provenance immutability, mapping review operations, UI and app wiring, local graph, integration tests and final builds.
Workflow agent: registrar, pipeline propagation, compose priorities, constraints, save/publish and corresponding tests.
Linkage agent: typed selection, feature identity/native observation, QGIS adapters and corresponding tests.
Read-only model audit agent: independent actual-diff review; never implements reviewed changes.
Only root runs compilation/CTest, max -j4. Shared interfaces are root-coordinated.

## Verification and failure modes

Corrupt JSON -> reject mutation, retain bytes. Missing source -> explicit Blocking finding; old absence -> honest early-version state.
Stale revision -> reject all targets before buffer mutation. Cross-project/session/horizon -> refuse navigation.
Deleted/rebuilt layer -> QPointer and generation guard; restore through stable IDs.
Rollback -> native undo boundary, preserve already committed but not yet versioned history.
Fingerprint/SHA/SQL failure -> no zero-version asset or published staging artifact.
Conflict -> preserve source and dispute status; warnings do not become unconditional publication blockers.

## Review status and scope

Domain/core four-test initial run passed; full integration still IN_PROGRESS. Earlier initial mixed-source build had two transient failures and is not claimed as a pristine master baseline.
Independent QGIS review found five P1 and two P2 issues in native macro/rollback/identity/history/cache paths. Fixes and real regression tests are in progress; no clean-review credit yet.
Source evidence localized by explicit relation or spatial facts; participation alone never implies a certain polygon influence area. Source extent navigation remains extent-level, not pixel evidence generation.
Large map freeze I/O is synchronous in the existing save API and must be measured; no unmeasured latency guarantee.

## Binding decisions

Retain complete requested scope and existing workflow entrances; build/QA/review sequential after disjoint implementation.
One PR, no merge, no remote CI wait. No existing issue assignment/closure.
Local actor is explicitly local and has no account authentication claim. No fabricated reliability scores.
