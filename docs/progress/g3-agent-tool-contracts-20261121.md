# G3：Agent 工具声明、参数准入与上下文资格一致性

日期：2026-11-21。分支 `codex/g3-agent-tool-contracts`，worktree
`../paleo-g3-agent-tool-contracts`。base `origin/master@dd73a322`（#1062 后）。
构建：`QGIS_PREFIX=/home/kevin/projects/paleo-deb-prefix-20261119/usr`
（共享只读 deb 闭包 4.2.3，P6/B 批先例；Qt 6.10.2 同链）。

## Scope ledger（开工登记）

### 生产入口（已核实，源码观察）

| 入口 | 位置 | 现状 |
|---|---|---|
| 工具表/schema/共享校验 | `src/ai/chat/domaintools.{h,cpp}` | `builtinAiToolSpecs()` 14+ 工具；`toJsonSchema()` 含 query_project 条件 allOf；`validateParameters()` 共享宽松校验（整数文本 PW-060 口径，tst_aichat.cpp:421 钉住） |
| 外部分派 | `dispatchAiTool()` | runner `startNext()` 与 controller 卡片各调用一次 |
| 执行器 | `src/workflow/aichattoolrunner.cpp` `startNext()` | 队列/预算/JSON 解析/host 检查/readiness/分派 |
| readiness/枚举 | `aichattoolrunner_readiness.cpp` | `readinessForSpec()` 逐项；`availableTools()` 全表 listing |
| query 分派 | `aichattoolrunner_query.cpp` / `_models.cpp` | well_facies_* / local_models 各自 allowed 清单 |
| 能力请求解码 | `capabilitychatprotocol.cpp` `decodeChatToolRequest()` | `contract_version` 用 `toInt(1)` 静默回退 |
| 服务查询 | `src/services/projectquerycapability.cpp` | HMAC cursor/预算/快照；list 主题不拒未知字段 |
| typed 校验器 | pythonnumerictablequery / pythonseismictracequery / wellfaciesrequest / pythonanalysisselection | 已严格（规范十进制串、精确整值 double、封闭字段） |

### 待验证缺口（源码观察，未运行复现 → 由本批测试转运行证据）

1. **W1/W3 顶层 JSON**：`startNext()` 只查 parseError；数组/字符串/null 顶层
   解析成功后 `.object()` 得空对象，静默走「缺必填」或无参执行
   （`paleo.python_environment` 无必填参数时数组顶层可执行）。
2. **W3 contract_version**：`toInt(1)` 把 `"2"`（串）、2.5、1e300 静默解释成 1；
   网关侧对真整数不支持版本已有精确 `unsupported_version`（capabilitygateway.cpp:976）。
   python 工具侧 `contract_version!=QJsonValue(1)` 已严格——仅能力工具链有此洞。
3. **W2 主题契约分散**：`well_facies_inputs` schema 未声明 well_ref 条件必填
   （runner allowed 清单有）；well_details/version/wells 等目录主题接受其他主题
   字段（如 topic=wells + version_id）后忽略；`query_project` 各主题
   cursor/page_size/max_bytes 允许域只在描述文本里。
4. **W4 schema 漏约束**：seeds 仅 array（运行期有整数线号/样点/上限/组合工作量
   规则）；operation_events page_size(1–64)/max_bytes(1024–32768)、
   operation_read max_bytes(1–24576) 与十进制 offset/after_revision 串协议
   未声明；read_result page_size(1–128)/max_bytes(768–65536) 未声明。
5. **W6 枚举快照**：`availableTools()` 逐项读 `m_context`，注入回调
   （trackingInput/runtimeReadiness）同步重绑 setContext 后清单混旧新；
   setContext 的 changed 判定不含注入函数 null↔非空 变化。
6. **W7 可发现性**：well_facies_models/local_models 等主题依赖 endpoint/ORT/
   TaskService，listing 不区分——不可用主题仍在 enum 里，仅调用时失败。

### 复用（不得重立）

#936–#941/#943 结构化结果、预检、宿主票据、分页/血缘、收据/恢复、效果策略、
分块校验；#1032–#1038 Pi 运行时与工具链；#1051–#1060 前批缓存/ONNX/导入/
保存/异步预览。aiwiring.cpp 已接 tracking/facies provider（旧 TODO 不实）。

### 实施增量（计划）

- 新 `src/services/projectquerytopics.{h,cpp}`（层：数据）：query_project
  主题字段契约单源（required/allowed/预算域），供 domaintools 校验、
  projectquerycapability.run()、runner 侧 well_facies_*/local_models 共用。
- `domaintools.{h,cpp}`：`AiToolSpec::validateExternalArguments()` 严格外部档
  （整数必须是 JSON double 有限整值 int 域；嵌套 seeds 整数）；query_project
  allOf 补 well_facies_inputs 条件；seeds item schema + 边界。
- `operationtools.cpp`：分页字段 min/max；十进制串协议描述。
- `capabilitychatprotocol.cpp`：contract_version 精确解码（缺席→1；非精确整数
  表示→参数错误）；parameters/inputs 形状防御。
- `aichattoolrunner.cpp`（薄接缝）：startNext 顶层须 object（空串=无参保留）；
  分派 Routed 后过严格外部档（业务 provider 前）。
- `aichattoolrunner_readiness.cpp`：快照一致性（枚举绑定世代复核、注入回调
  前后复核）；availableTools 的 spec 副本主题收敛（W7）。
- 测试：`tests/tst_agent_tool_contracts.cpp`、`tests/tst_agent_tool_readiness.cpp`；
  fragment `cmake/extra-g3-agent-tool-contracts.cmake`。

### 验收 oracle

- 修复前红/修复后绿的行为测试（顶层类型、版本表示、主题字段、seeds 形状、
  枚举快照、主题收敛）；提供者/任务/操作调用计数=0 的负向链；真实目录页、
  能力 prepare/幂等 replay、operation 查询的正向链；旧会话/旧 fixture 兼容。

## 工作包记录

### W1 — 契约基线矩阵（源码核对 + 行为测试转化）

对 `builtinAiToolSpecs()` 全部工具逐项追踪 schema → dispatch → runner →
service 后的四级分类（新测试 `tst_agent_tool_contracts` 覆盖标 ★）：

| 分类 | 条目 |
|---|---|
| schema 漏约束（本次补声明） | seeds 元素形状/maxItems★；window_samples/radius 域★；grid_rows/columns 域★；read_result page_size/max_bytes 域★；operation_events/read 预算域★；after_revision/offset 十进制串协议描述★；lineage 深度域★；well_facies_inputs well_ref 条件必填★；local_models 预算条件★ |
| schema 与本地解释不同（本次对齐） | 顶层非 object 静默空参★；integer 文本经 jsonTypeMatches/intArg 宽松档（保留为内部兼容，runner 严格档拒绝）★；contract_version toInt(1) 静默回退（decode 链修复）★；parameters/inputs 非 object 静默空 map（decode 链修复）★；主题字段集分散（契约单源）★ |
| 已有严格保障（复用不动） | typed 主题（numeric_table/seismic 的 canonical 十进制 source_ref/table_id/volume_id、2048–65536 预算）；wellfaciesrequest v2（封闭字段/端点指纹）；python analysis（contract_version==QJsonValue(1) 严格）；operation offset/after_revision decimal() 正则；local_models pageInteger；ChatResultStore page/budget 检查；lineage depth 0–12 严格；网关 unsupported_version/unknown_capability |
| 回调后新风险（本次加复核） | readinessForSpec 对 trackingInput/runtimeReadiness/tileOperations 注入回调无调用后快照复核★；availableTools 枚举途中最混旧新★；setContext 不比对注入函数 null↔非空→世代停滞★ |

历史整数文本调用核查：全仓 grep 无生产调用者以数字文本提交工具实参；
唯一钉住宽松档行为的是 `tests/tst_aichat.cpp:421`（validateParameters 直调
"16" 通过）与 `aichattoolrunner.cpp` intArg 的文档化口径。据此：宽松档保留
在 validateParameters/dispatchAiTool（内部 API 兼容），runner 入口（模型边界）
加严格档 `validateExternalArguments`，两者并存且测试覆盖分叉点。

### 实现改动（源码，2026-11-21/22）

| 文件 | 改动 |
|---|---|
| `src/services/projectquerytopics.{h,cpp}`（新，层：数据） | query_project 非 typed 主题字段/预算契约单源（required/allowed/分页域），`validateQueryTopicArguments()` 机器码 + 字段名 |
| `src/ai/chat/domaintools.h` | `AiToolParamSpec` 增 minimum/maximum（NaN=无）；`AiToolSpec::validateExternalArguments()` 声明（严格外部档，含 field 出参） |
| `src/ai/chat/domaintools.cpp` | 严格整数表示检查；`validateExternalArguments` 实现（主题契约 + integer/number 表示 + 声明域 + seeds 元素整数）；toJsonSchema 出 min/max；query allOf 补 well_facies_inputs/local_models 条件；seeds items/maxItems 注入；tile/horizon/read/lineage 参数补运行期同值域；well_curves 内联清单收敛至契约（宽松档不再重复） |
| `src/ai/chat/operationtools.cpp` | events/read 分页预算声明（1–64/1024–32768；1–24576）；after_revision/offset 十进制串协议描述 |
| `src/ai/horizonsuggest.h` | PW-013 上限常量单源（kMaxSuggestRadius/Seeds/Window、kMaxLineNumberAbs；aichattoolrunner.cpp 匿名命名空间副本删除） |
| `src/workflow/capabilitychatprotocol.cpp` | contract_version 精确解码（缺席→1；非精确整数表示→`contract_version_invalid`；不支持整值不回退，网关 `unsupported_version` 精确拒绝）；parameters/inputs 在场非 object → `invalid_request_shape`+field |
| `src/workflow/aichattoolrunner.cpp` | startNext 顶层须 JSON object（空串=无参保留 hasValidArguments 口径）→ `invalid_arguments_type`；分派 Routed 后、readiness/provider 前插严格外部档（`{code,field}` envelope）；setContext 增 provider 布尔性/指针变化判定（null↔非空、onnx/tile/python 指针） |
| `src/workflow/aichattoolrunner.h` | 私有 `contextMatchesLive()` / `readinessForSpecInContext()` 声明（快照语义） |
| `src/workflow/aichattoolrunner_readiness.cpp` | readiness 求值绑定单份 Context 快照 + 世代；trackingInput/runtimeReadiness/tileOperations 注入回调后复核（不符→`context_rebound`，不投递旧 ready）；availableTools 单快照枚举 + query_project 主题就绪探测收敛进 spec 副本（不改全局注册表；全主题不可用才保留原 enum） |
| `src/workflow/aichattoolrunner_query.cpp` | well_facies_models/inputs 字段集改用共享契约（含 field 出参） |
| `src/workflow/aichattoolrunner_models.cpp` | local_models 字段集改用共享契约（预算运行期检查保留） |
| `src/services/projectquerycapability.cpp` | run() 非 graph 入口接主题契约（typed 主题自动跳过；cursor/HMAC/缓存不动） |
| `cmake/extra-g3-agent-tool-contracts.cmake`（新） | projectquerytopics 编入 paleo_services_query；paleo_ai PUBLIC 链接之；两测试注册（readiness 的 ORT 夹具用例文件内 #if 守卫） |
| `CMakeLists.txt` | manifest 增本 fragment 一行 |
| `tests/tst_agent_tool_contracts.cpp`（新） | W1/W2/W3/W4/W5/W8 行为矩阵 |
| `tests/tst_agent_tool_readiness.cpp`（新） | W6/W7 快照边界与可发现性闭环 |

保持不动的复用面：readiness()/availableTools()/hostContextIdentity() 签名、
结果 envelope、HMAC cursor 与续页、ChatResultStore、operation 身份/恢复协议、
Python/井相/地震专业校验器、G1/G2/G4/G5 所有权文件（llmclient、controller
toolloop/Pi、OperationLog/recovery、UI）。

## 验证（实跑，2026-11-22）

环境：CachyOS x86_64，GCC 15.2.0/Qt 6.10.2（deb 闭包 prefix），
`-DCMAKE_BUILD_TYPE=Release`（首次全树 RelWithDebInfo 因磁盘 100% 中止并
清理，改目标化 Release 构建）；直跑测试经 `run-g3-target-tests.sh` 注入
prefix 库/插件/GDAL/PROJ 路径与树内 TMPDIR（沙箱监狱对策）。
以下保留当时的验证记录。该环境专用脚本已在后续整体 review 中移除；
当前本地回归使用 [BUILDING.md](../../BUILDING.md) 的标准测试入口。

### 新增测试（全绿）

| 命令 | 结果 |
|---|---|
| `./run-g3-target-tests.sh tst_agent_tool_contracts` | PASS（14 用例：schema 域/顶层 object/严格档 vs 宽松档/decode 版本链/runner 版本链/主题矩阵/快照不动/seeds/read_result/operation 预算/真实目录分页/prepare 幂等重放/operation_get） |
| `./run-g3-target-tests.sh tst_agent_tool_readiness` | PASS（8 用例：回调内重绑→context_rebound/单快照枚举/provider 撤除→世代推进→排队 stale/主题收敛不改全局/换工程拒绝旧参数/listing 保留必填工具） |

### 修复前红证据（stash src/ 重建后，旧代码上断言旧行为的探针全过）

| 行为差异（旧代码实测） | 修复后 |
|---|---|
| 顶层 `[1,2]` 调 python_environment：**工具实际执行**（返回环境报告 payload） | `invalid_arguments_type` 参数错误，零 provider 调用 |
| `contract_version:"2"`（文本）经 constraint prepare：**toInt(1) 静默变 1，prepare 成功**（ok=true，operationId 已分配） | 严格档 `invalid_integer_representation`（runner 链）/`contract_version_invalid`（decode 链），无 operation |
| `topic=wells + version_id`：接受后忽略，查询 ok=true | `unsupported_topic_argument`+field（分派/服务两处） |
| 无端点时 well_facies_models 仍在枚举 enum | spec 副本收敛，全局注册表不动 |
| 新 API 层面：`validateExternalArguments` 在旧头文件编译失败（修复前不存在） | —— |
| `tst_pythonanalysisquery` well_ref 缺失用例：旧 `invalid_well_ref` | `missing_required_field`（已按契约更新该行断言） |

### 受影响回归（全绿）

`./run-g3-target-tests.sh tst_aichat tst_aichatprojectquery tst_aichatcapability
tst_chatresults tst_aichattoolloop tst_agent_readiness tst_project_query
tst_modelcontractquery tst_capability_gateway tst_pythonanalysisselection
tst_pythonanalysisquery tst_pythonanalysiswiring tst_pythongeneratedoperations
tst_pythonenvironment tst_capability_host` → **15/15 PASS**。

### 源码闸门（直跑脚本，等价 CI Source gates 子集）

layering `--strict` ✓｜file_budget `--strict`（新增 0/回升 0）✓｜
layering `--qgs-audit`（新增 0）✓｜qgis_spread（169=基线）✓｜ui_invariants ✓｜
ui_size（7=基线）✓｜ui_tokens ✓｜i18n ✓｜docs_drift ✓｜qgis_versions ✓｜
`--transitive`（exit 0，黄牌口径不变）✓｜cpp_mainline（cmake -P）✓｜
check_tidy（全部改动 TU，含 capabilitychatprotocol 参数名对齐修复）✓｜
ci_triage_reds --validate ✓。

### 已知边界与未覆盖项

- **无 ORT 构建组合未本地实跑**：`tst_agent_tool_readiness` 的 ORT 夹具用例
  以 `#if PALEO_HAVE_ORT` 守卫，其余用例可在无 ORT 构建注册运行；未实测
  `-DPALEO_ONNXRUNTIME=OFF` 配置（磁盘/时间约束，测试文件按
  tst_agent_readiness 先例组织）。
- **Windows 平台未实测**（本批 Linux only；无平台特定代码路径）。
- **ctest 直跑在本机不可用**：系统 ctest 与 deb 闭包 expat 符号冲突、测试
  二进制需 prefix 库路径——直跑脚本等价注入（CI 车道不受影响）。
- std::function 同布尔值替换（换 provider 函数对象但 null 性不变）仍无法
  值比较（C++ 限制）；由注入回调后的快照复核与「枚举单快照」兜底，头文件
  注释已声明该边界。
- `invalid_request_shape`/`context_rebound`/`invalid_integer_representation`/
  `argument_out_of_range`/`missing_required_field`/`unsupported_seed_field` 为
  本批新增机器码；旧行为兼容点：坏 JSON 与宽松分派拒绝路径的 envelope 保持
  不变（{"tool","error"}），`tst_pythonanalysisquery` 一行断言按更精确的
  missing_required_field 更新。

## 独立复审（2026-11-22）

未参与实现的独立 reviewer 对 `git diff origin/master..HEAD` 逐维度核查
（严格档位置/合法调用不误伤、主题契约与各执行路径消费对拍、decode 两链、
readiness 门序与旧版逐行对照、setContext 世代、测试 oracle 独立性、分层/
预算闸门实跑、兄弟所有权），结论：**无阻断/无 major，可交付**。三个 minor
已随复审修复并复测：

1. 预算字段归因（page_size 有效而 max_bytes 越界时不再误指 page_size，
   projectquerytopics.cpp）；
2. local_models 拒绝时 field 提示不再丢弃（aichattoolrunner_models.cpp）；
3. seeds 元素 `additionalProperties:false` 由严格档真实执行
   （`unsupported_seed_field` + seeds.N.field），schema 不再「声明而未执行」。

复审确认的已知非缺陷项：空串实参=无参口径（与 hasValidArguments 对齐）、
全主题不可用时保留原 enum（有界策略）、controller 卡片仅用宽松分派（G2
所有权，严格失败仍以工具结果回流）。
