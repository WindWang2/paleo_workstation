# 综合编图证据体系深化：独立复审记录（mapping-evidence-final-review）

- 范围：`origin/master` c720aef5 → `codex/mapping-evidence-deepening`
  （619736e8 P0-A、2a61b3c6 P1-B、ece5b47b P1-C，及本文档提交）。
- 方法：每批实现落地后，由复审方逐行阅读完整 diff（不以实现方报告代替证据），
  按 gstack `review/checklist.md` 的关键类别（数据安全、并发、枚举完备、门禁完整性）
  及任务书第十二节「独立复审」清单逐项对抗；发现即回到下一批修复并补测试。
  未使用外部第二意见工具（本机无 codex/claude CLI），此为如实声明。

## 1. 复审发现与处理（Review → Fix → Re-test）

| # | 发现（复审时） | 类别 | 处理 | 验证 |
|---|---|---|---|---|
| R1 | 影响分析 after 侧重放沿用旧成果冻结网格：替换后的定网格栅格/点范围变化时，diff 不代表真实重算会用的网格（会把应为「潜在」的影响报成「确定」） | 不完整影响被夸大 | P1-B：after 侧传空 FrozenGrid，按替换后输入逐分支重建网格；网格不同即退化为潜在影响 | `tst_impactrecompute` R5 |
| R2 | 网格重建时，已从目录删除（类型未知）的输入若排在首个栅格之前，重建可能换网格 | 缺失来源被隐瞒 | P1-B：此情形判「冻结网格无法重建」 | `tst_composeattribution` |
| R3 | P0-A 起重放列出 0 像元输入，冲突分析会把它们当成 known=false 意见 | 科学语义混淆（噪声意见） | P1-B：`opinionsFromAttribution` 跳过 cells==0 | `tst_evidenceconflict` |
| R4 | `faciespolygonize.cpp` 要素序修复改动了算法文件 | 数值算法变更风险 | 复核：仅把 QHash 迭代改为按相编码排序后输出，几何与编码不变；补跑 polygonize 测试 | `polygonize` ctest 项 PASS |
| R5 | P1-B 把反查行 `source_version` 改为证据记录来源后，页面仍把命中行交给「来源定位」——双击会定位来源而非命中图斑（UI 假成功） | 错误的图斑关联 | P1-C：命中行走 `navigateFeature`，命中图斑成为主目标 | `tst_mappingworkbench` 链 C |
| R6 | 过期来源目录核验的路径拼接把 `section()` 作用在 suffix 上（碰巧正确） | 脆弱代码 | P1-C：改为 `(resolved + suffix).section(...)` | 同 e2e 过期来源用例 |
| R7 | Supersedes 边在关联/总览模式也会被收入图 | 图语义越界 | P1-C：仅成果追溯模式穿越与收边 | `tst_evidencegraph::supersedesEdgeIsLineageOnlyWithProvenance` |
| R8 | 受控重算新版本若字节相同是否会被目录去重而丢失 `recompute_provenance` | 历史覆盖/血缘丢失 | 复核 `MappingWorkbench::record` → `DerivedAssetRegistrar::stage`：每次发布新建版本，无按 sha 去重；不成立 | 代码阅读 |
| R9 | 重复按「受控重算」在任务模式下是否会并发产生两个新版本 | 并发 | compose runner 单飞（忙即拒绝并如实报因）；完成后再按由 `already_done` 幂等识别 | `tst_impactrecompute` R3 |

## 2. 第十二节清单逐项结论

- **科学语义被混淆**：图谱中生成血缘/空间候选不再连到「相类型判断」；归因胜出输入以
  ConsumedIn 呈现并在标签写明「非科学支持」；SUPPORTED_BY 只来自人工登记记录（测试断言
  Supported 边数 = 登记数）。通过。
- **错误的图斑或版本关联**：反查命中按 `layerForVersion(version_id)` 定位确切版本；
  跨工程/会话/层位拒绝；回退栈只收主目标变更。通过（R5 已修）。
- **数据丢失**：重算只新建版本；旧成果 sha 不变（测试断言）；人工确认留在旧版本、
  新版本无解释（R3 测试重读 GPKG 断言）。索引写失败回滚。通过。
- **UI 假成功**：过期计划整体拒绝、`blocked_upstream`/`no_change` 不执行且如实显示原因；
  潜在影响条目不再用旧冻结输入「空跑」。通过。
- **线程不安全**：索引同步 worker 自建/自销 SQLite 连接，只读版本快照，不触 DataCatalog
  与 QGIS GUI 对象；归因/影响/对比仍在属主线程。通过（未做 TSAN，如实声明）。
- **数据库事务不一致**：索引写事务所有失败分支 rollback；v1 索引整库重建（可重建结构）。通过。
- **证据历史被覆盖**：审核历史只经 `revise` 追加，连续修订号校验未改动；图谱只读投影。通过。
- **不完整影响被夸大**：R1 已修；受影响图斑仍为采样下界并标注；截断如实登记。通过。
- **缺失来源被隐瞒**：缺失输入带版本号原因；网格不可重建如实 unresolved；不对齐输入
  `fidelity=approximate` 附原因；反查返回 `index_state`（含同步失败）。通过。
- **旧测试通过但新路径未执行**：新增测试均直驱新路径（冻结网格逐像元比对真实成果栅格、
  9 输入缓存驱逐、过期计划、链 C 每步身份断言）；T2 证明旧 `gridOf` 会失败。通过。

## 3. 最终门禁

- 全量 `ctest -j8`（HEAD ece5b47b）：415 项，413 通过。
  - `tst_singlefactor_perf`：大样例中位数 15802 ms > 15000 ms 预算（-j8 满载）；单独 3/3 通过。
  - `tst_seismic_baseline`：分页文件校验失败（并行 I/O 负载）；单独 3/3 通过。
  - 两者均不在本分支改动的调用链上，判为负载相关的既有不稳定类（基线运行中未出现，如实记录）。
  - 基线唯一红项 `tst_evidencee2e` 已定位根因并修复（转面要素序随 QHash 种子漂移），单独 12/12 通过。
- 分层检查 `check_layering.py --strict`：通过。

## 4. 未解决问题与风险（如实）

- 本地操作者身份仍是 `local:$USER`，不是认证；无 RBAC（独立设计决策，见 TODOS.md）。
- 审核历史无防篡改哈希链。
- 多条目受控重算仍是「逐条执行、再按续跑」，未实现任务队列。
- #496 的 CRS 迁移与层位名 `|`/引号处理本轮未逐行复核（`UNVERIFIED`）。
- 截断版本每轮同步都会重扫（单版本上限 20 万要素，有界）。
- 「来源版本已不在目录」分支经公开 API 不可达（被引用来源受血缘保护不可删除），仅防御目录外部篡改。
- 未做新的大规模性能实测；性能结论沿用既有 BASELINE（`tst_evidencee2e`），新增路径均有界。
