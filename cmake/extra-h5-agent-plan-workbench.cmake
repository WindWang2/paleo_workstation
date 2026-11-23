# 并行方向挂点：H5 agent-plan-workbench（2026-11-22 批次）
# Agent v1 计算计划的提案、原生审阅与显式执行闭环：纯预检提案工具
# （paleo.workflow_plan_preview，decode+validatePlan 零执行）、计划展示/
# 执行 adapter（aiplanpresentation，经既有 CapabilityHost::preparePlan →
# start → CapabilityRecovery）、原生审阅面板与主窗装配。
# 约定见 CMakeLists.txt「并行开发挂点」：新增源文件/测试一律在这里登记，
# 不直接改根 CMakeLists 的模块源列表（并行分支在此汇合零冲突）。
target_sources(paleo_ai PRIVATE
  src/ai/chat/planpreviewtool.cpp           # 功能层：工具声明/schema/契约常量
)
target_sources(paleo_workflow PRIVATE
  src/workflow/aichattoolrunner_plan.cpp    # 功能层：runner 纯预检分支（decode+validate）
  src/workflow/aiplanpresentation.cpp       # 功能层：严格解码/提案契约/digest + 展示执行 adapter
)
target_sources(paleo_ui PRIVATE
  src/ui/ai/aiplanreviewwidget.cpp          # 视图层：原生计划审阅面板（编辑/预检/显式计算意图）
)
target_sources(paleo_app PRIVATE
  src/app/aiplanwiring.cpp                  # 组装根：计划 adapter 注入（幂等重挂）
)

# H5 严格边界单测（无 UI）：解码矩阵、提案契约、digest/请求身份、工具表
# 声明与分派、runner 入口（无宿主纯路径）。
add_paleo_test(tst_h5_plan_preview LIBS paleo_workflow paleo_ai)
# H5 adapter 单测：真实 CapabilityHost/CapabilityRecovery/OperationLog 本地
# 工程（tst_capability_recovery_native 的 Host 夹具口径），覆盖门禁/幂等/
# accept 后调度失败 lookup/已知身份恢复/取消矩阵。
add_paleo_test(tst_h5_plan_presentation LIBS paleo_workflow paleo_services paleo_store)
# H5 面板级测试：审阅面板打开/编辑/重新预检/按钮判定（paleo_ui 最小链接集）。
add_paleo_test(tst_h5_plan_workbench LIBS paleo_ui)
# H5 真实装配测试：AppContext + PaleoMainWindow + installConversationPresentation
# → 计划 presenter/dock/面板接线（真实主窗证据，W9）。
add_paleo_test(tst_h5_plan_assembly)
