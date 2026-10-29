# 项目文档

当前架构以 [PALEO_QGIS_PLAN.md](PALEO_QGIS_PLAN.md) 为准，未完成事项集中在
[TODOS.md](../TODOS.md)。`progress/`、`phase0/`、`evidence/` 与
[ledger-archive/](ledger-archive/README.md) 保存对应日期和提交的交付证据，不能用旧测试数或当时的缺口推断当前状态。

## 开发与维护

- [构建、依赖与 worktree](../BUILDING.md)
- [仓库分层与质量约束](../AGENTS.md)
- [C++ 主线与可选 Python 边界](CPP_PRIMARY_BOUNDARY.md)
- [UI 分层契约](UI_LAYER_PLAN.md)
- [设计系统](../DESIGN.md)
- [平台与依赖来源](PLATFORM_MATRIX.md)

## 后续实施规划

- [实施计划与验收条件](planning/task_plan.md)
- [现状依据与待核实事项](planning/findings.md)
- [规划与实施进度](planning/progress.md)

需求是否关闭以 TODOS 为准；planning files 记录批次、证据和实际执行结果。

## 工程与数据

- [工程文件、新建向导与坐标设置](PROJECT_FILE_DESIGN.md)
- [工区参数配置](AREA_PARAMETERS.md)
- [测井资料导入](import-well-logs.md)
- [测井相预测](WELL_FACIES_PREDICTION.md)
- [井矢量属性](WELL_VECTOR_ATTRIBUTES.md)
- [数据操作的未完成接缝](dataops/GAPS.md)
- [数据关系图](dataops/DATA_GRAPH.md)
- [原生地层对比](STRATIGRAPHY.md)

## 编图与解释

- [单因素工作流](workflows/SINGLE_FACTOR.md)
- [编图工作台](workflows/MAPPING_WORKBENCH.md)
- [沉积相编图](FACIES_MAPPING.md)
- [单井综合柱状图](wellcomposite/USER_GUIDE.md)
- [地震与剖面架构](seismic/ARCHITECTURE.md)
- [版本与发布状态](VERSION_PUBLISH_STATE_MACHINE.md)
- [Schema 迁移](SCHEMA_MIGRATION.md)
- [Data_service 模型接入与迁移](DATA_SERVICE_INTEGRATION.md)

## 历史记录

[历史记录索引](ledger-archive/README.md) 收纳旧任务账本、旧审计报告和清理前的待办快照。
历史任务书保留在 [goal-loop-prompts/](../goal-loop-prompts/README.md)，供追溯原始需求；
其中的机器路径、预算和“在飞”状态不作为当前执行指令。
