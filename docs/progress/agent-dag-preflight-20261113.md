# Agent DAG 预检（W2）

基线：`dd7baa1f`；对应 [Agent 调用可靠性整改计划](../planning/AGENT_CALL_READINESS_PLAN.md) W2。
状态：代码和专项预检回归、源码门禁已完成；完整生产依赖链编译/集成回归进行中。
未以本记录替代后续真实 QGIS/GDAL 回归。

`validatePlan` 在任何子操作前返回规范节点、类型化边、实际请求字段映射、结构化拓扑顺序
和 issues。执行入口自行重新校验，不把可变 `ValidatedPlan.ok` 当成宿主许可。
`checkPlan` 保留错误查询接口，message 不再承载逗号拼接的拓扑结果；执行结果的
`plan_order` 改为字符串数组。节点身份和结果以独立索引保存，支持逗号、竖线及中文 ID。

预检覆盖：准确能力/算法版本及曝光、所选输出和边两端端口、环与重复节点/边、同槽多次
赋值、字面输入与边冲突、必填输入、参数和端口基数，以及目标明确要求的种类、几何、
单位、轴和基准。输入描述先限制嵌套/展开，再核对 compact JSON 的 UTF-8 大小：最多
32 节点、64 边、256 KiB、深度 16、数组 4096 项及总值 16384 个。

本批仅开放已有真实单文件桥：IDW `OUTPUT` → contour `RASTER`。
IDW `INPUT/CONSTRAINTS/LOCAL_GRID_WKT` 对应 `points/lines/grid_wkt`，contour `RASTER`
对应 `raster_path`。移除 contour 登记但从未消费的 `LINES` 输入，输出 `LINES` 保留。
文件路径不能作为 IDW 内联点/约束行表，多输出、诊断值、批次数值/整道、多个终端结果和
未实现的 adapter 明确拒绝；组合首批只接受 compute-only。单能力 `execute` 的既有
时深、地震、发布等契约不变，后续开放组合形状须同时实现对应桥和验收。

已验证：

- 从 `dd7baa1f` 原函数逐字取出的反例：非空未知源端口返回 `ok`，期望 `port_type`，
  本地反例程序退出 1；新生产预检正确拒绝。临时验证壳未纳入产品构建。
- GCC 16.2.1 / Qt 6.11.2 原生构建成功；`tst_capability_plan` 46 项通过，零失败/跳过。
  覆盖未知端口、精确版本、图冲突/循环、语义缺失/冲突、预算、必填与后续节点参数、
  特殊字符身份、真实请求映射和唯一默认值来源。
- 修改后的 `capabilitygateway.cpp` 完整生产 TU 独立编译通过。
- 完整生产 CMake configure/generate 通过，使用独立 QGIS 4.2.3 / GDAL 3.12.2 闭包；
  `office-editor` 接线就绪。三个真实 target 的编译与回归仍在进行。
- 当前 CI Source gates 的 28 条命令，以及 qgis_spread 自检、ci_triage validate/自检、
  atomic_kernels 共 32/32 通过；传递门使用锁版 QGIS 4.2.3 头，所有基线未增长。
  `git diff --check` 通过。

生产集成验收已加入既有 `tst_capability_round2`：真实 IDW 输出交给 contour 并生成
可读 GPKG；无效的下游节点/未知源端口拒绝时，目录中零子操作记录和零输出。
待完整构建完成后记录该回归与 `tst_capability_gateway` 的实际运行结果，再提交 PR。
