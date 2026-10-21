# 外部工作排除矩阵（full-audit-20261020）

日期：2026-10-20。BASE_SHA：ee8518df。核查时点：本轮开工（gh 实时查询）。

## 一、开放 Issues（4 个，全部他属，本轮不修根因/不关闭/不重复立项）

| Issue | 特征（根因指纹） | 排除面 | 本轮处理 |
|---|---|---|---|
| #556 [med] 吞错路径 | mappingworkflow.cpp:616/624/632/643 四级失败仅 qWarning；folderimport.cpp:249、wellsitingworkflow.cpp:535/860；petrophyscomputeservice.cpp:648-656 单位假设静默；ErrorHub 采用率；错误通道三轨 | mappingworkflow 产物失败链、petrophys 单位假设、ErrorHub 推广 | 标记 EXCLUDED_EXISTING_ISSUE（审查可覆盖，不实施修复） |
| #558 [med] 传递性分层债 | paleomainwindow.h/workflows.h 扇出根（#578 已削两条头扇出）；transitive 基线冻结→收缩；paleo::singlefactor 命名空间漂移 | 主窗/工作流头拆分、transitive 基线语义、命名空间拆分 | EXCLUDED_EXISTING_ISSUE（#578 已合入部分，剩余归该 issue 跟踪） |
| #559 [med] 测试薄弱区 | 主窗 attach 域伞式覆盖、ui/pages 32 页仅 1 定向套件、dsp 测试（#576 已补 dsp FFT 专属套件）、73 处裸墙钟断言（#576 已接 perfbudget 放宽口径） | 大面测试补强工程 | EXCLUDED_EXISTING_ISSUE（不重复补测试工程；本轮修复自带回归测试除外） |
| #561 [low] 低危清单 | fencewidget 线性扫、井表 readAll 峰值、ShaCache inode（#577 已修六项）、superbuild CI、ASAN 观察档、io 文案语言混用 | 该清单剩余项（superbuild CI、ASAN 转阻断、文案 tr 化工程） | EXCLUDED_EXISTING_ISSUE |

## 二、开放 PR（1 个）

| PR | 分支 | 状态 | 排除面 | 冲突处理 |
|---|---|---|---|---|
| #580 综合质量整顿 round 3 | codex/full-review-fix-review-fix-20261019 | OPEN（17 commits，终审 PASS-WITH-NOTES，等人工合并） | 51 文件：seismichorizoncluster(B-01/B-06)、cuttingsdoc/ingestplan(A-02/03)、dataexport(B-02)、welllogset(B-08)、onnxpredictionservice(C-02)、constraintpage 族(D-01 部分/G1-05)、datalist 族(G1-03/D-03)、paleomainwindow(D-04)、folderconfirm(G1-04)、identifytool 白名单(G1-01)、各基线/词表文件、tst_* 9 件 | 上轮登记未修项（B-03/B-04/B-05/A-04/D-02/E-02/E-05/E-07/D-01 残留）**不在 #580 diff 内**，属本轮修复池；修复时避免触碰 #580 已改文件，无法避免时以 #580 合入后 rebase 解决 |

## 三、远程开发分支 / 其他 worktree

- 无其他远程开发分支；无其他活跃 worktree（issue #553-#561 的 .worktrees 均已清理，其 PR #569-#578 已全部合并）。
- `paleo_workstation-full-review-20261019` = #580 分支，只读不触碰。

## 四、上轮登记未修项归属裁定

上轮（#580）finding register 中状态 OPEN 且未被任何 issue/PR 占有的项，
在本轮按新发现同等对待（先复核仍成立，再修复）：

| 上轮 ID | 内容 | 归属 |
|---|---|---|
| B-03 | storagegovernance.cpp:85-119 vs paleomapfiles.cpp:22-24 sidecar 词表分叉 | 本轮池（复核后修） |
| B-04 | sattrio.cpp:212 + crossplotsources.cpp:87 取消谓词反转 | 本轮池 |
| B-05 | crossplotsamples.cpp:218-252 采样无预算闸 | 本轮池（低优先） |
| A-04 | catalogstore_json.cpp:280-294 负 ord 静默丢弃 | 本轮池 |
| D-02 | mappingreferencepanel.cpp:193-258 死参/空态 | 本轮池 |
| E-02 | ci_triage_reds.py lint 黑盒 | 本轮池 |
| E-05 | ctest 注册漂移（qgis_spread 族 LABELS/WORKING_DIRECTORY） | 本轮池 |
| E-07 | 口径张力（登记项） | 复核后定 |
| D-01 残留 | constraintpage.cpp:233 无版本换勾等值距建议 | 本轮池（constraintpage.cpp 在 #580 diff 内——需 rebase 期冲突处理，修复面尽量小） |
