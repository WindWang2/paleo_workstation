# 综合编图证据体系 R0 审计：逐项缺口矩阵（mapping-evidence-gap-matrix）

- 基线：`origin/master` = `c720aef58c00b8e53bd3004598b17ae752496be9`（2026-10-16，#505）
- 开发分支：`codex/mapping-evidence-deepening`（独立 worktree
  `../paleo_workstation-mapping-evidence`；主 checkout 的未提交改动未触碰）
- 审计方法：逐文件读源码 + 调用链 + 既有测试；**不以文件名/注释/PR 描述判定完成**。
  行号均指基线 commit。
- 状态词表：`VERIFIED_COMPLETE` / `PARTIAL` / `STUB_OR_INTERFACE_ONLY` / `MISSING` / `UNVERIFIED`。
- 风险：H（会产出错误结论、内存不安全或数据丢失）/ M（结论不完整或误导）/ L（体验/诊断）。

## 0. 审计结论摘要

PR #494 的骨架真实存在且大部分可用：冻结输入重放、证据面板「计算归因」、
冲突聚合、反向索引、三级影响分析、受控重算、`SelectionTarget` 业务身份与
工程重开恢复均有代码与测试。但源码审计发现以下 **已文档声明但实际不成立**
或 **会产生错误结论** 的缺口（按风险排序）：

| # | 缺口 | 风险 | 位置 |
|---|---|---|---|
| X1 | 归因采样图层缓存驱逐会 `delete` 正在使用的图层（悬垂指针/UAF） | H | `mappingevidencereview.cpp:293-296`；调用方 `:342,:426`、`mappingworkbench_impact.cpp:259,562-563` |
| X2 | 纯相点输入融合时，重放网格与真实 `composeRaster` 网格不同（`gridOf` 总是 `grow(100)` 且拒绝退化范围；真实算法仅在 <1m 时 grow） | H | `composeattribution.cpp:63-73` vs `mappingartifactwriter.cpp:574-579` |
| X3 | 不对齐栅格的图斑级重放用 `provider->sample()`，与真实算法的 `block()` 重采样口径不同；文档声称「unresolved 注明 fidelity」，代码中不存在任何 fidelity 字段 | M | `composeattribution.cpp:103`；`mapping-evidence-completion.md:77-79` |
| X4 | Level-3 单点重放在点击位置取样，而真实算法按像元中心取样 | M | `composeattribution.cpp:260-278` |
| X5 | 重算计划不绑定影响报告的 before/after：执行时取「当前版本」，若输入又更新到 V3 会静默按 V3 重算，报告已过期却仍执行 | H | `mappingworkbench_impact.cpp:385-411,498-553` |
| X6 | `whole_rerun` 条目直接复制 `ordered_inputs`（冻结 product id），重算拾取的是**旧输入**——产出与旧版本相同的「新版本」，伪装完成重算 | H | `mappingworkbench_impact.cpp:483-489,519-521` |
| X7 | 保护口径/受影响图斑只看直接 `facies_polygons` 子版本；人工确认实际保存在 `edited_facies` 后代版本，确认数恒为 0，需复核图斑漏列 | H | `mappingworkbench_impact.cpp:297-301,417-424`；`mappingworkbench_edit.cpp:198,390` |
| X8 | 反向索引 `contradicted` 不排除已解除（unlinked）历史行；反查结果行不带 `unlinked`/是否当前版本标记，历史关联被呈现为当前有效 | M | `evidenceindex.cpp:109-110`；`mappingworkbench_impact.cpp:670-687` |
| X9 | 反向索引同步错误被静默吞掉（`syncError` 未上报）；同步在 UI 线程 | M | `mappingworkbench_impact.cpp:653-654` |
| X10 | 反查命中行双击走「来源高亮」而非把命中图斑设为主目标：证据面板仍停在原图斑，`SelectionTarget.evidenceId` 被改成另一图斑的证据——身份混淆；无「返回原图斑」 | M | `mappingworkbenchpage.cpp:461-463` → `evidenceinteraction.cpp:190-222` |
| X11 | 图谱把生成来源（计算血缘）挂到「相类型判断（Assertion）」节点上，关系枚举为 `References`；计算参与与科学判断在图结构上混接 | M | `datagraph.cpp:786-817` |
| X12 | 索引写事务失败路径不 `rollback`，连接停留在打开事务中 | M | `evidenceindex.cpp:172-233` |
| X13 | `compareVersions` 在 UI 不可达（只在测试中调用）；重算新版本不记录它替代了哪一版（无 SUPERSEDES 事实） | M | `paleomainwindow_workbench.cpp:823-845`；`mappingworkbench_compose.cpp:219-232` |
| X14 | 冲突结果、井口落面清单只以纯文本呈现，不可跳转来源/井 | L | `mappingworkbenchpage.cpp:1460-1490` |
| X15 | 受保护计数查询把 UID 直接拼入表达式（未转义） | L | `mappingworkbench_impact.cpp:438-439` |
| X16 | 冻结输入解析三处复制且口径已漂移（缺失判定一处不带 suffix、一处带；一处不查文件存在；缺失原因文案不同） | M | `mappingevidencereview.cpp:317-350,405-432`；`mappingworkbench_impact.cpp:51-106` |

## 1. 逐项矩阵

### G03 图斑级冻结输入归因重放 — `PARTIAL`
- 用户场景：选中相面图斑 → 看它为何是当前相、由哪些冻结输入决定。
- 实现：`MappingWorkbench::composeAttribution` (`mappingevidencereview.cpp:302-389`)
  → `composedAncestorVersion`（BFS 深度 4，只走 parentVersionIds，:234-263）
  → `paleo::attribution::replay` (`composeattribution.cpp:317-484`)。
- 现有测试：`tst_composeattribution` 3 例（冻结版本+真实阈值、相点最近邻+缺失输入、人工草稿 manual）。
- 已验证：冻结版本不被当前版本替换；缺失输入 `unresolved`；人工图斑 `manual`；
  阈值损坏拒绝重放；采样上限 4096 + strided 标注。
- 未覆盖边界/缺口：X1、X2、X3、X4、X16；相面平滑吸收只给 `consistent_fraction`
  （诚实，保留）；网格不是冻结事实（compose 版本未记录所用网格）。
- 建议：compose 回执落 `compose_grid`（cols/rows/extent），重放优先用冻结网格；
  `gridOf` 改为与 `composeRaster` 逐分支同口径；栅格按网格对齐窗口 `block()`
  读取；逐输入 `alignment` + 顶层 `fidelity`；单点重放吸附到像元中心；
  图层缓存改共享所有权。
- 影响模块：`src/qgis/composeattribution.*`、`src/qgis/mappingartifactwriter.*`、
  `src/workflow/mappingevidencereview.cpp`、`mappingworkbench_compose.cpp`、`mappingworkbench.h`。
- 验收：①相点-only / 栅格对齐两类融合，重放网格逐像元等于真实成果栅格；
  ②不对齐输入如实 `fidelity=approximate`；③≥9 个输入时归因稳定（缓存驱逐不悬垂）。

### G04 井/地震/单因素局部证据关联 — `PARTIAL`
- 实现：`featureEvidence` 井口落面（:164-204，≤500 截断计数）、
  `seismic_sources`（:155-157）、归因输入 `upstream_receipts`（:362-380）。
- 已验证：`tst_evidencee2e` 场景 A 井意见进冲突；截断标记。
- 缺口：井坐标与图斑图层 CRS 未校验（图斑非局部网格时会错误判落面）；
  井口落面清单不可点击定位（X14）。
- 风险：L/M。验收：非局部网格图层不产生井落面结论并说明原因。

### G05 三级空间影响分析 — `PARTIAL`
- 实现：`analyzeImpact` (`mappingworkbench_impact.cpp:155-370`)；
  直接依赖 = `downstreamClosure`；确定性 = 冻结输入替换后 `replayGrid` diff；
  其余 = potential。网格口径不一致退化 potential（:116-123 防御正确）。
- 已验证：`tst_impactrecompute` 两例（输入版本 diff、约束潜在影响）。
- 缺口：X2（网格口径错误时 diff 像元不对应真实成果）、X7（人工编辑后代
  不进受影响清单）、X1（缓存）、X15；`affected_features` 为采样下界已诚实标注。
- 验收：受影响图斑覆盖 `edited_facies` 后代并带解释状态。

### G06 多源冲突分析 — `PARTIAL`（域层 `VERIFIED_COMPLETE`）
- 实现：`paleo::evidenceconflict::analyze`（七类/四级、未知≠一致≠冲突、不合成分数）。
- 测试：`tst_evidenceconflict` 15 例，覆盖语义边界。
- 缺口：冲突结果不可导航（X14）：well 意见的 `version_id` 为
  `welllog.formations:<entity>` 伪 id，finding 不携带可定位的 `entity_id/x/y`。
- 验收：冲突行可跳转来源井/输入版本/图斑复核入口。

### G08 证据反向索引 — `PARTIAL`
- 实现：`src/services/evidenceindex.*`（WAL、绑定值、有界 limit、按 sha 增量、损坏隔离）。
- 测试：`tst_evidenceindex` 9 例。
- 缺口：X8、X9、X12；行数上限截断不登记（版本被静默部分索引）；
  `pendingReview` 不含 `Disputed`；`staleSources` 只依赖记录写入时快照，
  来源版本之后被删除不会命中。
- 验收：反查行带 `unlinked`/`is_current_version`/`hit_reason`/`index_state`；
  事务失败回滚；截断登记；删除来源版本后 `stale_sources` 命中。

### G09 影响分析与受控重算 — `PARTIAL`
- 实现：`recomputePlan`/`executeRecompute`/`compareVersions`（:372-634）；
  主窗口 `impactAnalysis`/`recompute` 命令（`paleomainwindow_workbench.cpp:783-845`）。
- 缺口：X5、X6、X7、X13；`determined` 且 0 变化像元仍给 `whole_rerun`
  （应为无需重算）；`local_rerun` 实际执行整网格重算，需如实标注执行范围。
- 任务单飞：现状如实报「请等待」（未伪装），但无可续跑机制。
- 验收：报告过期拒绝执行；上游未重算时拒绝「空跑」；新版本记录
  `recompute_provenance` 并在图谱出 SUPERSEDES；重复执行幂等续跑；UI 可对比新旧。

### G10 跨算法统一回执 — `VERIFIED_COMPLETE`（本轮核验范围内）
- `paleo::receipts::normalize/compareWithPlan`，`tst_receiptsemantics` 11 例；
  compose/polygonize/single-factor/prediction 四路覆盖。未发现缺口。

### G11 大规模性能保护 — `PARTIAL`
- 已验证：采样钳、万井落面截断、索引 limit 钳、BASELINE 计时（无墙钟断言）。
- 缺口：索引首同步阻塞 UI 线程（X9，文档已承认）；归因缓存非 LRU（驱逐顺序为 QHash 任意序）。

### G12 证据反查与空间定位 — `PARTIAL`
- 已验证：证据→来源定位（点/线/面 rubber band，`evidencemapadapter.cpp:165-200`），
  UID 唯一性与重复检测、CRS 变换、图层重载后按 UID 恢复。
- 缺口：X10。

### G13 本地操作身份与审计边界 — `VERIFIED_COMPLETE`（本地语义内）
- `localOperator()` = `local:$USER`（`mappingevidencereview.cpp:48-51`），调用方身份一律忽略；
  `revise` 校验 `local:` 前缀、理由必填、`expected_revision` 乐观并发、追加式 history
  连续修订号校验（`interpretationevidence.cpp:78-98,100-113`）。
- 信任边界（如实声明）：本地操作者名来自环境变量，**不是认证**，无 RBAC；
  多人协作身份为独立设计决策，本轮不引入。
- 已知弱点：history 为 GPKG 属性内 JSON，API 只追加，但无防篡改哈希链（L，记入限制）。

### G14 端到端证据链验收 — `PARTIAL`
- `tst_evidencee2e` 3 例覆盖归因→反驳→冲突→反查→保存→重开。
- 缺口：无「反查得 A、B → 选 B → 返回 A」联动链；无「V1→P1→确认 F1→V2→影响→重算 P2
  → P1 确认保留、P2 待复核」全链。

### P0.1 统一身份体系 — `VERIFIED_COMPLETE`（核心）/ `PARTIAL`（边界）
- `SelectionTarget` 只含业务身份（`selectioncontext.h:9-17`），FID 不越界；
  图斑 UID `paleo_feature_uid`，重复 UID 判 ambiguous 拒绝定位；跨工程/会话拒绝
  （`evidenceinteraction.cpp:192-194`）；层位不符拒绝；工程重开按保存 target 恢复（:35-66）。
- 缺口：X10（反查导航改写身份）。

### P0.3 关系语义独立 — `PARTIAL`
- 归因/回执/面板：consumed 与 SUPPORTED_BY/DERIVED_FROM 分列，文案明确「不自动表示支持」。
- 缺口：X11（图谱结构混接）；归因中实际参与计算的输入未进图谱（无 CONSUMED_IN 边）；
  审核事件未进图谱（无 ReviewEvent/REVIEWED_BY）。

### P0.5/P0.6 双向联动与过期保护 — `PARTIAL`
- 地图→证据、证据→地图、图谱→地图、工程切换清空、旧 provider 销毁不清新目标：已实现。
- 证据面板快照为同步计算（无异步晚到问题）；图谱查询异步，`DataGraphPage` 以
  查询值对象比较 + 后台取消（未深入逐行核验，`UNVERIFIED`）。
- 缺口：X10；无导航返回栈。

### P1.1/P1.2 人工复核与状态机 — `VERIFIED_COMPLETE`（域层）
- 动作：link/unlink(墓碑)/update_evidence/verify_evidence/review/edit；
  记录核验（Unverified/Verified/Disputed）与解释状态（六态）严格分离；
  `canTransition` 不允许 Confirmed→Rejected 直跳（须经 NeedsReview/Disputed），
  非 review 动作把解释重置为 NeedsReview（新证据触发复核）；Superseded 只读、按新版本分叉。
- 测试：`tst_mappingworkbench` 证据用例（未逐个列举，`UNVERIFIED` 数量）。
- 缺口：多选时 UI 只传首要素的 `expected_revision`（其余要素可能被误拒，属安全侧失败，L）。

### P1.3 事务与审计 — `PARTIAL`
- 证据修订在 QGIS 编辑命令内批量写，失败 `destroyEditCommand` 回滚（:692-697）。
- 索引侧 X12。

### P1.4-P1.6 数据图谱 — `PARTIAL`
- 实现：`src/services/datagraph.*`（有界：maxNodes≤400、maxEdges≤1200、遍历工作量预算、
  Kosaraju 环诊断、缺失来源节点、历史已解除标注）、`DataGraphPage` 只读投影。
- 缺口：X11；边没有可追溯的事实来源字段；无 SUPERSEDES/REVIEWED_BY/CONSUMED_IN（归因）边。

### P2.1 索引后台化 — `MISSING`（文档已承认）
### P2.2 冻结输入解析去重 — `MISSING`（X16，且已漂移）
### P2.3 归因与真实计算一致性 — `PARTIAL`（X2/X3/X4）
### P2.4 容量 — `PARTIAL`（见 G11）
### P2.5 安全与完整性 — `PARTIAL`
- 已核验：索引 SQL 全绑定值、LIKE 转义；证据文本 ≤10000 字；图谱详情 64K 截断。
- 缺口：X15；层位名 `|`/引号与 CRS 迁移（#496 范围）本轮未逐行复核（`UNVERIFIED`）。

## 2. 最小增量改动计划（依赖序）

1. **P0-A 归因保真与内存安全**：X1、X2、X3、X4、X16（抽取 `resolveFrozenInputs`）、G04 CRS 守卫、X15。
2. **P1-B 影响/重算/索引**：X5、X6、X7、X13（provenance + UI 对比）、X8、X9（后台同步）、X12。
3. **P1-C 联动与图谱**：X10（主目标导航 + 返回栈）、X11（关系重挂 + 边事实来源 +
   CONSUMED_IN/REVIEWED_BY/SUPERSEDES）、X14（冲突/井可导航）。
4. 端到端验收测试扩展、对抗性复审、文档。

明确不做（理由）：不引入外部图数据库；不改 `composeRaster` 数值算法；
不做真实多用户认证；不做跨版本图斑一一匹配（重算后 UID 重新生成，空间对应以像元转移矩阵为准）。
