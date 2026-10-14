# data-asset-graph-deepening（goal/data-asset-graph-deepening）

任务 A：现有数据管理、资产版本、资料图谱、文件预览与工程数据链深化。
基线 `origin/master` @ `8e963cd7`（2026-10-13，#424/#425/#427/#429/#430/#431
已合并后）。独立 worktree `../paleo_workstation-goal-data-deepening`，
共享主仓 vendor superbuild prefix（只读 symlink）。

## 排除矩阵（O1）

- **Open Issues 70 个（#351–#421）全部排除，零重叠修复**。与主责域相近的
  排除指纹：#361（dataopsundo redo 泄漏）、#364（datanavtree 拖拽 UAF）、
  #372（预览时间切片 inlVals 越界）、#376（预览 grab 无界）、#380
  （officepreviewsession 内存增长）、#386–#390（io parser 系列）、#395
  （三个 store read-write 共享 sqlite）、#396（PreviewRenderCache 键）、
  #401（datalist_undo showHealthCheck 栈对象）、#413/#414/#415/#416/#418
  （各层低危清单）。本轮全部改动落在这些指纹之外（datagraph 家族是
  #424 新文件，全仓 issue 审计基线 `198401ab` 早于其存在；sidecar 读写
  非原子不在任何 issue 指纹内——#395 只覆盖 sqlite 读连接）。
- **开放 PR #432**（goal/ci-mappingpages-20261013，tst_mappingpages 测试
  修复）为外部会话占用，未触碰。另一外部会话在
  `~/.codex/worktrees/evidence-mapping-integration`（分支
  codex/evidence-mapping-integration，B/C 方向领地），无文件交集。
- 未占用 B 领地（src/qgis、src/ui/layers、layout、constraint/composepage、
  编图 attach）与 C 领地（src/ai、src/algorithms、井震/预测/单因素核）；
  `src/services/jobrunner.*` 只读未改。

## 能力矩阵（R0 摘要）

「工区打开 → 导入 → 解析 → Catalog/sidecar 登记 → 版本历史 → 实体关系 →
资料图谱 → 预览/修订/回收/恢复 → 派生溯源」全链的生产入口、数据结构与
测试覆盖盘点结论：

| 环节 | 生产入口 | 测试覆盖 | 本轮发现 |
|---|---|---|---|
| catalog 装载/journal/BatchSave | datacatalog.cpp 家族 | tst_catalog/tst_catalog_integrity/tst_catalogstore/tst_datacatalog_sqlite 充分 | #427 后无新缺口（本轮不再动） |
| 资产/版本 mutator | datacatalog_{assets,versions,links}.cpp | 同上 + tst_assetops | 无 |
| 软删/回收/标签 sidecar | dataopsmodel.h 四 store | 仅 tst_ui_blocking 写次数探针 | **非原子写+损坏静默**（轮 4 修） |
| 回滚/对比 | assetops rollbackToVersion/compareVersions | tst_assetops | **rollbackOf 图谱不消费**（轮 1 修） |
| 资料图谱服务 | services/datagraph.* | **零测试**（DATA_GRAPH.md:101 自证） | 本轮补 tst_datagraph 28 用例 |
| 图谱工作流 | workflow/datagraphworkflow.* | **零测试** | 同上（9 个工作流用例） |
| 图谱 UI | ui/pages/datagraphpage.* | 间接（无直接测试） | 入栈 4 维/恢复 6 维不对称（递延，见余项） |
| 属性侧血缘 | workflow/derivationworkflow.* | 零直接测试 | 递延（其纯函数核 tst_derivationgraph 有覆盖） |
| 导入/预览 | services/io 各族 | tst_import/tst_derivationgraph 等 | 未动（#425/#431 已收口，重做无据） |

## 轮 1+2：图谱契约修复 + 首个测试套件（e39509aa）

**问题**（六个，全部有修前红证据，基线源码 + 新测试对跑）：

1. 概览预算保留槽算术（datagraph.cpp 原 522 行 `budget - 32`）：
   钳制下限 20 时 `budget-32 ≤ 1` 恒真 → **每个资产都被隐藏，画布只剩
   分组**；分组 `add()` 失败不计入 hidden（低报）。修：reserve 钳制
   `qMin(32, qMax(0, budget-2))` + 失败如实计数。
   红：`overviewSmallBudgetKeepsAssetsAndCountsHidden`（旧 0 资产/hidden=6
   vs 断言 1/5）、`overviewGroupOverflowCountsHidden`（旧 hidden=20 vs 22）。
2. 导出元数据张冠李戴：`exportGraph` 捕获工作流当前 `m_query`（180ms
   合并窗内可能是尚未计算的新查询），JSON 的 query 块与传入的已渲染图
   脱钩。修：`Graph` 携带产出它的 `Query`，导出与图同源；`mode` 由裸 int
   改字符串枚举（与 kind/relation 一致）。红：
   `workflowExportDescribesRenderedQuery`（旧 JSON focus=asset:b ≠ 图的 asset:a）。
3. `exportFinished` 回执被吞：导出共享查询代次计数器，导出期间任何新
   请求令 finished 回调静默丢弃（文件已落盘但页面永不知道）。修：回执
   守卫改 owner 上下文核对（新查询不吞、换工区仍丢）。红：
   `workflowExportReceiptSurvivesNewRequest`（旧超时无回执）。
4. 8 槽查询缓存 FIFO：命中不移位，搜索微调 8 次即把常驻视图全部挤出。
   修：命中移尾（LRU）。红：`workflowCacheIsLruNotFifo`（探针：填满 8
   槽→命中 q1→第 9 查询→再请求 q1 应命中；FIFO 下 q1 已被逐出）。
   注：探针第一版写错（复请求默认 maxNodes 键），修正后才有区分度——
   旧代码上验证过红。
5. assetops 回滚血缘（`extra.rollbackOf` + parentVersionIds={target} 双写）
   图谱不消费：回滚被画成普通「衍生自」边，与真实派生不可区分。修：
   与 welltops `rollbackTo` 同口径按 Rollback 参考边呈现（parents 里
   剔除该目标），血缘遍历不受影响（lineage() 含 Rollback）。红：
   `assetRollbackIsReferenceNotDerived`（旧无 Rollback 边）。
6. 查询热路径两处全量扫（收边对 index.edges 逐条扫 + 布局重心每节点
   全量扫边做 QString 等值比较，O(4·N·E)）。修：收边走可见端点出边
   邻接 + 行号排序回全局序（预算截断语义不变）；布局建一次端点邻接 +
   整型行号。**等价锚**：22 个行为用例在旧/新代码上都绿（负向验证时
   旧代码 22 绿 5 红——绿的就是等价锚）。

**A/B 计量**（同机同夹具，`populateSyntheticDerivedCatalog` 血缘形状 +
dver-1 枢纽星型，RelWithDebInfo，QElapsedTimer，中位数）：

| 查询 | 旧（中位） | 新（中位） | 倍率 |
|---|---|---|---|
| 10k index 重建 | 552ms | ~545ms | 持平 |
| 10k 概览(类别) | 13ms | 10ms | -23% |
| 10k 检索概览 | 24ms | 21.5ms | -10% |
| 10k 宽邻域(depth6,400/1200) | 20ms | 4.5ms | **4.4×** |
| 10k 血缘 | 10ms | 2ms | **5×** |
| 100k index 重建 | 6994ms | 6463ms | 持平（-8%，含负载噪声） |
| 100k 宽邻域 | 95ms | 31ms | **3.1×** |
| 100k 血缘 | 83ms | 29ms | **2.9×** |

测量时后台有全量构建在跑（-j2 nice），10k 数据为 3 样本中位、100k 为
2-3 样本；宽邻域/血缘的 3-5× 在所有样本中稳定成立，index/概览的 ±10%
视为噪声级。100k 灌库 40-46s 属已知 catalog 写路径成本（非本轮范围）。

**试作后撤销**：buildIndex 末尾预计算小写检索串（Node.searchKey）——
10k 检索查询 -20%，但索引重建 +6~15%（10k 552→623ms、100k 6983→8088ms），
100k 下检索收益被分组计数吞没（216→212ms 持平）。按「无量化净收益不
落地」原则撤销，matches() 保留即时拼接并在注释记档。教训进了 DATA_GRAPH.md
的 A/B 引用。

**测试**：`tests/tst_datagraph.cpp` 28 用例（服务 16 + 工作流 9 + 规模 1 + init/cleanupTestCase），`cmake/extra-data-asset-deepening.cmake` 注册（LIBS
paleo_workflow paleo_io）。规模测试默认 n=4000（~11s 含灌库），
`PALEO_DATAGRAPH_SCALE=10000/100000` 可选门禁（100k 全程 ~60s）。

## 轮 4：dataops sidecar 原子落盘 + 损坏告警（007e19e0）

recycle_bin/asset_tags/asset_overrides/entity_overrides 四个
`<projectDir>/.paleo/*.json`：普通 QFile 直写（断电半截 JSON = 重开静默
全丢——回收清单里的资料全部"复活"、标签全丢）+ 解析失败静默回空。
修：`saveSidecarObject` 改 QSaveFile 原子提交（失败清临时文件、原文件
完好）；`loadSidecarObject` 解析失败 qWarning 留痕（缺文件仍是正常
初始态）。红：`corruptSidecarWarnsAndLoadsEmpty`（旧代码无告警，
QTest::ignoreMessage 不匹配即红）。tst_dataopssidecar 4 用例。

**递延（需要命令层失败语义，不是一行守卫）**：锁降级只读实例
（`refusesWrites()`）仍可写 sidecar——双实例互踩。堵它需要
dataopscommands 的 Tag/SoftDelete/TypeOverride 消费 `save()` 失败并回滚
内存态 + 用户可见反馈，否则制造「假在场」（UI 显示已打标签、重开即丢）。
记入余项。

## 验证

- **修前红/修后绿**：负向验证流程 = 检出基线 5 个源文件 + 保留新测试
  （仅剔除依赖新 API 的编译面：Graph.query 断言、新夹具的规模用例）→
  重建 → 记录红集合 → 恢复 → 全绿。红集合（6+1）：
  overviewSmallBudget…/overviewGroupOverflow…/assetRollbackIsReferenceNotDerived/
  workflowExportDescribesRenderedQuery/workflowExportReceiptSurvivesNewRequest/
  workflowCacheIsLruNotFifo + dataopssidecar::corruptSidecarWarnsAndLoadsEmpty。
- **等价锚**：负向运行中 22 用例在基线代码上绿（检索/布局/四模式语义
  与旧行为一致）；恢复后 28+6 全绿。
- **门禁**：`python3 tools/check_layering.py --strict`、
  `check_ui_invariants.py --strict`、`git diff --check` 见 PR 描述（全过）。
- 全量 ctest 对照见文末。

## 独立 Review（O6）

未参与实现的子 Agent 逐文件 diff 审查（含 layout 新旧 3000 张随机图对拍、
QSaveFile 只读探针、互指回滚目录新旧对跑）。结论：P0×1 / P1×0 / P2×2 / P3×7。
- P0-1（已修）tst_dataopssidecar 无平台守卫的 geteuid/unistd.h 会打断
  Windows CI 编译——补 Q_OS_WIN 守卫（仓内 tst_singlefactor_faults 同模式）。
- P2-1（已修）workflowExportReceiptSurvivesNewRequest 末尾等待空转
  （漏 ++seen，断言未生效）。
- P2-2（已修）rollback 参考边不参与环诊断的语义收窄未声明——DATA_GRAPH.md
  补口径 + assetRollbackIsReferenceNotDerived 加 cycles==0 互指钉死。
- P3（记档）：纯回滚版本 Lineage 回执文案变为「未登记计算输入」（技术上
  属实，回滚不是计算，接受）；m_exports 残留 QPointer（量级极小）；
  回执守卫保留 session 核对（session 被视作目录上下文一部分，与旧同）；
  layout indexOf 重复 id 理论差异（add() 去重后不可达）；夹具 i=0 双父
  去重坍缩（星型形状不受影响）；测试缩进/账本算术（已顺修）。
- 等价性实证：layout 3000 随机图 0 不匹配；收边发射序/预算截断逐条一致；
  预算边界 budget=34 起与旧实现逐位相同（改动严格限制在 bug 区间 ≤33）。

## 余项与阻断

1. datagraphpage 返回栈入栈只记 4 维（focus/mode/category/stage）但
   恢复 6 维（含 search/depth/history）——搜索词调整不可回退、返回会
   悄悄回滚搜索词。文档只承诺 4 维（DATA_GRAPH.md:27），实现不对称仍
   是意外行为源。改动会动 UI 状态机，留下一轮（UI 面需配页面级测试）。
2. derivationworkflow（属性侧血缘 JobRunner）仍零直接测试——其发布
   核对与 datagraphworkflow 同型（本 round 已给同型管线立了测试先例），
   补测是纯收益，但本轮预算用尽。
3. 只读实例写 sidecar（见轮 4 递延说明）。
4. 物理清除（storagegovernancecontroller purgeAssets）与 recycle_bin.json
   无对账：已物理删除的资产在回收清单跨会话残留、restore 静默 no-op。
   需要跨 ui/work层 的信号对账设计，未动。
5. 图谱预算（maxNodes/maxEdges）无 UI 旋钮（API 可调、文档已声明）；
   小预算诚实性已修，暴露旋钮属产品决策。

## 全量结果（O3）

本 worktree RelWithDebInfo 全量构建成功（2122 ninja 目标，多会话并存期
降 -j2 遵守整机 ≤4 编译任务预算；`build/paleo` 707MB 产出）。全量 ctest
394 项：**392 过 / 2 红**，两红均为已证实的 master 既有红，与本分支无关：

1. `tst_mappingpages`（ui）——#430 步骤门控后测例断言未同步 + 三测试类
   共用 -o 文件互相覆盖；**开放 PR #432（外部会话）正在修**，按排除矩阵
   不动。
2. `tst_ui::testSimultaneousWellsAndSurveyAreaVisibility`（ctest 下 5min
   超时）——**对照实验**：本分支二进制直跑 699ms 过、复刻 ctest 环境变量
   直跑 1239ms 过；将本分支全部 6 个源文件还原为 master 源码重建后，
   同一 ctest 整跑仍超时（321s）→ 既有红（该用例开真实工区
   /home/kevin/projects/paleo_data，整跑态下挂起；主仓 10-12 旧构建
   ctest 下此用例过但另一 #432 断言红，说明红项集合随近期合并漂移）。

受影响面回归（本分支改动可能触及的既有测试）全绿：tst_assetops（回滚
语义）、tst_ui_blocking（sidecar 写次数探针，dataopsmodel 消费方）、
tst_catalog_scale / tst_perf_catalog（perffixtures 消费方）+ 新增
tst_datagraph / tst_dataopssidecar。关键套件 review 修复后双跑全绿
（28+6 × 2 遍）。门禁：check_layering --strict / check_ui_invariants
--strict / git diff --check 全过。
