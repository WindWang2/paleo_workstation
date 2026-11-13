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
W1 新增的 `paleo.read_result` 是宿主只读缓存入口，必须在实际注册/测试后登记其就绪性。

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
