# 外部工作排除矩阵（full-audit-20261021）

核查时点：2026-10-21 开工（02:41 网络恢复后 gh 实时核查完成）。
origin/master 实时 = e6837e14 = BASE_SHA（未漂移）。

## 〇、实时开放 PR（10 个，全部他属）

| PR | 分支 | 文件数 | 排除面 |
|---|---|---|---|
| #580 round 3 审计 | codex/full-review-fix-review-fix-20261019 | 51 | 综合整顿 round 3 |
| #581 round 4 审计 | codex/full-review-fix-review-fix-20261020 | 54 | 综合整顿 round 4 |
| #602 review-from-zero 修复 115/117 | codex/fix-review-20261019 | 226 | 147 项发现的批次修复 |
| #599 predict 真实工作流 | goal/predict-realworld-workflow-20261020 | 24 | 预测 readiness/凭证移除 |
| #600 窄停靠栏打磨 | goal/ui-polish-20261020 | 8 | chip 滚动/按钮两行 |
| #601 算法原子化 P0 | goal/algo-atom-p0-20261020 | 43 | Spec/发布收口/适配器 |
| #603 统一测井导入 | codex/data-management-import-ux | 84 | XML/文本曲线公共契约 |
| #604 master lint 修复 | fix/master-lint-20261021 | 19 | tidy/传递闸/qgs 散布/函数行数/文档漂移 |
| #605 OSGeo4W 钉版 | fix/osgeo4w-pins-20261021 | 1 | vendor 闭包 |
| #606 单因素约束门控 | goal/singlefactor-realworld-workflow-20261020 | 15 | 约束门控/井点提取解耦 |

三支审计/修复分支并集 295 文件 + 新 7 支 PR 并集 177 文件 = 总排除 441 文件
（02-excluded-files.txt 已更新为并集 v2）。

## 一、活跃分支/PR（3 支）

| 分支 | 触碰面 | 排除规则 |
|---|---|---|
| codex/full-review-fix-review-fix-20261019（PR #580） | 51 文件（seismichorizoncluster/cuttingsdoc/dataexport/welllogset/onnxpredictionservice/constraintpage 族/datalist 族/paleomainwindow/folderconfirm/identifytool/基线词表/tst_* 9 件） | EXCLUDED_ACTIVE_WORK——不重复修复同根因；文件冲突时以对方合入后 rebase 解决 |
| codex/full-review-fix-review-fix-20261020（PR 应已开） | 54 文件（remotepredictrouter/faultsurface/singlefactor support/catalogstore_json/sidecarvocab/sattrio/segyreader/ziparchive/paleomapsource/crossplot*/paleomapfiles/storagegovernance/projectstatisticspage/constraintpage_wellfactors/datalist_batch/mappingreferencepanel/seismic3d 族/paleomapimport 族/projectareasummary/tst_* 13 件） | EXCLUDED_ACTIVE_WORK——同上 |
| codex/fix-review-20261019（PR 应已开，修复 review-from-zero 147 项） | 226 文件（覆盖 io/algorithms/services/workflow/ui 大面 + CMakeLists + known-reds） | EXCLUDED_ACTIVE_WORK——review-from-zero 报告全部 147 项根因均在排除面 |

review-from-zero-20261019.md（147 项：H1–H6 / A7 B17 C17 D24 E9 / L67）全文已
读入并作为已知问题特征库；本轮发现与之同根因的一律标
EXCLUDED_ACTIVE_WORK 不修。

## 二、开放 Issues（gh 实时核查，2026-10-21 02:4x）

| Issue | 特征 | 处理 |
|---|---|---|
| #556 [med] 吞错路径 | mappingworkflow 四级失败仅 qWarning；petrophys 单位假设静默；ErrorHub 采用率 | EXCLUDED_EXISTING_ISSUE |
| #558 [med] 传递性分层债 | 主窗/工作流头扇出、transitive 基线 | EXCLUDED_EXISTING_ISSUE |
| #559 [med] 测试薄弱区 | 大面测试补强工程 | EXCLUDED_EXISTING_ISSUE（本轮修复自带回归除外） |
| #561 [low] 低危清单 | superbuild CI、ASAN 转阻断、文案 tr 化工程 | EXCLUDED_EXISTING_ISSUE |
| #582–#597（16 个，review-2026-10-19 标签族） | review-from-zero 发现逐项转化 | EXCLUDED_ACTIVE_WORK（#602 修复中） |

## 三、上轮登记未修项归属

20261020 审计 finding register 中 OPEN 且未被任何分支 diff 覆盖的项
（D-06/D-09/E-14/A-12/A-16/C-12/D-14/E-07），本轮按新发现同等对待：
先复核仍成立，再修复。其中 A-12（journal 化）上轮裁定需独立批次，本轮维持不接。

## 四、修复时的文件纪律

1. 优先选择不在三支分支 diff 内的文件；
2. 无法避免触碰已占用文件时，改动面保持最小（几行级），并在此表登记冲突预期；
3. 基线文件（file-budget/ui-size/transitive/qgs-audit/qgis-spread/known-reds）
   只在「本轮新问题修复自身需要」时下调，不因外部工作补登。
