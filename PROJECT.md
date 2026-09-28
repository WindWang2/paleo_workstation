# Project: paleo_workstation Deep Review & Issue Resolution

## Architecture Overview
The authoritative architecture plan is `docs/PALEO_QGIS_PLAN.md` and `docs/UI_LAYER_PLAN.md`.
The codebase strictly adheres to 5 layer categories across 13 modules:
- **数据 (Data)**: `src/domain`, `src/catalog`, `src/io`, `src/metadata`, `src/services`, `src/algorithms`
- **功能 (Functional)**: `src/workflow`, `src/linkage`, `src/ai`
- **QGIS 封装 (QGIS Encapsulation)**: `src/qgis`
- **视图 (View)**: `src/ui/**`
- **组装根 (Assembly Root)**: `src/app`
- **测试壳 (Test Shell)**: `src/selfcheck`

Guardrails:
- `tools/check_layering.py` enforces header comments, forbidden includes, and whitelist compliance.
- `tools/layering-baseline.txt` enforces zero legacy violations.
- CMake/CTest tests verify functionality across 84 test cases.

---

## Feature & Issue Inventory
| # | Issue / Feature | Description | Milestone | Source |
|---|-----------------|-------------|-----------|--------|
| 1 | SingleFactorDef Layer Mismatch | `src/services/singlefactordef.{h,cpp}` marked `// 层：功能`, must be `// 层：数据` | M1 | Survey (Explorer 1) |
| 2 | PanelShared Duplicate Header | `src/ui/pages/panelshared.h` line 7 has duplicate `// 层：视图` | M1 | Survey (Explorer 1) |
| 3 | Check Layering Tooling Blindspot | `tools/check_layering.py` only checks vocabulary, doesn't enforce directory correspondence | M1 | Survey (Explorer 1) |
| 4 | Polygon Ring Closure Move Break | Non-topological drag of vertex 0 leaves vertex N unmoved, unclosing polygon ring | M2 | Survey (Explorer 2) |
| 5 | Polygon Ring Closure Delete Break | Deleting closure vertex removes both 0 and N or leaves ring open | M2 | Survey (Explorer 2) |
| 6 | Canvas Marker Desync on Undo/Redo | PaleoVertexTool does not connect to undoStack indexChanged, geometryChanged, or afterRollBack | M2 | Survey (Explorer 2) |
| 7 | Frozen TopoMarkers During Drag | `mDraggingVertex->topoMarkers` not updated in `canvasMoveEvent` | M2 | Survey (Explorer 2) |
| 8 | PaleoVertexTool Pointer Lifecycle | Raw pointer `mLayer` and missing `clearMarkers()`/`clearDragState()` in destructor | M2 | Survey (Explorer 2) |
| 9 | Null Pointer Hazards in VertexTool | `geometry.constGet()` unchecked before `vertexAt()` | M2 | Survey (Explorer 2) |
| 10 | Multi-Part Boundary Crossing in VertexTool | Double-click segment search crosses multipart/ring boundary without check | M2 | Survey (Explorer 2) |
| 11 | VertexTool Unit Tests | Unit tests in `tests/tst_edittools.cpp` for ring closure, marker sync, topo drag | M2 | Survey (Explorer 2) |
| 12 | Correlation Full Performance Failure | `fiftyWellsRenderUnderThreeSeconds` flakiness due to 100 rebuilds, 5050 pixmap conversions, float hashing | M3 | Survey (Explorer 3) |
| 13 | PaleoTaskService Orphaned Task Leak | Running tasks detached on destruction without `deleteLater()` upon completion | M3 | Survey (Explorer 3) |
| 14 | SeismicMapLink / WellMapLink Dangling Pointers | Raw `QgsVectorLayer*` and double-free hazard on canvas-parented markers/tools | M3 | Survey (Explorer 3) |
| 15 | GDAL Raster Band Null Dereference | `mappingworkflow.cpp` & `mapversioncontroller.cpp` lack `GDALGetRasterCount >= 1` check | M3 | Survey (Explorer 3) |
| 16 | SeismicTaskService Lambda `this` Capture | Background thread worker captures `this` solely for `tr()` | M3 | Survey (Explorer 3) |
| 17 | Full CTest & Layering Gate Validation | 100% test pass (84/84) and 0 layering violations | M4 | Survey (All) |
| 18 | Atomic Git Commits & Resolution Report | Clean commits per module and comprehensive issue resolution document | M4 | Survey (All) |

---

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Layering & Header Enforcement | Fix `singlefactordef.*`, clean `panelshared.h`, harden `check_layering.py` + selftests | None | DONE |
| M2 | VertexEditorTools Stabilization | Complete topology editing, ring closure integrity, undo/redo marker sync, `tst_edittools` | M1 | DONE |
| M3 | Correlation & Core Defect Fixes | Optimize WellCorrelationPanel for `tst_correlation_full`, fix TaskService, MapLinks, GDAL guards | M1 | DONE |
| M4 | Validation, Atomic Commits & Report | Validate via ctest & check_layering, atomic git commits per module, final report | M1, M2, M3 | DONE |

---

## Code Layout & Write Boundaries
- **M1 (Layering & Tools)**:
  - `src/services/singlefactordef.h`
  - `src/services/singlefactordef.cpp`
  - `src/ui/pages/panelshared.h`
  - `tools/check_layering.py`
- **M2 (Topology Editing & Edit Tools)**:
  - `src/ui/edittools/vertexeditortools.h`
  - `src/ui/edittools/vertexeditortools.cpp`
  - `tests/tst_edittools.cpp`
- **M3 (Core Defects & Correlation Panel)**:
  - `src/ui/correlationpanel.h`
  - `src/ui/correlationpanel.cpp`
  - `src/ui/correlation/correlationtrack.h`
  - `src/ui/correlation/correlationwellcolumn.h`
  - `src/services/paleotaskservice.cpp`
  - `src/services/seismictaskservice.cpp`
  - `src/linkage/seismicmaplink.h`
  - `src/linkage/seismicmaplink.cpp`
  - `src/linkage/wellmaplink.h`
  - `src/linkage/wellmaplink.cpp`
  - `src/workflow/mappingworkflow.cpp`
  - `src/workflow/mapversioncontroller.cpp`
- **M4 (Validation & Documentation)**:
  - All test targets, git staging/commits, final issue resolution report.

---

## Interface Contracts
### PaleoVertexTool ↔ QgsVectorLayer / QgsMapCanvas
- `mLayer` held as `QPointer<QgsVectorLayer>`.
- Layer signal subscriptions: `selectionChanged`, `geometryChanged`, `layerModified`, `mLayer->undoStack()->indexChanged`.
- All visual markers (`TaggedVertexMarker`, `QgsVertexMarker`) and rubber bands (`QgsRubberBand`) must be safely destroyed on tool deactivation and destructor.
- Any polygon ring vertex move/delete must preserve topological ring closure (`v0 == vN`).

### WellCorrelationPanel ↔ CorrelationWellColumn
- `WellCorrelationPanel` provides batch updates (`setUpdatesEnabled(bool)`) to prevent quadratic `rebuildScene()` during multi-well/track loading.
- `CorrelationTrack::depths()` and `values()` return `const QVector<float>&`.
- `CachedImage` caches both `QImage` and `QPixmap` to eliminate duplicate format conversions.

### PaleoTask ↔ PaleoTaskService
- Tasks detached on service destruction must self-destruct via `deleteLater()` upon completion if `!parent()`.
