# TODOS — paleo_workstation

## P2 — 多 realization / 不确定性支持（deferred from CEO review D6, 2026-09-25)

- **What:** 每层位存 N 个预测 realization，派生置信度面，同一 canvas 切换 realization。
- **Why:** 相对商业软件的研究级差异化能力；井点稀疏区的不确定性可视化。
- **Pros:** 真正的不确定性量化；预测-验证闭环更强。
- **Cons:** 触及数据模型、存储、预测管线、版本、UI — 约使预测子系统翻倍。
- **Context:** 当前文档 §7 只有单个"预测置信度"图层。schema 已预留可空 `realization_id`（见 PALEO_QGIS_PLAN.md NOT-in-scope 决议）。做时先定 realization 与 version 的正交关系。
- **Effort:** human: XL / CC: L
- **Priority:** P2
- **Depends on:** 智能预测管线落地后

## P2 — 相界地质语义类型（deferred from CEO review, 2026-09-25）

- **What:** 相界线不只是 polygon 边，区分整合接触 / 尖灭 / 相变 / 断层切割等类型，影响拓扑编辑与图面表达。
- **Why:** 真实古地理图的边界有地质含义；不同边界类型的编辑行为和符号不同。
- **Pros:** 编图专业正确性；验证模块可按类型核查。
- **Cons:** 数据模型与编辑工具复杂度上升；需要地质专家参与定义。
- **Context:** 文档 §14–15 目前把相界当普通 polygon 拓扑处理。先做单一"相界线"类型跑通，再扩类型。
- **Effort:** human: L / CC: M
- **Priority:** P2
- **Depends on:** P0 矢量编辑落地

## P1 — 工程评审待办（from outside-review findings, 2026-09-25）

以下项已由 CEO 评审确认纳入计划，但**细节规格属工程评审范畴**：

- **保存/发布语义状态机**：`保存版本` / `发布`（result/ 快照）/ `Published` 状态三者关系；发布后能否再编辑；工作副本与已发布快照的发散规则。（§41.7）
- **native:* 算法逐项审计**：§10–12 所列每个算法在 native provider 下是否有 C++ 实现；缺的（raster contour、距井距离/hub 类——注意 `qgis:*` provider 也是 Python）逐一指定 GDAL C++ 替代或直接自研。约 6–10 个 GDAL 封装算法的工作量待评估。
- **算法测试框架自建**：QGIS 官方算法测试基座（AlgorithmsTestBase）是 Python/YAML，C++-only 下不可用；需自写 C++ 测试 harness（§33 修正）。
- **schema 迁移策略**：metadata/project.sqlite + project.gpkg + .qgz 三类文件的版本化与迁移方案——产品核心价值是长生命周期版本数据，此项不可缺。
- **崩溃报告机制**：本地转储 vs 回传，实现期决定（§38）。
- **速度模型 + line-geometry↔CDP 映射**：depth↔TWT 转换与 map↔地震剖面定位的两个新建依赖（§40 SeismicMapLink）。
- **性能测试**：§33 目前无性能用例——画布刷新/工程打开预算（§41.6 NFR）需要可执行测试。
- **渲染对比测试稳定性**：需 vendor 字体 + 钉 Qt 光栅化版本，否则跨平台噪点失败。
- **合成最小 SEG-Y fixture**：golden fixture 工区的地震数据用合成文件，真地震数据太重。
- **多 CRS 假设核查**：§17 MapContext 假设单一 CRS；真实项目可能混合地震工区 CRS 与地图基准，需工程评审确认。

## P1 — Phase 0 spike 交付物（§39, E3）

- vendor superbuild 骨架 + 依赖清单（含裁剪开关 WITH_3D/WITH_MESH/WITH_PDAL 决定 + Qt LGPL 动态链接条目）。
- 目标平台矩阵（OS × 架构）钉定。
- AI 推理运行时选型输出（ONNX Runtime / libtorch / 其他）。
- app-only 功能审计清单（qgis_gui vs src/app 逐能力标注）。
