# Agent E
Baseline: 42296fd3
Scope: src/workflow, src/linkage (agent-host filenames skipped: agent, aichat, llm, operation, capability, planexec, piagent, toolhost, python sandbox)
Read in full: linkage (seismicmaplink, wellmaplink, selectioncontext, threewaylocator, unitselectionbridge); wellsectionexport; stratigraphyworkflow save/activateClassification and stratigraphyworkflow_edit; unitonnxcontract::prepareUnitOnnxPlan; mappingexplanationservice worker; mappingeditsave; batchjobqueue resume/readState/pump; folderimport runImportJob/importFile; horizon batch step/publish; mapexport/geologicalexport publish gates; constraint local-direction/geostat/contour publish and runFactorWork Pending.
Pattern-scanned: QtConcurrent/worker lambdas vs QPointer; sync publish without ~Pending cleanup; resume() after a bad state file; cancel detected by a message string; export write() size vs success; full-queue drop; linkage horizon/pick writes vs generation.

## Findings

### E-01
- Sev: P1
- Conf: VERIFIED
- Loc: src/workflow/stratigraphyworkflow.cpp:282
- Title: Applying well tops replaces project_area.json on a short write and still reports success
- Bug: `activateClassification` rewrites the whole `project_area.json` (every key already in the object, plus `sequence_boundaries` / `target_horizon`). `QSaveFile::write` is accepted when it returns any non-negative count, including a short write. `commit()` then replaces the previous file. The status line says the tops were applied to the project. A later `AreaRules::loadFromProjectDir` fails the JSON parse, and the previous configuration is gone.
- Evidence: `output.write(QJsonDocument(configuration).toJson())<0` is the only size check (`stratigraphyworkflow.cpp:282`); success is `statusChanged(tr("已将所选井的地层界面应用到当前工程…"))` at line 284. Caller: `StratigraphyPage` button `activateStratigraphy` calls `m_workflow->activateClassification` (`src/ui/pages/stratigraphypage_controls.cpp:79`). `configFilePath` is `project_area.json` (`src/domain/arearules.cpp:21`, `:370`). Sibling writers in this slice require `write()==size` before `commit()`.
- Not excluded because: persisted project configuration loss with a success status, not an i18n or style nit.
- Trigger: User clicks “应用分层到工程” while the save file cannot accept the full JSON (disk full / short write).

### E-02
- Sev: P2
- Conf: VERIFIED
- Loc: src/workflow/unitonnxcontract.cpp:101
- Title: Cancelling a unit seismic-window read is stored as seismic_source_unavailable
- Bug: A cancel inside `DataService::readSeismicHorizonWindow` sets the error to `地震层位窗口读取已取消` and returns false. `prepareUnitOnnxPlan` maps that text with an exact equality check for `已取消` only, so the code becomes `seismic_source_unavailable`. The capability prepare worker returns that code and never reaches the later `cancelled()` check. The host treats only the code `cancelled` as a cancel, so the user cancel is a failed prepare.
- Evidence: Matcher is `message == QLatin1String("已取消")` (`unitonnxcontract.cpp:101`); any other text falls through to `seismic_source_unavailable` (`:106`). Call site returns immediately (`:274`). Producer: `readSeismicHorizonWindow` sets `error = "地震层位窗口读取已取消"` and returns false (`vendor/Data_service/src/adapters/seismic_horizon_window.cpp:264-267`); `readSeismicAnalysisWindow` copies `detail` into the input error (`src/services/seismicunitinput.cpp:37`, `:53`). Caller `unitonnxservice.cpp:167` passes that `workerError` out of the prepare worker. The in-loop cancel inside `prepareSeismicUnitInput` (`seismicunitinput.cpp:77`) does use the exact token `已取消`, so only the window-read cancel is mis-labeled.
- Not excluded because: wrong failure code on a cancel the user can request; unitonnxservice is not an agent-host filename.
- Trigger: Cancel a unit ONNX prepare (or any `prepareUnitOnnxPlan` call) while the seismic horizon window is still being read.

### E-03
- Sev: P2
- Conf: HIGH
- Loc: src/workflow/inversionworkflow.cpp:414
- Title: A wavelet JSON that fails to flush is still committed as a version
- Bug: `publishWaveletJob` checks `write()==size`, then `close()`s without `flush()` or `error()`, then `registrar.commit`s the staging path. `commit` only requires the file to exist and hashes whatever bytes are on disk. A buffered write that did not reach disk becomes a catalogued wavelet version. The function returns that version id.
- Evidence: `inversionworkflow.cpp:414-418` then `commit` at `:427`. `DerivedAssetRegistrar::commit` (`derivedassets.cpp:114-140`) does not compare file size to the buffer that was written. The same workflow already documents the hazard for property volumes: flush must be recorded before `close()` because `close()` clears `error()` (`propertymodelworkflow.cpp:999-1002`). Wavelet publish does not do that.
- Not excluded because: a truncated wavelet is a wrong persisted result, and the caller receives a version id.
- Trigger: Publish a Wiener wavelet when the staging write fits in the QFile buffer but the flush at close fails.

### E-04
- Sev: P2
- Conf: HIGH
- Loc: src/workflow/evolutionworkflow.cpp:415
- Title: Evolution metrics CSV is registered after an unchecked flush
- Bug: The metrics table is written with `QFile::write`, the size check is only on that return, and the `QFile` is destroyed (close/flush) before `commitExternal`. `commit` hashes the on-disk file and does not require it to match `bytes.size()`. On success the function returns true and emits `pairAnalyzed`, so a short metrics file is a published version.
- Evidence: write at `evolutionworkflow.cpp:415-427`; the `QFile` ends at `:428`; `commitExternal` of `metricsStaging.absolutePath` at `:486` and `return true` at `:501`. Same `commit` size gap as E-03 (`derivedassets.cpp:114-140`).
- Not excluded because: the metrics table is a persisted pair-analysis product, and the API return is success.
- Trigger: Run an evolution pair compare when the CSV flush fails after `write()` already returned the full buffer size.

### E-05
- Sev: P3
- Conf: HIGH
- Loc: src/workflow/mappingexplanationservice.cpp:156
- Title: Auto-explanation queue drops the oldest request and still reports it queued
- Bug: While one AI explanation is running, further requests are capped at 32. The worker `pop_front`s the oldest without emitting `completed` / `explanationReady`. `requestBackgroundExplanation` has already returned true, and `scheduleAutoExplanation` does not surface a drop. That feature never receives a draft.
- Evidence: `kMaxQueuedRequests = 32` (`mappingexplanationservice.cpp:14`); drop loop `:156-158`. Caller `MappingWorkbench::requestBackgroundExplanation` returns true after `enqueue` (`mappingeditcontext.cpp:331-332`). `enqueue` posts to the worker with a queued connection (`mappingexplanationservice.cpp:410-413`), so the bool is not the worker’s accept result. Coalesce replaces the same layer+feature key (`:147-154`); a different feature is what gets dropped.
- Not excluded because: a requested explanation draft is silently discarded on a path the mapping workbench calls.
- Trigger: Generate explanations for 33 different features while one AI call is still in `m_running`.

### E-06
- Sev: P3
- Conf: HIGH
- Loc: src/workflow/mappingeditcontext.cpp:197
- Title: Auto-explanation sidecar write failure is ignored and the draft is shown as ready
- Bug: `saveDraftExplanations` returns void. If `QSaveFile` open/write fails, or `commit()` fails, nothing is reported. `onBackgroundExplanationReady` still stores the in-memory record and emits `autoExplanationReady`. `QSaveFile` does not replace the previous sidecar until `commit` succeeds, so the new draft is not on disk. Restart reloads the old sidecar (or none).
- Evidence: `if (out.open(...) && out.write(bytes) == bytes.size()) out.commit();` with the commit result ignored (`mappingeditcontext.cpp:197-198`). Caller `onBackgroundExplanationReady` (`:341-358`) emits `autoExplanationReady` unconditionally after `saveDraftExplanations`.
- Not excluded because: the workbench presents the auto-interpretation draft as generated while the only durable copy was not written.
- Trigger: Background explanation finishes when `artifacts` cannot accept the sidecar (permissions or a full volume).

### E-07
- Sev: P4
- Conf: HIGH
- Loc: src/workflow/batchjobqueue.cpp:707
- Title: resume() reruns a corrupt batch from scratch and returns success
- Bug: If a state file exists but `readState` fails (damaged JSON or `state_version` mismatch), `resume` substitutes a fresh expand, logs `qWarning` only, clears `*error`, and returns true. Completed items are not skipped, so a non-idempotent executor creates another set of results. The caller cannot tell resume failed.
- Evidence: `hadStateFile` / `readErr` at `batchjobqueue.cpp:700-708`; `error->clear(); return true` at `:779-780`. `readState` sets `批次状态文件损坏` / `批次状态版本不兼容` and returns false (`:597-608`). `start()` is `return resume(...)` (`:659-661`). Production caller is empty: `BatchQueue` is constructed from tests and `BatchJobPanel` only observes an existing queue; `AppContext` does not start one.
- Not excluded because: the resume contract tells the caller success while the batch is a full rerun. Severity is P4 because that caller is empty outside tests.
- Trigger: Call `BatchQueue::resume` / `start` with a batch id whose state file is truncated or has the wrong `state_version`.

## Covered and clean
- Linkage (`seismicmaplink`, `wellmaplink`, `selectioncontext`, `threewaylocator`, `unitselectionbridge`): selection and section-request signals only. No horizon or well-pick write on the GUI thread, and no raw worker capture of a layer. `sectionExtractedFromMap(true)` means the polyline was accepted; the section dock extracts on `sectionExtractRequested`. `WellMapLink`’s `QTimer::singleShot` uses `this` as the receiver.
- `folderimport.cpp`: cancel is a bool (`ImportDone` third argument / `session->cancelled`), not a translated string compare. `#801` is in `projectsetup.cpp:374-377`. `failSession` clears `fileResult.assetId` before the single-file done callback, so a failed commit is not reported as an asset id.
- `horizonbatchexport.cpp`, `mapexport.cpp`, `geologicalexport.cpp`, `wellsectionexport.cpp`: metadata and section CSV use `QSaveFile` with `write()==size` and `commit()`, and a failed publish calls `failPublication` rather than leaving `ok` set. World-file `.prj` writes in layout export also check size (qgis, read only to confirm the workflow gate).
- `runFactorWork` sync path calls `cleanup()` when publish returns false (`constraintfactorjobs_factor.cpp:1223-1228`); the async path uses `~Pending`. Local-direction staging failure calls `cleanupEngineTemp` before return. Contour `publishAnalysisContourJob` removes the staged gpkg when `publishArtifacts` fails.
- `mappingeditsave.cpp`: a corrupt edit-save journal returns an error and does not fall through into `registerEditingVersion`.
- `stratigraphyworkflow_edit.cpp`: `canEdit()` is false while `d->task` is set, so boundary edits do not race the compute task. The finished handler also checks `generation`.
- `copyForEditing` / `createBlankMap`: `record()` aborts the catalog transaction and deletes the managed copy when declare/flush fails. Blank-map declare failure also removes the working directory.

## Not filed
- Geostat `stageGeostatOutputs` returns false without deleting `paleo-gs-*` or partial staging copies (`constraintfactorjobs_geostat.cpp:1226-1255`, `:1518`, `:1597`). The async `~Pending` cleanup removes them. The sync fallback runs only when `m_factorTasks` is null; `AppContext` always `setTaskService`s the constraint workflow (`src/app/appcontext.cpp:321`). No production caller for that branch.
- `BatchQueue` commit lambdas capture raw `this`. No production owner destroys a running queue.
- `io/dataimport_file.cpp:287` treats errors that start with `已取消` as cancel. Outside this slice; folder import itself uses the structured flag.
- Explanation queue overflow beyond the six items above: none. The geostat sync leak is the only extra candidate and is excluded for lack of a production caller.
