# Full Audit 20261017 — 05 修复优先级与计划

优先级：未分配 P0/P1 → 高影响可独立修 P2 → P3 快胜。每簇独立 commit，测试先行。

| 顺序 | 簇 | 涉及文件 | 回归测试 | commit 主题 |
|---|---|---|---|---|
| 1 | F2 velocitymodel 雅可比 | src/algorithms/velocitymodel.cpp | tst_velocitymodel：差分一致性 + 带噪恢复 oracle | fix(algorithms) |
| 2 | F3 catalog 自标 stale | src/catalog/datacatalog_versions.cpp | 新用例：同资产 DERIVED v2(parent=v1) 不 stale；assetops 回滚断言 | fix(catalog) |
| 3 | F1 attribution UAF | src/workflow/mappingevidencereview.{h,cpp} | tst_mappingworkbench：10 输入 composeAttribution 无 UAF/借用保护 | fix(workflow) |
| 4 | F4 constraintstore 还原 | src/io/constraintstore.cpp | tst_singlefactor_store：BUSY 拒绝 + 还原后无 -wal 残留/逐行相等 | fix(io) |
| 5 | F5 supercover 统一 | src/workflow/surfacegridding.cpp | tst_surfacegridding_thread：mask 与 rasterizeBarrierParts 逐位对拍 + 贴边断言 | fix(workflow) |
| 6 | F7 slice join | src/services/seismictaskservice_slice.cpp | 现有测试回归 | fix(services) |
| 7 | F8 SGS 次键 | src/algorithms/geostat/{sgs.cpp,neighborhood.h} | tst_geostat：比较器严格全序单测 | fix(algorithms) |
| 8 | F9 配准大小闸 | src/io/geojsonaffine.cpp + src/workflow/registration.cpp | tst_import_geoaffine：超限拒绝且不落派生 | fix(io) |
| 9 | F6 impact 门控 | src/ui/pages/mappingworkbenchpage.cpp + src/ui/paleomainwindow_workbench.cpp | tst_mappingpages：busy/无层位/无选中三态门控 + reason | fix(ui) |
| 10 | F10 perf 标签 | CMakeLists.txt | ctest 清单验证 | test(cmake) |
| 11 | F11-F17 P3 快胜 | 各自文件 | 各自小回归 | 按域分 commit |
| 12 | F18-F21 视容量 | — | — | — |

约束：
- 不放宽断言/不改 golden/不加超时；根因修复优先。
- 每簇修复前先写红测（可红测者）；生命周期类用结构断言。
- 同文件与外部活跃分支重叠的（B-04/F7）在 PR 注明。
- 全程 -parallel 4 + 全局锁；ctest -j4。
