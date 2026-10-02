# Project: Geological IO & Logic Enhancement (BIZ-01, BIZ-02, BIZ-05, BIZ-12)

## Architecture
- **Data Layer (`src/domain`, `src/io`)**:
  - `src/io/lasparser.cpp` / `src/io/lasparser.h`: Wireline/LWD LAS 2.0 streaming parser and header scanner.
  - `src/domain/seismic/timedepthmodel.cpp` / `src/domain/seismic/timedepthmodel.h`: Pure domain model for time-to-depth and depth-to-time transformation with constant velocity or piecewise-linear calibrated checkshots.
- **View Layer (`src/ui`)**:
  - `src/ui/edittools/vertexeditortools.cpp` / `src/ui/edittools/vertexeditortools.h`: Interactive map canvas vertex editing tool supporting topological multi-layer node editing, movement, insertion, and deletion.
- **Tests Layer (`tests/`)**:
  - `tests/tst_perf_las.cpp`: QtTest suite for LAS parsing and performance.
  - `tests/tst_edittools.cpp`: QtTest suite for vertex and shape editor tools.
  - `tests/tst_sections_alignment.cpp` / `tests/tst_seismic_section.cpp`: QtTest suites for time-depth model conversion and alignment.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | BIZ-01: LAS Multi-Curve Buffer Truncation Fix | Replace fixed 64-element array in `LasParser::parseDepthRange` with dynamically sized vector for curves, supporting >64 curves without truncation. | M1 | AUDIT_ISSUES.md §5 |
| 2 | BIZ-02: LAS Streaming BOM Offset Accuracy | Store `bomBytes` directly in `HeaderScanResult` and compute accurate `asciiDataOffset` to eliminate 4MB peeking desynchronization. | M1 | AUDIT_ISSUES.md §5 |
| 3 | BIZ-05: Topological Polygon Vertex Deletion Defense | Group deletions by `(layer, fid, part, ring)` and enforce `ringVertices - deleteCount >= minRemaining` to prevent ring collapse below 4 distinct vertices. | M1 | AUDIT_ISSUES.md §5, docs/CODE_REVIEW_REPORT.md |
| 4 | BIZ-12: TimeDepthModel Check Ordering | Defensively check input finiteness (`!std::isfinite`) and point emptiness (`m_points.empty()`) before accessing `.front()` / `.back()`. | M1 | AUDIT_ISSUES.md §5 & §7.2 |
| 5 | Adversarial LAS Unit Tests | Unit tests in `tests/tst_perf_las.cpp` for 70+ curves reading and UTF-8 BOM streaming offset accuracy. | M1 | ORIGINAL_REQUEST.md §2026-10-01T11:45:54Z |
| 6 | Adversarial Polygon Topology Unit Tests | Unit tests in `tests/tst_edittools.cpp` verifying coincident pinch-point deletion defense (rejection on 5-vertex ring, success on 6-vertex ring). | M1 | ORIGINAL_REQUEST.md §2026-10-01T11:45:54Z |
| 7 | Full Regression & Layering Gate | Verify 100% CTest pass (127+ suites, 0 failed, 0 skipped) under `-j4` and zero layering violations with `python3 tools/check_layering.py --strict`. | M1 | ORIGINAL_REQUEST.md §2026-10-01T11:45:54Z |
| 8 | Git Commit, Push & Pull Request | Atomic semantic git commits on `feat/geological-io-enhancement`, push to remote, and create PR to `master` via `gh pr create`. | M1 | ORIGINAL_REQUEST.md §2026-10-01T11:45:54Z |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | M1: Geological IO & Logic Enhancement | Implement BIZ-01, BIZ-02, BIZ-05, BIZ-12, write adversarial tests, verify build/ctest/layering, commit, push & create PR. | none | DONE |

## Code Layout
- `src/io/lasparser.cpp` (Data Layer: `// 层：数据`)
- `src/ui/edittools/vertexeditortools.cpp` (View Layer: `// 层：视图`)
- `src/domain/seismic/timedepthmodel.cpp` (Data Layer: `// 层：数据`)
- `tests/tst_perf_las.cpp` (LAS unit tests)
- `tests/tst_edittools.cpp` (Vertex editing unit tests)
- `tests/tst_sections_alignment.cpp` (Time-depth unit tests)
