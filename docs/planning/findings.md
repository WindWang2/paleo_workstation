# 现状依据与待核实事项

更新时间：2026-10-27。基线：`75bf08cb`。
本文区分源码中可见的事实、历史验证记录和规划判断；新增发现附可追溯来源。
实施顺序见 [task_plan.md](task_plan.md)，实际运行结果见 [progress.md](progress.md)。

## 本轮检查到的事实

| ID | 事实 | 依据 | 对计划的影响 |
|---|---|---|---|
| F01 | C++ 主线、QGIS GIS 底座和分层规则已确定 | [架构](../PALEO_QGIS_PLAN.md)、[分层契约](../../AGENTS.md)、[C++ 边界](../CPP_PRIMARY_BOUNDARY.md) | 规划沿用现行模块，功能编排不落视图 |
| F02 | 新建工程使用有界身份探测，井表预览有行数限制 | [sourceprobe.cpp](../../src/io/sourceprobe.cpp)、[welltable.cpp](../../src/io/welltable.cpp)、[扫描交付说明](../progress/bounded-source-scan-20261027.md) | T08 验证预算与完整导入的分工，不恢复整文件扫描 |
| F03 | 资料横纵轴按语义识别；EPSG:2436 的明显异常被拒绝，不会自动去前缀或换轴 | [welltable.cpp](../../src/io/welltable.cpp)、[welltableprojection.cpp](../../src/io/welltableprojection.cpp) | T08 分开验收正常 CRS 声明与异常编码处理 |
| F04 | `QgisRuntime` 显式禁用 CRS 实例缓存 | [qgisruntime.cpp](../../src/qgis/qgisruntime.cpp)、[生命周期记录](../progress/native-crs-lifetime.md) | T05 先证明工作线程退出后的投影生命周期安全，再评估恢复缓存 |
| F05 | 影响分析和版本对比入口已存在；后台化仍在当前待办 | [mappingworkbench_impact.cpp](../../src/workflow/mappingworkbench_impact.cpp)、[TODOS](../../TODOS.md) | T03 要审查调用链和执行线程，不能只给同步入口加进度文案 |
| F06 | 结构因素写入 `structural_sha256`；等值线 prepare 读取结构侧卡路径，未读取该键 | [constraintfactorjobs_factor.cpp](../../src/workflow/constraintfactorjobs_factor.cpp)、[constraintfactorjobs_contours.cpp](../../src/workflow/constraintfactorjobs_contours.cpp) | T04 补侧卡字节完整性复验；已有栅格 SHA 校验不能替代此项 |
| F07 | `DataCatalog::updateEntity`、`updateLink` 已存在；`EntityEditCmd` 仍写 override 存储 | [datacatalog.h](../../src/catalog/datacatalog.h)、[dataopscommands.h](../../src/ui/pages/dataops/dataopscommands.h) | T06 解决消费、撤销和跨页面一致性，不重复增加同义 API |
| F08 | `bindChatToolRunner` 尚未绑定 traceFetch / faciesInput | [aiwiring.cpp](../../src/app/aiwiring.cpp) | T10 需要真实数据提供者与工程生命周期约束 |
| F09 | 3D 层位传播、显式倾角引导、属性体与 DLIS/LIS 已有实现和交付记录 | [seismictaskservice.h](../../src/services/seismictaskservice.h)、[horizontrack.h](../../src/algorithms/horizontrack.h)、[多格式交付](../progress/welllog-multiformat.md) | 不再作为“尚未实现”的新任务；只规划剩余边界与消费扩展 |
| F10 | catalog 运行时使用 SQLite；清单默认字段仍可能指向旧 JSON，成员检查兼容两者 | [工程文件说明](../PROJECT_FILE_DESIGN.md)、[paleoprojectfile.cpp](../../src/metadata/paleoprojectfile.cpp) | 存储验收同时覆盖旧清单与当前数据库，不根据文件名判定数据迁移失败 |

## 历史证据与本轮尚未验证的事项

| ID | 线索 | 现有证据 | 实施前要补的证据 |
|---|---|---|---|
| H01 | 编图套件四个异步/布局相关用例曾失败 | [2026-10-24 测井相交付记录](../progress/well-facies-data-support-20261024.md) | 当前提交、当前依赖链下的完整结果及最小可复现路径 |
| H02 | 新 worktree 首次 configure 曾漏依赖 include | [TODOS 的 #484 后续](../../TODOS.md)、[构建入口](../../BUILDING.md) | 全新 build 目录与依赖存在/缺失两种配置结果 |
| H03 | 缓存、视觉和单因素性能存在旧红项 | [清理前快照](../ledger-archive/backlog-before-cleanup-20261027.md) | 同机首轮结果、复跑、测量范围及当前豁免规则；不能继承旧耗时结论 |
| H04 | 多项真实 GUI、符号与专业计算仍需人工或专家验收 | [TODOS](../../TODOS.md) | 可复核的工区样例、预期结果和实际操作记录 |

本轮未执行产品构建、功能测试或真实工区操作。以上 H* 是待复核线索，
不构成当前提交的失败结论；F* 的源码检查也不等同于运行验证。

## 数据与业务条件

- 新旧资料模式必须共存；已声明的单位、坐标系、主关联与来源信息继续有效。
- 资料 CRS、工程计算坐标系与地图显示 CRS 是不同设置，验收需分别记录。
- MD/TVD、米/英尺、有效厚度/砂厚、真实置信度/显示提示必须有明确含义，缺输入不能用零代替。
- 净毛比、孔渗层段统计、复杂断层拓扑和解释合并需要业务规则与核对样例。
- BE 格式需要规范或样件；新模型维度、新 SEG-Y 道头方言、网络盘行为需要实际输入证据。
- 私有数据、机器路径、密钥及原始工区记录不进入提交；交付记录引用合成夹具或本机证据说明。

## 规划决策

| 决策 | 理由 | 调整条件 |
|---|---|---|
| 先复核基线，再安排修复 | 历史失败和当前故障需要区分，避免重复修复或误改断言 | 取得当前可复现结果后更新 T02 子项 |
| 优先后台化与来源完整性 | 分别对应交互阻塞与派生结果正确性，影响主链 | 新发现的数据损坏或崩溃问题优先处理 |
| 采用 prepare/worker/publish 和冻结输入 | 复用现有架构，覆盖取消、切工程和迟到结果 | 有明确性能证据后再调整快照粒度 |
| 专业扩展按可独立验收的子项交付 | 地质规则、样本与服务接口的准备程度不同 | 输入契约就绪时从条件任务转入执行批次 |
| 保留规划、依据和进度三个文件 | 执行顺序、事实和运行结果各有一个维护位置 | 不复制新的待办总表；需求关闭仍同步 TODOS |
