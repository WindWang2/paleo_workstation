# 并行方向挂点：G5 agent-conversation-workbench（2026-11-21 批次）
# AI 助手原生会话工作台：上下文归属、实时+历史统一工具卡、operation 三轴
# 状态/详情/控制（经既有 CapabilityRecovery）、草稿与键盘生命周期、有界呈现。
# 约定见 CMakeLists.txt「并行开发挂点」：新增源文件/测试一律在这里登记，
# 不直接改根 CMakeLists 的模块源列表（并行分支在此汇合零冲突）。
target_sources(paleo_workflow PRIVATE
  src/workflow/aiconversationpresentation.cpp  # 功能层：工具结果/历史归属只读投影
  src/workflow/aioperationpresentation.cpp     # 功能层：操作查询/取消/恢复展示 adapter
)
target_sources(paleo_ui PRIVATE
  src/ui/ai/aiconversationcard.cpp             # 视图层：统一会话工具卡（惰性展开/主题刷新）
  src/ui/ai/aicontextbar.cpp                   # 视图层：上下文归属条（W1）
  src/ui/ai/aioperationdetailwidget.cpp        # 视图层：操作详情面（事件/端口/内容检视）
)
target_sources(paleo_app PRIVATE
  src/app/aiconversationwiring.cpp             # 组装根：adapter 注入（幂等重挂）
)

# G5 纯投影单测（无 UI）：envelope/raw/坏 JSON 归一、历史关联、UTF-8 边界。
add_paleo_test(tst_g5_conversation_projection LIBS paleo_workflow)
# G5 adapter 单测：真实 CapabilityHost/CapabilityRecovery/OperationLog 本地
# 工程（tst_capability_recovery 的 Host 夹具口径），覆盖门禁/去重/代次。
add_paleo_test(tst_g5_operation_presentation LIBS paleo_workflow paleo_services paleo_store)
# G5 dock 级测试（FakeLlmServer loopback）：历史卡恢复、草稿/键盘生命周期、
# 上下文条、有界卡窗口（paleo_ui 最小链接集）。
add_paleo_test(tst_g5_conversation_workbench LIBS paleo_ui)
# G5 真实装配测试：AppContext + PaleoMainWindow + attachAiAssistant +
# installConversationPresentation 全链（真实主窗 AI 页签证据，W9）。
add_paleo_test(tst_g5_assembly)
if(TARGET paleo_lrelease)
  target_compile_definitions(tst_g5_assembly PRIVATE PALEO_TEST_HAS_TRANSLATIONS)
endif()
