# I2：地层单元 RMS → 本地 ONNX 推理原子（实施记录）

- 分支：`codex/paleo-i2-agent-unit-onnx-atoms-20261123`（worktree `../paleo-i2-agent-unit-onnx-atoms`）
- 基线：`origin/master` = `63b26e784a475b17c734218b5bf53be7dbc9def9`（2026-11-23 快照，fetch 后确认即最新主干）
- 构建挂点：`cmake/extra-i2-agent-unit-onnx-atoms.cmake`（根 manifest 仅追加一行）
- 日期：2026-11-23/24

## 1. 交付范围（W1–W10）

把「明确分析单元（S1–S4）→ 受管地震体层段窗口 RMS → 钉权重本地 ONNX 分类推理」
开放为 Agent 可调用的持久操作原子：

| 身份 | 值 |
|---|---|
| capabilityId | `unit.onnx_predict`（contractVersion 1，Callable） |
| 工具名 | `paleo.unit_onnx_predict`（别名，经 `isAiCapabilityTool` → `runCapability` 通用生产链） |
| AlgorithmSpec | `onnx:unit_rms_predict` v1（无参数；端口 UNIT_RMS / OUTPUT / DIAGNOSTICS(私有诊断)） |
| 实现版本 | 1 |

三段语义（与 tile v2 同构，复用 CapabilityHost/CapabilityRecovery 公共链）：

- **prepare**：owner 线程轻查询（注册表钉 SHA + catalog 地震版本 + 资产类型 +
  文件有界指纹），任务线程完成真实 ORT 加载与签名/特征合同核对、
  `freezeAnalysisUnit`、`prepareSeismicUnitInput` 物化 64×64 RMS；冻结计划
  （含样本字节 + schemaHash 自校验）整体进入 phase=prepare 的持久
  FrozenOperation，返回 operationId。不登记正式成果、不做展示声明。
- **compute-only**：只接受 prepare 的 `source_operation_id`；继承机制复制冻结
  计划/目标/父版本；worker 只消费冻结样本（NaN→0 喂入 + 有效 mask 重施），
  `runPinnedTensor` 钉权重推理；OUTPUT.tif（Byte 类码，nodata=255）入私有
  operation 工件，DIAGNOSTICS.json 入私有诊断（发布时自动剔除）。重启后按
  同一冻结输入计算，不重读地震原件。
- **publish**：只接受已计算 source 的 `source_operation_id`；泛化
  freezeSourcePublication + copySource 路径提交（catalog +1 版本、receipt），
  不重复推理、不要求当前 ORT/模型仍可用；不自动选择/显示图层（presentation
  = not_requested）。

### 与任务书建议文件的对应

| 建议 | 实际 |
|---|---|
| `src/services/unitonnxcapabilityspec.*` | ✓（身份常量 + 特征合同 + CapabilitySpec 工厂） |
| `src/workflow/unitonnxcontract.*` | ✓（冻结计划 + `prepareUnitOnnxPlan` 物化） |
| `src/workflow/unitonnxinputs.*` | 并入 unitonnxcontract（物化即冻结计划构造，无独立输入层） |
| `src/workflow/unitonnxoperation.*` | ✓（`computeUnitOnnxPlan` 受控推理 + 产物/诊断） |
| `src/ai/chat/unitonnxtools.*` | ✓（工具表工厂 + 形状校验 + readiness 委托） |
| `src/app/unitonnxwiring.*` | ✓（`installUnitOnnxCapability`） |
| （新增）`src/workflow/unitonnxservice.*` | per-capability 适配器（intent/capture/restore/validateFrozen） |

### 共享文件增量（均为最小追加）

- `CMakeLists.txt`：manifest +1 行。
- `src/services/algorithmspec.cpp`：allSpecs() +1 工厂调用（`appendUnitOnnxSpec`）。
- `src/services/capabilityspec.cpp`：table() +1 项（`paleo::unitOnnxCapabilitySpec()`）。
- `src/ai/chat/domaintools.cpp`：+include、builtinAiToolSpecs +1 append、
  isAiCapabilityTool +1 名称、validateParameters 末尾 +1 委托分支。
- `src/workflow/aichattoolrunner_readiness.cpp`：+1 分相 readiness 委托。
- `src/app/aiwiring.cpp`：bindChatToolRunner +1 安装调用。
- `tests/tst_aichat.cpp` / `tests/tst_aichatprojectquery.cpp` /
  `tests/tst_aichattoolloop.cpp`：既有工具表精确清单钉 +1（登记新工具的
  配套更新，与 H3/H4/H5 同例；不删断言）。
- `translations/paleo_zh_CN.ts` / `translations/paleo_en.ts`：+7 条工具文案
  （QObject 上下文末尾最小增量，zh 译文=源串，en 英文译文）。

### 关键契约决定

- **v1 特征合同**：`{version:1, kind:"layer_window_rms", unit:"amplitude",
  domain:"TWT", preprocessing:"raw_float32_v1"}`（经
  normalizeRasterFeatureContract 词表）。模型 manifest `input.features` 必须
  声明与之一致的单特征（`input.featureContractVersion:1`）；按模型名/
  dataType 猜语义一律拒绝（`unit_feature_contract_mismatch`）。预处理语义
  如实记录：缺覆盖格 NaN→0 喂模型、输出端重施有效 mask，不宣称缺测格
  获得可信预测。
- **输出合同**：仅 logits/probabilities 分类；argmax 类码=模型通道索引，
  无地学标签映射（provenance/note 明示）；probabilities 走严格
  probabilityGrid 校验，不裁剪不归一。
- **不支持的明确拒绝**：任意张量形状（非 [1,1,·,·]/固定维≠64）、非 float32、
  多特征/特征版本≠1、classes 与实测通道不符、未知输出语义、aggregate/
  伪造 S 子段、binding_mode 非 pinned-value、contract_version≠1。
- **幂等规范化**：`adapter.intent` 把省略的 binding_mode 规范为 pinned-value
  （与 CapabilityHost::prepare 对 saved.request.bindingMode 的填充一致）——
  显式写出与省略是同一业务意图，不因表示不同分裂幂等键。
- **父版本**：只有实际消费的地震 catalog 版本；模型无 catalog 版本不造父。
- **源指纹**：注册表钉 SHA（模型）+ catalog 版本 sha256 + 文件 size/mtime
  有界指纹（地震）；不对大体整哈希。compute 不重读地震原件——冻结样本即
  快照。
- **取消**：物化/推理/写盘前后协作检查；ORT Run 本身不可硬中断（既有
  `runPinnedTensor` 合同），Run 返回后丢弃未提交输出；已提交 receipt 不改写。
- **错误码**：机器可处理 ASCII 标签，按调用点分类（单元冻结失败→
  `unit_selection_invalid`；层段取数失败→地震来源/窗口分类码；ORT 加载→
  `onnxPublicLoadCode`；推理→`unit_inference_failed`），不透传中文句子、
  不含本地路径。

## 2. 本地验证（真实命令与结果）

### 2.1 依赖与配置

- 依赖来源：**钉版 deb 闭包**（QGIS 4.2.3，锁 `vendor/deb-closure.lock`，回执
  `.paleo-deb-closure.sha256` 与锁文件哈希一致）。worktree 内 symlink：
  `vendor/prefix → /home/kevin/projects/paleo-h6-agent-model-diagnostics/vendor/prefix`、
  `vendor/onnxruntime`（1.30.0）、`vendor/ranuts-document`（Office 组件，
  configure 报「office-editor 已就绪」无缺件警告）。
  **注**：BUILDING.md 首选的 superbuild 前缀在本机是过期 4.2.2（未随三路
  统一 4.2.3 重建，且缺源码收据），与仓库锁定口径不一致，故改走 deb 闭包
  （CI Linux 必过车道同栈；本机 glibc 2.44 ≥ 闭包地板 2.43）。
- `vendor/Data_service` 按主仓 gitlink `905b5c1b` 初始化（`git -c
  protocol.file.allow=always submodule update --init`）。
- 配置命令（本机 Arch/CachyOS 宿主需显式 multiarch triplet，否则 deb 校验
  找 `lib//qt6/...` 失败；磁盘公告要求 -g1）：
  ```
  ./paleo-dev configure -DQGIS_PREFIX=$PWD/vendor/prefix/usr \
    -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu \
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g1 -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g1 -DNDEBUG"
  ```
- 构建：共享构建锁（`/tmp/paleo-build-locks/README.txt` + 主仓
  `heavy-build.sh` mkdir 锁双持），选择性构建 24 个测试 target，`-j4`，
  构建树约 4.0G（-g1）。
- 本机怪癖（同批任务同款，出处 docs/progress/a-async-previews-20261120.md）：
  系统 ctest 与闭包旧 expat 不兼容，测试以
  `LD_PRELOAD=/usr/lib/libexpat.so.1 ./paleo-dev test -j1` 运行（paleo-dev
  向测试进程注入闭包库路径；裸调 ctest 会因缺运行库失败）。

### 2.2 ORT-on 构建与测试（commit 见 git log）

- 选择性构建 24 个 target（tst_unit_onnx_operation/agent + 22 个受影响回归），
  通过；本任务文件零编译警告。
- 测试（串行 `-j1`）：**24/24 通过**（350.61s）。
  - `tst_unit_onnx_operation`：10 用例全过、0 跳过。
  - `tst_unit_onnx_agent`：14 用例全过、0 跳过。
  - 22 个回归（tst_aichat/projectquery/toolloop/capability/readiness/
    tool_contracts/tool_readiness/h1_contextprojection/h1_requestassembly/
    h4_operation_list_tool/tile_operation/tile_agent/p4_onnx_prediction/
    algorithm_spec/algorithm_contract/capability_gateway/capability_host/
    capability_recovery/capability_recovery_native/capability_round2/
    pichatbridge/aiwiring）全过。
- 无标签源码闸门 ctest 项（59 项）：**58/59 通过**。含 `check_ts_stale`、
  `l10n`、`layering_transitive`、`layering(_strict/_symbol_audit/_qgs_audit)`、
  `qgis_spread(_strict)`、`file_budget(_strict)`、`i18n`、`ui_tokens(_strict)`、
  `qgis_versions`、`docs_drift`、`cpp_mainline(_selftest)` 等全过。
  唯一失败 `first_configure`：**本机工具链环境限制**（其内层全新 configure
  不转发 `CMAKE_LIBRARY_ARCHITECTURE`，非 Debian 多架构主机该变量为空 →
  deb 校验找 `lib//` 失败），与本改动无关；同批 h4 记录同一结论
  （h4-agent-operation-discovery-20261122.md §无标签闸门项 62/63）。
- 显式受控 QGIS 头路径的全闸：
  `QGIS_PREFIX=$PWD/vendor/prefix/usr python3 tools/check_layering.py --transitive`
  → exit 0（四指标均在基线内）；`--symbol-audit` 零基线维持。

### 2.3 ORT-off 构建与测试（独立 build-noort 树）

- 配置：`BUILD_DIR=build-noort ./paleo-dev configure -DPALEO_ONNXRUNTIME=OFF`
  （其余与主树同口径）。`paleo_ai_onnx` target 不存在；`tst_unit_onnx_agent`
  不注册；`tst_unit_onnx_operation`（无 ORT 分支）与 `tst_unit_onnx_noort`
  已登记。
- 构建 4 个 target（tst_unit_onnx_operation/tst_unit_onnx_noort/tst_aichat/
  tst_agent_noort），通过。
- 测试（`BUILD_DIR=build-noort ./paleo-dev test -j1`）：**4/4 通过**。
  - `tst_unit_onnx_operation`：无 ORT 分支断言 `ort_unavailable`（结构化
    不可用，非 QSKIP）。
  - `tst_unit_onnx_noort`：5 用例（委托函数 + 有宿主 readiness 路径：prepare/
    新 compute 在调度前即 `ort_unavailable`，publish/列表不设门；工具表保留
    静态路由）。
  - `tst_aichat` 17、`tst_agent_noort` 4（既有无 ORT 回归）全过。

### 2.4 关键测试证据（生产链）

- `prepareFreezesUnitRmsAgainstRealService`：冻结 64×64 样本与直接调用生产
  `prepareSeismicUnitInput` 的同单元输出**逐字节一致**（含 extent/CRS/
  validCells）；序列化回读含 schemaHash 自校验。
- `computeProducesVerifiedClassRaster`：逐像元 argmax 与「冻结样本 × 夹具
  模型增益」手工对拍一致；缺覆盖格 = nodata 255；GeoTIFF 尺寸/geotransform/
  CRS/nodata/provenance 核验；诊断 JSON 不含来源路径。
- `prepareComputePublishWithIdempotentReplay`：prepare（显式 v1+pinned-value）
  → 同键重放返回原 operation → 同键异图 idempotency_conflict → compute-only
  （computeCount=1、不登记成果）→ publish（committed、computeCount=0、
  catalog+1、presentation=not_requested）→ 同 publish 重放数值不增 →
  结果端口分组（compute：OUTPUT+私有诊断；publish：仅 OUTPUT）→ 换会话
  作用域隔离。
- `freshHostRestoresQueuedComputeFromTheDurableJournal`：compute 受理为
  queued（不启动）→ 全新 CapabilityHost + 生产重绑（运行期钩子全失）→
  `paleo.operation_resume` 恢复 → 与基准计算确定性一致（unitKey/validCells/
  classHistogram/modelSha256），算后移除模型仍可 publish（committed）。
- `interruptionBeforeAcceptanceRegistersNothing`：受理前 interrupt/cancel 后
  持久日志 `not_found`（未受理、未登记 operation）；取消不污染后续调用。
- `modelSwapAfterPrepareIsRejectedAtCompute`：prepare 后换权重 + 重钉 manifest
  → compute `model_changed_since_prepare`、不可 publish；换回原权重可恢复计算。
- `readinessIsPhaseScoped`：解绑推理服务时 prepare/compute 拒，publish/列表
  放行；已受理请求键按 Read 重放不再要求 ORT。
- `invalidUnitsAndFeatureContractsFailBeforeAnyOperation`：aggregate/整个
  Interval 工具表面拒；形状合法但方案不存在的子段 id 物化段
  `unit_selection_invalid`；特征不匹配 `unit_feature_contract_mismatch`；
  缺特征声明 `model_feature_contract_unavailable`；未知输出语义
  `unit_model_output_semantics_unsupported`；未钉哈希 `model_unavailable`；
  失败请求键不留受理记录、错误载荷不含本地路径。

## 3. 独立审查

### 3.1 第一轮（独立评审 Agent，未参与实现）

`g++ -fsyntax-only` 双构建档语法核验 + 宿主契约逐项对照，给出 5×P0 / 1×P1 /
1×P2 / 4×P3：

| 发现 | 处理 |
|---|---|
| P0：unitonnxcontract.cpp toJson 多余括号（两档均不编译） | 修复 |
| P0：`out.unit = toMap(...)` QVariantMap→QJsonObject 类型不符 | `QJsonObject::fromVariantMap` |
| P0：unitonnxoperation.cpp 缺 unitonnxcapabilityspec.h include | 补 |
| P0：`AlgorithmRunResult::diagnostics` 是 QVariantMap，私有诊断通道结构性不可达 | computeUnitOnnxPlan 增加 diagnostics 出参，unitWorker 接线 `OperationWorkResult.diagnostics`；并补 `diagnostics.ok=true` |
| P0：测试 QCOMPARE 宏参数被花括号逗号拆分 | 括号包裹宏参数 |
| P1：freeze/seismic/run 失败透传中文句子进 ExecuteResult.code | 按调用点分类为稳定机器码（已是机器码的原样保留） |
| P2：重启恢复路径零覆盖 | 新增 freshHostRestoresQueuedComputeFromTheDurableJournal（真实恢复三元组） |
| P3：isHex64 只认小写 | 接受大写 A-F |
| P3：fromJson 无总字节预算 | 序列化 >96 KiB 即拒 |
| P3：wiring 每次重绑重注册适配器，翻转宿主全局准备失效标志 | UnitOnnxBinding changed 守卫 |
| P3：publish 夹带参数的测试断言与实际失败层不符 | 改为断言工具表面拒绝 |
| P3：地震版本无资产类型检查 | capture 增加 asset.type ∈ {seismic, seismic_volume} 早拒 |

评审确认干净的面：宿主/适配器集成（publish 无新推理、compute 不换模型）、
信任边界（无绝对路径外泄、actor/approved 上游拒、输出有界）、层/线程、
冻结计划完整性、CMake 依赖方向与双档符号可用性、共享缝对既有工具无回归。

### 3.2 构建/测试驱动的后续修复（发现→处理→验证）

| 发现 | 处理 | 验证 |
|---|---|---|
| compute 必失败：宿主 validateDiagnostics 要求诊断 ok | computeUnitOnnxPlan 置 diagnostics.ok=true | operation/agent 测试全过 |
| 测试 manifest 的 features 放模型顶层（注册表从 input.features 读） | 改为 input 内 + featureContractVersion:1 | 特征用例真实生效 |
| 测试缺 using namespace paleo::strat | 补 | 编译通过 |
| “固定 2×2 形状”用例用动态模型，闸不触发 | 用真正固定尺寸夹具模型 | 用例有效 |
| 被取消的 prepare 复用出参导致后续 compute 拿到空计划 | 测试用独立出参；fromJson 改为局部值解析、通过才交付 | operation 10/10 |
| 显式 binding_mode 与省略被判不同意图（idempotency_conflict） | adapter.intent 规范化为 pinned-value | 重放用例通过 |
| unitonnxwiring.cpp 缺 AiChatToolRunner 完整类型 | 补 include | 编译通过 |
| 测试新增 7 条 tr() 文案未同步 .ts（check_ts_stale 红） | 最小增量补两个 .ts（不跑全量 lupdate——本机版本会丢弃 512 条已有译文并产生 12.5 万行噪音 diff） | ts_stale/l10n/i18n/英文骨架全绿 |
| 无 ORT 档：匿名命名空间跨条件编译未闭合；辅助函数未使用告警 | 结构对称化 + 辅助函数移入 ORT 区 | ORT-off 构建通过、零告警 |

### 3.3 第二轮审查（能力受限，如实记录）

第二轮独立评审 Agent 因额度限制未能启动。按任务书约定，本轮由作者做了
一次单独的 diff 审查（不冒称独立审查）：通读最终实现与测试，重点追查了
binding_mode 省略路径（CapabilityHost::prepare 对 saved.request.bindingMode
的填充与 canonicalIntent 一致）、geotransform 约定（与 predictionworkflow
单元路径一致）、端口分组（DIAGNOSTICS 不占 staging，publish copySource
剔除诊断后 prepareAlgorithmOutputSet 端口数自洽）、恢复三件套与 ORT-off
行为。未发现新的阻断问题。

## 4. 未覆盖/限制

- prepare→compute 的「进程重启恢复」经持久 journal + 适配器恢复三元组 +
  `operation_resume` 实证（单进程内模拟运行期钩子全失）；未做 kill -9 中断
  注入的双进程实证。
- `first_configure` 在本机因 multiarch 怪癖失败（与本改动无关，同批任务同款
  记录）；未改 CMake 验证脚本（不属本任务范围）。
- 动态尺寸模型的 warmup 特定失败按 OnnxModelMeta.warmupError 原样透传，
  未做逐失败分类断言。
- 两模型并发、LRU 淘汰交错下的 pin 语义由 `runPinnedTensor` 既有合同承载
  （每次 compute 复验冻结 SHA/签名/类数）；本任务未新增并发推理压测。
- v1 只开放 logits/probabilities 单特征分类；不支持回归、多特征、任意形状、
  模型下载/训练/参数搜索。
