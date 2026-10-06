# Goal-Loop Ledger: SeismicTaskService Decomposition (`seismic-svc-split`)

**Target Branch**: `goal/seismic-svc-split-20261006` from `origin/master` (commit `6c3ad21`)  
**Target Worktree**: `/home/kevin/projects/paleo_workstation/.worktrees/seismic-svc-split`  
**Date**: 2026-10-06  
**Status**: Completed (100% Pass)  

---

## 1. R0 Baseline Survey

### 1.1 Source Baseline
- `src/services/seismictaskservice.cpp`: 5,041 lines
- `src/services/seismictaskservice.h`: 834 lines (Public API invariant)
- Total lines: 5,875 lines

### 1.2 Baseline Build & Test Execution
- CMake configuration: Ninja + RelWithDebInfo + QGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
- Baseline build: `cmake --build build -j8` succeeded cleanly.
- Baseline layering: `python3 tools/check_layering.py --strict` & `--selftest` passed 100%.
- Baseline seismic tests: `ctest -R "seismic" -j8` 21/21 passed (14.65s).
- Baseline concurrency tests: `ctest -R "tst_seismic_sectionui|tst_seismic_budgets|tst_seismic_perf"` 3/3 passed (6.25s).

---

## 2. Decomposition Topology & Budget Verification

All translation units and internal header have been successfully created and verified against line budgets:

| Translation Unit / Header | Target Role / Functional Cluster | Actual Lines | Budget Limit | Utilization | Status |
|---|---|---|---|---|---|
| `seismictaskservice.cpp` | Base Core: Lifecycle, Registry, Gating (`startBounded`), Memory, Classify | 267 lines | ≤ 800 lines | 33.4% | **PASS** |
| `seismictaskservice_transcode.cpp` | Indexing, Workspace & Paged Transcode, Probes, Trace Header | 928 lines | ≤ 1,200 lines | 77.3% | **PASS** |
| `seismictaskservice_slice.cpp` | Slice/Section Extraction, Line Cache, Synthetic Seismogram, 2D Attr Slice | 981 lines | ≤ 1,200 lines | 81.8% | **PASS** |
| `seismictaskservice_attr.cpp` | Time-Slice Attribute Scanning, SATV 3D Attribute Volume | 1,093 lines | ≤ 1,200 lines | 91.1% | **PASS** |
| `seismictaskservice_interp.cpp` | Horizon Picking, 3D Volume Propagation, Gridding, Sessions, CSV | 1,108 lines | ≤ 1,200 lines | 92.3% | **PASS** |
| `seismictaskservice_viz.cpp` | Quick Open, Tiled Slices, Voxel Windows, LOD Switch, 3D Preview & Assets | 751 lines | ≤ 1,200 lines | 62.6% | **PASS** |
| `seismictaskservice_internal.h` | Shared Dataset Registry, Mutex Entry, Dataset Helpers (`// 层：数据`) | 54 lines | N/A | N/A | **PASS** |

- **Hard Ceiling Rule**: Maximum file size is 1,108 lines (`seismictaskservice_interp.cpp`), strictly below the 1,500-line ceiling.
- **Base Core Budget Rule**: `seismictaskservice.cpp` is 267 lines, strictly below the 800-line limit.
- **Modular TU Budget Rule**: All five modular TUs are ≤ 1,200 lines.

---

## 3. Concurrency & Invariance Invariants

1. **Public API Invariance**:
   - `git diff origin/master -- src/services/seismictaskservice.h` produces 0 diff lines (exact bit-for-bit invariance).
2. **Consumer Invariance**:
   - `git diff origin/master --stat` verifies changes are strictly limited to `CMakeLists.txt` and `src/services/` (plus the ledger).
   - Zero modifications to downstream callers in UI, linkage, workflow, or tests.
3. **SemaphoreGuard RAII**:
   - `startBounded` concurrency gating (`kMaxConcurrentTasks = 4`) preserved verbatim with `SemaphoreGuard` in `seismictaskservice.cpp`.
4. **Supersede Maps**:
   - `activeTranscodeOutputs_` in `seismictaskservice_transcode.cpp`
   - `inFlightAttrScans_` & `attrScanSuperseded_` in `seismictaskservice_attr.cpp`
   - `inFlightPropagation_` & `propagationSuperseded_` in `seismictaskservice_interp.cpp`
   - `inFlightTiledTasks_` in `seismictaskservice_viz.cpp`
5. **Dual Guards**:
   - Generation counter and request ID cancellation checks preserved across all async pipelines.
6. **Object Lifecycle**:
   - Shared registry lifetime decoupled via `std::shared_ptr<SeismicDatasetRegistry>` to guarantee worker completion safety upon service destruction.
7. **Line 635 Timer Invariance**:
   - Synchronous timer deletion in `startSectionExtraction` preserved verbatim for Direction 48 RUNTIME-04 coordination.
8. **Layering Architecture**:
   - All source files begin with `// 层：数据` in top 3 lines. No `QtWidgets` or `ui/` includes.

---

## 4. Verification Execution & Results

### 4.1 Layering Compliance
- Command: `python3 tools/check_layering.py --strict && python3 tools/check_layering.py --selftest`
- Result: **100% PASS** (No layer violations; 21 fixtures + 2 vocab fixtures + 5 strict semantic fixtures all hit expectations).

### 4.2 Build Verification
- Command: `cmake --build build -j8`
- Result: **0 compilation or linking errors**. All library targets and test binaries built cleanly.

### 4.3 Concurrency Test Matrix (3 Consecutive Runs)
- Command: `ctest --test-dir build -j8 -R "tst_seismic_sectionui|tst_seismic_budgets|tst_seismic_perf" --output-on-failure`
- **Run 1**: 3/3 passed (5.16s real)
- **Run 2**: 3/3 passed (4.97s real)
- **Run 3**: 3/3 passed (4.92s real)
- Stability: 100% pass across 3 consecutive executions without flakiness or deadlocks.

### 4.4 Full Seismic Suite Verification
- Command: `ctest --test-dir build -j8 -R "seismic" --output-on-failure`
- Result: **21/21 passed** (12.73s real)
  - `tst_seismic_baseline` (Passed, 6.36s)
  - `tst_seismicattrui` (Passed, 0.79s)
  - `tst_seismicmapping` (Passed, 0.94s)
  - `tst_seismic_3d` (Passed, 1.10s)
  - `tst_seismicattrscan` (Passed, 1.15s)
  - `tst_seismic_index` (Passed, 0.34s)
  - `tst_seismic_engine` (Passed, 0.73s)
  - `tst_seismic_section` (Passed, 0.48s)
  - `tst_seismicattrsvc` (Passed, 0.68s)
  - `tst_seismicattr` (Passed, 0.26s)
  - `tst_seismic_core` (Passed, 0.27s)
  - `tst_seismic_transcode` (Passed, 0.42s)
  - `tst_seismic_welltie` (Passed, 0.44s)
  - `tst_seismic_realarea` (Passed, 0.31s)
  - `tst_seismic_interpret` (Passed, 2.49s)
  - `tst_seismic_sectionui` (Passed, 3.16s)
  - `tst_seismic_3dui` (Passed, 3.79s)
  - `tst_seismic_3dviz` (Passed, 4.08s)
  - `tst_seismic_budgets` (Passed, 1.59s)
  - `tst_seismic_perf` (Passed, 0.32s)
  - `tst_seismicattrperf` (Passed, 0.37s)

### 4.5 Auxiliary Related Tests
- `ctest --test-dir build -j8 -R "horizontrack" --output-on-failure`: **3/3 passed** (51.25s)
- `ctest --test-dir build -j8 -R "tst_taskservice" --output-on-failure`: **1/1 passed** (1.07s)
