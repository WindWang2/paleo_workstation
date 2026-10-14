# Evidence mapping baseline — 2026-10-13

Base origin/master: `8e963cd7882db6337121547574fd328549600d79`.
Remote: WindWang2/paleo_workstation. Primary checkout clean; open PR list empty.
Task worktree: `/home/kevin/.codex/worktrees/evidence-mapping-integration/paleo_workstation`.
Branch: `codex/evidence-mapping-integration`. No edits in master.

# Audit matrix

All classifications below are source audit results, not passed-test claims.

| Capability | Classification | Receipt/function | Behavior and gap |
|---|---|---|---|
| Generation and storage | Partial | domain/predictionevidence.cpp:63 build; catalog/catalogstore_crud.cpp:115 | v1 inputs/model/params/support persisted in extra_json; no semantic evidence identity |
| Legacy reads | Partial | predictionevidence.cpp:126 summary; services/datagraph.cpp:378 | absent evidence safe; exact v1 consumer requires compatible extension; malformed data needs explicit diagnostics |
| Source versions | Partial | workflow/evidencerecord.cpp:18 | missing parent silently omitted; record unresolved source instead |
| Feature evidence | Partial | workflow/mappinghierarchy.cpp:185,208,275 | existing facies_evidence UUID/source_version/category and QGIS undo; FID temporary, unlink physically erases record |
| Layer evidence | Partial | mappingworkbench.cpp:572 record | generation chain bound to catalog/manifest layer, no typed feature navigation |
| Manual revision | Partial | mappingworkbench_edit.cpp:183 saveEditingVersion | immutable GPKG copies; previous.extra carries stale generation evidence into new version |
| Prediction | Partial | mappingworkbench_predict.cpp:763 finishPrediction | actual/skipped wells/models/log versions retained; no per-feature evidence or reverse well query |
| Async prediction | Implemented | mappingworkbench_predict.cpp:472,489,714 | request ID/late reply/cancel/all-skipped guards; local validation pending |
| Single factor | Partial | constraintwellfactors.cpp:743; constraintfactorjobs_factor.cpp:284 | #430 source parents/QC present; registrar lacks automatic v1 evidence |
| Quantitative gate | Partial | singlefactorrequest.h:35; mappingworkbench_compose.cpp:100 | cartographic gate present; legacy CompositionWorkflow metadata aliases need same gate |
| Compose priority | Implemented foundation | mappingworkbench_compose.cpp:78,199; qgis/mappingartifactwriter.cpp:552 | ids ordered first-valid and input_layers saved; explicit versions/priorities/all params still required |
| Constraints | Partial | mappingworkbench_edit.cpp:431 snapshotConstraints | per-horizon geometry snapshots; actor/history and failed import rollback gaps |
| Lineage/impact | Partial | catalog/datacatalog_derivation.cpp:15; services/derivationgraph.* | indexed downstream closure/stale flags; cannot claim certain spatial effect from all downstream |
| Graph | Implemented foundation | services/datagraph.cpp; workflow/datagraphworkflow.* | bounded async index/query/cancel/cache; geological evidence relationships absent |
| Selection | Partial | linkage/selectioncontext.* | single broadcast centre for wells/seismic/fault IDs; no typed stable feature/version context |
| Save/publish | Partial | mapversioncontroller.cpp:86,177; metadata/mapversionstore.cpp:354,430 | saved map/PDF/residual gates and readonly copies; evidence snapshot/integrity binding incomplete |
| History protection | Partial | catalog/datacatalog_versions.cpp:125,245 | updateVersionExtra can rewrite evidence; immutable generation provenance guard needed |
| Failure rollback | Partial | workflow/derivedassets.cpp:170,183,209 | asset created before fingerprint failure; zero-version orphan possible |
| Cross-layer location/lifecycle | Further verification | qgis services/app/workbench wiring | layer IDs exist; UID locate/highlight and typed scope/session recovery must be integrated |
| Tests/performance | Further verification | tst_predictionevidence, mappingworkbench, derivedassets, datagraph, versions | real regression fixtures exist; initial build running, no performance claim yet |

# Recent merges and issues

#424 d0eb5581: DataGraph/DerivationGraph/background work and workspace entry points.
#425 a43c9fbd: safety guards and failures; verify source, do not reopen unrelated issue sweep.
#430 1d0f6999: factor source versions, preparation UI, mapping evidence propagation.
#431 8e963cd7 and #429: tests aligned with changed contracts.
Existing issues reviewed by number/title only to avoid duplicate broad repair. No assignment, comment or closure.

# Environment

Vendored QGIS superbuild prefix shared read-only; independent Debug Ninja build.
Max build/test concurrency 4; one large command at a time. Baseline logs `/tmp/paleo-evidence-baseline-build.log`.
62 GiB RAM; 495 GiB available disk at start. gstack v1.91.95.0 installed.
Native create_goal started the real active goal. No installed goal-loop/matt/planning-with-files SKILL.md found in enumerated Codex/Claude skill roots; no assumed slash command.
