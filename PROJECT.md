# Project: paleo_workstation Core P0 & P1 Issue Resolution (#25, #26, #27, #28)

## Architecture Overview
The authoritative architecture plan is `docs/PALEO_QGIS_PLAN.md` and `docs/UI_LAYER_PLAN.md`.
The codebase strictly adheres to 5 layer categories across 13 modules:
- **数据 (Data)**: `src/domain`, `src/catalog`, `src/io`, `src/metadata`, `src/services`, `src/algorithms`
- **功能 (Functional)**: `src/workflow`, `src/linkage`, `src/ai`
- **QGIS 封装 (QGIS Encapsulation)**: `src/qgis`
- **视图 (View)**: `src/ui/**`
- **组装根 (Assembly Root)**: `src/app`
- **测试壳 (Test Shell)**: `src/selfcheck`

Strict guardrails:
- `tools/check_layering.py --strict` enforces header comments (`// 层：...`), forbidden includes, and whitelist compliance.
- `tools/layering-baseline.txt` enforces zero legacy violations.
- CMake/CTest tests verify functionality.

---

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | R1: CONC-01 & CONC-02 Fixes | Worker UAF on dock closure in `SeismicSectionDockWidget` & RAII release of `slotSemaphore` in `SeismicTaskService::startBounded` | M1 | Survey (Explorer 1) |
| 2 | R1: MEM-01 Fix | Eliminate write queue UAF and `void*` property smuggling in `ConstraintStore` and `workflows.cpp` | M1 | Survey (Explorer 1) |
| 3 | R1: ARCH-01 Decoupling | Disconnect Data layer (`src/io/dataimportservice`) from QGIS wrapper (`src/qgis/qgislayerservice`), adhere to `UI_LAYER_PLAN.md` | M1 | Survey (Explorer 1) |
| 4 | R2: Issue #25 Line Switching Cancellation | Safely cancel in-flight `vol->ExtractSlice` via `progressCb` when switching inline/crossline/time slices in `SeismicSectionDockWidget` | M2 | Survey (Explorer 1) |
| 5 | R2: Issue #25 Stale Render Elimination | Fix line 1088-1096 in `SeismicSectionDockWidget` to return when `nextType == type` and drop stale slice renders | M2 | Survey (Explorer 1) |
| 6 | R2: Issue #25 Reader Cache Preservation | Preserve `m_segyReaders` across cancellations in `PreviewDocService` using generation checks rather than evicting reader | M2 | Survey (Explorer 1) |
| 7 | R2: Issue #25 Automated Regression Test | Add `testRapidLineSwitchingNoRaceOrCrash()` in `tests/tst_seismic_sectionui.cpp` verifying continuous line switching | M2 | Survey (Explorer 1) |
| 8 | R3: Issue #26 Pre-Creation Lock Check | Prevent `QgisProjectService::createProject` from clobbering existing project files before `ProjectDirLock::tryLock` is evaluated | M3 | Survey (Explorer 2) |
| 9 | R3: Issue #26 User Lock Notice & Dialog | Display clear user-facing error dialog in `AppContext` / `PaleoMainWindow` when lock is held, gracefully abort or downgrade to read-only | M3 | Survey (Explorer 2) |
| 10 | R3: Issue #26 Read-Only UI & Title Binding | Connect `PaleoMainWindow` to `AppContext::projectReadOnlyChanged` to disable save/edit actions and update window title | M3 | Survey (Explorer 2) |
| 11 | R3: Issue #26 Clean Lock Release | Implement `AppContext::closeProject()` to cleanly release `ProjectDirLock` when returning to startup or switching projects | M3 | Survey (Explorer 2) |
| 12 | R3: Issue #26 Concurrent Open Unit Test | Add automated unit test verifying concurrent project open detection and refusal in `tests/tst_projectsvc.cpp` or `tests/tst_appcontext.cpp` | M3 | Survey (Explorer 2) |
| 13 | R4: Issue #27 EnqueueWrite Routing | Route `MapVersionController::saveVersion` operations through `PaleoProjectStore::enqueueWrite` rather than bypassing the write queue | M4 | Survey (Explorer 2) |
| 14 | R4: Issue #27 Edit Token Cleanup | Ensure `"edit"` busy token is reliably cleared with dual-key (`d.layerId` and `layer->id()`) on commit success or failure | M4 | Survey (Explorer 2) |
| 15 | R4: Issue #27 EditingToolbar Signal Binding | Connect `PaleoEditingToolbar` to `QgisEditingService::editCommitted` and `editRolledBack` to keep toolbar state in sync | M4 | Survey (Explorer 2) |
| 16 | R4: Issue #27 Version Saving Regression Test | Add regression test verifying version saving releases edit tokens and processes through queue in `tests/tst_versions.cpp` | M4 | Survey (Explorer 2) |
| 17 | R5: Issue #28 Shared Context & Feedback | Transition `m_context` and `m_feedback` to `std::shared_ptr` tied to task `destroyed` signal in `PaleoAlgorithmWidget` | M5 | Survey (Explorer 3) |
| 18 | R5: Issue #28 CloseEvent Safety Abort | Update `PaleoAlgorithmWidget::closeEvent` to call `e->ignore()` while worker task is running, deferring close until completion | M5 | Survey (Explorer 3) |
| 19 | R5: Issue #28 Algorithm Destructor Safety | Ensure destructor cancels running task and never deletes `m_feedback` or `m_context` while worker thread executes | M5 | Survey (Explorer 3) |
| 20 | R5: Issue #28 Dialog Lifetime Regression Test | Add automated test simulating dialog closure during active algorithm execution in `tests/tst_procdialog.cpp` | M5 | Survey (Explorer 3) |
| 21 | Final Verification & Zero Layering Violations | Full CTest regression pass, zero compiler warnings, 100% adherence to `check_layering.py --strict` | M6 | Survey (All) |

---

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Finalize & Validate In-Flight P0 Fixes (R1) | Validate CONC-01, CONC-02, MEM-01, ARCH-01 in tree, run strict layering & targeted tests | None | DONE |
| M2 | Issue #25 (R2): SEG-Y Line Switching Race | Generation-tracking & cancellation in `SeismicSectionDockWidget`, cache preservation in `PreviewDocService`, `tst_seismic_sectionui` | M1 | PLANNED |
| M3 | Issue #26 (R3): ProjectDirLock Application Flow | Pre-creation lock check in `QgisProjectService`, UI dialog/read-only binding in `AppContext`/`PaleoMainWindow`, `closeProject()`, lock test | M1 | PLANNED |
| M4 | Issue #27 (R4): Save Version Queue & Token | Route saveVersion via `enqueueWrite`, dual-key edit token cleanup, `PaleoEditingToolbar` signal sync, `tst_versions` | M1 | PLANNED |
| M5 | Issue #28 (R5): Algorithm Dialog Lifetime | Shared `TaskPayload` for `context`/`feedback`, `closeEvent` ignore guard, safe cancellation in `PaleoAlgorithmWidget`, `tst_procdialog` | M1 | PLANNED |
| M6 | Comprehensive Verification & Gate Validation | Strict layering check, clean build, automated regression testing across all new and existing suites | M2, M3, M4, M5 | PLANNED |

---

## Code Layout & Write Boundaries
- **R1 (In-Flight P0 Fixes)**:
  - `src/ui/seismicsection/seismicsectiondockwidget.h/.cpp`
  - `src/services/seismictaskservice.cpp`
  - `src/io/constraintstore.cpp`
  - `src/workflow/workflows.h/.cpp`
  - `src/io/dataimportservice.h/.cpp`
  - `src/app/appcontext.cpp`
  - `CMakeLists.txt`
  - `tests/tst_constraintstore.cpp`, `tests/tst_seismic_sectionui.cpp`, `tests/tst_seismic_budgets.cpp`, `tests/tst_import.cpp`
- **R2 (Issue #25)**:
  - `src/ui/seismicsection/seismicsectiondockwidget.h/.cpp`
  - `src/services/previewdoc.cpp`
  - `src/services/seismictaskservice.cpp` (if needed for task serialization)
  - `tests/tst_seismic_sectionui.cpp`
- **R3 (Issue #26)**:
  - `src/qgis/qgisprojectservice.h/.cpp`
  - `src/app/appcontext.h/.cpp`
  - `src/ui/paleomainwindow.h/.cpp`
  - `tests/tst_projectsvc.cpp` or `tests/tst_appcontext.cpp`
- **R4 (Issue #27)**:
  - `src/workflow/mapversioncontroller.cpp`
  - `src/ui/edittools/editingtoolbar.h/.cpp`
  - `src/metadata/paleoprojectstore.h/.cpp` (if token clearing helper needed)
  - `tests/tst_versions.cpp`
- **R5 (Issue #28)**:
  - `src/qgis/qgisprocessingservice.cpp`
  - `tests/tst_procdialog.cpp`

---

## Interface Contracts
### SeismicSectionDockWidget ↔ Background Worker
- Generation counter `m_sliceGeneration` incremented on every `extractSliceAsync`.
- `std::shared_ptr<std::atomic<bool>> cancelFlag` passed into worker lambda.
- `progressCb` returns `false` if `*cancelFlag` is true, immediately halting `vol->ExtractSlice`.
- GUI completion checks generation; if stale, returns early without updating canvas or triggering compare extraction.

### AppContext ↔ QgisProjectService ↔ ProjectDirLock
- `QgisProjectService::createProject` checks `ProjectDirLock::isLocked(projectDir)` before clearing or writing files.
- `AppContext` handles `projectLockRefused` by prompting user (with headless fallback to read-only downgrade or abort) and setting project read-only.
- `PaleoMainWindow` connects to `AppContext::projectReadOnlyChanged(bool)` to toggle save actions, edit tools, and window title (`[*] (只读)`).
- `AppContext::closeProject()` cleans up `m_projectLock`.

### MapVersionController ↔ PaleoProjectStore
- `MapVersionController::saveVersion` executes `m_store->saveVersion` inside `m_projectStore->enqueueWrite([this, ...])`.
- On version save completion or failure, all horizon vector layers have their `"edit"` tokens cleared using both `d.layerId` and `layer->id()`.
- `PaleoEditingToolbar` connects to `QgisEditingService::editCommitted` and `editRolledBack` to synchronize active edit layer status.

### PaleoAlgorithmWidget ↔ QgsProcessingAlgRunnerTask
- Context and feedback managed via `std::shared_ptr<QgsProcessingContext>` and `std::shared_ptr<QgsProcessingFeedback>`.
- Shared `TaskPayload` bound to task lifecycle via `connect(task, &QObject::destroyed, ...)`.
- `closeEvent()` calls `e->ignore()` if task is still running, deferring window closure until completion.
