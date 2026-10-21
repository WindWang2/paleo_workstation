# 优先级与修复计划（full-audit-20261019）

## 严重度统计（聚合后）

- P0：0
- P1：1（G1/E-01 master 门禁回红簇——7 处未登记红，闸门权威性失守）
- P2：3（A-01 geojson 无闸 OOM；E-02 triage lint 盲区；G1-03/04/05 file_budget 回升收敛并入 G1 簇）
- P3：~15（C/D/A/E 各项）+ RECORD-ONLY 若干

## 排除统计

- EXCLUDED_EXISTING_ISSUE：#553 GDAL RAII（#570 PR）、#554 巨函数拆分（#572）、
  #555 soffice（#571）、#556 ErrorHub（#569）、#557 渲染热点、#558 传递分层债、
  #559 测试薄弱、#560 folderimport 三胞胎（含 C-04 preview 双胞胎）、#561 低危批；
  #533（ui_qtwidgets_notes 已登记裁决挂起）
- EXCLUDED_ACTIVE_WORK：PR #568 已合并（paleomap 链）、PR #569–#572 文件面
  （petrophcomputeservice/folderimport.workflow/mappingworkflow/wellsitingworkflow/
  mincurvature/paleoalgorithms/rasterout/dataimport_document/dataimportservice/
  segyreader/seismic3dviewpanel/constraintfactorjobs_geostat + 两 baseline 文件新增行）

## Fix 1 计划（每簇一 commit，全部带定向回归）

### 第一优先：G1 簇（P1，恢复 master 闸门绿）

| # | 项 | 文件 | 验证 |
|---|---|---|---|
| c1 | identifytool 视图原生控件白名单裁决 | tools/layering_vocab.json | layering_qgs_audit/qgis_spread 绿 |
| c2 | identifypopup 间距 token 化 + ui-token 2 stale 清理 + 计数 356→354 | identifypopup.cpp, ui-token-exceptions.json, ui-token-baseline.txt | ui_tokens(+strict) 绿 |
| c3 | datalist_tree 辅助分支提取 buildAuxiliaryNavSpec 新 TU | datalist_tree.cpp/.h, datalist_auxnav.cpp(新), CMakeLists, file-budget/ui-size 基线同步下调 | ui_size/file_budget(+strict) 绿 + tst_ui/tst_panels |
| c4 | folderconfirm 文档焦点回落提取助手 | folderconfirm.cpp, file-budget 冻结值下调 | file_budget 绿 |
| c5 | constraintpage_interpolation updateEngineRows 拆两个助手 | constraintpage_interpolation.cpp, constraintpage.h | file_budget 绿（≤200）+ tst_factorworkflow |
| c6 | transitive heavy_tus 带归因 re-pin 87→88 | transitive-baseline.txt（头注归因 #562 拆分 TU） | layering_transitive 仅剩已登记 ui_qtwidgets_notes 红 |
| c7 | attachConstraintPage 冻结值违约回退 719→718（压 1+ 行） | paleomainwindow_attach_constraint.cpp, file-budget-baseline.txt | file_budget 绿 |
| c8 | known-reds STALE 条目清理（转绿即删条目，纪律④） | docs/ci/known-reds.json | 逐条本地复测佐证 |

### 第二优先：P2

| # | 项 | 验证 |
|---|---|---|
| c9 | A-01 geojson 图例抽取大小闸（同族 512MB 口径） | tst_import 畸形大夹具（拒绝路径） |
| c10 | E-02 triage lint 明细接线（ci.yml 传 lint 日志 + triage 解析 gate 级红） | ci_triage_reds.py selftest/单元演算 |

### 第三优先：P3（按影响排序，时间允许全做）

| # | 项 |
|---|---|
| c11 | C-01 导出 dedup 复用清空 horizon 归属（数据完整性） |
| c12 | D-01 #544 漏修 cellChanged 路径（错误默认值会静默提交） |
| c13 | B-01 SMI 散点投票半格偏移（数值正确性，VERIFIED 手算夹具） |
| c14 | B-02 崩溃暂存清扫白名单补三个根 |
| c15 | C-03+C-06 horizonbatchexport 多文件产物 dedup + registerMapPdfAsset horizon 参 |
| c16 | F(MA)-01 识别弹窗层序反转（identifyPoint 契约内化 + 两调用方） |
| c17 | E-04 tst_horizonbatchexport_diag 注册构建（兼作 C-03/C-06 回归载体） |
| c18 | C-02 onnx m_loaded 锁外读 |
| c19 | A-02 岩屑文本 readAll 闸 |
| c20 | A-03 ingestplan 进度计数契约 |
| c21 | B-03 sidecar 词表统一（storagegovernance 对齐 paleomapfiles） |
| c22 | B-04 SATR 取消谓词语义正名 |
| c23 | B-06 readInline 失败 QC 计数 |
| c24 | B-08 welllogset TVD 回退 warnings |
| c25 | D-02/03/04 小项合集（死参数/虚构计数/死写） |
| c26 | A-04 负 ord 告警 |
| c27 | E-05 qgis_spread ctest 注册对齐 |

### B 域未核查面（下轮输入）

singlefactor 其余 ~12 文件、faultsurface/、curveexpr、faciesmapping/ 6 文件、
seismictaskservice_interp 逐行——B 已如实标注，不假设干净。

### 记录不修（RECORD-ONLY）

R-1..R-5（A）、C-05（Mock 标定 95% 进度粒度）、C-08、S-D1..S-D4、E-07（口径张力）。

## 基线测试口径

基线构建（BASE_SHA）完成后立即 ctest 全量记录红集；之后 rebase 到最新 master
（01f0558e+），Fix-1 全部落地后增量重建 + 定向回归 + 全量 ctest 对照
（新红=本轮回归须修；基线红=EXISTING_BASELINE_FAILURE 如实分账）。
