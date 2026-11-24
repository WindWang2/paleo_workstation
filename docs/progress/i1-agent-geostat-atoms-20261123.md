# I1｜地统计计算原子：普通克里金与 SGS（实施记录）

- 分支：`codex/paleo-i1-agent-geostat-atoms-20261123`
- worktree：`/home/kevin/projects/paleo-i1-agent-geostat-atoms`
- 基线：`origin/master @ 63b26e784a475b17c734218b5bf53be7dbc9def9`
- 构建挂点：`cmake/extra-i1-agent-geostat-atoms.cmake`（根 `_paleo_extra_manifest` 仅新增一行）
- 日期：2026-11-23/24

## 1. 交付范围

将普通克里金与序贯高斯模拟（SGS）开放为 Agent 可发现、可校验、可独立调用的业务原子：

1. 由现有查询工具发现受管点样本版本；
2. 模型给出精确样本引用、单因素因子、S1–S4 分析单元、计算网格和科学参数；
3. 宿主在 owner 线程解析并冻结身份，在 TaskService worker 中物化样本快照；
4. 经既有 `CapabilityHost` / `CapabilityRecovery` / `OperationLog` 执行 prepare、compute-only、publish；
5. 数值执行只调用既有 `NativeCppAdapter::runOrdinaryKriging` / `runSgs`，没有第二套克里金/SGS 实现；
6. 成果通过既有 output set、版本提交和 receipt 链交付为 catalog 受管版本，不自动显示。

首版明确不包含 co-kriging、协变量、工程约束、硬屏障、自动拟合、自动退化 IDW、任意 JSON 样本数组、通用 `runById`、DAG 新算子和个体实现集。SGS 固定 `persistRealizations=false`，只交付 OUTPUT/STD，不生成 `MEMBER_n`。UI 原有地统计工作流及其 v1 算法描述不改。

## 2. 公开能力、版本和工具名

| 项 | 普通克里金 | SGS |
|---|---|---|
| capabilityId / contractVersion | `geostat.kriging` / 1 | `geostat.sgs` / 1 |
| tool alias | `paleo.geostat_kriging` | `paleo.geostat_sgs` |
| 精确算法契约 | `geostat:ordinary_kriging` **contractVersion 2** | `geostat:sgs` **contractVersion 2** |
| exposure | Callable | Callable |
| execution shape | global | global |
| 输出端口 | OUTPUT（估值）、VARIANCE（克里金方差） | OUTPUT（实现均值）、STD（实现间离散度） |
| 结果类型 | `geostat_estimate_raster`、`geostat_variance_raster` | `geostat_mean_raster`、`geostat_std_raster` |
| 取消/重启 | 协作取消；从持久冻结计划确定性重跑，不宣称检查点恢复 | 同左；相同 seed 和输入逐字节复现 |

两个旧 `geostat:*` v1 spec 保留，`AlgorithmSpecRegistry::spec(id)` 的首项仍是 v1。Agent 能力只引用 v2，未知版本明确失败。`CapabilityRegistry::exposureForAlgorithm` 同步改为版本精确，避免 v2 能力行把同 id 的 v1 算法也标成 Callable。

Builtin 与 Pi 使用同一 `AiToolSpec`、`AiChatToolRunner::runCapability`、Host 和效果门。Pi 的 `chatinvocationbinding` 按 schema 顶层 `client_request_id` 自动纳入请求号绑定，没有第二条执行链。

## 3. 输入、参数与输出

### 3.1 信封

- `contract_version`：缺省 1；未知版本失败。
- `client_request_id`：必填，用于 `(document, callerScope, clientRequestId)` 幂等。
- `binding_mode`：首版生产路径为 `pinned-value`。
- `publish_intent`：`prepare` / `compute-only` / `publish`。
- `parameters`：科学参数；compute-only 继承时应省略。
- `inputs`：封闭键集合；不接受 actor、approved、任意路径、WKT 或权限自称字段。

### 3.2 业务输入

| 键 | 语义 |
|---|---|
| `sample_ref` | `catalog:<versionId>` 或既有受管井点层键（如 `wells.T70`）。宿主解析，不能自称受管路径。 |
| `factor_id` | 首版为 `sandthick` / `sandratio` / `strathick` / `perm`。数值单位与字段来自 `SingleFactorRegistry` 的井点口径；`poro` 的单位在词表中仍是「小数或 %」，明确拒绝。 |
| `field` | 可省略；给出时必须等于因子登记字段，防止同名字段被误认。 |
| `unit_selection` | `schemeId` / `intervalId` / `subIntervalId`，可选 `revision`。宿主用 `freezeAnalysisUnit` 做 Data Service 实体核验；Boundary/Interval/aggregate 拒绝。 |
| `grid` | `min_x/max_x/min_y/max_y/cell_size`，样本投影 CRS 下的米制 extent。north-up，左上外角原点，跨度非整数倍时东/南最多外扩一个像元。 |
| `source_operation_id` | compute-only 继承 prepare 时为唯一业务输入；publish 已计算结果时为唯一输入。 |

样本↔单元关联有两条可验证证据路径：井点层键挂冻结单元父上界面（`wells.<topBoundary>`），或版本 extra 的 `schemeId + intervalId + subIntervalId` 三元组逐字一致。仅子层段名相同不能证明关联，证据不足返回 `geostat_sample_unit_unverified`。

### 3.3 参数

默认值只来自 v2 `AlgorithmSpec`，经既有 `validateAndFillParams` 产生 effective parameters。

- 共同：`VARIOGRAM_MODEL`（spherical/exponential/gaussian，默认 spherical）、`NUGGET≥0`（默认 0）、`SILL>0`（默认 1，部分基台）、`RANGE>0`（默认 1）、`ANISOTROPY_RATIO≥1`（默认 1）、`AZIMUTH_DEG∈[0,360)`（默认 0）、`MAX_POINTS∈[1,64]`（默认 16）、`SEARCH_RADIUS≥0`（默认 0）。
- Kriging：`MIN_POINTS∈[1,64]`（默认 4，仅半径模式生效）。
- SGS：`REALIZATIONS∈[1,64]`（默认 4）、`SEED` 十进制字符串（默认 `"42"`）。支持完整 quint64；>2^53 不经 JSON double，摘要和回包仍用精确十进制字符串。

显式 `constraints_ref`、协变量、屏障、任意样本数组、`MEMBERS`、`persist_realizations` 等均返回明确 `geostat_*_unsupported`，不静默忽略。

### 3.4 输出与成果元数据

- compute-only：attempt 私有目录内恰好 OUTPUT/VARIANCE 或 OUTPUT/STD；必需成员缺失、额外端口、全域无有效值都不作为成功成果暴露。
- publish：只消费已有 `source_operation_id`，不重新取样本或计算。每个端口形成一个受管 catalog 版本。
- 元数据来自冻结计划：`unitSelection`/`schemeRevision`/`intervalId`/`subIntervalId`、`factor_id`、`field`、`value_unit`、`crs_authid`、`cell_size`、`variogram`、源版本/SHA、快照 SHA、算法/版本、finite/nodata/solver failure 等。
- 结果 `kind` 与 UI 单因素成图产品区分，不冒充 `single_factor_raster`，不借 `subinterval_verified` 进入单元证据投影。成果以 catalog 受管版本交付，不私造 Data_service schema，不宣称跨 store ACID。
- SGS STD 是实现间离散度，不是克里金方差、置信度或科学通过阈值。

## 4. 资源估算与准入上限

估算在大数组/栅格创建前完成，使用 checked arithmetic。口径为结构估算，不是 RSS 实测或科学质量阈值。

| 常量 | 上限 | 依据 |
|---|---:|---|
| `kGeostatAtomMaxSamples` | 20,000 | 限制样本扫描/索引/快照成本 |
| `kGeostatAtomMaxCells` | 4,000,000 | 与 UI 地统计工作流预算同值 |
| `kGeostatAtomMaxRealizationBytes` | 512 MiB | `REALIZATIONS × cells × 8B`；内核 4 GiB 硬闸内的交互档 |
| `kGeostatAtomMaxSnapshotBytes` | 8 MiB | 私有样本 JSON 快照 |
| `kGeostatAtomMaxGridSide` | 100,000 | 单边维度护栏 |
| `kGeostatAtomMaxPeakBytes` | 640 MiB | 样本 + 网格工作数组 + 实现数组 + 邻域/簿记的最坏组合 |
| `kGeostatAtomMaxOutputBytes` | 64 MiB | 两个 Float32 端口 + 头余量 + 快照估算 |

估算字段为 `sampleBytes`、`gridBytes`、`realizationBytes`、`scratchBytes`、`peakBytes`、`snapshotBytes`、`outputBytes`。prepare 准入按样本上限作保守上界，物化后按实际样本数重算并写入冻结计划。恢复时按计划字段重新计算并逐项比对，估算被改即拒绝。

## 5. 生产实现与装配

### 5.1 I1 专属文件

- `src/services/geostaticapabilityspec.{h,cpp}`：v2 算法描述、capability 行、因子准入与词表一致性检查。
- `src/workflow/geostatatomcontract.{h,cpp}`：严格参数/输入/网格解码、资源估算、冻结计划 JSON、完整性与一致性校验、模型可见有界事实。
- `src/workflow/geostatatominputs.{h,cpp}`：后台样本物化、内容寻址快照、恢复和 SHA 校验。
- `src/workflow/geostatatomoperation.{h,cpp}`：`CapabilityExecutionAdapter` 的 capture/validate/restoreWorker/restoreInputValidator/validateFrozen，以及真实数值 worker 和工具回包装饰。
- `src/qgis/geostatsamplereader.{h,cpp}`：QGIS 层的有界点样本读取。workflow 不新增 qgs 头散布。
- `src/ai/chat/geostatictools.{h,cpp}`：工具描述与嵌套 JSON schema。
- `src/app/geostaticwiring.{h,cpp}`：生产 provider 与代次绑定。
- `tests/tst_geostat_atom_contract.cpp`、`tests/tst_geostat_atom_operation.cpp`。

### 5.2 共享文件最小接缝

- `src/services/capabilityspec.cpp`：追加两条 capability 行；`exposureForAlgorithm` 版本精确化。
- `src/services/algorithmspec.cpp`：`allSpecs()` 追加 I1 v2 工厂。
- `src/services/commandspec.cpp`：追加 `geostat.kriging` / `geostat.sgs` 两个 Agent-only 命令，使 `CommandRegistry::agentCommandIds()` 与 `CapabilityRegistry::callableIds()` 一致；不新增 ribbon 入口。
- `src/ai/chat/domaintools.cpp`：追加工具工厂、精确工具名识别和 schema 委派。
- `src/app/aiwiring.cpp::bindChatToolRunner`：一次 `bindGeostatAtomCapabilities` 调用。
- `src/workflow/aichattoolrunner.cpp::runCapability`：完成回调中一次 I1 回包装饰委托；不改 `startNext`、`finishActive`、H7 quota 或 controller。
- 根 `CMakeLists.txt`：manifest 一行。
- `tests/tst_aichat*`：工具数从 26→28、17→19 及成员集合更新。
- readiness 使用既有 `isAiCapabilityTool` 通用门，不要求 ORT；未改 readiness 文件。

### 5.3 三阶段与恢复语义

- **prepare**：owner 严格解码和实体核验 → worker 有界读取并写快照 → owner 复核代次/源版本 → `Recovery::accept` 持久化 phase=prepare。返回的是 durable operationId，不是进程内 opaque handle。数值 worker 不执行，`computeCount=0`。
- **快照**：`.paleo/capability-ops/geostat-snapshots/<sha256>/samples.json`，内容寻址；同版本同字段去重。快照含源版本/SHA、因子、字段、CRS WKT/摘要、样本三元组与 skipped 计数。恢复时形状、相对路径、常规文件、SHA、头字段逐项对账。
- **保留**：冻结计划以 `sourceVerification{path,versionId}` 引用快照，这是 `operationretention` 已识别的引用形态，快照目录随操作记录成为保留根。没有另改全局 GC，也没有删除成功成果的路径。孤儿快照回收与既有共享输入束一样仍是后续工作。
- **compute-only 继承**：只从持久 prepare 记录继承冻结计划。省略参数表示继承；给出参数必须与冻结 effective parameters 完全一致。快照在工作线程、立项前验证；缺失/被改返回 `geostat_snapshot_unavailable`，不回退当前样本。
- **compute**：调用既有 native adapter，写 attempt 私有目录。worker 只持纯值；QGIS CRS 由 WKT 在执行线程重建。取消接 `Control.cancelled`。
- **publish**：只走 `freezeSourcePublication` 和 output set/receipt 链，copySource 不增加源操作 computeCount。
- **重启/恢复**：新 Host 或真实新进程仅凭 operation journal 与快照恢复；`validateFrozen`、`restoreInputValidator`、`restoreWorker` 分别校验计划、输入和数值执行。未显式 resume 的中断操作不得自动重算。
- **幂等**：同键同摘要复用原 operation/receipt；同键不同参数/单元/网格/seed 报 `idempotency_conflict`；compute-only 继承时不同参数报 `geostat_source_parameters_conflict`。
- **取消**：取消请求与实际终态分离；已提交成果不可改写成未发生。提交后取消返回 `cancel_after_commit`。

## 6. 审查、发现与修复

### 6.1 第一轮独立评审

未参与主要实现的独立 Agent 读取了真实 diff 和宿主契约，结论为无 P0，3 个 P1、6 个 P2、4 个 P3。逐项处理如下：

| 发现 | 修复 | 回归证据 |
|---|---|---|
| SGS 摘要 seed 被内核 double 诊断覆盖 | 冻结计划/摘要统一精确十进制字符串，标量诊断不覆盖 seed | `seedRoundTripsExactlyBeyondDoublePrecision`、`sgsSameSeedReproducesWithoutMembers` |
| 单元关联只比较 subIntervalId | 改为 scheme+interval+sub 三元组或既有层键上界面关联 | `sampleUnitAssociationIsVerified` |
| schema 边界与解码不一致 | extent 不设正负下限；SILL/RANGE/cell_size 用 exclusiveMinimum；azimuth 用 exclusiveMaximum | `toolSchemasMatchContract` |
| 快照不受保留/无限积累语义不清 | 使用 retention 已识别的 `sourceVerification` 引用；快照目录成为保留根；孤儿 GC 限制如实记录 | `krigingPrepareComputePublishReplayChain` 中 `operationRetention().complete/protectsPath` 断言 |
| preparation worker 携带完整 ExecuteRequest | 仅捕获纯值身份，不携带 catalog/claim/QObject | 线程/对象切换端到端用例 |
| compute-only 继承参数被静默忽略 | 省略=继承；不一致=冲突 | `inheritRejectsConflictingParameters` |
| 恢复快照路径未限制工程相对形状 | 内容寻址固定路径 + 链接/绝对路径/../拒绝 | 快照缺失与篡改用例 |
| 冻结计划未校验单元 | scheme/revision/interval/sub 必填，aggregate 拒绝 | `frozenPlanRejectsTampering` |
| 其他 P3 | 删除死 provider；取消哨兵精确匹配；空 intent 语义；ceil 网格语义入文档和 schema | 对应契约/操作测试 |

### 6.2 自测与最终单独 diff 复核发现并修复

- prepare 原本只有 operationId，成本事实对模型不可见：新增 `geostatAtomToolResponse`，按同文档、同 callerScope 读取持久计划并投影 `frozenPlan`，再经过回包预算；不暴露路径/WKT/大数组。
- 全 nodata 曾被适配器当成功：增加 `finite_cells <= 0 → geostat_no_finite_cells`，不交付空栅格。
- 样本单位原先没有真实来源：改为 `factor_id` + `SingleFactorRegistry` 井点口径；poro 等未确认单位明确拒绝。
- NULL 数值曾被 QGIS 表示为 Double/0/isNull 而被误读为 0 值样本：读取器与 UI 链一致检查 `isValid/isNull`，并增加 0 值误收回归断言。
- `exposureForAlgorithm` 不按版本导致 v1 也被标 Callable：改为版本精确，既有 `tst_algorithm_contract` 保持通过。
- 最终 diff 复核补出：非整数网格/样本计数可能被 `toInt()` 截断；effective parameters 与 variogram/seed 可互相矛盾；`MEMBERS` 只报未知参数。现已增加整数表示检查、参数一致性校验和明确 unsupported。
- 完整 fast lane 另暴露 CommandRegistry 的 Agent 面缺少两条命令（13 callable vs 11 agent commands）：补齐 `commandspec.cpp` 两个 Agent-only 行后，`tst_commandregistry` 通过，Agent 命令集合与 Callable 集合一致。
- 提交故障测试证明 `ExecuteResult.ok` 只代表 prepared set 就绪，不能代替 commitState；验收以持久提交状态为准。

第二轮独立评审 Agent 因调用方配额限制未能完成（失败时间 2026-11-24）。不冒称完成了第二轮独立评审；按任务书限制记录，随后对最终 diff 做了逐文件、逐函数的单独复核并补了上述校验。

## 7. 生产调用重放（真实 runner 产物）

以下请求和回包均由 `AiChatToolRunner` 生产路径实际产生，不是 helper 假调用。原始 JSON 一并保存在
[`docs/progress/evidence/i1-agent-geostat-atoms-20261123/`](evidence/i1-agent-geostat-atoms-20261123/)：

| 文件 | 真实调用 |
|---|---|
| `01-query_project.json` | `paleo.query_project {topic:"assets", asset_type:"well_factor_points"}`，返回受管版本 `ver-1`。 |
| `02-query_project.json` | `paleo.query_project {topic:"version", version_id:"ver-1"}`，读取该版本详情。 |
| `03-geostat_kriging.json` | Kriging prepare；显式 spherical、RANGE=40、6×6 网格。回包 `execution=prepared`、`computeCount=0`、持久 operationId 和 `frozenPlan`（25 样本、单位 m、CRS EPSG:3857、资源估算）。 |
| `04-geostat_kriging.json` | 用 prepare operationId compute-only。回包 `execution=succeeded`、`commitState=not_requested`、OUTPUT/VARIANCE 固定端口、36 finite cells。 |
| `05-operation_get.json` | 既有 operation_get 查看真实状态与 computeCount=1。 |
| `06-operation_results.json` | 既有 operation_results 读取 OUTPUT/VARIANCE 的 SHA、大小、端口单位/axis，无路径泄露。 |
| `07-geostat_kriging.json` | 用新请求号 publish，返回 committed receipt。 |
| `08-geostat_kriging.json` | 同一发布请求重发，复用同一 operationId 和 `commitReceiptRef`，computeCount 未增加。 |
| `09-geostat_sgs.json` | 同一受管输入做固定 seed SGS，回包 OUTPUT/STD，未生成 MEMBER_n。 |
| `10-geostat_kriging.json` | 显式 `constraints_ref` 被真实入口拒绝：`{"code":"geostat_input_unsupported"}`。 |

实际重放命令（本 worktree，环境前缀见第 9 节）：

```bash
. /tmp/i1-test-env.sh
PALEO_I1_RECORD_DIR=/tmp/i1-record-final \
  ./build/tst_geostat_atom_operation realRunnerDeliversPrepareAndComputeWithFrozenPlan
```

文档中的 operationId/versionId 是该临时工程实例的真实值，重放时以新工程查询结果为准。

## 8. 本地验证记录

以下只记录实际执行结果；不把源码静态推断写成已通过。

### 8.1 已完成的 I1 与契约验证

- `tst_geostat_atom_contract`：**14/14 slots passed**。覆盖精确版本/别名、参数默认值、严格类型、seed 边界、网格和实现预算、因子词表、冻结计划往返/防篡改、schema 一致性、有界无路径事实。
- `tst_geostat_atom_operation`：**22/22 slots passed**。覆盖：
  - prepare → 宿主重建 → compute-only 继承 → 宿主重建 → publish → replay；
  - 数值 oracle：样本格心估值=样本值、方差=0，离开样本处方差>0；
  - SGS 同 seed OUTPUT/STD 逐字节一致、异 seed 不同、无 MEMBER_n；
  - 伪造单元/旧修订/错误版本/错误关联/未知因子在数值前拒绝；
  - 地理 CRS、字段缺失/非数值、NULL 缺测、无有效点、重复坐标合并；
  - 全 nodata 不成功、快照缺失/篡改不可恢复；
  - 继承参数冲突、方案修订后的 pinned-value、取消前回放拒绝；
  - 准备在途切工程 stale、计算中取消；
  - 必需成员缺失、提交 journal 写盘故障及恢复；
  - 真实 `AiChatToolRunner` 全流程与 constraints_ref 拒绝；
  - 三个真实子进程场景：prepare 后重启 compute、compute 后重启 publish、running 崩溃后 resume，以及快照删除后的恢复失败。
- `tst_algorithm_contract`：6/6，验证 `exposureForAlgorithm` 版本精确化未破坏既有断言。
- `tst_aichat` / `tst_aichatprojectquery` / `tst_aichattoolloop`：工具表更新后的回归已执行通过。

### 8.2 全量构建与受影响回归

- 全量构建：`./paleo-dev build -- -j4`，**exit 0**；补齐 CommandRegistry 后再次完整构建及 ORT-off 重建也均 **exit 0**。
- 受影响回归命令：
  `LD_PRELOAD=/usr/lib/libexpat.so.1 QT_QPA_PLATFORM=offscreen ctest --test-dir build -j1 -R "tst_geostat_|capability|tst_aichat|tst_agent|tst_algorithm_contract" --output-on-failure`
  → **35/35 passed，336.37s**。覆盖 geostat kernel/workflow、capability Host/Recovery、aichat/agent 工具契约和 algorithm contract。
- `tst_geostat_atom_contract` 14/14、`tst_geostat_atom_operation` 22/22、`tst_algorithm_contract` 6/6 均在最终改动后重链重跑。

### 8.3 ORT-off

独立目录 `build/ortoff`，使用同一 vendored 前缀：

```bash
BUILD_DIR=build/ortoff ./paleo-dev configure \
  -DPALEO_ONNXRUNTIME=OFF -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu
BUILD_DIR=build/ortoff ./paleo-dev build -- -j4 \
  tst_geostat_atom_contract tst_geostat_atom_operation
```

configure/build 均 exit 0；补齐 CommandRegistry 后也重新构建成功。cache 确认 `PALEO_ONNXRUNTIME:STRING=OFF`。随后直接运行：
`./build/ortoff/tst_geostat_atom_contract` → **14/14 passed**；
`./build/ortoff/tst_geostat_atom_operation` → **22/22 passed**（补齐 CommandRegistry 后重跑同为 14/14、22/22）。
因此两条地统计能力不依赖 ORT，纯 C++ 生产链可用。

### 8.4 完整 fast lane

命令：

```bash
LD_PRELOAD=/usr/lib/libexpat.so.1 QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build -j1 -LE 'heavy|perf' --output-on-failure
```

完整一轮实际结果：**637 项中 629 通过，2852.13s**。8 个失败项及处置：

| 项 | 处置 |
|---|---|
| `tst_commandregistry` | 真实接线缺口：Agent 命令面只有 11 项而 Callable 有 13 项。补齐 `commandspec.cpp` 两行后定向重跑 **9/9 passed**；I1 两套测试同步重跑通过。 |
| `tst_cache_segyindex` | 直接重跑 `build/tst_cache_segyindex` **27/27 passed**（exit 0），保留为环境/并发瞬时失败，不改测试。 |
| `first_configure` | 用未修改 origin/master 原样导出复现同一 multiarch 缺失，见第 8.5 节证据。 |
| `tst_seismic_3d`、`tst_propmodelpanel`、`tst_seismic_3dui`、`tst_seismic_3dviz`、`tst_seismic_highdpi` | 均为 `X_GLXCreateNewContext BadValue`。直接运行与 `LIBGL_ALWAYS_SOFTWARE=1` 仍同样失败；本 diff 不涉及这些 OpenGL/地震视图实现，未跳过或改写测试。 |

因此本 diff 引入的 `commandregistry` 失败已修复并定向验证；`cache_segyindex` 单独重跑通过。其余 6 项保留为已取证的宿主环境红项，不改基线、不删测试、不宣称全套绿色。

### 8.5 源码闸门

在最终 diff 上实际运行并通过：

```text
check_layering.py --strict / --selftest / --symbol-audit / --transitive
check_layering.py --qgs-audit --strict
check_qgis_spread.py --strict
check_ui_invariants.py --strict / --selftest
check_ui_size.py --strict / --selftest
check_ui_tokens.py --strict / --selftest
check_file_budget.py --strict / --selftest
check_i18n.py / --selftest
check_ts_stale.py / --selftest
sync_en_ts_skeleton.py --check
check_qgis_versions.py
check_docs_drift.py
cmake -P cmake/verify-cpp-mainline.cmake
```

`--transitive` 显式使用 `QGIS_PREFIX=$PWD/vendor/prefix/usr`，结果 48 heavy_edges / 41 heavy_tus，与冻结基线一致；qgs-audit 134、qgis-spread 169，均新增 0。`git diff --check` 通过。

`first_configure` 的红项有独立对照证据：把未改动的 `origin/master@63b26e78` 原样导出到临时目录，以相同 deb 前缀执行 `cmake/verify-first-configure.cmake`，G1 同样因 Arch 宿主 `CMAKE_LIBRARY_ARCHITECTURE` 为空而报 `lib//qt6/plugins/platforms/libqoffscreen.so` 缺失，G2–G5 通过。因此该项是主干在本宿主的既有环境问题，不是本 diff 引入；未通过修改基线或跳过检查掩盖。

## 9. 构建与环境

- 平台：CachyOS x86_64，Linux 7.3.4-1-cachyos，GCC 16.2.1，CMake 4.5.1。
- 构建：RelWithDebInfo，`-j4`；测试 `ctest -j1`。
- QGIS：vendored deb 闭包 `vendor/prefix/usr`，链接到同机已准备的受控前缀；QGIS 4.2.3 三路版本闸通过。
- configure 参数：`QGIS_PREFIX=$PWD/vendor/prefix/usr`、`CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`。Arch 不自动填 multiarch，显式指定是本机必要参数。
- `vendor/onnxruntime`、`vendor/ranuts-document` 使用主仓受控依赖链接；configure 报 `office-editor: 已就绪`，无 Office 缺件警告。
- `vendor/Data_service` 按主仓 gitlink `905b5c1` 初始化。
- 默认构建 `PALEO_ONNXRUNTIME=AUTO`；另设 `build/ortoff` 做 `PALEO_ONNXRUNTIME=OFF` 验证。
- 构建锁：`/home/kevin/projects/paleo_workstation/.git/paleo-parallel-20261122-agent/heavy-build.sh`，owner `i1-agent-geostat-atoms`。早期增量构建曾与 I2 锁持有期重叠（各自独立 build 目录，无共用可写编译树）；发现后全部改为锁内串行。I2 锁记录在其构建/测试进程消失后仍滞留，按协议只释放锁记录，未动 I2 文件或分支。
- 直接运行测试二进制使用 vendored Qt/QGIS/GDAL/PROJ 环境；本机 `paleo-dev test` 注入的 deb 库会使系统 `ctest` 自身出现 `XML_GetCurrentColumnNumber64` 符号冲突，因此 ctest 使用 `LD_PRELOAD=/usr/lib/libexpat.so.1`，与直接二进制的运行结果区分记录。

## 10. 并行边界与剩余限制

- 复用 H1–H7 的上下文/回复预算、DAG、操作恢复、计划审阅、归档和 operation 发现；没有新建第二套任务/权限/目录系统。
- P5 编辑生命周期分支保持不动；没有改 constraintstore、facies partition/editor、mapping workbench 或编辑生命周期逻辑。
- 不依赖 I2–I5 的新 API、文件或提交。共享注册文件只追加本任务必要行；合并时保留各方增量。
- 提 PR 前复核到开放 PR I3 #1076、I5 #1077、I4 #1078 也触及 `domaintools.cpp`、`aichattoolrunner.cpp`、根 manifest 等共享接缝；这些文件的增量均独立成段。合入时必须保留双方追加并复查重复 ID/版本/工具名，不宣称共享文件没有文本冲突。
- 首版只支持受管 OGR 点样本的投影米制 CRS；缺 CRS、地理经纬度、非米制单位、未知单位、无有效点均明确拒绝，不重投影、不降级。
- 首版网格只支持显式 extent + cell size，不支持受管参考网格派生。
- 不宣称所有地统计、所有约束、个体实现集、co-kriging、自动拟合或任意 DAG 可调用。
- 快照与既有共享输入束一样，保留契约已接通，但孤立快照回收仍是后续工作；本任务没有借清理之名删除成功提交资产。
- Windows/MSVC 未在本 Linux 宿主实跑；新增源码做了 C++20/MSVC 兼容性静态检查（无 and/or/not、无指定初始化、直接 include 所需标准头、无 braced narrowing 宏逗号问题）。
