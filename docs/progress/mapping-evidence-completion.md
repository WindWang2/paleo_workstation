# 综合编图证据体系最终深化（mapping-evidence-completion）

分支 `goal/mapping-evidence-completion`，基线 `d6261046`（origin/master，含已合并的 #456/#457/#458）。
目标：把证据体系从「知道算法用了什么数据」深化为「能解释每个图斑为什么如此划分、
哪些证据支持或反驳它、证据变化会影响哪些解释与成果」。
未跑的不记为通过；「当前源码」与早期分析冲突时以当前源码为准。

## 核心语义边界（本 PR 的不变量）

- **consumed ≠ SUPPORTED_BY ≠ DERIVED_FROM**：计算参与（won_cells/约束条数/井数）
  是运行后事实；地质支持关系只来自人工登记；血缘只来自 parentVersionIds。
  三者在归因/回执/图谱中各自独立呈现，绝不互相冒充。
- **归因只重放、不重算**：用成果保存时冻结的输入版本（ordered_inputs）重放确定性
  决策；输入缺失如实 `unresolved`，人工图斑如实 `manual`，不用当前版本静默替换。
- **影响三级分明**：直接依赖（血缘事实）/潜在影响（派生链可达、无法局部证明）/
  可确定空间影响（像元级重放 diff 证明）。证明不了的不伪装。
- **重算永不覆盖**：executeRecompute 只生成新版本；已确认解释保留在旧版本，
  迁移由人工复核决定。
- **反向索引不是第二事实源**：evidence-index.sqlite 是可重建的检索结构，
  原始证据/解释仍以成果 GPKG 属性为唯一权威存储。

## 差距矩阵（审计前 → 本 PR 后）

| 编号 | 能力 | 前 | 后 | 证据 |
|---|---|---|---|---|
| G03 | 图斑级自动证据归因 | MISSING（composeRaster 胜者不落盘；无 FeatureAttribution） | **已实现且验证** | `src/qgis/composeattribution.*` 冻结输入重放（priority-first-valid/threshold/categorical-nearest 与 composeRaster 逐条对齐）；`featureEvidence["attribution"]`；`composeAttributionAt` Level 3；`tst_composeattribution`（含输入版本更新后仍用冻结版、人工图斑 manual、缺失 unresolved） |
| G04 | 井/地震/单因素局部关联 | PARTIAL（分散） | **已实现且验证** | `wells_in_feature`（井口落面 + 地层优势相：Formations 表按 formation==horizon 取行，与预测写路径同口径，e2e 实测井意见进冲突分析）；`seismic_sources`；归因输入附带上游单因素回执；`tst_evidencee2e` |
| G05 | 证据变化空间影响 | PARTIAL（只有版本级血缘） | **已实现且验证** | `analyzeImpact` 三级分明 + 像元 diff + 报告快照落 `artifacts/reports/`；约束编辑保存发 `constraintsSaved` 精确点名；`tst_impactrecompute` |
| G06 | 多源冲突分析 | PARTIAL（两源一致性 + 单记录健康检查） | **已实现且验证** | `paleo::evidenceconflict`（七类 conflict_type / 四级 severity / 不合成置信度）；接入 featureEvidence 与证据面板；`tst_evidenceconflict` 15 用例 |
| G08 | 全工程反向检索 | PARTIAL（版本级反查有；证据→图斑无） | **已实现且验证** | `src/services/evidenceindex.*`（增量同步/幂等/损坏跳过/有界查询）；workbench `evidenceReverseQuery` + 证据面板「反查」；`tst_evidenceindex` 11 用例 |
| G09 | 复核重算闭环 | PARTIAL（构件齐全、编排缺失） | **已实现且验证** | `recomputePlan`（local/whole 分级 + 保护口径）/`executeRecompute`（**rerun_layer_id 修正**：ordered_inputs 的 layer_id 可能是冻结当时的 product id，重算必须用当前版本 product 声明）/`compareVersions`（像元转移矩阵）；compose 页「影响分析/受控重算」按钮；`tst_impactrecompute` |
| G10 | 跨算法统一回执 | PARTIAL（单因素有；compose/polygonize 无） | **已实现且验证** | composeRaster 每输入胜出像元（`compose_receipts`）；polygonize PROVENANCE 入版本 extra；`paleo::receipts::normalize` + `compareWithPlan`；`evidenceRecord` 聚合 `upstream_receipts`；`tst_receiptsemantics` |
| G11 | 大规模性能 | PARTIAL | **机制验证 + 实测 BASELINE** | 采样钳 4096（strided 抽样生效）、万井落面 ≤500 截断、索引 limit 钳 1000；BASELINE：500×500 成果点击 30ms、10 次重复 304ms、10k 井 50ms（`tst_evidencee2e`，遵循 tst_mapsnapshot_scale 口径不设墙钟断言） |
| G12 | 空间证据查询/容量/定位 | PARTIAL（反向无） | **已实现且验证** | 反向命中行复用 `evidenceActivated → EvidenceInteraction::navigate` 定位链（source_layer/source_version/source_feature_uid 钉定） |
| G13 | 身份与权限边界 | COMPLETE（本地语义内） | 保持 | `localOperator()` + #442 强制 + 乐观并发 + 追加式审计；本 PR 新增写路径（约束信号、重算）不引入任何绕过 |
| G14 | 端到端验收 | MISSING | **已实现且验证** | `tst_evidencee2e`：场景 A/C/D 合流（真实计算链归因→反驳证据→冲突→反查→保存版本→工程重开仍可追溯）+ 约束信号 + 有界性 |

## 新增/修改面

新增：`src/qgis/composeattribution.{h,cpp}`、`src/domain/receiptsemantics.{h,cpp}`、
`src/domain/evidenceconflict.{h,cpp}`、`src/services/evidenceindex.{h,cpp}`、
`src/workflow/mappingworkbench_impact.cpp`（analyzeImpact/recomputePlan/
executeRecompute/compareVersions/evidenceReverseQuery）。
修改：compose/polygonize 回执落库、featureEvidence 扩展（attribution/wells_in_feature/
conflicts/seismic_sources）、约束编辑会话信号、证据面板「计算归因」tab 与「反查」行、
compose 页影响分析/受控重算按钮、主窗口命令分发与 constraintsSaved 提示。

## 操作说明

- **图斑归因**：选中相面/编辑副本图斑 → 证据面板「计算归因」tab：状态摘要
  （优先级与占比、一致率）+ 逐输入决策分布（值域/编码分布）+ 上游单因素回执 +
  井口落面清单 + 冲突分析结果 + 未确定原因。
- **证据反查**：证据面板「对象与来源」tab 底部：类型下拉（待复核/反驳/失效来源/
  引用来源版本/全文检索）→「反查」→ 双击结果定位图斑。索引随查询增量同步。
- **影响分析与受控重算**：编图页选中发生版本变化的输入图件 →「分析所选输入的
  版本变化影响」→ 状态栏给出三级计数与报告路径 →「按最近影响报告受控重算」→
  任务中心生成新版本；历史版本与已确认解释不动。
- **约束变更**：QGIS 约束编辑保存成功后状态栏点名变更约束；下游影响经
  数据图谱「下游影响」或上述入口分析。

## 测试说明

新增测试目标：`tst_composeattribution`（G03/G10a）、`tst_impactrecompute`（G05/G09）、
`tst_receiptsemantics`（G10b）、`tst_evidenceconflict`（G06）、`tst_evidenceindex`
（G08/G12）、`tst_evidencee2e`（G14/G11，含 BASELINE 计时输出）。
回归：17 项证据/映射/版本套件 + 分层 `--strict` 通过；全量 ctest 见 PR 描述。

## 已知限制（如实声明）

- ~~反向索引首查在 UI 线程同步增量同步（用户触发、每版本/行数双上限）；
  未做后台化与取消。~~ 后续批次已后台化：`JobRunner` 同步 + 取消 +
  `index_state` 汇报，见 mapping-evidence-implementation.md §X9。
- ~~executeRecompute 在任务模式下逐条重算：runner 单飞，首个条目启动后
  其余条目如实报「请等待完成」（同步/测试模式下逐条串行）。~~ 单飞仍在；
  现支持 `already_done` 幂等续跑与模式词表（no_change/blocked_upstream/
  local_rerun/whole_rerun），见 implementation §X5/§X6。
- ~~冻结输入解析在 composeAttribution / composeAttributionAt /
  resolveFrozenCompose 三处同口径实现（已加一致性注释），未抽公共助手。~~
  已收敛为唯一解析器 `resolveFrozenCompose`，见 implementation §X16。

- ~~归因重放的栅格取样用 provider sample（像元中心最近邻）：……（unresolved
  注明 fidelity）。~~ 已改网格对齐 `provider->block` 块读 + 真实
  `fidelity`/`alignment` 字段；超阈值退回逐点采样并记 `sampled_fallback`，
  见 implementation §X3。
- polygonize 平滑聚合（MIN_FRACTION 吸收小斑块）会使少数像元的相编码与图斑
  编码不同——归因给 `consistent_fraction` 与编码分布，不强行归一。
- 影响分析的确定性 diff 只覆盖 composed_facies 且直接消费变化输入的成果；
  上游单因素需重算后才能确定空间范围（如实标 potential/whole_rerun）。
- ~~反向索引首查需增量同步未索引版本（用户触发、有界）；未做后台定时刷新。~~
  后台同步已落地（同上 X9）；「无查询时主动定时刷新」仍递延。
