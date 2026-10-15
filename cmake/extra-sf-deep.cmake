# goal/singlefactor-deep — 单因素纵深（P1：约束语义统一 + QGIS 绘编一致 + CRS 契约）。
# 源文件与测试集中在此（根 CMakeLists 的模块源列表不改，并行分支零冲突）。
#
# P1-A：domain/constraintsemantics —— 约束语义解析 + 方法能力判定的唯一事实源。
#   算法侧 constraintparse.cpp、工作流约束使用计划、UI 计数同调此处；
#   引擎回执（QC constraint_receipts）独立产出，测试断言计划 == 回执。

target_sources(paleo_domain PRIVATE
  src/domain/constraintsemantics.cpp)

# H1：语义/能力判定用例（纯 domain，无 QGIS）。
add_paleo_test(tst_singlefactor_constraintuse LIBS paleo_domain)

# 约束使用计划（UI/工作流口径；QgsGeometry 拆部件后调 domain 判定）。
target_sources(paleo_workflow PRIVATE
  src/workflow/constraintuseplan.cpp)

# H1（引擎侧）：真实 Processing 引擎回执 == 统一计划。
add_paleo_test(tst_singlefactor_constraintreceipts LIBS paleo_workflow paleo_algorithms paleo_ui_deps)

# P1-C：约束库 CRS 契约（画布 → 本地工程网格显式变换；无 SRS 库层补 CRS）。
target_sources(paleo_qgis PRIVATE
  src/qgis/constraintcrs.cpp)
add_paleo_test(tst_singlefactor_constraintcrs LIBS paleo_ui)

# P2：网格化引擎（距离变换 / 最小曲率）按统一语义读完全阻断打断线并出回执。
target_sources(paleo_algorithms PRIVATE
  src/algorithms/constraintbarriers.cpp)
