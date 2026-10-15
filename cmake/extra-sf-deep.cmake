# goal/singlefactor-deep — 单因素纵深（P1：约束语义统一 + QGIS 绘编一致 + CRS 契约）。
# 源文件与测试集中在此（根 CMakeLists 的模块源列表不改，并行分支零冲突）。
#
# P1-A：domain/constraintsemantics —— 约束语义解析 + 方法能力判定的唯一事实源。
#   算法侧 constraintparse.cpp、工作流约束使用计划、UI 计数同调此处；
#   引擎回执（QC constraint_receipts）独立产出，测试断言计划 == 回执。

target_sources(paleo_domain PRIVATE
  src/domain/constraintsemantics.cpp)

# H1：语义/能力判定用例（纯 domain，无 QGIS）。
