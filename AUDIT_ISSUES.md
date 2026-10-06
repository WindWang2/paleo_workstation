# Paleo Workstation: Definitive Master Codebase Audit Report (AUDIT_ISSUES)

**Document Version**: 1.1.0 (Calibrated Master Audit — Reconciled with Adversarial Findings)  
**Target Codebase**: `paleo_workstation` (`src/`, `tests/`, `tools/`, `cmake/`)  
**Auditor**: Multi-Agent Forensic Audit Team (Synthesized by Worker 1; Calibrated & Reconciled by Worker 2 with Challenger 1 Empirical Findings)  
**Date of Audit**: 2026-10-01  
**Status**: Completed, Empirically Verified & Re-Checked Against Source Code  
**Normative Standards**: `AGENTS.md`, `docs/UI_LAYER_PLAN.md`, `docs/PALEO_QGIS_PLAN.md`, `tools/check_layering.py --strict`, ISO C++17 Standard  

**2026-10-03 snapshot note:** This is the 2026-10-01 audit record, not a current
open-issue list. Its file positions, C++17 reference, test counts and severity
claims remain historical evidence; current code uses C++20 and registers 243
CTest suites. The old `src/workflow/workflows.cpp` has been split into
`validationworkflow.cpp`, `compositionworkflow.cpp`, `predictionworkflow.cpp`,
`constraintworkflow.cpp` and `constraintfactorjobs.cpp`, with private helpers in
`workflows_internal.h` / `constraintworkflow_internal.h`. Use `TODOS.md` and
`docs/progress/job-framework.md` for current reconciliation and test evidence;
this refactor does not reopen or independently revalidate every audit claim.

**2026-10-06 closure note（方向48 审计清账）:** Every item below now carries a
`> **终态（2026-10-06，方向48 清账复核）**` marker with re-verification
evidence against `origin/master 3a4f8b5`. Ledger:
`.goal-loop-ledger-audit-closure.md`. Distribution: 已修 26（历史 commit 带
证）/ 不复现 2（MEM-06、CONC-05，带论证与复现尝试）/ 方向48 当批修复 7
（TEST-07=ARCH-08、RUNTIME-04、BIZ-09、ARCH-05、TEST-04、MEM-07、ARCH-06，
commit `e308372`–`ca18f88`）/ 移交方向50 io-robustness 7（BIZ-05/06/07/10/
11/14、RUNTIME-03）/ 移交方向52 test-deepening 1（TEST-06）/ 环境项消解 1
（TEST-01，fresh configure 后 7 套件全注册）。快照漂移教训已落实：10-05
三路复核的「8 未修/15 待证」与本轮实测差异（BIZ-12/13、ARCH-03/04/07 已
修，MEM-06/CONC-05 不复现）以本轮逐条 rg+读码重验为准。

上述分布是方向48的 R0 历史快照；方向50已关闭其中 BIZ-05/06/14，条目终态已同步。原 BIZ-07（配准）、BIZ-10/11（CRS）和 RUNTIME-03（LAS 合乘诊断）仍保留各自状态，不冒记为本方向已修。用户本方向的 IO BIZ-07/10/12 描述以第11节 IO-R0-07/10/12 映射单独验收。

---

## 1. Executive Summary & Audit Scorecard

This audit report represents the definitive, exhaustive synthesis of the deep architectural, safety, business logic, and test integrity investigation conducted across the entirety of the `paleo_workstation` codebase (420 C++ implementation and header files across 12 architectural subdirectories, 126 test suites, CMake build definitions, and automation tooling).

The `paleo_workstation` is an enterprise desktop geological workstation embedding the QGIS C++ API stack. Its design is governed by the core architectural tenet:
> **"视图只发信号不干活，功能只编排不画像素，数据只问答不管谁来问。"**  
> *(The view only emits signals without business logic; the functional layer orchestrates without painting pixels; the data layer answers queries without concern for caller identity.)*

### 1.1 Key Findings Summary

1. **Memory & Concurrency Hazards**: The audit confirmed **5 genuine P0 fatal defects** across memory, concurrency, runtime safety, and architecture:
   - `CONC-01`: UI thread pool worker use-after-free and crash upon dock widget closure in `SeismicSectionDockWidget`.
   - `CONC-02`: Permanent unrecoverable semaphore deadlock in `SeismicTaskService::startBounded` when worker tasks encounter unhandled exceptions.
   - `RUNTIME-01`: Fatal null pointer dereference in `PaleoMainWindow` when `m_projectSvc` is null.
   - `ARCH-01`: Critical architectural inversion where Data layer (`paleo_io`) couples to QGIS wrapper layer (`paleo_qgis`).
   - `MEM-01`: Genuine Use-After-Free vulnerability in `ConstraintStore::m_enqueue` capturing raw `PaleoProjectStore*` in an asynchronous write queue closure without lifetime tracking. (Note: the previously claimed double-delete on project reload was mitigated by dynamic property re-assignment at line 121).
   - *Adversarial Invalidation*: `MEM-02` (claimed double-delete in `QgisTopologicalIndex`) was empirically refuted — `QgsPointLocator` does not inherit Qt parent-child hierarchy from `layer` (`parent` defaults to nullptr), so Qt does not cascade delete it; manual deletion executes exactly once. It has been moved to §7 and removed from active defects.
2. **Architectural Inversion & Contract Circumvention**: While mechanical line-marker checks (`tools/check_layering.py --strict`) exit with code 0, deep inspection uncovered a critical **P0 architectural inversion**: the Data layer (`src/io/dataimportservice`) directly includes and invokes the QGIS Wrapper layer (`src/qgis/qgislayerservice`), and CMake transitively links `paleo_qgis` to `paleo_io`. This escaped detection due to a fundamental blind spot in `check_layering.py`. Furthermore, raw `void*` pointers are smuggled through dynamic `QObject` properties in `src/workflow/workflows.cpp` to avoid interface refactoring.
3. **Business & Geometric Robustness Deficiencies**: Severe data corruption risks were uncovered in data I/O and topological editing:
   - Modern wireline LAS files with $>64$ curves suffer silent log truncation due to a hardcoded stack buffer in `LasParser::parseDepthRange`.
   - UTF-8 BOM offset calculations in LAS range parsers evaluate `f.peek(3)` at the 4MB mark instead of byte 0, causing 3-byte offset corruption.
   - Multi-threaded SEG-Y scanning omits skipped bad trace counts when computing `m_scannedOffset`, causing resumed scans to duplicate seismic traces.
   - Topological polygon vertex deletion in `PaleoVertexTool` collapses multi-coincident vertices simultaneously, producing degenerate 2-point polygon rings.
   - `TimeDepthModel`'s claimed P0 segfault (`BIZ-12`) was calibrated to P2 defensive brittleness: while evaluating `m_points.front()` on an empty vector is undefined behavior in isolation, the state `(m_strict == true && m_points.empty() == true)` is unreachable via public APIs because `setCheckshots()` enforces `points.size() >= 2` before setting `m_strict = true`. Defensively, the check should still be reordered.
4. **Test Suite Integrity & CI Blockers**: 
   - 7 test executables merged from feature branch `wave/deepen-perf` were never generated into `build/CTestTestfile.cmake` due to build directory drift, leaving key performance and lifecycle paths completely unexecuted.
   - Tautological assertions such as `QCOMPARE(render(), render())` and `QVERIFY(spy.count() >= 0)` mask non-functional rendering and broken event handlers.
   - Test `dataops_d6_tabFocusTraversalReachesNewControls` in `tst_panels` consistently fails under headless offscreen QPA.

---

## 2. Master Severity Breakdown Matrix

Following adversarial verification and calibration against empirical source code proofs (via Challenger 1), the audit categorizes **44 unique active issues** (plus 1 complementary cross-listed view: `ARCH-08` / `TEST-07`, and 1 empirically refuted claim: `MEM-02`) across 4 core dimensions:

| Dimension / Category | P0 (Genuine Fatal / Blocker) | P1 (Critical / Data Loss / Race) | P2 (Moderate / Defensive / Linter) | P3 (Minor / Smell / Teardown) | Refuted / Calibrated Claims | Active Issues | Total Recorded |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1. Memory & Runtime Safety** | 4 | 5 | 5 | 1 | 1 (`MEM-02`) | **15** | **16** |
| **2. Architecture Layering & Contracts** | 1 | 3 | 3 | 1 | 0 | **8** | **8** |
| **3. Business & Boundary Robustness** | 0 | 5 | 9 | 0 | 0 | **14** | **14** |
| **4. Test Coverage & Assertion Integrity** | 0 | 3 | 4 | 0 | 0 | **7** | **7** |
| **Total** | **5** | **16** | **21** | **2** | **1** | **44\*** | **45\*** |

*\* Note: `ARCH-08` and `TEST-07` describe the same underlying test blocker in `tests/tst_panels.cpp`, evaluated from both architectural contract and test suite failure perspectives. Excluding this dual-view, there are 44 distinct active root causes, plus 1 empirically refuted claim (`MEM-02`), yielding 45 total discrete items tracked (46 total entries).*

### Severity Classification Standard
- **P0 (Blocker / Genuine Fatal Crash / Architecture Inversion)**: Deterministic segmentation faults, confirmed use-after-free hazards in asynchronous closures, permanent multi-thread deadlocks, or fundamental inversions of the core architectural hierarchy. (5 genuine blockers confirmed: `CONC-01`, `CONC-02`, `RUNTIME-01`, `ARCH-01`, `MEM-01` UAF).
- **P1 (Critical / Data Loss / Race Condition / Broken Test Suite)**: Silent truncation of scientific data, multi-threaded race conditions, use-after-free hazards under asynchronous UI operations, or broken CI test suites. (16 issues).
- **P2 (Moderate / Boundary Vulnerability / Defensive Brittleness / False Green)**: Numerical overflow/underflow, missing non-finite/NaN sanitization, defensive check ordering vulnerabilities (e.g. `BIZ-12`), tautological test assertions that always pass, or static checker loopholes. (21 issues).
- **P3 (Minor / Code Smell / Sub-optimal Placement)**: Inconvenient class organization (e.g. interactive map tools residing in functional libraries) or dangling pointers on application teardown. (2 issues).
- **Empirically Refuted / Calibrated Claims**: Claims originally alleged as P0 defects that were formally refuted via API analysis and empirical test execution (`MEM-02`). Retained for audit transparency in §7.

---

## 3. Dimension 1: Memory, Concurrency & Runtime Safety

### [MEM-01] Asynchronous Write Queue Use-After-Free & Dynamic Property Smuggling in `ConstraintStore` / `ConstraintWorkflow`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：constraintstore.cpp QPointer safeStore；commit `57f0161`）

- **Severity**: **P0** (Fatal Crash / Use-After-Free in Asynchronous Write Queue Closure)
- **Dimension**: Memory & Runtime Safety (Cross-Ref: `ARCH-02`)
- **Exact Code Location**: `src/workflow/workflows.cpp`, Lines 112–123, 711–715; `src/io/constraintstore.cpp`, Lines 48–53
- **Code Snippet**:
```cpp
// src/workflow/workflows.cpp:112-123
    PaleoProjectStore *store = storeOf( wf );
    if ( store && !store->gpkgPath().isEmpty() )
    {
      QVariant ov = wf->property( kOwnedConstraintStoreProp );
      if ( ov.isValid() && ov.value<void *>() )
      {
        auto *owned = static_cast<ConstraintStore *>( ov.value<void *>() );
        if ( owned->gpkgPath() == store->gpkgPath() )
          return owned;
        delete owned;
      }
      auto *owned = new ConstraintStore( store->gpkgPath(), store );
      const_cast<QObject *>( wf )->setProperty( kOwnedConstraintStoreProp, QVariant::fromValue( static_cast<void *>( owned ) ) );
      return owned;
    }
...
// src/io/constraintstore.cpp:48-53
ConstraintStore::ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store)
  : m_gpkgPath(gpkgPath)
{
  if (store) {
    m_enqueue = [store](const WriteFn &fn) {
      return store->enqueueWrite(fn);
    };
  }
}
```
- **Trigger Condition & Call Chain**:
  1. **Dynamic Property Re-assignment Calibration**: In `src/workflow/workflows.cpp:112-123`, line 118 executes `delete owned;` when the active project changes, but line 121 immediately re-assigns `kOwnedConstraintStoreProp` with the newly allocated pointer (`owned = new ConstraintStore(...)`). This re-assignment overwrites the property, mitigating the immediate double-delete hazard on standard project reload (a double-delete on reload would only trigger if the subsequent allocation threw `std::bad_alloc`).
  2. **Confirmed P0 Use-After-Free Vulnerability**: The critical, confirmed fatal vulnerability resides in `ConstraintStore::ConstraintStore` (`src/io/constraintstore.cpp:48-53`). The lambda captures `PaleoProjectStore *store` by raw pointer without lifetime tracking (`QPointer` or connection to project destruction). `ConstraintWorkflow` (attached to the workbench/UI lifecycle) can outlive `PaleoProjectStore` (which is closed and destroyed when switching or closing projects). Any subsequent asynchronous write operation scheduled via `m_enqueue` dereferences a dangling `store` pointer (`store->enqueueWrite(fn)`), causing heap corruption or a fatal SIGSEGV.
- **Impact Analysis**: Heap corruption, glibc abort, or SIGSEGV in worker threads whenever a project is closed while asynchronous constraint writes are pending or dispatched.
- **Concrete Remediation**:
  Eliminate `void*` property smuggling entirely. Give `ConstraintWorkflow` direct typed ownership of `ConstraintStore` via `std::unique_ptr<ConstraintStore>`, and guard `PaleoProjectStore` with `QPointer` in `ConstraintStore::m_enqueue`:
```cpp
// In src/workflow/workflows.h (inside ConstraintWorkflow):
private:
  std::unique_ptr<ConstraintStore> m_ownedConstraintStore;
  ConstraintStore *m_externalConstraintStore = nullptr;

// In src/io/constraintstore.cpp:
ConstraintStore::ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store)
  : m_gpkgPath(gpkgPath)
{
  if (store) {
    QPointer<PaleoProjectStore> safeStore(store);
    m_enqueue = [safeStore](const WriteFn &fn) -> PaleoProjectStore::WriteResult {
      if (!safeStore) return {false, QStringLiteral("Project store closed")};
      return safeStore->enqueueWrite(fn);
    };
  }
}
```
- **Empirical Verification Evidence**:
  Inspect `src/workflow/workflows.cpp:112-123` and `src/io/constraintstore.cpp:48-53`. Run project open/close cycles under AddressSanitizer: `ctest -R tst_workflow_constraint` with `-DPALEO_ENABLE_ASAN=ON`.

---

### [MEM-02] [EMPIRICALLY REFUTED] Claimed Double Delete Hazard in `QgisTopologicalIndex` on Layer Destruction
> **终态（2026-10-06，方向48 清账复核）**：维持反驳（§7.1；不复核翻案）

- **Severity**: **REFUTED / INFORMATIONAL** (Claimed P0 Double-Free Empirically Disproved — Excluded from Active P0 Tally)
- **Dimension**: Memory & Runtime Safety
- **Exact Code Location**: `src/qgis/topologicalindex.cpp`, Lines 76–85, 109–110
- **Code Snippet**:
```cpp
// src/qgis/topologicalindex.cpp:76-85
  connect( layer, &QObject::destroyed, this, [this, lp] {
    for ( int i = m_entries.size() - 1; i >= 0; --i )
    {
      if ( m_entries.at( i ).layer == lp || m_entries.at( i ).layer.data() == nullptr )
      {
        delete m_entries[i].locator;
        m_entries.removeAt( i );
      }
    }
  } );
...
// src/qgis/topologicalindex.cpp:109-110
  delete e.locator;
  e.locator = new QgsPointLocator( e.layer ); // 无目标 CRS：索引 = 层坐标
```
- **Adversarial Invalidation & Empirical Analysis**:
  1. **Original Audit Claim**: The exploratory audit claimed that `new QgsPointLocator(e.layer)` passes `e.layer` as the Qt `QObject` parent. When `layer` is deleted, Qt's parent-child hierarchy allegedly cascades and deletes `locator`. Then, `layer->destroyed` fires and executes `delete m_entries[i].locator;`, triggering a double-free crash (`free(): double free detected in tcache 2`).
  2. **Adversarial Source Inspection**: Challenger 1 examined the official vendored QGIS C++ header (`vendor/prefix/usr/include/qgis/qgspointlocator.h:120-125`) and implementation (`third_party/qgis/src/core/qgspointlocator.cpp:871-873`):
     ```cpp
     explicit QgsPointLocator(
       QgsVectorLayer *layer,
       const QgsCoordinateReferenceSystem &destinationCrs = QgsCoordinateReferenceSystem(),
       const QgsCoordinateTransformContext &transformContext = QgsCoordinateTransformContext(),
       const QgsRectangle *extent = nullptr
     );
     ```
     `QgsPointLocator` inherits `QObject`, but its constructor **does not take a `QObject *parent` parameter**. The base `QObject` constructor defaults to `parent = nullptr`.
  3. **No Cascade Deletion**: The argument `layer` is stored internally in `mLayer` as the target dataset to index. `e.locator` is **not** a child of `e.layer` in Qt's object tree. Therefore, deleting `e.layer` does **not** delete `e.locator`.
  4. **Legitimate Single Deletion**: The deletion in `m_entries` callback is executed **exactly once**.
  5. **Empirical Test Verification**: Executing `tst_edittools` with the full vendored QGIS runtime confirms this: `Totals: 69 passed, 0 failed, 0 skipped, 0 blacklisted`. Test `vertexAdversarialLayerDestructionArmedToolSafety` passed with 0 errors.
- **Status & Verdict**: **REFUTED & EXCLUDED FROM ACTIVE P0 TALLY**. Detailed adversarial proof preserved in §7.1 for forensic transparency.

---

### [MEM-03] Use-After-Free & Double Delete in `PaleoMainWindow::flashHorizon` Timer
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：m_horizonFlashTimer 成员守卫；commit `6845d84`+`bcecd04`）

- **Severity**: **P1** (Use-After-Free / UI Destruction Crash)
- **Dimension**: Memory & Runtime Safety
- **Exact Code Location**: `src/ui/paleomainwindow.cpp`, Lines 1156–1176
- **Code Snippet**:
```cpp
// src/ui/paleomainwindow.cpp:1156-1176
  auto *band = new QgsRubberBand(cv, Qgis::GeometryType::Polygon);
  band->setToGeometry(QgsGeometry::fromRect(layer->extent()),
                      qobject_cast<QgsVectorLayer *>(layer));
  band->setColor(QColor(27, 115, 208, 60)); // #1B73D0 @ ~24% 填充透明度
  band->setStrokeColor(QColor(QStringLiteral("#1B73D0")));
  band->setWidth(2);
  setProperty("horizonFlashActive", true);
  auto *timer = new QTimer(this);
  timer->setObjectName(QStringLiteral("horizonFlashTimer"));
  int blinks = 4;
  connect(timer, &QTimer::timeout, this, [this, timer, band, blinks]() mutable {
    band->setVisible(band->isVisible() ? false : true);
    if (--blinks <= 0)
    {
      timer->stop();
      timer->deleteLater();
      delete band; // 画布条目直接删——不在信号发送者栈上
      setProperty("horizonFlashActive", false);
    }
  });
  timer->start(100);
```
- **Trigger Condition & Call Chain**:
  1. User triggers horizon flash navigation (`PaleoMainWindow::flashHorizon`). A `QgsRubberBand` is allocated, parented to canvas `cv`.
  2. A `QTimer` is started to alternate visibility every 100ms for 4 cycles (400ms duration).
  3. Within this 400ms window, the user closes the workspace, switches projects, or closes the main window.
  4. Destruction of `cv` (`QgsMapCanvas`) deallocates all graphics scene canvas items, destroying `band`.
  5. The active `timer` (parented to `this`) fires its timeout. The lambda captures raw pointer `band` and invokes `band->setVisible(...)` or `delete band;`.
- **Impact Analysis**: Dereferencing and deleting an already-freed `QgsRubberBand` pointer triggers a fatal memory corruption or crash during window closing or rapid tab switching.
- **Concrete Remediation**:
  Wrap `band` in `QPointer<QgsRubberBand>` within the lambda capture:
```cpp
  QPointer<QgsRubberBand> bandGuard(band);
  connect(timer, &QTimer::timeout, this, [this, timer, bandGuard, blinks]() mutable {
    if (!bandGuard) {
      timer->stop();
      timer->deleteLater();
      setProperty("horizonFlashActive", false);
      return;
    }
    bandGuard->setVisible(!bandGuard->isVisible());
    if (--blinks <= 0) {
      timer->stop();
      timer->deleteLater();
      delete bandGuard.data();
      setProperty("horizonFlashActive", false);
    }
  });
```
- **Empirical Verification Evidence**:
  Inspect `src/ui/paleomainwindow.cpp:1156-1176`. Rapidly invoke `flashHorizon` followed by closing the project canvas under ASan.

---

### [MEM-04] Dangling Pointers in `QgisLayerService::m_instances` and `isEditingAnyLayer`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：QHash<QString, QPointer<QgsMapLayer>>；PR #193 `0a02a0e`）

- **Severity**: **P1** (Use-After-Free / Dangling Pointer Dereference)
- **Dimension**: Memory & Runtime Safety
- **Exact Code Location**: `src/qgis/qgislayerservice.h`, Line 55; `src/qgis/qgislayerservice.cpp`, Lines 240–275
- **Code Snippet**:
```cpp
// src/qgis/qgislayerservice.h:55
QHash<QString, QgsMapLayer *> m_instances; // layerId -> layer (owned by QgsProject)

// src/qgis/qgislayerservice.cpp:240-275
void QgisLayerService::purgeDanglingInstances()
{
  ...
  for (auto it = m_instances.begin(); it != m_instances.end();)
  {
    // Pointer comparison only — entries may dangle and must not be dereferenced.
    if (!live.contains(it.value()))
      it = m_instances.erase(it);
    else
      ++it;
  }
}

QgsMapLayer *QgisLayerService::layer(const QString &layerId) const
{
  return m_instances.value(layerId); // Returns raw pointer without liveness check
}

bool QgisLayerService::isEditingAnyLayer(QString *layerName) const
{
  for (auto it = m_instances.cbegin(); it != m_instances.cend(); ++it)
  {
    if (auto *vl = qobject_cast<QgsVectorLayer *>(it.value())) // DEREFERENCES DANGLING POINTER!
    {
      if (vl->isEditable()) { ... }
    }
  }
  return false;
}
```
- **Trigger Condition & Call Chain**:
  1. `m_instances` holds raw pointers to `QgsMapLayer` instances whose ownership belongs to `QgsProject`.
  2. When a layer is removed directly from `QgsProject` (or during rollback/close before `purgeDanglingInstances` is invoked), the hash table still retains the deallocated pointer address.
  3. `purgeDanglingInstances` explicitly acknowledges this at line 242: `// Pointer comparison only — entries may dangle and must not be dereferenced.`
  4. Yet `layer(layerId)` returns the dangling pointer directly, and `isEditingAnyLayer()` executes `qobject_cast<QgsVectorLayer *>(it.value())` across the entire map.
  5. `qobject_cast` dereferences the freed memory to access the `QObject` vtable and `metaObject()`.
- **Impact Analysis**: Segmentation fault during project close, window state checks, or when user closes modified layers.
- **Concrete Remediation**:
  Store `QPointer<QgsMapLayer>` instead of raw pointers in `m_instances`:
```cpp
// In src/qgis/qgislayerservice.h:55
QHash<QString, QPointer<QgsMapLayer>> m_instances;

// In isEditingAnyLayer():
for (auto it = m_instances.cbegin(); it != m_instances.cend(); ++it)
{
  if (!it.value()) continue;
  if (auto *vl = qobject_cast<QgsVectorLayer *>(it.value().data()))
    if (vl->isEditable()) return true;
}
```
- **Empirical Verification Evidence**:
  Inspect `src/qgis/qgislayerservice.h:55` and `src/qgis/qgislayerservice.cpp:250-275`.

---

### [MEM-05] Heap-Allocated `QThreadPool` Leaked in `PaleoTaskService`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：析构超时兜底+delete m_pool；PR #193 `0a02a0e`）

- **Severity**: **P2** (Permanent Resource & Thread Handle Leak)
- **Dimension**: Memory & Runtime Safety
- **Exact Code Location**: `src/services/paleotaskservice.cpp`, Lines 161–164, 180–182
- **Code Snippet**:
```cpp
// src/services/paleotaskservice.cpp:161-164
PaleoTaskService::PaleoTaskService(PaleoProjectStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
  m_pool = new QThreadPool(); // 无父：析构策略见 ~PaleoTaskService
  m_pool->setMaxThreadCount(4);
  m_pool->setExpiryTimeout(30 * 1000);
}

// src/services/paleotaskservice.cpp:180-182
PaleoTaskService::~PaleoTaskService()
{
  ...
  // D4.5：清空未开始的排队（运行中的协作取消已发）；池不再 delete——
  // waitForDone 会卡住仍在跑的长任务，孤儿化池（线程随 expiry 退出）与
  // 任务的孤儿化语义一致。
  m_pool->clear();
  m_pool->setParent(nullptr);
}
```
- **Trigger Condition & Call Chain**:
  1. `PaleoTaskService` instantiates a dedicated `m_pool = new QThreadPool()` with no `QObject` parent.
  2. Upon destruction, `~PaleoTaskService()` intentionally calls `m_pool->clear()` and `m_pool->setParent(nullptr)` and abandons `m_pool` without deleting it.
  3. In test fixtures (`tst_taskcenter`, `tst_appcontext`) or project open/close workflows where `PaleoTaskService` is repeatedly created and destroyed, `QThreadPool` objects and POSIX thread structures leak indefinitely.
- **Impact Analysis**: Memory leak of thread pool control structures and thread stack memory over long workstation sessions.
- **Concrete Remediation**:
  Cancel active tasks, wait boundedly for running tasks, and safely delete `m_pool`:
```cpp
PaleoTaskService::~PaleoTaskService()
{
  for (PaleoTask *t : m_tasks)
    if (t->running()) {
      t->requestCancel();
      t->setParent(nullptr);
    }
  m_pool->clear();
  m_pool->waitForDone(500); // Bounded wait for cooperative cancellation
  delete m_pool;
  m_pool = nullptr;
}
```
- **Empirical Verification Evidence**:
  Inspect `src/services/paleotaskservice.cpp:161-182`. Run `ctest -R tst_taskcenter` under Valgrind/ASan.

---

### [MEM-06] Spared `mouseGrabberItem` and Double Delete Hazard in `HorizonMarkerSet::rebuild`
> **终态（2026-10-06，方向48 清账复核）**：不复现（方向48 复核：rebuild 已重写为现场查询 mouseGrabberItem+逐项豁免+无跨帧存指针，`235ca00` 范式；跨帧死亡时 QGraphicsItem 析构自动 ungrab→查询返回 null）

- **Severity**: **P2** (Dangling Pointer / Potential Crash on Mouse Drag Cancel)
- **Dimension**: Memory & Runtime Safety
- **Exact Code Location**: `src/ui/correlation/horizonmarkers.cpp`, Lines 322–330, 431–432
- **Code Snippet**:
```cpp
// src/ui/correlation/horizonmarkers.cpp:322-330
  QGraphicsItem *grabber = scene->mouseGrabberItem();
  const QList<QGraphicsItem *> existing = scene->items();
  for (QGraphicsItem *it : existing)
  {
    if (it == grabber)
      continue;
    if (it->data(CorrelationItemRoles::HorizonMarker).isValid())
      delete it;
  }
...
// src/ui/correlation/horizonmarkers.cpp:431-432
  if (grabber && !grabberReused && grabber->scene() == scene)
    delete grabber;
```
- **Trigger Condition & Call Chain**:
  1. During well correlation picking, user drags a horizon marker line with the mouse (`grabber = scene->mouseGrabberItem()`).
  2. A background pick update or well reorder triggers `WellCorrelationPanel::relayoutMarkers`, which deletes and recreates `m_chrome` (the parent host of marker items).
  3. When `m_chrome` is destroyed, Qt's `QGraphicsItem::~QGraphicsItem()` destroys all of its child items, which may include `grabber`.
  4. `HorizonMarkerSet::rebuild` executes line 431: `if (grabber && !grabberReused && grabber->scene() == scene) delete grabber;`.
  5. Evaluating `grabber->scene()` dereferences a dangling pointer.
- **Impact Analysis**: Heap use-after-free crash during interactive horizon dragging if marker rebuild coincides with parent item recreation.
- **Concrete Remediation**:
  Verify item liveness against `scene->items().contains(grabber)` before dereferencing, or store `grabber` via `QPointer`:
```cpp
  if (grabber && !grabberReused && scene->items().contains(grabber))
  {
    scene->removeItem(grabber);
    delete grabber;
  }
```
- **Empirical Verification Evidence**:
  Inspect `src/ui/correlation/horizonmarkers.cpp:322-330, 431-432`.

---

### [MEM-07] Unchecked Dangling Canvas Pointer in `PaleoEditingToolbar` Destructor
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（mCanvas QPointer 化+拆卸顺序钉死；commit `c814130`）

- **Severity**: **P3** (Teardown Lifetime Hazard)
- **Dimension**: Memory & Runtime Safety
- **Exact Code Location**: `src/ui/edittools/editingtoolbar.h`, Line 143; `src/ui/edittools/editingtoolbar.cpp`, Lines 258–264
- **Code Snippet**:
```cpp
// src/ui/edittools/editingtoolbar.h:143
QgsMapCanvas *mCanvas = nullptr; // not owned

// src/ui/edittools/editingtoolbar.cpp:258-264
PaleoEditingToolbar::~PaleoEditingToolbar()
{
  if ( mActiveEditTool )
  {
    QgsMapTool *tool = mActiveEditTool;
    mActiveEditTool = nullptr;
    mCanvas->unsetMapTool( tool ); // Unchecked raw pointer dereference
    delete tool;
  }
}
```
- **Trigger Condition & Call Chain**:
  `mCanvas` is a raw pointer. If `mCanvas` is deallocated prior to `PaleoEditingToolbar` during main window teardown, `mCanvas->unsetMapTool(tool)` invokes a member function on a deleted object.
- **Impact Analysis**: Potential segmentation fault on application exit.
- **Concrete Remediation**:
  Change `mCanvas` to `QPointer<QgsMapCanvas>` and guard `if (mCanvas) mCanvas->unsetMapTool(tool);`.
- **Empirical Verification Evidence**:
  Inspect `src/ui/edittools/editingtoolbar.h:143` and `src/ui/edittools/editingtoolbar.cpp:258-264`.

---

### [CONC-01] Thread Pool Task Capturing Raw UI Widget Pointer in `SeismicSectionDockWidget`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：四处 QPointer guard；commit `5e3aea1`/PR #193）

- **Severity**: **P0** (Fatal Crash / Use-After-Free / Thread Safety Violation)
- **Dimension**: Memory, Concurrency & Runtime Safety
- **Exact Code Location**: `src/ui/seismicsection/seismicsectiondockwidget.cpp`, Lines 1046–1065, 1400–1410
- **Code Snippet**:
```cpp
// src/ui/seismicsection/seismicsectiondockwidget.cpp:1046-1065
    auto vol = m_volume;
    QThreadPool::globalInstance()->start([this, vol, type, index, title, origin]() {
        SgySliceImage image;
        std::string err;
        auto progressCb = [this](int processed, int total) -> bool {
            if (total > 0) {
                const int pct = std::clamp(static_cast<int>(std::round(100.0 * processed / total)), 0, 100);
                QMetaObject::invokeMethod(this, [this, pct]() {
                    m_progressBar->setValue(pct);
                }, Qt::QueuedConnection);
            }
            return true;
        };

        const bool ok = vol->ExtractSlice(type, index, image, err, progressCb);

        QMetaObject::invokeMethod(this, [this, ok, image, type, index, title, vol, err, origin]() {
            m_isExtractingSlice = false;
            m_progressBar->setVisible(false);
            ...
        });
    });
...
// Lines 1400-1410 (Neighbor comparison extraction):
    QThreadPool::globalInstance()->start([this, vol, type, neighbor, label]() {
        SgySliceImage image;
        std::string err;
        vol->ExtractSlice(type, neighbor, image, err);
        QMetaObject::invokeMethod(this, [this, image, label]() {
            m_extractingCompare = false;
            m_canvas->setCompareData(image, label);
        }, Qt::QueuedConnection);
    });
```
- **Trigger Condition & Call Chain**:
  1. User triggers seismic slice extraction or neighbor comparison on a large SEG-Y volume.
  2. The extraction task is launched onto `QThreadPool::globalInstance()`, capturing `this` (a `QDockWidget` UI widget) as a raw pointer.
  3. The user closes the dock widget, closes the project, or switches tabs while extraction is computing on the background worker thread.
  4. `~SeismicSectionDockWidget() override = default;` completes, deallocating the widget and all its child controls (`m_progressBar`, `m_canvas`).
  5. The thread pool worker executes `progressCb` or finishes `vol->ExtractSlice` and calls `QMetaObject::invokeMethod(this, ...)`.
  6. Qt evaluates `context->thread()` on a deleted `QObject`, crashing immediately with SIGSEGV. If the invocation gets queued, accessing `m_progressBar->setValue(pct)` writes to freed heap memory.
- **Impact Analysis**: Immediate crash when closing or navigating away from seismic sections while slice extraction is in progress.
- **Concrete Remediation**:
  Capture `QPointer<SeismicSectionDockWidget> guard(this);` and abort extraction if the widget dies:
```cpp
    QPointer<SeismicSectionDockWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, vol, type, index, title, origin]() {
        if (!guard) return;
        SgySliceImage image;
        std::string err;
        auto progressCb = [guard](int processed, int total) -> bool {
            if (!guard) return false; // Early abort worker task
            if (total > 0) {
                const int pct = std::clamp(static_cast<int>(std::round(100.0 * processed / total)), 0, 100);
                QMetaObject::invokeMethod(guard.data(), [guard, pct]() {
                    if (guard) guard->m_progressBar->setValue(pct);
                }, Qt::QueuedConnection);
            }
            return true;
        };
        const bool ok = vol->ExtractSlice(type, index, image, err, progressCb);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, ok, image, type, index, title, vol, err, origin]() {
            if (!guard) return;
            guard->m_isExtractingSlice = false;
            guard->m_progressBar->setVisible(false);
            ...
        }, Qt::QueuedConnection);
    });
```
- **Empirical Verification Evidence**:
  Inspect `src/ui/seismicsection/seismicsectiondockwidget.cpp:1046-1065, 1400-1410`.

---

### [CONC-02] Missing RAII on `gate->slotSemaphore` in `SeismicTaskService::startBounded`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：SemaphoreGuard RAII；PR #193 `0a02a0e`）

- **Severity**: **P0** (Permanent Deadlock on Exception)
- **Dimension**: Concurrency & Thread Safety
- **Exact Code Location**: `src/services/seismictaskservice.cpp`, Lines 2586–2591
- **Code Snippet**:
```cpp
// src/services/seismictaskservice.cpp:2586-2591
  const std::function<QString(PaleoTask *)> gated =
      [gate, work](PaleoTask *task) -> QString {
        gate->slotSemaphore.acquire();
        const QString err = task->cancelRequested() ? QString() : work(task);
        gate->slotSemaphore.release();
        return err;
      };
```
- **Trigger Condition & Call Chain**:
  1. `SeismicTaskService::startBounded` limits concurrent seismic tasks using a `QSemaphore slotSemaphore` (initialized to 4 permits).
  2. A task acquires a permit via `gate->slotSemaphore.acquire()`.
  3. Inside `work(task)`, an exception is thrown (e.g. `std::bad_alloc`, SEGY format error, or out-of-range slice index).
  4. The exception is caught at the outer framework level in `TaskRunner::run()` (`src/services/paleotaskservice.cpp:30-42`).
  5. Because there is no RAII guard around `slotSemaphore.acquire()`, execution unwinds past `gate->slotSemaphore.release()`.
  6. Exactly one semaphore permit is permanently lost.
  7. After 4 such exceptions, all 4 permits are exhausted.
  8. Every subsequent seismic task blocks indefinitely on `gate->slotSemaphore.acquire()`, freezing the entire seismic processing pipeline permanently.
- **Impact Analysis**: Total permanent freeze / deadlock of all background seismic operations (slice extraction, 3D volume indexing, horizon export) after 4 task failures.
- **Concrete Remediation**:
  Enforce RAII on the semaphore permit release:
```cpp
  const std::function<QString(PaleoTask *)> gated =
      [gate, work](PaleoTask *task) -> QString {
        gate->slotSemaphore.acquire();
        struct SemaphoreGuard {
          QSemaphore &sem;
          ~SemaphoreGuard() { sem.release(); }
        } guard{gate->slotSemaphore};
        return task->cancelRequested() ? QString() : work(task);
      };
```
- **Empirical Verification Evidence**:
  Inspect `src/services/seismictaskservice.cpp:2586-2591`.

---

### [CONC-03] Use-After-Free of `EvictableCache` in `CacheBudgetManager::enforce()`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：evictionZeroCond+evictionScope RAII；PR #193 `0a02a0e`）

- **Severity**: **P1** (Use-After-Free / Thread Race Condition)
- **Dimension**: Concurrency & Thread Safety
- **Exact Code Location**: `src/io/cachebudget.cpp`, Lines 76–110
- **Code Snippet**:
```cpp
// src/io/cachebudget.cpp:76-110
qint64 CacheBudgetManager::enforce()
{
  qint64 budget = 0;
  QVector<EvictableCache *> snapshot;
  {
    QMutexLocker lock(&m_mutex);
    budget = m_budgetBytes;
    snapshot = m_caches;
  } // Mutex unlocked here!
  qint64 used = 0;
  for (const EvictableCache *c : snapshot)
    used += c->bytes();
...
  for (EvictableCache *c : snapshot)
  {
    if (need <= 0) break;
    const qint64 cacheBytes = c->bytes();
    if (cacheBytes <= 0) continue;
    const qint64 got = c->evictLRUEntries(qMax<qint64>(0, cacheBytes - need));
    freed += got;
    need -= got;
  }
```
- **Trigger Condition & Call Chain**:
  1. `enforce()` locks `m_mutex`, copies `m_caches` into local `snapshot`, and immediately releases the lock at line 81.
  2. While `enforce()` calculates byte totals and calls `c->evictLRUEntries(...)` on Thread 1, Thread 2 terminates a worker task and destroys an `LruCache` instance.
  3. `~LruCache()` invokes `unregisterCache(this)` on Thread 2, which removes `this` from `m_caches`, and the object memory is returned to the OS.
  4. Thread 1 iterates over `snapshot` and dereferences `c->bytes()`, `c->lastAccessMs()`, or `c->evictLRUEntries()` on the freed pointer.
- **Impact Analysis**: Intermittent heap corruption or SIGSEGV when short-lived worker thread caches are destroyed while memory pressure enforcement is active.
- **Concrete Remediation**:
  Introduce an active enforcement counter and wait condition in `CacheBudgetManager` so `unregisterCache` blocks until running `enforce()` loops finish:
```cpp
// In CacheBudgetManager:
void CacheBudgetManager::unregisterCache(EvictableCache *cache)
{
  QMutexLocker lock(&m_mutex);
  m_caches.removeAll(cache);
  while (m_inEnforce.load() > 0)
    m_enforceWaitCond.wait(&m_mutex);
}
```
- **Empirical Verification Evidence**:
  Inspect `src/io/cachebudget.cpp:76-110`.

---

### [CONC-04] Concurrent Callback Data Race in `SegyReader::scanTraceHeadersFast`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：progressMutex 串行化；PR #193 `0a02a0e`）

- **Severity**: **P1** (Data Race / Unsynchronized Callback Invocation)
- **Dimension**: Concurrency & Thread Safety
- **Exact Code Location**: `src/io/segyreader.cpp`, Lines 817–821, 862–863
- **Code Snippet**:
```cpp
// src/io/segyreader.cpp:817-821, 862-863
  for (int s = 0; s < shardCount; ++s)
  {
    Shard &sh = shards[static_cast<size_t>(s)];
    pool.start([&sh, &cancelled, path, firstTraceOffset, traceSize, traceCount, &sidx, ns,
                opts]() {
...
        if (opts && opts->progress && ((i - sh.from) % 128) == 0)
          opts->progress(offset, firstTraceOffset + traceCount * traceSize);
```
- **Trigger Condition & Call Chain**:
  1. `scanTraceHeadersFast` partitions trace indexing across up to 4 concurrent worker threads in `pool`.
  2. At every 128 traces, worker threads concurrently invoke `opts->progress(...)`.
  3. `opts->progress` is a non-thread-safe functor capturing UI state or non-atomic progress counters.
  4. Multiple threads execute `opts->progress` simultaneously without synchronization.
- **Impact Analysis**: Data race under ISO C++17 memory model, resulting in corrupted progress bar values or crashes if the functor manipulates non-thread-safe objects.
- **Concrete Remediation**:
  Serialize progress reporting with a mutex:
```cpp
  std::mutex progressMutex;
  // Inside worker lambda:
  if (opts && opts->progress && ((i - sh.from) % 128) == 0) {
    std::lock_guard<std::mutex> lock(progressMutex);
    opts->progress(offset, firstTraceOffset + traceCount * traceSize);
  }
```
- **Empirical Verification Evidence**:
  Inspect `src/io/segyreader.cpp:817-821, 862-863`. Run with ThreadSanitizer (`-DPALEO_ENABLE_TSAN=ON`).

---

### [CONC-05] Data Race on Non-Atomic Diagnostic Struct `LasCache::m_timings`
> **终态（2026-10-06，方向48 清账复核）**：不复现（方向48 复核：LasCache 全部消费面在 GUI 线程——crossplotcontroller/previewdoc/wellsectionworkflow/selfcheck 无 worker 调用；头注释已声明单线程诊断口径）

- **Severity**: **P2** (Data Race / Torn Reads)
- **Dimension**: Concurrency & Thread Safety
- **Exact Code Location**: `src/io/lascache.h`, Line 91; `src/io/lascache.cpp`, Lines 228, 235
- **Code Snippet**:
```cpp
// src/io/lascache.h:91
Timings m_timings; // 最近一次（粗粒度诊断面；不做多线程记账）

// src/io/lascache.cpp:228, 235
m_timings.diskLoadNs = diskTimer.nsecsElapsed();
...
m_timings.coldParseNs = parseTimer.nsecsElapsed();
```
- **Trigger Condition & Call Chain**:
  `LasCache::shared()` is accessed concurrently from multiple threads. Worker threads writing `m_timings.diskLoadNs` race against GUI threads reading `lastTimings()` without mutex protection or atomic variables.
- **Impact Analysis**: Undefined behavior and 64-bit torn reads under high concurrent load.
- **Concrete Remediation**:
  Change `Timings` fields to `std::atomic<qint64>` or protect reads and writes with `m_cfgMutex`.
- **Empirical Verification Evidence**:
  Inspect `src/io/lascache.h:91` and `src/io/lascache.cpp:228, 235`.

---

### [CONC-06] `DataImportService::catInvoke` Deadlock Hazard with `Qt::BlockingQueuedConnection`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：produce-then-commit 重设计，无 BlockingQueuedConnection 回 owner；commit `57f0161`）

- **Severity**: **P1** (Deadlock Hazard Across Thread Boundaries)
- **Dimension**: Concurrency & Thread Safety
- **Exact Code Location**: `src/io/dataimportservice.h`, Lines 220–243
- **Code Snippet**:
```cpp
// src/io/dataimportservice.h:220-243
    template <typename Fn> auto catInvoke(Fn &&fn) const
    {
      using R = std::invoke_result_t<Fn>;
      if (QThread::currentThread() == m_catalog->thread())
      {
        if constexpr (std::is_void_v<R>) { fn(); return; }
        else return fn();
      }
      if constexpr (std::is_void_v<R>)
        QMetaObject::invokeMethod(m_catalog, std::forward<Fn>(fn),
                                  Qt::BlockingQueuedConnection);
      else
      {
        R result{};
        QMetaObject::invokeMethod(m_catalog, [&result, &fn] { result = fn(); },
                                  Qt::BlockingQueuedConnection);
        return result;
      }
    }
```
- **Trigger Condition & Call Chain**:
  1. `catInvoke` uses `Qt::BlockingQueuedConnection` to block the calling background worker thread while executing `fn` on the GUI thread (`m_catalog->thread()`).
  2. If the GUI thread ever waits synchronously for the worker task (e.g. `future.get()`, `thread.wait()`, or a modal progress dialog blocking the event loop), a circular wait occurs: worker waits for GUI thread; GUI thread waits for worker.
- **Impact Analysis**: Immediate application freeze / deadlock during data import workflows.
- **Concrete Remediation**:
  Ensure catalog operations triggered by workers are purely asynchronous, or verify `QCoreApplication::eventDispatcher()` is active before allowing blocking invocations.
- **Empirical Verification Evidence**:
  Inspect `src/io/dataimportservice.h:220-243`.

---

### [RUNTIME-01] Unchecked Nullptr Dereference of `m_projectSvc` in MainWindow Operations
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：attach/workbench 全守卫；`aa2e2ba` 波次及后续）

- **Severity**: **P0** (Fatal Crash / Null Pointer Dereference)
- **Dimension**: Runtime & Error Safety
- **Exact Code Location**: `src/ui/paleomainwindow_attach.cpp`, Line 1411; `src/ui/paleomainwindow_workbench.cpp`, Lines 149–155, 574
- **Code Snippet**:
```cpp
// src/ui/paleomainwindow_attach.cpp:1411
auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(canvas->currentLayer()->id());

// src/ui/paleomainwindow_workbench.cpp:149-155
if (auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(old->id()))
...
if (auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(layer->id()))

// src/ui/paleomainwindow_workbench.cpp:574
auto *layer = m_projectSvc->project()->mapLayer(nativeId);
```
- **Trigger Condition & Call Chain**:
  1. In `QgisProjectService` (`src/qgis/qgisprojectservice.cpp:31, 37-40`), `m_project` is instantiated in the constructor (`m_project(new QgsProject(this))`). `m_projectSvc->project()` never returns `nullptr` as long as `m_projectSvc` itself is valid.
  2. However, in `src/ui/paleomainwindow_workbench.cpp:149-155, 574`, `m_projectSvc` is dereferenced directly without null-checking.
  3. In `PaleoMainWindow` (`src/ui/paleomainwindow.h`), `QgisProjectService *projectSvc` is an optional parameter (can be `nullptr` in modular testing or headless modes).
  4. In `paleomainwindow_attach.cpp:1409`, although `m_projectSvc` is checked, `canvas->currentLayer()` or `project()->layerTreeRoot()` traversal can be vulnerable during unloads.
  5. Dereferencing `m_projectSvc->project()` when `m_projectSvc == nullptr` triggers an immediate SIGSEGV.
- **Impact Analysis**: Instant fatal crash on map interactions or workbench tab switching in headless or modular configurations where `m_projectSvc` is null.
- **Concrete Remediation**:
  Add defensive null guards:
```cpp
if (!m_projectSvc || !m_projectSvc->project() || !m_projectSvc->project()->layerTreeRoot())
  return;
```
- **Empirical Verification Evidence**:
  Inspect `src/ui/paleomainwindow_attach.cpp:1409-1411` and `src/ui/paleomainwindow_workbench.cpp:149-155, 574`.

---

### [RUNTIME-02] Task Timeout and Use-After-Free in `PaleoAlgorithmWidget::~PaleoAlgorithmWidget()`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：超时不达即孤儿化+shared_ptr feedback；commit `57f0161`）

- **Severity**: **P1** (Use-After-Free / Background Thread Crash)
- **Dimension**: Runtime & Error Safety
- **Exact Code Location**: `src/qgis/qgisprocessingservice.cpp`, Lines 325–333, 410–417
- **Code Snippet**:
```cpp
// src/qgis/qgisprocessingservice.cpp:325-333
~PaleoAlgorithmWidget() override
{
  if (m_running && m_currentTask)
  {
    m_currentTask->cancel();
    m_currentTask->waitForFinished(5000);
  }
  delete m_feedback;
}
```
- **Trigger Condition & Call Chain**:
  1. User starts a long-running QGIS processing algorithm (e.g. large raster polygonize or Voronoi diagram) in `PaleoAlgorithmWidget`.
  2. The user closes the dialog. `~PaleoAlgorithmWidget()` calls `cancel()` and waits up to 5000ms.
  3. If the algorithm cannot reach a cancellation checkpoint within 5 seconds, `waitForFinished` returns false.
  4. Destructor proceeds to execute `delete m_feedback;` and destroys `m_context` (a member of `PaleoAlgorithmWidget`).
  5. The background worker thread continues running in QGIS and attempts to write to `m_feedback` or access `m_context`.
- **Impact Analysis**: Fatal segmentation fault on background worker thread after closing an unresponsive algorithm dialog.
- **Concrete Remediation**:
  Orphan the feedback object if the task fails to terminate within the timeout, deferring deletion until task completion:
```cpp
~PaleoAlgorithmWidget() override
{
  if (m_running && m_currentTask)
  {
    m_currentTask->cancel();
    if (!m_currentTask->waitForFinished(5000))
    {
      m_feedback = nullptr; // Detach feedback; avoid deleting under worker
      return;
    }
  }
  delete m_feedback;
  m_feedback = nullptr;
}
```
- **Empirical Verification Evidence**:
  Inspect `src/qgis/qgisprocessingservice.cpp:325-333`.

---

### [RUNTIME-03] `InflightCoalescer` Issue Reporting Desynchronization in `LasCache::load`
> **终态（2026-10-06，方向48 清账复核）**：仍存在→方向50 io-robustness（方向48 R0 证据：lascache.cpp InflightCoalescer 合乘者 issues 不回填；LasDoc 无 issues 字段）

- **Severity**: **P2** (Diagnostic Information Loss)
- **Dimension**: Runtime & Error Safety
- **Exact Code Location**: `src/io/lascache.cpp`, Lines 220–254; `src/io/inflight.h`, Lines 23–67
- **Code Snippet**:
```cpp
// src/io/lascache.cpp:220-254
  std::shared_ptr<LasDoc> fromDisk;
  const auto ticket = m_inflight.submit(
      fp, [this, &path, &fp, &issues, &fromDisk]() -> std::shared_ptr<LasDoc> {
        ...
        LasDoc doc = LasParser::parseDoc(path, issues);
```
- **Trigger Condition & Call Chain**:
  1. Threads A and B concurrently request the same LAS file from `LasCache::load`.
  2. Thread A's lambda executes; Thread B receives the coalesced ticket.
  3. Only Thread A's `issues` pointer is passed to `LasParser::parseDoc`.
  4. Thread B receives the shared `LasDoc`, but its `issues` list remains completely empty.
- **Impact Analysis**: Incomplete diagnostics for callers sharing an inflight load operation.
- **Concrete Remediation**:
  Store `QList<LasIssue> issues` directly inside `LasDoc` so all callers obtain identical issue diagnostics.
- **Empirical Verification Evidence**:
  Inspect `src/io/lascache.cpp:220-254`.

---

### [RUNTIME-04] Synchronous `delete timer;` Inside `QTimer::timeout` Handler
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（deleteLater+findChildren 清净断言；commit `e308372`）

- **Severity**: **P2** (Unsafe Object Deletion on Active Call Stack)
- **Dimension**: Runtime & Error Safety
- **Exact Code Location**: `src/services/seismictaskservice.cpp`, Lines 619–625
- **Code Snippet**:
```cpp
// src/services/seismictaskservice.cpp:619-625
      auto *timer = new QTimer(this);
      timer->setSingleShot(true);
      connect(timer, &QTimer::timeout, this, [timer, hit, stats, onFinished]() {
        delete timer; // SYNCHRONOUS DELETION DURING ACTIVE EVENT DELIVERY
        onFinished(true, hit, stats, QString());
      });
      timer->start(0);
```
- **Trigger Condition & Call Chain**:
  On cache hits in `startSectionExtraction`, a single-shot timer fires. The slot executes `delete timer;` directly.
- **Impact Analysis**: Deleting a `QObject` while its own `timerEvent` is on the call stack violates Qt lifecycle guidelines and can crash custom event dispatchers.
- **Concrete Remediation**:
  Replace `delete timer;` with `timer->deleteLater();`.
- **Empirical Verification Evidence**:
  Inspect `src/services/seismictaskservice.cpp:619-625`.

---

## 4. Dimension 2: Architecture Layering & Contracts

### [ARCH-01] Critical Layer Inversion: Data Layer (`src/io/dataimportservice`) Directly Couples to and Calls QGIS Wrapper Layer (`src/qgis/qgislayerservice`)
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：paleo_io 无 paleo_qgis 链接、layerDeclared 信号；commit `57f0161`）

- **Severity**: **P0** (Architecture Inversion / Target-Level Circularity)
- **Dimension**: Architecture Layering & Module Isolation
- **Exact Code Location**: 
  - `src/io/dataimportservice.h`, Line 285
  - `src/io/dataimportservice.cpp`, Lines 12, 105, 505, 606, 945, 1182
  - `CMakeLists.txt`, Line 307
- **Code Snippet**:
```cpp
// src/io/dataimportservice.h:285
class QgisLayerService; // Forward declaration
...
class DataImportService : public QObject {
...
private:
  QgisLayerService *m_layers; // <-- DATA LAYER HOLDS QGIS WRAPPER POINTER
};

// src/io/dataimportservice.cpp:1, 12, 606
// 层：数据
#include "dataimportservice.h"
#include "../qgis/qgislayerservice.h" // <-- DATA LAYER INCLUDES QGIS WRAPPER HEADER (Line 12)
...
// In importAsset() (Line 606 & Line 945):
LayerDeclaration decl;
decl.layerId = QStringLiteral("horizon.%1").arg(horizon);
if (!catInvoke([&] { return m_layers->declare(decl, &derr); })) // <-- DIRECT INVOCATION
  return fail(derr.isEmpty() ? QStringLiteral("manifest declare failed") : derr);
```
```cmake
# CMakeLists.txt:307
add_library(paleo_io STATIC ...)
target_link_libraries(paleo_io PUBLIC paleo_deps paleo_domain paleo_store paleo_qgis) # <-- DATA LAYER LINKS QGIS WRAPPER
```
- **Trigger Condition & Call Chain**:
  1. `DataImportService` is registered as a Data Layer service (`// 层：数据`).
  2. During import of spatial assets (such as horizon scatter files or grids), `DataImportService::importAsset` directly invokes `m_layers->declare(decl, &derr)` on `QgisLayerService` (`// 层：QGIS 封装`).
  3. To support this call, `CMakeLists.txt:307` links `paleo_qgis` into `paleo_io`.
- **Impact Analysis**:
  1. **Strict Hierarchy Inversion**: `docs/UI_LAYER_PLAN.md` §1 dictates: Data Layer is the foundational tier; QGIS Wrapper is an adapter above it; Data Layer must NEVER depend on higher layers.
  2. **Inter-Module Circularity**: `paleo_qgis` depends on Data Layer (`paleo_store`, `paleo_algorithms`); `paleo_io` (Data Layer) depends on `paleo_qgis`; `paleo_services` (Data Layer) depends on `paleo_io`. This forms an illegal circular dependency loop.
  3. **Unit Test Decoupling Failure**: Isolated unit tests for I/O parsers in `paleo_io` cannot be compiled or linked without pulling in QGIS GUI and runtime libraries.
- **Concrete Remediation**:
  Decouple `DataImportService` from `QgisLayerService`:
  1. `DataImportService` should only persist `LayerDeclaration` to `LayerManifest` (`src/metadata/layermanifest.h`, Data layer).
  2. Emit an asynchronous signal `signals: void layerDeclared(const LayerDeclaration &decl);` from `DataImportService`.
  3. The Workflow/Functional layer (`FolderImportWorkflow`) or the assembly root (`AppContext`) connects to `layerDeclared` and forwards it to `QgisLayerService::declare(decl)`.
  4. Remove `#include "../qgis/qgislayerservice.h"`, remove `m_layers`, and remove `paleo_qgis` from `paleo_io`'s `target_link_libraries` in `CMakeLists.txt`.
- **Empirical Verification Evidence**:
  Verify via `grep -n "qgislayerservice.h" src/io/dataimportservice.cpp` and inspect `CMakeLists.txt:307`.

---

### [ARCH-02] Subversive `void*` Pointer Smuggling and Manual Deletion via Dynamic QObject Properties in Functional Layer (`src/workflow/workflows.cpp`)
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：unique_ptr<ConstraintStore>；commit `57f0161`）

- **Severity**: **P1** (Severe Contract Violation / Type-Safety Destruction)
- **Dimension**: Architecture Layering & Interface Contracts (Cross-Ref: `MEM-01`)
- **Exact Code Location**: `src/workflow/workflows.cpp`, Lines 95–125, 711–715, 718–721
- **Code Snippet**:
```cpp
// src/workflow/workflows.cpp:95-125
const char kConstraintStoreProp[]      = "paleo.wf.constraintstore";      // void* (ConstraintStore*)
const char kOwnedConstraintStoreProp[] = "paleo.wf.owned_constraintstore"; // void* (ConstraintStore*)

ConstraintStore *constraintStoreOf( const QObject *wf )
{
  QVariant v = wf->property( kConstraintStoreProp );
  if ( v.isValid() && v.value<void *>() )
    return static_cast<ConstraintStore *>( v.value<void *>() );

  PaleoProjectStore *store = storeOf( wf );
  if ( store && !store->gpkgPath().isEmpty() )
  {
    QVariant ov = wf->property( kOwnedConstraintStoreProp );
    if ( ov.isValid() && ov.value<void *>() )
    {
      auto *owned = static_cast<ConstraintStore *>( ov.value<void *>() );
      if ( owned->gpkgPath() == store->gpkgPath() )
        return owned;
      delete owned;
    }
    auto *owned = new ConstraintStore( store->gpkgPath(), store );
    const_cast<QObject *>( wf )->setProperty( kOwnedConstraintStoreProp, QVariant::fromValue( static_cast<void *>( owned ) ) );
    return owned;
  }
  return nullptr;
}
```
- **Trigger Condition & Call Chain**:
  Developers smuggled non-QObject pointers through `QObject::setProperty` using `static_cast<void*>` and `const_cast<QObject*>`, hooking destruction to `&QObject::destroyed`.
- **Impact Analysis**: Subverts C++ strong typing, introduces memory corruption risks (see `MEM-01`), and violates architectural contracts against hidden coupling.
- **Concrete Remediation**:
  Store `std::unique_ptr<ConstraintStore>` as a private member of `ConstraintWorkflow`.
- **Empirical Verification Evidence**:
  Inspect `src/workflow/workflows.cpp:95-125, 711-721`.

---

### [ARCH-03] Coarse-Grained Umbrella Linker Leak: `paleo_deps` Transitively Propagates `QtWidgets` and All Source Includes to Data and Functional Modules
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：四层 tier 依赖模型；commit `b451452`）

- **Severity**: **P1** (Linker Contract Leak / Hidden Dependency Exposure)
- **Dimension**: Build & Linker Boundaries
- **Exact Code Location**: `CMakeLists.txt`, Lines 88–96, 232–235, 248, 254, 269, 293, 307, 320, 330, 343
- **Code Snippet**:
```cmake
# CMakeLists.txt:88-96, 232-235
add_library(paleo_qgis_iface INTERFACE)
target_link_libraries(paleo_qgis_iface INTERFACE ${QGIS_CORE_LIB} ${QGIS_GUI_LIB} ${QGIS_ANALYSIS_LIB}
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Xml Qt6::Sql Qt6::Concurrent Qt6::Svg Qt6::PrintSupport Qt6::Network Qt6::SerialPort Qt6::Positioning Qt6::UiTools Qt6::OpenGLWidgets Qt6::Pdf Qt6::PdfWidgets)

add_library(paleo_deps INTERFACE)
target_link_libraries(paleo_deps INTERFACE paleo_qgis_iface ${GDAL_LIB})
target_include_directories(paleo_deps INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

# Every data & functional module links paleo_deps PUBLIC:
target_link_libraries(paleo_domain PUBLIC paleo_deps paleo_sbm paleo_segyio paleo_glm)
target_link_libraries(paleo_algorithms PUBLIC paleo_deps paleo_domain)
target_link_libraries(paleo_store PUBLIC paleo_deps)
target_link_libraries(paleo_services PUBLIC paleo_deps paleo_io paleo_domain paleo_store)
target_link_libraries(paleo_workflow PUBLIC paleo_deps paleo_qgis paleo_store paleo_io paleo_services paleo_domain paleo_ai)
```
- **Trigger Condition & Call Chain**:
  `paleo_deps` bundles `paleo_qgis_iface` (including `Qt6::Widgets`, `Qt6::UiTools`, `QGIS_GUI_LIB`) and exposes `${CMAKE_SOURCE_DIR}/src` to every static library target.
- **Impact Analysis**:
  1. **Nullification of Linker Enforcement**: Linker cannot catch unauthorized `QtWidgets` usage in Data/Functional layers because `Qt6::Widgets` is linked to all targets.
  2. **Include Pollution**: Any module can include private headers from any other layer without build errors.
- **Concrete Remediation**:
  Split `paleo_deps` into tiered targets (`paleo_core_deps` without QtWidgets; `paleo_gui_deps` with QtWidgets). Only link `paleo_gui_deps` to `paleo_qgis`, `paleo_ui`, and `paleo_app`.
- **Empirical Verification Evidence**:
  Inspect `CMakeLists.txt:88-96, 232-235`.

---

### [ARCH-04] Tooling Guardrail Blind Spot: `tools/check_layering.py` Fails to Detect Data Layer Inversions into Higher Layers
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：data-qgis/data-functional 规则；commit `57f0161`）

- **Severity**: **P2** (Linter Guardrail Blind Spot / False Green)
- **Dimension**: Architecture Guardrails & Verification Tooling
- **Exact Code Location**: `tools/check_layering.py`, Lines 174–184; `tools/layering_vocab.json`, Lines 23–24
- **Code Snippet**:
```python
# tools/check_layering.py:174-184
dst = top_dir(norm)
if layer_dir == "ui":
    if dst == "io" and norm not in UI_IO_WHITELIST:
        violations.append(("ui-io-include", n, line.strip()))
    elif dst == "metadata" and norm not in UI_METADATA_WHITELIST:
        violations.append(("ui-metadata-include", n, line.strip()))
    elif dst == "algorithms":
        violations.append(("ui-algorithms-include", n, line.strip()))
elif layer_dir in NON_VIEW and dst == "ui":
    violations.append(("reverse-ui-include", n, line.strip()))
```
- **Trigger Condition & Call Chain**:
  The linter only verifies:
  1. From `src/ui/**` outward (checking whitelists for `io`, `metadata`, `algorithms`).
  2. From `NON_VIEW` outward (checking `dst == "ui"`).
  It has **zero rules** verifying whether Data layer modules (`domain`, `io`, `catalog`, `metadata`, `services`, `algorithms`) include `qgis/`, `workflow/`, or `linkage/`.
- **Impact Analysis**:
  Because of this blind spot, `src/io/dataimportservice.cpp:12` including `../qgis/qgislayerservice.h` (ARCH-01) passes `python3 tools/check_layering.py --strict` with exit code 0!
- **Concrete Remediation**:
  Add Data layer boundary rules to `tools/check_layering.py`:
```python
DATA_DIRS = {"domain", "catalog", "io", "metadata", "services", "algorithms"}
if layer_dir in DATA_DIRS and dst in {"qgis", "workflow", "linkage", "ui"}:
    violations.append(("data-layer-inversion", n, f"Data layer '{layer_dir}' forbidden from including '{dst}/'"))
```
- **Empirical Verification Evidence**:
  Run `python3 tools/check_layering.py --strict` (currently passes despite ARCH-01 being present in `src/io/dataimportservice.cpp`).

---

### [ARCH-05] Inverted / Out-of-Layer Dependency: Algorithm Layer (`src/algorithms/paleoalgorithms.cpp`) Couples to Catalog Storage Layer (`src/catalog/datacatalog.h`)
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（canonicalCrsWkt 参数化+LOCAL_GRID_WKT 注入，rg "catalog/" src/algorithms/ 零命中；commit `d7d650c`）

- **Severity**: **P2** (Boundary Violation / Undeclared CMake Link Dependency)
- **Dimension**: Architecture Layering & Module Isolation
- **Exact Code Location**: `src/algorithms/paleoalgorithms.cpp`, Lines 3, 75–76; `CMakeLists.txt`, Lines 250–254
- **Code Snippet**:
```cpp
// src/algorithms/paleoalgorithms.cpp:1-3, 75-76
// 层：数据
#include "paleoalgorithms.h"
#include "../catalog/datacatalog.h" // ALGORITHM INCLUDES CATALOG STORAGE
...
const auto local = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
const QByteArray wkt = ((crs == local || crs.toWkt()==local.toWkt()) ? DataCatalog::localGridCrsWkt() : ...);
```
- **Trigger Condition & Call Chain**:
  `paleoalgorithms.cpp` queries `DataCatalog::localGridCrsWkt()`. `CMakeLists.txt:250-254` builds `paleo_algorithms` without linking `paleo_store`. It compiles only because `localGridCrsWkt()` is an inline static method in `datacatalog.h`.
- **Impact Analysis**: Algorithmic calculations couple to database storage headers; moving `localGridCrsWkt()` to a `.cpp` breaks compilation immediately.
- **Concrete Remediation**:
  Move `localGridCrsWkt()` definition to `src/domain/types.h` or `src/domain/arearules.h`.
- **Empirical Verification Evidence**:
  Inspect `src/algorithms/paleoalgorithms.cpp:3, 75-76`.

---

### [ARCH-06] Side-Channel Property Smuggling: Dynamic String Properties Used as Covert Cross-Layer Data Channel between UI and Workflow Layers
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（typed 成员/访问器替代动态属性暗道；commit `6e7a1e2`）

- **Severity**: **P2** (Interface Degeneration / Fragile Untyped Coupling)
- **Dimension**: Interface Contracts & Hidden Coupling
- **Exact Code Location**: `src/workflow/mappingworkflow.cpp`, Lines 558–566; `src/ui/pages/constraintpage.cpp`, Lines 393–397
- **Code Snippet**:
```cpp
// src/workflow/mappingworkflow.cpp:558-566
setProperty( "paleo.thickness.samples", rows );
setProperty( "paleo.thickness.message", message );
if ( m_constraints ) {
  m_constraints->setProperty( "paleo.thickness.samples", rows );
  m_constraints->setProperty( "paleo.thickness.message", message );
}

// src/ui/pages/constraintpage.cpp:393-397
auto *wf = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
const QVariantList rows = wf ? wf->property( "paleo.thickness.samples" ).toList() : QVariantList();
const QString message = wf ? wf->property( "paleo.thickness.message" ).toString() : QString();
```
- **Trigger Condition & Call Chain**:
  `MappingWorkflow` transmits thickness samples to `ConstraintPage` by injecting untyped string properties into `ConstraintWorkflow`, which `ConstraintPage` reads on `showEvent()`.
- **Impact Analysis**: Bypasses typed signal-slot interfaces. Typos fail silently at runtime.
- **Concrete Remediation**:
  Replace dynamic property polling with strongly typed signal: `signals: void thicknessSamplesUpdated(const QVariantList &rows, const QString &message);`.
- **Empirical Verification Evidence**:
  Inspect `src/workflow/mappingworkflow.cpp:558-566` and `src/ui/pages/constraintpage.cpp:393-397`.

---

### [ARCH-07] Modularity Placement Smell: Interactive `QgsMapTool` with RubberBand Visualization Located in Functional Layer (`src/linkage/seismicsectiontool.h`)
> **终态（2026-10-06，方向48 清账复核）**：已修（移至 src/qgis/seismicsectiontool，QGIS 封装层 QtWidgets 豁免；PR #193 `0a02a0e`）

- **Severity**: **P3** (Modularity Smell / Responsibility Blur)
- **Dimension**: Architecture Layering & Module Isolation
- **Exact Code Location**: `src/linkage/seismicsectiontool.h`, Lines 1–36; `src/linkage/seismicsectiontool.cpp`, Lines 9–20, 89
- **Code Snippet**:
```cpp
// src/linkage/seismicsectiontool.h:1-20
// 层：功能
#pragma once
#include <qgsmaptool.h>
#include <qgspointxy.h>
#include <qgsrubberband.h>

class SeismicSectionTool : public QgsMapTool {
  Q_OBJECT
...
protected:
  void canvasPressEvent(QgsMapMouseEvent *e) override;
  void canvasMoveEvent(QgsMapMouseEvent *e) override;
private:
  QgsRubberBand *m_rubberBand = nullptr;
};
```
- **Trigger Condition & Call Chain**:
  `SeismicSectionTool` manages canvas mouse events, cursor icons, and rubberband rendering. It is tagged `// 层：功能` in `src/linkage`.
- **Impact Analysis**: "功能只编排不画像素" contract is violated. All other canvas tools reside in `src/ui/maptools/`.
- **Concrete Remediation**:
  Move `SeismicSectionTool` to `src/ui/maptools/` and emit coordinates to `linkage`.
- **Empirical Verification Evidence**:
  Inspect `src/linkage/seismicsectiontool.h:1-36`.

---

### [ARCH-08] Test Suite Failure in UI Tab Navigation: `tst_panels.cpp:4736` Failure under Headless Offscreen Platform
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（同 TEST-07，nextInFocusChain 链断言；commit `ca18f88`）

- **Severity**: **P1** (Test Suite Blocker)
- **Dimension**: Architecture Layering & Automation Verification (Cross-Ref: `TEST-07`)
- **Exact Code Location**: `tests/tst_panels.cpp`, Line 4736; `build/Testing/Temporary/LastTest.log`, Lines 151–153
- **Code Snippet**:
```
FAIL!  : TestPanels::dataops_d6_tabFocusTraversalReachesNewControls()
         'visited.contains(QStringLiteral("filterDimCombo")) || visited.contains(QStringLiteral("filterValueCombo"))' returned FALSE. ()
   Loc: [/home/kevin/projects/paleo_workstation/tests/tst_panels.cpp(4736)]
```
- **Trigger Condition & Call Chain**:
  Under `QT_QPA_PLATFORM=offscreen`, tab focus navigation via `QTest::keyClick(w, Qt::Key_Tab)` fails to reach `filterDimCombo` because the options pane was not exposed or laid out in offscreen mode.
- **Impact Analysis**: Blocks automated test runs from achieving a 100% green exit status.
- **Concrete Remediation**:
  Ensure widget exposure prior to focus navigation:
```cpp
panel->show();
panel->activateWindow();
QTest::qWaitForWindowExposed(panel);
```
- **Empirical Verification Evidence**:
  Inspect `build/Testing/Temporary/LastTest.log:151-153` and `tests/tst_panels.cpp:4736`.

---

## 5. Dimension 3: Business & Boundary Robustness

### [BIZ-01] `LasParser::parseDepthRange` Hardcoded 64-Curve Stack Buffer Truncates Logs Silently
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：动态 nCurves 向量；commit `e69ae58`/`34df4ea`）

- **Severity**: **P1** (Data Truncation / Silent Data Loss)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/io/lasparser.cpp`, Lines 793–823
- **Code Snippet**:
```cpp
// src/io/lasparser.cpp:793-823
      if (eol > i)
      {
        double rowVals[64];
        int col = 0;
        qint64 p = i;
        while (p < eol && col < 64)
        {
          while (p < eol && (chunk.at(p) == ' ' || chunk.at(p) == '\t'))
            ++p;
          if (p >= eol) break;
          qint64 q = p;
          while (q < eol && chunk.at(q) != ' ' && chunk.at(q) != '\t')
            ++q;
          bool ok = false;
          rowVals[col++] =
              QByteArray::fromRawData(chunk.constData() + p, static_cast<int>(q - p)).toDouble(&ok);
          if (!ok) rowVals[col - 1] = nan();
          p = q;
        }
        if (col > 0)
        {
          const double depth = rowVals[0];
          ...
          if (depth == depth && depth >= fromDepth && depth <= toDepth)
            for (int c = 0; c < nCurves; ++c)
            {
              const double v = c < col ? rowVals[c] : nan();
              cols[c].values.append(v == nullValue || v != v ? nan() : v);
            }
        }
      }
```
- **Trigger Condition & Call Chain**:
  1. A user loads an industry wireline/LWD LAS file with $>64$ curves (common in modern multi-array sonic, NMR, or geochemical logs).
  2. The application requests a depth range via `LasParser::parseDepthRange`.
  3. The parser loops while `col < 64`, stopping at 64 tokens even though `nCurves > 64`.
  4. For all curve indices $c \ge 64$, `c < col` evaluates to false, appending `nan()` for every row.
- **Impact Analysis**: Complete silent data loss for all curves beyond the 64th curve. Downstream correlation panels and analytics display blank curves without warning.
- **Concrete Remediation**:
  Dynamically size a reusable vector to `nCurves`:
```cpp
std::vector<double> rowVals(nCurves, nan());
// parse into rowVals while col < nCurves
```
- **Empirical Verification Evidence**:
  Inspect `src/io/lasparser.cpp:793-823`. Parse a synthetic LAS file with 70 curves; curves 64-69 will be all NaN.

---

### [BIZ-02] `LasParser` BOM Offset Miscalculation in `parseRange` and `parseDepthRange`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：bomBytes 扫描期落位；commit `e69ae58`）

- **Severity**: **P1** (File Offset Desynchronization / Data Corruption)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/io/lasparser.cpp`, Lines 605–607, 653–656, 744–763
- **Code Snippet**:
```cpp
// Lines 605-607 in scanHeaderFromFile():
const QByteArray head = f.read(4 * 1024 * 1024); // Reads 4MB; cursor at 4MB!
*header = scanHeaderBytes(head, issues);

// Lines 653-656 in parseRange():
HeaderScanResult header;
if (!scanHeaderFromFile(f, &header, error, issues, path)) return false;
const qint64 asciiOff = header.asciiDataOffset + bomAdjustment(f.peek(3)); // PEEKS AT 4MB!

// Lines 744-763 in parseDepthRange():
HeaderScanResult header;
if (!scanHeaderFromFile(f, &header, error, issues, path)) return false;
qint64 pos = header.asciiDataOffset + bomAdjustment(f.peek(3)); // PEEKS AT 4MB!
```
- **Trigger Condition & Call Chain**:
  1. `scanHeaderFromFile` reads 4MB from `f`, leaving the file read pointer at offset 4194304 (or EOF).
  2. `scanHeaderBytes` detects the BOM and computes `header.asciiDataOffset` relative to stripped coordinates.
  3. `parseRange` / `parseDepthRange` invokes `bomAdjustment(f.peek(3))` on `f` without rewinding to byte 0.
  4. `f.peek(3)` reads 3 bytes from the 4MB position instead of byte 0, fails to match `\xEF\xBB\xBF`, and returns 0 instead of 3.
  5. `pos` seeks 3 bytes too early (into `~A` header debris).
- **Impact Analysis**: The first ASCII data row is parsed with 3 bytes of corrupted header remnants, corrupting the first depth sample or causing NaN load failures.
- **Concrete Remediation**:
  Store `bool hasBom` directly in `HeaderScanResult` during header scan, avoiding secondary file peeking:
```cpp
const qint64 asciiOff = header.asciiDataOffset + (header.hasBom ? 3 : 0);
```
- **Empirical Verification Evidence**:
  Inspect `src/io/lasparser.cpp:605-607, 653-656, 763`.

---

### [BIZ-03] `SegyReader::scanParallel` Offset Desynchronization & Trace Duplication on Resume
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：m_index+m_badTraceOffsets 计位；PR #193 `0a02a0e`）

- **Severity**: **P1** (Trace Duplication / Index Corruption)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/io/segyreader.cpp`, Lines 847–851, 875–888, 941–949
- **Code Snippet**:
```cpp
// Lines 847-851:
if (traceNs < 0 || traceNs > ns) {
  sh.bad.append(offset); // Skipped bad trace
  continue;
}
// Lines 875-888:
for (const Shard &sh : shards) {
  for (int i = 0; i < sh.inlines.size(); ++i)
    m_index.append(IndexEntry{sh.inlines.at(i), sh.xlines.at(i), sh.offsets.at(i)});
  m_badTraceOffsets += sh.bad;
  if (!sh.ok) {
    m_lastScanPartial = true;
    m_scannedOffset = firstTraceOffset + static_cast<qint64>(m_index.size()) * traceSize;
    if (error) *error = sh.err;
    return false;
  }
}
```
- **Trigger Condition & Call Chain**:
  1. A SEG-Y file with bad traces in Shard 0 skips $K$ traces (`sh.bad.append(offset)`).
  2. Shard 1 is interrupted or cancelled (`!sh.ok`).
  3. `m_scannedOffset` is calculated as `firstTraceOffset + m_index.size() * traceSize`.
  4. Because $K$ bad traces were skipped, `m_index.size()` is $N - K$, rolling `m_scannedOffset` backwards by $K \times \text{traceSize}$ bytes.
  5. When `resumeScan` runs, it rescans and duplicates the last $K$ valid traces from Shard 0.
- **Impact Analysis**: Duplicate traces in the seismic index, corrupting CDP geometry and slice extraction.
- **Concrete Remediation**:
  Include bad trace count in offset calculation:
```cpp
m_scannedOffset = firstTraceOffset + static_cast<qint64>(m_index.size() + m_badTraceOffsets.size()) * traceSize;
```
- **Empirical Verification Evidence**:
  Inspect `src/io/segyreader.cpp:847-851, 884`.

---

### [BIZ-04] `SegyReader::openCached` Inverted Probe Logic Rejects Valid 3D Seismic Datasets
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：anyDifferent 探针逻辑；PR #193 `0a02a0e`）

- **Severity**: **P1** (Severe Performance Regression / Fallback to Sequential Scan)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/io/segyreader.cpp`, Lines 1108–1132
- **Code Snippet**:
```cpp
// src/io/segyreader.cpp:1108-1132
const qint64 probes[] = {0, traceCountTotal / 3, traceCountTotal * 2 / 3, traceCountTotal - 1};
qint32 firstInline = 0;
bool haveFirst = false;
for (qint64 t : probes)
{
  ...
  const qint32 v = beI32(h + sidx.inlineWordOffset);
  if (!haveFirst) {
    firstInline = v;
    haveFirst = true;
  }
  else if (v == firstInline) {
    eligible = false; // 全程不变——ordinal 方言
    break;
  }
}
```
- **Trigger Condition & Call Chain**:
  1. A 3D survey has wide inlines (e.g. 5,000 crosslines per inline).
  2. Probe 0 (trace 0) and Probe 1 (trace $N/3$) belong to the same inline (`v == firstInline`).
  3. Line 1128 flags `eligible = false` and aborts parallel scan, falsely classifying the valid 3D survey as an ordinal dialect.
- **Impact Analysis**: Multi-gigabyte 3D seismic files take 4x–8x longer to open due to false fallback to single-threaded sequential scan.
- **Concrete Remediation**:
  Only mark `eligible = false` if *all* probes are identical:
```cpp
bool anyDifferent = false;
for (qint64 t : probes) {
  ...
  if (!haveFirst) { firstInline = v; haveFirst = true; }
  else if (v != firstInline) { anyDifferent = true; }
}
if (!anyDifferent) eligible = false; // All probes identical -> ordinal dialect
```
- **Empirical Verification Evidence**:
  Inspect `src/io/segyreader.cpp:1108-1132`.

---

### [BIZ-05] `SegyReader::decodeTrace` Missing IEEE 754 Non-Finite/NaN/Inf Validation

> **终态（2026-10-06，方向48 清账复核）**：方向48 R0 曾移交方向50（当时 decodeTrace 无 isfinite 清洗）；方向50已修 `1719684`，读面/SDK计数与手工缺失掩码对拍见下述证据。

- **Direction 50 status (2026-10-06): Resolved, `1719684`.** All decoded non-finite IEEE/IBM samples become quiet NaN and are counted in the request read report; finite samples retain their bit patterns. SDK direct, voxel, mmap, planned and SF3C paths share the missing-value contract and expose quality counts, including cancellation after decoding. `tst_io_robustness` / `tst_io_seismicquality` assert manual missing-mask attribute/inversion results and persisted reports. The historical suggestion to replace samples with zero is superseded: zero fabricates an amplitude and changes downstream missing-value semantics.
- **Severity**: **P2** (Data Corruption / Numerical Instability)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/io/segyreader.cpp`, Lines 564–573
- **Code Snippet**:
```cpp
// src/io/segyreader.cpp:564-573
  if (m_formatCode == 5) {
    for (int i = 0; i < ns; ++i) {
      const quint32 be = qFromBigEndian<quint32>(p + i * 4);
      float v;
      std::memcpy(&v, &be, sizeof(float));
      t.samples[i] = v; // Unsanitized NaN / Inf
    }
  }
```
- **Trigger Condition & Call Chain**:
  Field recordings with non-finite or NaN bit patterns inject NaNs into `t.samples`, causing downstream AGC and OpenGL texture generators to produce black or blank displays.
- **Impact Analysis**: Visual corruption in seismic section displays.
- **Concrete Remediation**:
  Sanitize samples: `t.samples[i] = std::isfinite(v) ? v : 0.0f;`.
- **Empirical Verification Evidence**:
  Inspect `src/io/segyreader.cpp:564-573`.

---

### [BIZ-06] `TimeDepthTool` and `parseWellTopsText` Depth Null Sentinel Inconsistencies

> **终态（2026-10-06，方向48 清账复核）**：方向48 R0 曾移交方向50（当时仅 -99999 哨兵、深度缺失行仍 append）；方向50已修 `8c67a12` / `f7fe231`，统一词表、逐列拒收及消费报告见下述证据。

- **Direction 50 status (2026-10-06): Resolved, `8c67a12` / `f7fe231`.** Domain vocabulary exactly recognizes -99999/-999.25/-9999/-999; required blank/non-numeric/non-finite depths reject the row with physical line/column reasons. Both time-depth interpolation directions filter the same vocabulary and count ignored rows. Import reports expose sentinel/blank/invalid-cell counts through version metadata and the import ledger, including duplicate skips. File-level top rejection reasons also reach section/workbench reports through ProjectDataFacade; missing TVDSS retains valid MD/TVD and becomes NaN, while a row with no usable depth is rejected. Explicit LAS NULL values remain authoritative. The historical suggestion to reject every negative value is superseded: valid negative coordinates/elevations remain usable.
- **Severity**: **P2** (Boundary Robustness Defect)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/io/timedeptool.cpp`, Lines 25–28; `src/io/wellfileparsers.cpp`, Lines 95–118
- **Code Snippet**:
```cpp
// src/io/timedeptool.cpp:25-28
if ((useMd ? !r.hasMd : !r.hasTvd) || !std::isfinite(key) ||
    !std::isfinite(r.timeMs) || r.timeMs <= kNullSentinel + 0.5) // only checks -99999.0
  continue;

// src/io/wellfileparsers.cpp:95-118
if (parseColumn(t.at(2), &r.md)) r.hasMd = true;
tops.append(r); // Appended even if r.hasMd is false!
```
- **Trigger Condition & Call Chain**:
  `TimeDepthTool` only filters `-99999.0`, treating standard industry `-999.25` or `-9999.0` as valid travel times. In `wellfileparsers.cpp`, rows failing depth parsing are appended as depthless tops.
- **Impact Analysis**: Corrupts well tops catalog and section alignment with depth 0 tops.
- **Concrete Remediation**:
  Filter invalid tops (`if (r.hasMd) tops.append(r);`) and expand null sentinel checking (`r.timeMs < 0.0`).
- **Empirical Verification Evidence**:
  Inspect `src/io/timedeptool.cpp:25-28` and `src/io/wellfileparsers.cpp:95-118`.

---

### [BIZ-07] `RegistrationWorkflow` and `geoAffineTransformFile` Lack Non-Finite/Zero Scale Defense
> **终态（2026-10-06，方向48 清账复核）**：仍存在→方向50 io-robustness（方向48 R0 证据：sx/sy/rotDeg 无有限性校验）

- **Severity**: **P2** (Boundary Vulnerability)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/workflow/registration.cpp`, Lines 71–85; `src/io/geojsonaffine.cpp`, Lines 12–20
- **Code Snippet**:
```cpp
// src/workflow/registration.cpp:71-76
p.sx = params.value(QStringLiteral("sx"), 1.0).toDouble();
p.sy = params.value(QStringLiteral("sy"), 1.0).toDouble();
// src/io/geojsonaffine.cpp:12-20: Transforms coordinates without checking sx > 0
```
- **Trigger Condition & Call Chain**:
  Input scale factors of 0 or NaN collapse vector geometries to single points or produce GeoJSON `[null, null]` coordinates.
- **Impact Analysis**: Creates unreadable or degenerate GeoJSON files registered in the catalog.
- **Concrete Remediation**:
  Validate `p.sx > 1e-6 && p.sy > 1e-6 && std::isfinite(p.rotDeg)`.
- **Empirical Verification Evidence**:
  Inspect `src/workflow/registration.cpp:71-85`.

---

### [BIZ-08] `PaleoVertexTool::deleteVertexAtMapPoint` Degenerates Polygons on Multi-Coincident Vertex Deletion
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：defense-in-depth 逐环最小数校验；commit `eff046e`）

- **Severity**: **P1** (Topological Geometry Corruption)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/ui/edittools/vertexeditortools.cpp`, Lines 482–486, 566–614
- **Code Snippet**:
```cpp
// src/ui/edittools/vertexeditortools.cpp:482-486, 601-603
const int minimum = gt == Qgis::GeometryType::Line ? 3
                    : gt == Qgis::GeometryType::Polygon ? 5 : 2;
return ringVertices >= minimum;
...
const int remainingCount = mutated.constGet()->vertexCount( part, ring );
if ( remainingCount >= 2 ) { ... }
```
- **Trigger Condition & Call Chain**:
  1. A polygon has coincident pinch points on the same ring (e.g. 5 vertices: $V_0, V_1, V_2, V_3, V_4=V_0$, where $V_1$ and $V_3$ coincide).
  2. User clicks the coordinate to delete topologically.
  3. `ringFitsDelete` checks each vertex individually against `ringVertices >= 5` (evaluating to true for both).
  4. Both vertices are deleted sequentially, reducing the ring to 3 vertices total (only 2 distinct coordinates).
  5. Closure restoration moves the closing vertex and writes the degenerate 2-point polygon ring into the layer.
- **Impact Analysis**: Corrupts polygon layers with degenerate geometries, crashing subsequent QGIS topology checks and polygonization algorithms.
- **Concrete Remediation**:
  Track cumulative vertices deleted *per ring* and enforce `ringVertices - verticesToDeleteInRing >= minimum`.
- **Empirical Verification Evidence**:
  Inspect `src/ui/edittools/vertexeditortools.cpp:482-486, 566-614`.

---

### [BIZ-09] `PaleoVertexTool::coincidentVertices` 1-Nanometer Tolerance Radius Rejects Real-World Shared Boundaries
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（kTopoCandidateEnvelope 具名常量+量纲注释+包络上界钉死；commit `c814130`）

- **Severity**: **P2** (Topological Tolerance Fragility)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/ui/edittools/vertexeditortools.cpp`, Lines 1053–1064
- **Code Snippet**:
```cpp
// src/ui/edittools/vertexeditortools.cpp:1053-1064
const QList<QgisTopologicalIndex::VertexHit> hits =
    mTopoIndex->verticesNear( layerPoint, 1e-9, mTopoEditing && mCrossLayerTopology );
```
- **Trigger Condition & Call Chain**:
  Neighboring polygons with sub-micrometer floating-point coordinate discrepancies ($10^{-8}$ meters) fail the $10^{-9}$ meter search radius, tearing shared boundaries during topological edits.
- **Impact Analysis**: Silently breaks topological boundary tracking, creating slivers and gaps.
- **Concrete Remediation**:
  Use canvas snapping tolerance or layer resolution (e.g. $10^{-4}$ meters) instead of hardcoded $10^{-9}$ meters.
- **Empirical Verification Evidence**:
  Inspect `src/ui/edittools/vertexeditortools.cpp:1053-1064`.

---

### [BIZ-10] `ConstraintIDWAlgorithm` & `PaleoDistanceTransformAlgorithm` Fatal Abort on Engineering CRS
> **终态（2026-10-06，方向48 清账复核）**：仍存在→方向50 io-robustness（方向48 R0 证据：g.transform 前无 xform->isValid() 前置）

- **Severity**: **P2** (Processing Pipeline Failure)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/algorithms/paleoalgorithms.cpp`, Lines 258–285; `src/algorithms/distancetransform.cpp`, Lines 138–166
- **Code Snippet**:
```cpp
// src/algorithms/paleoalgorithms.cpp:258-285
std::unique_ptr<QgsCoordinateTransform> xform;
if ( from.isValid() && to.isValid() && from != to ) {
  xform = std::make_unique<QgsCoordinateTransform>( from, to, context.transformContext() );
}
...
const Qgis::GeometryOperationResult tr = g.transform( *xform );
if ( tr != Qgis::GeometryOperationResult::Success )
  throw QgsProcessingException( QStringLiteral( "Constraint geometry failed to transform..." ) );
```
- **Trigger Condition & Call Chain**:
  Local engineering CRS (`ENGCRS`) lacks a geodetic datum; PROJ cannot transform between `ENGCRS` and standard CRS. `xform->isValid()` is false. Calling `g.transform(*xform)` fails and throws `QgsProcessingException`.
- **Impact Analysis**: Crashes mapping workflows when constraints use local engineering grids.
- **Concrete Remediation**:
  Check `xform->isValid()` before calling `g.transform(*xform)`.
- **Empirical Verification Evidence**:
  Inspect `src/algorithms/paleoalgorithms.cpp:258-285`.

---

### [BIZ-11] Single-Factor Well Distance Algorithms Emit Unprojected GeoTIFFs
> **终态（2026-10-06，方向48 清账复核）**：仍存在→方向50 io-robustness（方向48 R0 证据：welldist/distancetransform 无 GDALSetProjection 旁路——注：两者已走共享写口写 CRS，残余面=非 LOCAL_GRID_WKT 直调场景）

- **Severity**: **P2** (Missing Spatial Metadata)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/algorithms/welldist.cpp`, Lines 34–53; `src/algorithms/distancetransform.cpp`, Lines 42–61
- **Code Snippet**:
```cpp
// src/algorithms/welldist.cpp:34-53
GDALDatasetH createFloatRaster( const QString &outPath, int nCols, int nRows,
                                const double geoTransform[6], double nodata )
{
  ...
  GDALSetRasterNoDataValue( band, nodata );
  return ds; // GDALSetProjection is NEVER called!
}
```
- **Trigger Condition & Call Chain**:
  `createFloatRaster` in `welldist.cpp` and `distancetransform.cpp` sets geo-transform and nodata, but never calls `GDALSetProjection`.
- **Impact Analysis**: Output GeoTIFFs have no CRS projection metadata, causing warnings and spatial misalignment when loaded onto reprojected QGIS canvases.
- **Concrete Remediation**:
  Pass source CRS into `createFloatRaster` and call `GDALSetProjection(ds, wkt)`.
- **Empirical Verification Evidence**:
  Inspect `src/algorithms/welldist.cpp:34-53` and compare with `src/algorithms/paleoalgorithms.cpp:71-80`.

---

### [BIZ-12] `TimeDepthModel` Defensive Check Ordering & Potential Undefined Behavior

> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：empty 检查先行；commit `9a9cec1`）

- **Direction 50 R0 status (2026-10-06): Already fixed, `d2ca076`.** The empty-point check was reordered before strict bounds access. This original TimeDepthModel issue is distinct from the request's XLSX/coordinate-table label BIZ-12; workbook evidence is tracked as IO-R0-12 below.
- **Severity**: **P2** (Defensive Brittleness / Fragile Check Ordering — Calibrated from P0)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/domain/seismic/timedepthmodel.cpp`, Lines 61–70, 108–116
- **Code Snippet**:
```cpp
// src/domain/seismic/timedepthmodel.cpp:61-70
double TimeDepthModel::DepthToTwtMs(double depthM) const {
  if (!std::isfinite(depthM) ||
      (m_strict &&
       (depthM < m_points.front().depthM || depthM > m_points.back().depthM)))
    return std::numeric_limits<double>::quiet_NaN();

  if (m_points.empty()) {
    return (depthM * 2000.0) / m_velocity;
  }
...
// Lines 108-116:
double TimeDepthModel::TwtMsToDepth(double twtMs) const {
  if (!std::isfinite(twtMs) || (m_strict && (twtMs < m_points.front().timeMs ||
                                             twtMs > m_points.back().timeMs)))
    return std::numeric_limits<double>::quiet_NaN();

  if (m_points.empty()) { ... }
```
- **Trigger Condition & Call Chain**:
  1. **Theoretical Defect**: In isolation, calling `m_points.front()` on an empty `std::vector` is undefined behavior (read out of bounds).
  2. **Adversarial Invariance Proof**: Challenger 1 established that under class invariants, `m_strict == true` is **ONLY** set in `setCheckshots()` (`src/domain/seismic/timedepthmodel.cpp:26-48`), which strictly requires `if (points.size() < 2) return false;`. In the constructor, `m_strict` initializes to `false`. In `setPoints()`, `m_strict` is explicitly set to `false`. Therefore, the invariant $\mathbf{m\_strict == true} \implies \mathbf{m\_points.size() \ge 2}$ holds across all execution paths.
  3. **Unreachable P0 Crash**: Because of short-circuit boolean evaluation in `if (!std::isfinite(depthM) || (m_strict && (depthM < m_points.front().depthM ...)))`, when `m_points.empty()` is true, `m_strict` is guaranteed `false`, so `m_points.front()` is never evaluated. The state `(m_strict == true && m_points.empty() == true)` is logically unreachable via public APIs.
  4. **Defensive Rationale for P2 Calibration**: Relying on disparate methods to maintain vector size guarantees for `front()` access in mathematical transformation routines is defensively fragile. If future internal helpers set `m_strict` or clear `m_points`, a crash would occur. Defensively, `m_points.empty()` should be evaluated first.
- **Impact Analysis**: Sub-optimal defensive ordering. Unreachable crash via public APIs, but fragile against future internal refactoring.
- **Concrete Remediation**:
  Check `m_points.empty()` *first*:
```cpp
if (m_points.empty()) {
  return m_strict ? std::numeric_limits<double>::quiet_NaN()
                  : (depthM * 2000.0) / m_velocity;
}
if (!std::isfinite(depthM) ||
    (m_strict && (depthM < m_points.front().depthM || depthM > m_points.back().depthM)))
  return std::numeric_limits<double>::quiet_NaN();
```
- **Empirical Verification Evidence**:
  Inspect `src/domain/seismic/timedepthmodel.cpp:26-48, 61-70, 108-116`.

---

### [BIZ-13] `ConstraintIDWAlgorithm` Generates NaNs Due to Floating-Point Overflow Near Sample Points
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：d2<1e-12 epsilon 门；commit `09c8db2`）

- **Severity**: **P2** (Numerical Degeneration / NaN Hole Artifacts)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/algorithms/paleoalgorithms.cpp`, Lines 536–550
- **Code Snippet**:
```cpp
// src/algorithms/paleoalgorithms.cpp:536-550
        if ( d2 == 0.0 ) {
          exact = true;
          exactValue = s.z;
          break;
        }
        const double w = 1.0 / d2; // power = 2
        weightSum += w;
        valueSum += w * s.z;
...
      rowBuf[c] = exact ? static_cast<float>( exactValue )
                  : weightSum > 0.0 ? static_cast<float>( valueSum / weightSum ) : PALEO_NODATA;
```
- **Trigger Condition & Call Chain**:
  When a grid cell is extremely close to a sample point ($d^2 < 10^{-155}$), `d2 == 0.0` is false, but `w = 1.0 / d2` overflows IEEE double to `+infinity`. `valueSum / weightSum` computes $\infty / \infty = \text{NaN}$.
- **Impact Analysis**: Interpolated rasters develop NaN holes at sample locations.
- **Concrete Remediation**:
  Use an epsilon threshold: `if (d2 < 1e-12) { exact = true; exactValue = s.z; break; }`.
- **Empirical Verification Evidence**:
  Inspect `src/algorithms/paleoalgorithms.cpp:536-550`.

---

### [BIZ-14] `SectionWorkbench::sectionWells` Fragile Depth Unit Filtering Aborts Valid Well Logs

> **终态（2026-10-06，方向48 清账复核）**：方向48 R0 曾移交方向50（当时仅认 M/FT/F）；方向50已修 `8c67a12`，规范别名表与真实 LAS 消费证据见下述说明。

- **Direction 50 status (2026-10-06): Resolved, `8c67a12`.** Shared domain vocabulary normalizes case/whitespace and accepts M/METER/METERS/METRE/METRES and FT/F/FOOT/FEET; foot scaling remains exactly 0.3048. Tests pin the vocabulary and import real LAS aliases into SectionWorkbench. Unknown or empty units produce an explicit alignment reason and do not block other log files. The historical suggestion to guess metres for an empty unit is superseded by the honest unknown-unit contract.
- **Severity**: **P2** (Defensive Robustness Omission)
- **Dimension**: Business & Boundary Robustness
- **Exact Code Location**: `src/workflow/sectionworkbench.cpp`, Lines 187–199
- **Code Snippet**:
```cpp
// src/workflow/sectionworkbench.cpp:187-199
const auto unit = depths.unit.trimmed().toUpper();
if (unit == "FT" || unit == "F")
  scale = .3048;
else if (unit != "M") {
  out.alignmentStatus += tr(" · 深度单位未知，曲线未叠加");
  break;
}
```
- **Trigger Condition & Call Chain**:
  Standard LAS files formatted with units `"METER"`, `"METRE"`, `"METERS"`, or blank unit string fail `unit != "M"`, aborting curve alignment.
- **Impact Analysis**: Well curves fail to display on cross-sections for standard valid well logs.
- **Concrete Remediation**:
  Accept standard metric synonyms: `if (unit == "M" || unit == "METER" || unit == "METERS" || unit == "METRE" || unit.isEmpty()) scale = 1.0;`.
- **Empirical Verification Evidence**:
  Inspect `src/workflow/sectionworkbench.cpp:187-199`.

---

## 6. Dimension 4: Test Coverage & Assertion Integrity

### [TEST-01] Build Directory Drift Leaves 7 Newly Merged Test Targets Unbuilt & Unexecuted
> **终态（2026-10-06，方向48 清账复核）**：已消解（方向48 复核：fresh configure 后 7 套件全注册，ctest -N 证据；残余风险=构建目录纪律，非代码缺陷）

- **Severity**: **P1** (Test Suite Execution Blind Spot)
- **Dimension**: Test Coverage & Automation Verification
- **Exact Code Location**: `CMakeLists.txt`, Lines 717–720; `cmake/extra-deepen-*.cmake`; `build/CTestTestfile.cmake`
- **Code Snippet**:
```cmake
# CMakeLists.txt:717-720
file(GLOB _paleo_extra CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/extra-*.cmake")
foreach(_f IN LISTS _paleo_extra)
  include(${_f})
endforeach()
```
- **Trigger Condition & Call Chain**:
  1. Feature track `wave/deepen-perf` was merged into `master`, introducing `cmake/extra-deepen-a.cmake`, `b.cmake`, `c.cmake`, `d.cmake`.
  2. The local `build/` directory was configured prior to the merge.
  3. Consequently, the following 7 test executables were never registered into `build/CTestTestfile.cmake` and never built:
     - `tst_catalog_scale` (`cmake/extra-deepen-b.cmake:15`)
     - `tst_correlation_async` (`cmake/extra-deepen-b.cmake:6`)
     - `tst_importqueue_progress` (`cmake/extra-deepen-b.cmake:9`)
     - `tst_pyramid_consume` (`cmake/extra-deepen-b.cmake:12`)
     - `tst_sectionlifecycle` (`cmake/extra-deepen-d.cmake:12`)
     - `tst_seismic_realarea` (`cmake/extra-deepen-a.cmake:9`)
     - `tst_wellcomposite_shell` (`cmake/extra-deepen-d.cmake:10`)
- **Impact Analysis**: The automated test suite executes an incomplete subset of tests, leaving recent seismic performance, asynchronous LAS parsing, and section lifecycles unverified.
- **Concrete Remediation**:
  Re-run `cmake -B build` to re-scan `extra-*.cmake` and regenerate `CTestTestfile.cmake`.
- **Resolution Status**: **RESOLVED** (Master manifest static wiring)  
  All extra cmake modules (`cmake/extra-*.cmake`) are tracked directly via `_paleo_extra_manifest` in `CMakeLists.txt` (including `extra-test-deepening.cmake`), ensuring that test targets are never skipped due to configuration directory drift. All 7 previously omitted test suites are actively registered and built.
- **Empirical Verification Evidence**:
  Inspect `build/CTestTestfile.cmake`; search for `tst_sectionlifecycle` or `tst_pyramid_consume` (both present and registered in `ctest -N`).

---

### [TEST-02] Flaky Wall-Clock Performance Assertion in `tst_correlation_full` Causes Intermittent CI Failures
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：offscreen 感知+平台自适应预算；commit `bc79b6a`）

- **Severity**: **P1** (Flaky Test Suite / CI Blocker)
- **Dimension**: Test Coverage & Automation Verification
- **Exact Code Location**: `tests/tst_correlation_full.cpp`, Lines 2501–2535
- **Code Snippet**:
```cpp
// tests/tst_correlation_full.cpp:2501-2535
panel.show();
const bool exposed = QTest::qWaitForWindowExposed(&panel);
const QImage grabbed = exposed ? panel.grab().toImage() : QImage();
const qint64 ms = t.elapsed();
return grabbed.isNull() ? std::numeric_limits<qint64>::max() : ms;
...
const qint64 budgetMs = 3000;
QVERIFY2(best < budgetMs, qPrintable(QStringLiteral("50-well setup+render took %1 ms").arg(best)));
```
- **Trigger Condition & Call Chain**:
  Under headless CI execution (`QT_QPA_PLATFORM=offscreen`), `qWaitForWindowExposed` returns false. `grabbed.isNull()` is true, causing `buildOnce` to return `std::numeric_limits<qint64>::max()`. The assertion `best < budgetMs` fails with a timeout error. Under parallel execution (`ctest -j4`), CPU contention also exceeds 3000ms (`TODOS.md:168`: `3041/3000ms`).
- **Impact Analysis**: Flaky CI test runs and false negative build failures.
- **Concrete Remediation**:
  In offscreen mode, skip exposure checks or adapt thresholds based on runner load.
- **Resolution Status**: **RESOLVED** (Offscreen platform detection & budget relaxation)  
  `tests/tst_correlation_full.cpp` incorporates `QGuiApplication::platformName() == "offscreen"` detection and a relaxed 9000ms wall-clock budget for multi-threaded setups, eliminating timeout flakes in CI and headless sandboxes.
- **Empirical Verification Evidence**:
  Inspect `tests/tst_correlation_full.cpp:2501-2535` and `TODOS.md:168`.

---

### [TEST-03] False Green Assertion in `tst_wellcomposite_depth.cpp` Passes Without Signal Emission
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：spy.count()==1；PR #193 `0a02a0e` 系）

- **Severity**: **P2** (Tautological Assertion / False Green)
- **Dimension**: Test Coverage & Automation Verification
- **Exact Code Location**: `tests/tst_wellcomposite_depth.cpp`, Lines 413–416
- **Code Snippet**:
```cpp
// tests/tst_wellcomposite_depth.cpp:413-416
QWidget *bar = panel.legendWidget()->miniBar();
QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(bar->width() / 2, bar->height() / 2));
QVERIFY(spy.count() >= 0); // TAUTOLOGICAL ASSERTION
```
- **Trigger Condition & Call Chain**:
  `spy.count() >= 0` is mathematically always true for `int QSignalSpy::count()`. If the mouse click fails or the signal is never emitted, the test passes green.
- **Impact Analysis**: Masks broken navigation jump click handlers in the mini-navigator.
- **Concrete Remediation**:
  Ensure widget geometry is initialized and assert `QVERIFY(spy.count() > 0);`.
- **Empirical Verification Evidence**:
  Inspect `tests/tst_wellcomposite_depth.cpp:415`.

---

### [TEST-04] Tautological Self-Comparison Assertions Across Multiple UI Test Suites
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（四处换真实不变量+mutation 验证；commit `bf93dc4`）

- **Severity**: **P2** (False Positive Visual Regression Tests)
- **Dimension**: Test Coverage & Automation Verification
- **Exact Code Location**: 
  - `tests/tst_decorations.cpp`, Line 59: `QCOMPARE( render(), render() );`
  - `tests/tst_wellcomposite_shell.cpp`, Line 387: `QCOMPARE(ExportEngine::nativePrintAvailable(), ExportEngine::nativePrintAvailable());`
  - `tests/tst_layoutexport.cpp`, Line 51: `QCOMPARE( exports.exportPngAction(), exports.exportPngAction() );`
  - `tests/tst_layoutshell.cpp`, Lines 58, 66: `QCOMPARE( iface->layoutMenu(), iface->layoutMenu() );`
- **Code Snippet**:
```cpp
// tests/tst_decorations.cpp:51-59
const auto render = [&canvas, &mgr] {
  QImage img( canvas.size(), QImage::Format_ARGB32_Premultiplied );
  img.fill( Qt::white );
  QPainter p( &img );
  mgr.paintDecorations( &p );
  p.end();
  return img;
};
QCOMPARE( render(), render() ); // EXPRESSION COMPARED TO ITSELF
```
- **Trigger Condition & Call Chain**:
  If `paintDecorations` does nothing, `render()` returns a blank white image. Comparing `render()` with `render()` evaluates `whiteImage == whiteImage` (true).
- **Impact Analysis**: Creates false confidence in decoration rendering and print availability.
- **Concrete Remediation**:
  Assert painted pixel counts or verify non-blank image hashes: `QVERIFY(countNonWhitePixels(render()) > 0);`.
- **Empirical Verification Evidence**:
  Inspect `tests/tst_decorations.cpp:59`.

---

### [TEST-05] Tautological Concurrency Invariant Check in `tst_cache_core.cpp`
> **终态（2026-10-06，方向48 清账复核）**：已修（R0 证据：size≤cap+bytes 关系断言；commit `7f7381f`）

- **Severity**: **P2** (Tautological Invariant Check)
- **Dimension**: Test Coverage & Automation Verification
- **Exact Code Location**: `tests/tst_cache_core.cpp`, Lines 343–347
- **Code Snippet**:
```cpp
// tests/tst_cache_core.cpp:343-347
for (auto &f : futs)
  f.waitForFinished();
QVERIFY(cache.size() >= 0); // TAUTOLOGICAL ASSERTION
cache.clear();
QCOMPARE(cache.bytes(), qint64(0));
```
- **Trigger Condition & Call Chain**:
  Four concurrent threads execute 4,000 operations on `cache`. The test asserts `cache.size() >= 0`, which is always true.
- **Impact Analysis**: Fails to test whether internal LRU node pointers or item counts were corrupted by race conditions.
- **Concrete Remediation**:
  Verify structural invariants: `QCOMPARE(cache.size(), cache.countedEntries());`.
- **Empirical Verification Evidence**:
  Inspect `tests/tst_cache_core.cpp:345`.

---

### [TEST-06] 33 Source Headers and Critical Subsystems Completely Untested
> **终态（2026-10-07，方向58 标记同步）**：方向52 已清 16 个高/中危零测试头（其账本 R1/R3，见下 Resolution Status）。原「33 个」为审计期 228 头口径；方向58 按 f130fb2 重扫（445 头）：直接零引用 45、传递可达 14、完全不可达 31，其中 13 个为转发垫片/`_internal.h`（不计入），**低风险剩余 18 个逐项终态见 `docs/evidence/audit-tail/test06-untested-headers.md`（待补 7 / 不值得 11）**。高/中危部分已修（方向52）；低风险剩余为登记清单，不冒记全清。

- **Severity**: **P2** (Test Coverage Blind Spot)
- **Dimension**: Test Coverage & Automation Verification
- **Exact Code Location**: Multiple modules across `src/`
- **Evidence**:
  A cross-reference of all 228 headers in `src/` against all 126 test suites identified 33 headers completely unreferenced by any test. Key untested business paths include:
  1. `src/ai/remotepredictionservice.h`: Remote AI model prediction HTTP/IPC protocol.
  2. `src/workflow/registration.h`: Interactive GeoJSON registration & tie-point affine alignment.
  3. `src/metadata/atomicfile.h`: Critical atomic write and file rotation primitives for data durability.
  4. `src/catalog/catalogindex.h`: Secondary indexing for project catalogs.
  5. `src/ui/typedconstraintdrawcontroller.h`: Interactive map digitizing of source/distribution lines.
  6. `src/ui/seismicsection/seismicpickpanel.h`: Seismic interpretation picking management.
- **Impact Analysis**: Regressions in atomic saving, AI predictions, and registration can pass unnoticed.
- **Concrete Remediation**:
  Author targeted test suites for `RegistrationWorkflow`, `AtomicFile`, and `RemotePredictionService`.
- **Resolution Status**: **RESOLVED** (Direction 52 Zero-test header clearing)  
  Under Goal-Loop Direction 52, all high and medium-risk zero-test headers have been systematically tested via 15 dedicated unit test suites in `tests/`:
  1. `src/metadata/atomicfile.h`: Tested in `tests/tst_metadata_atomicfile.cpp` (12 test cases covering atomic writes, error injection, crash simulation with `.running` dirty marker, and data corruption defense assertions).
  2. `src/workflow/registration.h`: Tested in `tests/tst_workflow_registration.cpp` (6 test cases).
  3. `src/catalog/catalogindex.h`: Tested in `tests/tst_catalog_catalogindex.cpp` (7 test cases).
  4. `src/domain/wellsitingplan.h`: Tested in `tests/tst_domain_wellsitingplan.cpp` (7 test cases).
  5. `src/domain/faciesclassification.h`: Tested in `tests/tst_domain_faciesclassification.cpp` (7 test cases).
  6. `src/domain/importrows.h`: Tested in `tests/tst_domain_importrows.cpp` (6 test cases).
  7. `src/domain/sectiontrace.h`: Tested in `tests/tst_domain_sectiontrace.cpp` (6 test cases).
  8. `src/io/laswriter.h`: Tested in `tests/tst_io_laswriter.cpp` (6 test cases).
  9. `src/io/inflight.h`: Tested in `tests/tst_io_inflight.cpp` (6 test cases).
  10. `src/io/ziparchive.h`: Tested in `tests/tst_io_ziparchive_unit.cpp` (6 test cases).
  11. `src/algorithms/geostat/linsolve.h`: Tested in `tests/tst_geostat_linsolve.cpp` (6 test cases).
  12. `src/algorithms/geostat/neighborhood.h`: Tested in `tests/tst_geostat_neighborhood.cpp` (6 test cases).
  13. `src/algorithms/inversion/volume.h`: Tested in `tests/tst_inversion_volume.cpp` (6 test cases).
  14. `src/algorithms/singlefactor/cartographicsmooth.h`: Tested in `tests/tst_singlefactor_cartographicsmooth.cpp` (8 test cases).
  15. `src/algorithms/singlefactor/structural.h`: Tested in `tests/tst_singlefactor_structural_unit.cpp` (6 test cases).
  16. `src/ai/horizonsuggest.h`: Tested in `tests/tst_ai_horizonsuggest.cpp` (6 test cases, conditional on `PALEO_HAVE_ORT`).
  `src/ai/remotepredictionservice.h` transferred to Direction 51.
- **Empirical Verification Evidence**:
  All 15 suites compile, link, and are verified registered in CTest (`ctest -N`). Mutation verification demonstrated for every suite in `.goal-loop-ledger-test-deepening.md`.

---

### [TEST-07] Headless Offscreen Focus Traversal Failure in `tst_panels`
> **终态（2026-10-06，方向48 清账复核）**：方向48 已修（nextInFocusChain 链断言+折叠披露 mutation 红绿；commit `ca18f88`；本机 Qt6.11 offscreen 原用例偶绿——CI 红证据为审计 LastTest.log:151-153）

- **Severity**: **P1** (Deterministic Test Failure in CI)
- **Dimension**: Test Coverage & Automation Verification (Cross-Ref: `ARCH-08`)
- **Exact Code Location**: `tests/tst_panels.cpp`, Line 4736; `build/Testing/Temporary/LastTest.log`, Lines 151–153
- **Code Snippet**:
```cpp
// tests/tst_panels.cpp:4730-4744
for (int i = 0; i < 80; ++i) {
  visited.insert(w->objectName());
  w->setFocus();
  QTest::keyClick(w, Qt::Key_Tab);
  w = QApplication::focusWidget();
  if (!w) break;
}
QVERIFY(visited.contains(QStringLiteral("assetSearchEdit")));
QVERIFY(visited.contains(QStringLiteral("filterDimCombo")) ||
        visited.contains(QStringLiteral("filterValueCombo"))); // FAILS UNDER OFFSCREEN
```
- **Trigger Condition & Call Chain**:
  Under offscreen QPA, `keyClick(w, Qt::Key_Tab)` does not advance focus through the hidden options pane.
- **Impact Analysis**: Causes `ctest` to exit with failure code 1.
- **Concrete Remediation**:
  Test tab focus order programmatically using `QWidget::nextInFocusChain()` rather than simulating physical key clicks in headless environments.
- **Empirical Verification Evidence**:
  Verbatim failure in `build/Testing/Temporary/LastTest.log:152`.

---

## 7. Empirically Refuted & Calibrated Claims (Adversarial Verification)

To maintain rigorous forensic integrity, this section documents defects originally alleged during exploratory audit sweeps that were subsequently challenged, subjected to adversarial code-level verification, and formally refuted or calibrated by Challenger 1.

### 7.1 Refutation of [MEM-02]: `QgisTopologicalIndex` Claimed Double Delete on Layer Destruction
- **Original Claim**: The exploratory audit claimed that `new QgsPointLocator(e.layer)` in `src/qgis/topologicalindex.cpp:110` registers `e.layer` as the Qt `QObject` parent of `e.locator`. When `e.layer` is destroyed, Qt's parent destructor allegedly cascades and deletes `locator`. Then, the `connect(layer, &QObject::destroyed, ...)` callback at line 76 executes `delete m_entries[i].locator;`, resulting in a fatal double-free crash (`free(): double free detected in tcache 2`).
- **Adversarial Investigation & Proof**:
  1. **QGIS C++ API Header Inspection**: Challenger 1 examined the vendored QGIS C++ declaration in `vendor/prefix/usr/include/qgis/qgspointlocator.h:120-125`:
     ```cpp
     explicit QgsPointLocator(
       QgsVectorLayer *layer,
       const QgsCoordinateReferenceSystem &destinationCrs = QgsCoordinateReferenceSystem(),
       const QgsCoordinateTransformContext &transformContext = QgsCoordinateTransformContext(),
       const QgsRectangle *extent = nullptr
     );
     ```
  2. **QGIS C++ Implementation Inspection**: In `third_party/qgis/src/core/qgspointlocator.cpp:871-873`:
     ```cpp
     QgsPointLocator::QgsPointLocator( QgsVectorLayer *layer, const QgsCoordinateReferenceSystem &destCRS,
                                       const QgsCoordinateTransformContext &transformContext, const QgsRectangle *extent )
       : mLayer( layer )
     { ... }
     ```
  3. **Findings**:
     - `QgsPointLocator` inherits `QObject`, but its constructor **does not accept a `QObject *parent` parameter**. The base `QObject` constructor is invoked with the default parameter (`parent = nullptr`).
     - The first argument `layer` is merely stored in internal member `mLayer` as the target dataset to be spatially indexed. It is **not** assigned as the `QObject` parent.
     - Consequently, deleting `layer` does **not** trigger any cascading deletion of `e.locator` by Qt's object tree mechanism.
     - The deletion performed in `QgisTopologicalIndex`'s `destroyed` connection (`src/qgis/topologicalindex.cpp:80`) is the **sole, legitimate deletion** of `e.locator`.
  4. **Empirical Test Verification**: Executing the full edit tools test suite against the vendored QGIS runtime:
     `LD_LIBRARY_PATH="/home/kevin/projects/paleo_workstation/vendor/prefix/usr/lib:$LD_LIBRARY_PATH" QT_QPA_PLATFORM=offscreen ./build/tst_edittools`
     Result: **69 passed, 0 failed, 0 errors**. Specifically, test case `vertexAdversarialLayerDestructionArmedToolSafety` passed cleanly with zero memory corruption or double-free errors.
- **Verdict**: **DISPUTED & REFUTED**. Removed from active defect tally (0 genuine defect). No remediation code required.

### 7.2 Calibration of [BIZ-12]: `TimeDepthModel` Strict Mode Check Ordering
- **Original Claim**: The exploratory audit claimed a P0 fatal segmentation fault in `src/domain/seismic/timedepthmodel.cpp:61-70, 108-116`, asserting that `DepthToTwtMs()` and `TwtMsToDepth()` evaluate `m_points.front()` on an empty vector when `m_strict == true`.
- **Adversarial Invariance Proof**:
  1. Inspecting write accesses to private member `m_strict` in `timedepthmodel.cpp`:
     - Constructor: `m_strict = false`, `m_points` is empty.
     - `setPoints`: explicitly resets `m_strict = false;`.
     - `setCheckshots`:
       ```cpp
       bool TimeDepthModel::setCheckshots(const std::vector<TdPoint> &points) {
         if (points.size() < 2)
           return false;
         ...
         m_points = points;
         m_strict = true;
         return true;
       }
       ```
  2. Findings: `m_strict` can only become `true` if `points.size() >= 2` passes validation. The class invariant strictly maintains:
     $$\mathbf{m\_strict == true} \implies \mathbf{m\_points.size() \ge 2}$$
  3. The condition `(m_strict == true && m_points.empty() == true)` is logically unreachable via public APIs.
  4. Due to short-circuit boolean evaluation in `if (!std::isfinite(...) || (m_strict && (depthM < m_points.front().depthM ...)))`, when `m_points.empty()` is true, `m_strict` is guaranteed false, and `m_points.front()` is never evaluated.
- **Verdict**: **CALIBRATED FROM P0 TO P2**. While the state is currently unreachable, the ordering is defensively fragile. Reordering `m_points.empty()` first is retained as a P2 defensive hygiene task.

### 7.3 Calibration of [MEM-01]: Call Trace Clarification & Confirmed UAF
- **Original Claim**: The exploratory audit claimed that reloading a project triggers a double-delete on the `void*` dynamic property `kOwnedConstraintStoreProp` in `src/workflow/workflows.cpp:118, 714`.
- **Adversarial Finding**:
  - In `src/workflow/workflows.cpp:118-121`, `delete owned;` is followed immediately in the same block by `auto *owned = new ConstraintStore(...);` and `wf->setProperty(kOwnedConstraintStoreProp, ...)`.
  - The dynamic property is immediately overwritten with the new pointer, preventing a double-free on the old pointer during normal project reload.
  - However, adversarial verification confirmed a critical, genuine P0 Use-After-Free: `ConstraintStore::ConstraintStore` (`src/io/constraintstore.cpp:50`) captures raw `PaleoProjectStore *store` in lambda `m_enqueue`. Because `ConstraintWorkflow` can outlive `PaleoProjectStore`, subsequent asynchronous write executions dereference dangling `store` pointers.
- **Verdict**: **DOUBLE-DELETE CLAIM MITIGATED; UAF CONFIRMED AS GENUINE P0**.

### 7.4 Root Cause Refinement of [RUNTIME-01]: Unchecked `m_projectSvc`
- **Original Claim**: Claimed `QgisProjectService::project()` returns `nullptr` when no project is open.
- **Adversarial Finding**: In `QgisProjectService`, `m_project` is instantiated in the constructor and never returns null. However, `m_projectSvc` itself can be null in `PaleoMainWindow` under modular testing or headless setups. In `paleomainwindow_workbench.cpp:149, 574`, `m_projectSvc` is dereferenced directly without null-checking, causing a genuine P0 SIGSEGV.
- **Verdict**: **CONFIRMED AS GENUINE P0 (ROOT CAUSE REFINED)**.

---

## 8. Cross-Cutting Architectural Synthesis & Root Causes

### 8.1 Dynamic Property Abuse as a Circumvention of Pimpl / Interface Contracts
A systemic anti-pattern identified across both functional and UI layers is the reliance on dynamic string properties (`QObject::setProperty` / `property`) to bypass C++ strong typing:
1. In `src/workflow/workflows.cpp` (MEM-01 & ARCH-02), `kOwnedConstraintStoreProp` was used to store `void*` pointers to avoid declaring member pointers or updating CMake dependencies. While dynamic property re-assignment mitigates double-free on standard reloads, capturing raw parent pointers in closures (`ConstraintStore::m_enqueue`) creates fatal Use-After-Free hazards.
2. In `src/workflow/mappingworkflow.cpp` (ARCH-06), thickness samples were passed to `ConstraintWorkflow` via dynamic variant properties rather than typed signals.
3. In `src/ui/paleomainwindow.cpp` (MEM-03), `"horizonFlashActive"` was stored as an untyped dynamic property.

**Root Cause**: When the team enforced strict header isolation rules in early sprints, developers lacked an approved mechanism for non-QObject helper classes. Rather than creating proper Pimpl classes or forward-declared unique pointers, dynamic properties were abused as an untyped escape hatch.

### 8.2 Linter Asymmetry and Guardrail Blind Spots
The project relied heavily on `tools/check_layering.py --strict` as an infallible architectural gate. However, `check_layering.py` was implemented with asymmetric rules:
- It rigorously guarded against view layer incursions into data and algorithms.
- It completely omitted checks on whether the **Data Layer** depended on higher layers (`qgis`, `workflow`, `ui`).

Because the linter was green, developers assumed the architecture was clean, allowing `src/io/dataimportservice` to link and call `QgisLayerService` directly.

### 8.3 Headless Offscreen Assumptions in Test Design
Multiple test failures and flaky timeouts (`ARCH-08`, `TEST-02`, `TEST-03`, `TEST-07`) stem from writing tests that assume active window managers, physical screens, and hardware VSync. In headless Linux environments (`QT_QPA_PLATFORM=offscreen`):
- `qWaitForWindowExposed` fails or times out.
- Tab key press simulation does not propagate focus hints.
- Signal spies for mouse clicks on unexposed widgets receive zero events.

---

## 9. Prioritized Remediation Roadmap & Implementation Plan

### Work Package 1: P0 Blocker & Fatal Crash Remediation (Immediate)
| Issue ID | Module | Target File | Core Action |
| :--- | :--- | :--- | :--- |
| **MEM-01** | `workflow` / `io` | `workflows.cpp`, `constraintstore.cpp` | Replace `void*` dynamic property with `std::unique_ptr<ConstraintStore>`; guard `store` with `QPointer<PaleoProjectStore>` in `m_enqueue`. |
| **CONC-01** | `ui` | `seismicsectiondockwidget.cpp` | Capture `QPointer<SeismicSectionDockWidget>` in thread pool extraction tasks. |
| **CONC-02** | `services` | `seismictaskservice.cpp` | Add RAII guard around `slotSemaphore.acquire()` to prevent permanent deadlock. |
| **RUNTIME-01** | `ui` | `paleomainwindow_attach.cpp`, `paleomainwindow_workbench.cpp` | Add defensive null checks for `m_projectSvc` before accessing `project()->layerTreeRoot()`. |
| **ARCH-01** | `io` | `dataimportservice.{h,cpp}` | Decouple `DataImportService` from `QgisLayerService`; emit `layerDeclared` signal. |
| *MEM-02* | `qgis` | `topologicalindex.cpp` | *Refuted by adversarial analysis (QgsPointLocator has no Qt parent cascade); no action required.* |

### Work Package 2: P1 Critical Data Integrity & Test Blocker Remediation
| Issue ID | Module | Target File | Core Action |
| :--- | :--- | :--- | :--- |
| **BIZ-01** | `io` | `lasparser.cpp` | Replace fixed 64-element array with dynamically sized vector for curves. |
| **BIZ-02** | `io` | `lasparser.cpp` | Store `hasBom` flag during header scan; eliminate incorrect 4MB peek. |
| **BIZ-03** | `io` | `segyreader.cpp` | Account for bad traces when computing `m_scannedOffset` to stop trace duplication. |
| **BIZ-04** | `io` | `segyreader.cpp` | Correct probe logic in `openCached` to prevent false fallback to sequential scan. |
| **BIZ-08** | `ui` | `vertexeditortools.cpp` | Add cumulative ring vertex count validation in topological vertex deletion. |
| **TEST-01** | `build` | `CMakeLists.txt` | Reconfigure CMake to build and register the 7 unexecuted test executables. |
| **TEST-02** | `tests` | `tst_correlation_full.cpp` | Replace wall-clock timeout with adaptive offscreen render checks. |
| **TEST-07** | `tests` | `tst_panels.cpp` | Use `nextInFocusChain()` for tab traversal assertions under offscreen QPA. |
| **MEM-03** | `ui` | `paleomainwindow.cpp` | Guard rubberband with `QPointer` in `flashHorizon` timer. |
| **MEM-04** | `qgis` | `qgislayerservice.{h,cpp}` | Store `QPointer<QgsMapLayer>` in `m_instances` to eliminate dangling pointers. |
| **CONC-03** | `io` | `cachebudget.cpp` | Add wait condition to block `unregisterCache` during active eviction loops. |
| **CONC-04** | `io` | `segyreader.cpp` | Protect `opts->progress` callback with a mutex during parallel shard scanning. |
| **CONC-06** | `io` | `dataimportservice.h` | Guard `catInvoke` against synchronous main-thread deadlocks. |
| **ARCH-03** | `build` | `CMakeLists.txt` | Decompose `paleo_deps` to prevent leaking `Qt6::Widgets` to data libraries. |

### Work Package 3: P2 & P3 Hardening and Quality Polish
| Issue ID | Module | Target File | Core Action |
| :--- | :--- | :--- | :--- |
| **BIZ-12** | `domain` | `timedepthmodel.cpp` | Defensively reorder `m_points.empty()` before calling `front()`/`back()` in `TimeDepthModel`. |
| **ARCH-04** | `tools` | `check_layering.py` | Add Data layer inversion checks into `check_layering.py`. |
| **ARCH-05** | `algorithms`| `paleoalgorithms.cpp` | Move `localGridCrsWkt()` to domain layer and decouple algorithms from catalog. |
| **ARCH-06** | `workflow` | `mappingworkflow.cpp` | Define typed signal `thicknessSamplesUpdated` across workflows and pages. |
| **ARCH-07** | `linkage` | `seismicsectiontool.{h,cpp}` | Relocate interactive map tool to `src/ui/maptools/`. |
| **BIZ-05** | `io` | `segyreader.cpp` | Sanitize non-finite and NaN values in `decodeTrace`. |
| **BIZ-06** | `io` | `timedeptool.cpp` | Standardize null sentinels and drop depthless well tops. |
| **BIZ-07** | `workflow` | `registration.cpp` | Validate scale factor `sx > 1e-6` before applying affine transform. |
| **BIZ-09** | `ui` | `vertexeditortools.cpp` | Derive topological snapping radius from map scale rather than fixed 1nm. |
| **BIZ-10** | `algorithms`| `paleoalgorithms.cpp` | Guard against invalid transforms on datum-less engineering CRSs. |
| **BIZ-11** | `algorithms`| `welldist.cpp` | Assign CRS projection WKT in `createFloatRaster`. |
| **BIZ-13** | `algorithms`| `paleoalgorithms.cpp` | Add epsilon threshold (`d2 < 1e-12`) to prevent IDW $\infty/\infty$ NaN holes. |
| **BIZ-14** | `workflow` | `sectionworkbench.cpp` | Accept standard metric depth unit synonyms in `sectionWells`. |
| **TEST-03** | `tests` | `tst_wellcomposite_depth.cpp` | Fix tautological `spy.count() >= 0` assertion. |
| **TEST-04** | `tests` | `tst_decorations.cpp` | Replace self-comparison `QCOMPARE(x, x)` with non-blank pixel checks. |
| **TEST-05** | `tests` | `tst_cache_core.cpp` | Assert internal LRU structure invariants in multi-threaded test. |
| **TEST-06** | `tests` | Multiple modules | Author targeted test suites for `RegistrationWorkflow` and `AtomicFile`. |
| **MEM-05** | `services` | `paleotaskservice.cpp` | Cleanly terminate and delete `m_pool` in destructor. |
| **MEM-06** | `ui` | `horizonmarkers.cpp` | Validate grabber liveness against scene before deletion in rebuild. |
| **MEM-07** | `ui` | `editingtoolbar.cpp` | Guard `mCanvas` with `QPointer` in destructor. |
| **CONC-05** | `io` | `lascache.h` | Make diagnostic timings struct use `std::atomic<qint64>`. |
| **RUNTIME-03**| `io` | `lascache.cpp` | Store issues in `LasDoc` so inflight callers receive full diagnostics. |
| **RUNTIME-04**| `services` | `seismictaskservice.cpp` | Replace `delete timer;` with `timer->deleteLater();`. |

---

## 10. Conclusion

The original forensic audit established a historical catalogue of **44 unique active issues (45 total categorized views)** across Memory & Concurrency Safety, Architectural Layering, Business Robustness, and Test Suite Integrity, alongside **1 empirically refuted claim (`MEM-02`)**.

The findings definitively prove that superficial green test/linter statuses masked deep architectural violations (`ARCH-01`), type-safety bypasses (`ARCH-02`), silent scientific data loss (`BIZ-01`), and genuine fatal runtime crashes (`CONC-01`, `CONC-02`, `RUNTIME-01`, `ARCH-01`, and `MEM-01` UAF), while rigorous adversarial challenge successfully weeded out false positives (`MEM-02`) and calibrated unreachable failure conditions (`BIZ-12`).

The original report provides the remediation blueprint. Dated closure notes and the following Direction 50 section supersede the historical active status for their explicitly listed findings.


## 11. Direction 50 IO R0 closure (2026-10-06)

The request's BIZ-07/10/12 descriptions use IO labels which do not match this audit's original numbering. This section preserves the original RegistrationWorkflow BIZ-07 and engineering-CRS BIZ-10 as independent findings; IO closure must not be interpreted as closing those issues. R0 was run against fetched `origin/master` **3a4f8b5**, with a separate Linux worktree/build and the existing vendored QGIS prefix.

| Request label / stable IO key | R0 evidence and already-fixed provenance | Final disposition and verification |
| --- | --- | --- |
| BIZ-07 / **IO-R0-07** — custom SEG-Y header words / field order | Direct open with offsets 180/184 or swapped 192/188 already returns IL=100/101, XL=200/201 (`69cda1f`). Reusing a default-field cache after changing configuration returned stale IL=200/201, XL=100/101. | **Resolved `1719684`**: index v2 freezes all four header-word offsets; complete/checkpoint caches require the same identity, v1 regenerates. `configuredHeaderWords` and `changedHeaderWordsInvalidateCache` exercise real `custom_words.sgy` / `swapped_words.sgy` and both direct/cached paths. No guessed field remapping. |
| BIZ-10 / **IO-R0-10** — truncated SEG-Y | Incomplete binary header and last payload short by one byte already reject (`11f1d5c` sample guard). Last trace header with only 120 bytes silently succeeded with 3 traces; negative ns plus a short payload succeeded with 3 traces / 1 bad offset. SDK already rejects all four truncated variants. | **Resolved `1719684`**: reject incomplete trailing headers/payloads, permit binary-ns fallback only when its complete payload exists, and report decode failure offsets after post-open file changes. Four real truncated fixtures and variable-ns payload fixtures pin these boundaries; existing `tst_cache_segyindex` assertions remain intact. |
| BIZ-12 / **IO-R0-12** — XLSX / coordinate edges | Sparse XY/non-finite/duplicate-well/oversize-column issues already report (`6094775` / `cfa3296`). `B2junk` / `C0` accepted false coordinates; C/A/B cell order shifted values; physical row 100 reported row 2; legal inlineStr emitted a missing-SST issue. | **Resolved `5654926`**: strict cell/row references, column-indexed placement, physical row bookkeeping, shared-string issues only for cells needing SST, error cells cleared, repeated columns and overlapping merges rejected with reasons. `tst_io_workbook_edges` verifies malformed/unordered/duplicate/physical-row XLSX and SpreadsheetML, including INT_MAX merge-span downgrade without overflow. |

BIZ-05/06/14 closure also covers their actual SDK/import/workflow consumers, with pure io parsing, domain vocabulary and workflow consumption. The fixture generator `tools/reference/make_io_robustness_fixtures.py --check` regenerates **32 files byte-for-byte**, with fixed ZIP metadata, real SEG-Y binary words/payloads and UTF-8 manifest hashes. Good finite samples are compared by bits (including ±0, subnormals and maximum finite IEEE values); accepted well coordinates/depths and interpolation results are compared directly against clean data.

R0 full serial ctest: **300/301 passed, 1786.17 s**. The only pre-existing red target was `tst_startup_trace`: qgis_init_share_max median 0.2570 exceeded 0.2200, and the injected degradation ratio 0.544/0.253 missed the required factor of 3. No performance thresholds were relaxed, and this historical R0 result is retained.

Pre-integration full serial regressions against the same frozen implementation (`f7fe2311`) both passed **306/306**, in **884.69 s / 893.05 s**, with no new failures. `tst_seismic_engine` (22 Qt cases), `tst_wellfileparsers` (34), `tst_welllogset` (10), the five Direction 50 test targets, all three layering targets and both i18n targets passed twice. Startup passed 9/9 twice under its original thresholds; singlefactor performance passed with its original watchdog and assertions. The existing optional datapreview synchronous-loading capture case remained skipped because `PALEO_VISUAL_CAPTURE` was not enabled; functional and asynchronous-loading cases ran. Full CTest and preserved raw Qt receipts, Oracle case names and per-target counts are recorded in `.goal-loop-ledger-io-robustness.md`; diagnostic or partial runs do not count as acceptance passes.

Fixture, well, SEG-Y/SDK, workbook and final consumer batches each completed two consecutive five-dimension self reviews with **High=0 / Medium=0 / Low=0** after fixes and green targeted tests. Linux execution is verified; the Windows commands supplied in the request were not executed on this Linux host. Old converted artifacts are not retroactively certified; resumable transcode counts describe this run's actual reads.

The final PR execution source (`e97ba7da`, build 22) also preserves the master format-family, seismic-service, main-window and audit-closure integrations. Two complete serial regressions against the same artifacts passed **311/311 and 311/311**, in **852.59 s / 840.58 s**. Each run preserved all 298 raw Qt outputs before starting another test. The seismic/well red lines, Direction 50 targets, three layering checks and both i18n checks passed twice; all 40 pre-existing optional skip records are unchanged. Historical diagnostic performance failures and the inversion short write at only 309 MB free remain recorded separately and never count as acceptance passes. The IO ledger records the exact receipts, private resource recovery, unchanged thresholds and two final zero-High/Medium/Low reviews.
