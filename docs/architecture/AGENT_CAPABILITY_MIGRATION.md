# Agent 入口迁移清单

基线：2026-11-13，master `dd7baa1f`。整改依据为
[调用可靠性计划](../planning/AGENT_CALL_READINESS_PLAN.md)。
此清单区分静态登记、生产绑定、允许的效果与运行时就绪性；路由存在不等于可运行。
表中“目标”是验收要求，不表示已经接线。每批实现后更新本页及对应 progress 证据。

## 聊天入口基线

| 工具 | 生产路由与输入 | 实际效果 | 基线缺项 / 目标批次 |
|---|---|---|---|
| `paleo.query_project` | runner 的 summary/wells/horizons/assets/well_details；version 转领域查询 | 领域只读；模型井有效投影可能参与 | W1 小结果；W3 可信文档；W4 有界索引页及投影水位 |
| `paleo.asset_lineage` | runner → DerivationGraph；catalog 版本图 | 领域只读 | W1 小结果；W4 增量方向化遍历，不能先构全图 |
| `paleo.depth_convert_batch` | runner → CapabilityTaskRunner → gateway → VelocityModel | 控制日志/私有计算暂存；按意图计算 | W3 共用宿主；W5 操作恢复；批量点不能冒充栅格策略 |
| `paleo.seismic_attribute_group` | 同一 capability 任务链；自有道缓冲或受控地震输入 | 控制日志/私有计算暂存；按意图计算 | W3 冻结输入/绑定、共享核验；W5 恢复 |
| `paleo.constraint_idw` | 同一 capability 任务链；冻结点/约束/参数 → IDW | 暂存文件；publish 意图登记成果 | W2 精确桥；W3 可信效果；W5 凭证/目标 CAS |
| `paleo.tile_classification` | runner → AiAssistWorkflow；gridFetch + ORT/模型 | 登记相码、低置信掩膜、置信度成果并声明图层 | W6 拆 prepare/compute/publish；发布依赖 W5；不得标只读 |
| `paleo.horizon_tracking_suggestion` | runner → 建议引擎；要求 traceFetch | 生成候选建议；接受建议是另一个写意图 | 生产 traceFetch 未绑；W6 真体引用/线号/采样语义接线前 unavailable |
| `paleo.well_facies_prediction` | runner → WellFaciesService；要求 faciesInput/精确模型/端点 | 取曲线、向已配置服务传输并预测 | 生产 provider/模型回调未绑；W6 先冻结模型再取数，接线前 unavailable |

原生 `catalog.version.describe.v1` 是 query_project 的 version 主题，不另计第九个工具。
W1 新增 `paleo.read_result`，已实际注册并经生产 runner/控制器回归验证。
它读取会话级不可变 JSON 报告，不读任意文件路径、不写工程；支持子树、页及 Unicode
字符串片段，作用域/TTL 失效明确报错。既有 8 工具加此入口共 9 项；新增数量不代表
其他能力已满足就绪性。[W1 编译/测试与门禁证据](../progress/agent-results-20261114.md)。

## Capability 与命令接缝

CapabilityRegistry 的六项是：depth.convert_batch、seismic.attributes.compute、constraint.idw、
singlefactor.contours（静态 Callable），facies.draft.publish（Internal），
wellfacies.predict（Unavailable）。静态 Callable 不能替代当前 provider/文档/权限检查。
contours 没有独立聊天别名；只有真实 adapter 支持的输出形状才可进入组合图。
AlgorithmSpec 仍唯一拥有默认值/端口；CapabilitySpec 管效果和曝光；CommandSpec 管意图及入口。
94 条命令不是 94 个 Agent 工具；依赖 widget 的交互命令不在首批自由调用范围。

| 接缝 | 首批验收 |
|---|---|
| 聊天/Dispatcher/DocumentSession | app 装配同一可信宿主核验；持久 documentId 不取工程路径；epoch 独立 |
| prepare/execute/commit | 不可变宿主凭据；重新检查状态、相关输入/绑定与目标 CAS；禁止模型自授效果 |
| tool 结果 | runner 保留原领域 schema；模型 envelope 单独校验；每个 accepted call 必有完整 JSON |
| 查询 | 真实 after-key / 方向化 BFS；修订失效明确报错；有效模型井不被分页静默丢弃 |
| 发布/恢复 | Catalog 同事务凭证是提交事实；控制日志可修复；呈现不得覆盖后续操作 |

## 每项能力关闭缺口的条件

每批记录：入口/契约版本、真实生产 provider、冻结输入、类型化效果、取消与恢复语义、
结果契约、运行时缺项码、测试名、编译/测试/门禁命令及结果。只有生产装配和对应验收
均通过才提高 exposure/readiness；不以工具数量或一次桩测试宣布迁移完成。
执行证据见 [实施记录](../progress/agent-readiness-20261113.md)。

## 本轮交付后的入口契约

上面的表保留 master 基线。当前入口按 W0–W6 实现分别约束，不能把静态登记数量当作
某个工程此刻的可调用数量；生产 `availableTools` 按可信宿主状态过滤，旧会话调用仍返回缺项。

| 入口 | 当前实现与边界 |
|---|---|
| query_project / asset_lineage | W4 after-key 索引页、准确父引用页、方向独立 BFS；游标绑定文档/聊天/epoch/修订/筛选/预算，变化明确失效。模型井保持有效投影与 SDK 水位。 |
| read_result | W1 不可变会话报告引用；有界子树/成员/数组/Unicode 片段，合法 envelope 与整轮预算。临时引用不冒充持久操作结果。 |
| depth / seismic / constraint | W3 实际 AppContext/Dispatcher/聊天共享 Host；W5 持久幂等、Catalog 凭证与受控恢复。确切 phase 由工具 schema 与 adapter 共同约束。 |
| tile_classification（旧） | legacy adapter，生产聊天不暴露，不允许借旧入口绕过效果/冻结校验。 |
| tile_classification_v2 | contract 2：prepare 冻结来源/模型/语义，compute-only 私有产物，publish 三必需端口一组提交。原生图层状态 CAS、呈现 pending/superseded 与提交事实独立。 |
| horizon_tracking_suggestion | app 绑定 SDK 精确体/线号/窗/时间基准 provider。缺实际上下文、模型、ORT 或 provider 在受理前说明；生成建议不等于接受解释。 |
| well_facies_prediction | typed v2 外部适配：先模型契约后精确井输入、独立分段操作、远端 job 恢复。Native DAG/execute 拒绝该外部 adapter，不能假装为本地数值核。 |
| operation_get/events/results/read | W5 有界持久查询；真实字节读取校验受管引用。已完成回执的只读回放不依赖新建计算的权限或在线 provider。 |
| operation_resume/cancel | 可信 scope、物理写 claim、状态 CAS 和 attempt fence；先核对提交事实。已提交后取消保留成果，未知远端提交不重发。 |
| python_environment | 宿主固定清单核验、策略与真实 trusted execution 就绪快照；只读诊断，不接收安装或路径覆盖。 |
| well_curve_quality / attribute_correlation | PY2 固定可信模板；精确 well_curves 引用、已声明 MD 轴和有界窗口，经同一 Host prepare/compute/publish。仅所选井数值窗口，不表示地震体优选/融合已接入 Python。 |

`query_project` 的新增主题：

- `local_models`：已安装 registry 声明的有界页，**仅元数据**。`manifest_status_ok` 不是可运行证明；
  返回 `metadataOnly=true`、`requires_prepare=true`、`runtimeVerified=false`。未知语义/缺特征
  不猜测，实际 prepare 再核验支持的模型契约和权重。无 ORT 构建明确 `ort_unavailable`。
- `well_facies_models`：需要 `Read | ExternalTransfer` 的端点模型 GET，只证明
  `model_contract_only`，不读取井曲线；缺 feature-unit 等字段时报告 readiness 原因。
- `well_facies_inputs`：SDK 有界井输入元数据，不向远端发送曲线值。无意义的分页字段明确拒绝。
- `well_curves`：精确 WellRef 下的曲线元数据 keyset 页；源/曲线文本与水位受限，不取数值数组。
  只有语义明确、来源可核验的条目给出 input_ref；拒绝把 inferred 轴冒充 declared。

W5 首批仅承诺单 Catalog 同事务凭证；跨 store 全局提交、94 条命令全面迁移和不具备完整
契约的远端服务仍不属于本轮承诺。当前支持的栅格/模型/单位边界、编译测试及源闸门证据见
[W6 记录](../progress/agent-providers-20261114.md)和 [well 记录](../progress/agent-well-facies-20261114.md)。
