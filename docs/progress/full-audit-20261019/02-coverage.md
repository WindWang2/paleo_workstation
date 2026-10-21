# 审查覆盖清单（full-audit-20261019）

来源与方式：MA=主Agent；A/B/C/D/E=subagent（A 数据/IO、B 算法/服务、C workflow/AI/联动、
D qgis/ui、E 构建/测试/架构）。完整逐文件清单见 /tmp/paleo-audit-r3/findings-agent-*.md
（归档副本将随 PR 附上）。风险分级：H=逐行（含调用链推演）/I=接口契约级/G=抽查。

## 模块覆盖矩阵

| 模块 | 规模 | 方式 | 审查者 | 备注 |
|---|---|---|---|---|
| src/domain | 84f/12K | H(新码)+I | A | 新合入 5 文件逐行 |
| src/catalog | 28f/6K | H+I | A | catalogstore_json 事务边界深读 |
| src/io | 85f/25K | H(解析器族)+I | A | segyreader/las/表格/工作簿/geojson/cuttings 深读；#553/#560 根因面排除 |
| src/metadata | 28f/5.5K | I | A | 六头持久化/恢复 |
| src/algorithms | 144f/37K | H(geostat/插值)+I | B | #554 拆分面排除、数值 bug 仍查 |
| src/services | 83f/22K | H(新码+任务系统)+I | B | 新合入 11 文件逐行 |
| src/workflow | 142f/48K | H(新码23文件)+I | C | 全部新文件逐行；#556/#568 根因面排除 |
| src/ai | 34f/6K | H | C | onnx 链路 #143-#479 修复自洽复核 |
| src/linkage | 8f/876 | H | C | 全读 |
| src/qgis | 105f/21K | H(新码24)+I | D+MA | paleomapsource 等按 #568 接口级 |
| src/ui | 424f/117K | H(新码111文件+高风险9)+I | D | 分级审查，未读区如实登记（correlation/seismic3d/layout 存量大面） |
| src/app | 12f/3.7K | I | E | main/appcontext |
| src/selfcheck | 3f | I | E | |
| tests/ | 401f | 抽查 | E+C | 永真断言/沙箱/注册面；#559 范围排除 |
| tools/ | 30+ py | H | E+MA | 检查器逻辑实读；基线纪律对账 |
| cmake/, CMakeLists | — | H | E | 漏编/漏测/沙箱面 |
| .github/workflows | — | H | E | 与本地闸门一致性 |

## 基线闸门实测（BASE_SHA 干净树，主 Agent 复跑复核）

红（未登记 known-reds）：
- layering_qgs_audit：identifytool.{h,cpp} 新增直连
- qgis_spread（+strict）：同上 2 文件新增散布（149→151）
- ui_tokens（+strict）：identifypopup.cpp:58 setSpacing(2)
- ui_size：datalist_tree.cpp 1100>1094
- file_budget（+strict）：超标 62 vs 基线 61（新增 1 + 回升 3）
- layering_transitive：heavy_tus 88>87（**基线钉错**：#564 实测即 88 却写 87；
  +1 TU 归因 #562 拆分文件 ui/paleomainwindow_paleomaps.cpp，edges 恒 52）

红（已登记 known-reds，非本轮债）：
- layering_transitive：ui_qtwidgets_notes 7>6（#533 口径裁决挂起，transitive-baseline 头注在案）

绿：layering --strict/symbol-audit/selftest、ui_invariants --strict、i18n、docs_drift、
file_budget selftest、qgis_spread selftest、其余 selftest 族。

## 未核查区域（如实声明）

- D：src/ui 存量 correlation/seismic3d/layout 族大面（接口级为主）
- B：algorithms 存量大文件主体（上两轮已深扫，本轮聚焦新码+上轮修复复核）
- A：io 的 segy 大函数主体（#554 接手拆分）
- Vendor 第三方：仅集成边界（照任务书）
