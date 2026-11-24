# 并行方向挂点：codex/paleo-i1-agent-geostat-atoms-20261123（I1 地统计计算原子）
# 普通克里金与 SGS 的 Agent 面开放：v2 精确算法契约 + capability 行 + 严格
# 解码/受管样本物化 + CapabilityExecutionAdapter（capture/restore）+ 聊天
# 工具与产品装配。约定见 CMakeLists.txt「并行开发挂点」：新增源/测试一律
# 在本文件登记，不改根模块源列表。
#
# 目标归属：
#   · services/geostaticapabilityspec — v2 算法描述 + capability 行工厂；
#   · qgis/geostatsamplereader — 受管点样本的有界读取口（QgsVectorLayer
#     封装在 QGIS 层，workflow 不散布 qgs* 头）；
#   · workflow/geostatatom{contract,inputs,operation} — 严格解码/快照物化/
#     执行适配器（复用 Host/Recovery/TaskService 与 NativeCppAdapter 核）；
#   · ai/chat/geostatictools — paleo.geostat_kriging / paleo.geostat_sgs 工具
#     描述与嵌套 schema；
#   · app/geostaticwiring — 产品装配（aiwiring 单点调用）。

target_sources(paleo_services PRIVATE src/services/geostaticapabilityspec.cpp)
target_sources(paleo_qgis PRIVATE src/qgis/geostatsamplereader.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/geostatatomcontract.cpp
  src/workflow/geostatatominputs.cpp
  src/workflow/geostatatomoperation.cpp)
target_sources(paleo_ai PRIVATE src/ai/chat/geostatictools.cpp)
target_sources(paleo_app PRIVATE src/app/geostaticwiring.cpp)

# W2：严格解码/资源预算/冻结计划往返/注册表一致性（纯值，无 QGIS 运行时）。
add_paleo_test(tst_geostat_atom_contract LIBS paleo_workflow)

# W3-W8：生产链端到端——真实 tool decode → Host adapter（capture）→
# TaskService → NativeCppAdapter 数值核 → OperationRecovery/receipt/发布。
# 与 tst_capability_recovery_native 同口径的模块面。
add_paleo_test(tst_geostat_atom_operation LIBS paleo_workflow paleo_services paleo_store paleo_algorithms)
