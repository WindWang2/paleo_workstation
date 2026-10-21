# 修复优先级与 Fix 1 计划（full-audit-20261020）

依据任务书 §5：无本任务归属 P0/P1（最高为 P2×4）；按「根因修复、独立成簇、每簇一 commit」组织。

## Fix 1 提交簇（每簇：读源→定根因→红/绿测试→最小修改→定向+同域回归→commit）

| 簇 | commit 主题 | Findings | 文件面 | 回归测试 |
|---|---|---|---|---|
| F1 | fix(io): ziparchive ZIP64 界检查回绕（A-10，P2） | A-10（+RECORD-ONLY 附带：stored 等长校验） | src/io/ziparchive.cpp | tst_io_zip 新增 crafted EOCD64 回绕夹具：修复前红（越界/断言）、修复后绿（报错文案） |
| F2 | fix(ui): seismic3d 换体清缓存（D-05，P2）+ 顺手 D-07/D-10/D-08 死代码 | D-05/D-07/D-10（D-08 死代码段） | seismic3dviewpanel.cpp / seismic3dviewportwidget.cpp | tst_seismic3d 新增换体缓存清空断言 + 按钮弹回 |
| F3 | fix(workflow): #579 回收防线对称化（C-10，P2） | C-10 | paleomaplibrary.cpp / paleomapimport.cpp（prepare 携带 matched 资产回收检查；declareMaps 过滤回收清单） | tst_paleomaplibrary 新增文件重导入命中软删资产拒绝用例 + declareMaps 过滤断言 |
| F4 | fix(services,io): sidecar 词表统一 + bundle 族扫描（A-14/B-03） | A-14/B-03 | 新共享词表 + storagegovernance.cpp + paleomapfiles.cpp（+ingestplan 口径核对） | tst_storagegovernance：bundle 版本旁 .aux.xml/.qix/.qml 不进 orphans；非 bundle qml/qmd 同断言 |
| F5 | fix(io,services): SATR 谓词反转 + 交会预算闸（B-04 + A-15/B-05） | B-04、A-15/B-05 | sattrio.cpp / crossplotsources.cpp / crossplotsamples.cpp | tst_sattrio 两例谓词语义；tst_crossplot_samples 大 count 拒绝 |
| F6 | fix(ui): constraintpage 换勾建议归属守卫（D-13/D-01 残留）+ 参考面板空态（D-12）+ §35 两处（D-10 已并入 F2、D-11） | D-13/D-12/D-11 | constraintpage_wellfactors.cpp / mappingreferencepanel.{h,cpp} / datalist_batch.cpp | tst_mappingpages 新增跨因素建议守卫（wf 非空路径）；既有 4 断言保持绿；参考面板空态断言 |
| F7 | fix(io,catalog): catalog ord 告警（A-13）+ segy 并行 eligibility（A-11） | A-13/A-11 | catalogstore_json.cpp / segyreader.cpp | tst：坏 ord 行 qWarning 捕获；变道长文件并行/顺序一致 |
| F8 | fix(ci,tools): triage 三盲区（E-10）+ flaky 自测注册（E-11 部分）+ ctest 注册对齐（E-12）+ python REQUIRED（E-13） | E-10/E-11(部分)/E-12/E-13 | tools/ci_triage_reds.py / CMakeLists.txt | ci_triage_reds selftest 四新夹具（a-d）；ctest -N 注册数对账 |
| F9 | fix(workflow,qgis,ai): 单位守卫（C-11）+ copyDataset 自清（C-14）+ QPointer 守卫（C-15） | C-11/C-14/C-15 | paleomapsource.cpp / projectareasummary.cpp / paleomapfiles.cpp / remotepredictrouter.cpp | tst：度制 corners → -1；copyDataset 失败目录空；（C-15 静态修复无确定性测试，review 钉） |
| F10 | fix(algorithms): resolveParameters 前置闸（B-11）+（视夹具证实）faultsurface 多链（B-10） | B-11（B-10 视证实） | support.cpp / localdirectionalgorithm.cpp（faultsurface.cpp 视证实） | tst_singlefactor_contract 21k 拒绝文案；tst_faultsurface U 形夹具 |

## 不修（本轮 RECORD-ONLY / 递延）

- A-12（map-versions 孤儿目录 journal 化）、A-16、C-12、D-08（拾取投影一致性）、D-14 杂项、E-07、E-14（i18n 词表扩面——若 F8 顺路可做小扩表）、B-10 若证伪。
- E-11 的跨 run 台账聚合（artifact 回灌）涉及 CI 数据面接线，本轮只做 selftest 注册 + 文档注记（其机制属 #501 承诺面，避免大改）。

## 验收口径

- 每簇定向测试 + 同域回归（改 io 跑 io 域、改 ui 跑 ui 域）；
- 全量 ctest 在 Fix 1 完成后跑一次，Fix 2 后跑终局；
- 门禁：layering(--strict)/qgs-audit/qgis-spread/ui_tokens/file_budget/ui_size/ui_invariants/i18n/docs_drift 全绿对照基线；
- 基线红（在 master 即红者）不得因本轮改动新增或转绿-by-放宽。
