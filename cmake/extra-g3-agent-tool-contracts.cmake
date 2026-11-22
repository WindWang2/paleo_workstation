# G3/agent-tool-contracts — Agent 工具声明、参数准入与上下文资格一致性
#（2026-11-21，docs/progress/g3-agent-tool-contracts-20261121.md）。
# query_project 主题字段契约单源（services/projectquerytopics）：工具表
# （paleo_ai 的 domaintools）与服务查询（paleo_services_query）共用一份；
# paleo_ai 显式链接查询契约所在库表达该依赖（功能层 → 数据层，无环）。
target_sources(paleo_services_query PRIVATE src/services/projectquerytopics.cpp)
target_link_libraries(paleo_ai PUBLIC paleo_services_query)
add_paleo_test(tst_agent_tool_contracts LIBS paleo_workflow paleo_ai)
# 层位建议资格夹具需真 ORT 小模型的用例在文件内 #if PALEO_HAVE_ORT 守卫，
# 其余快照/枚举/主题收敛用例在无 ORT 构建同样注册运行。
if(PALEO_HAVE_ORT)
  add_paleo_test(tst_agent_tool_readiness LIBS paleo_workflow paleo_ai_onnx)
else()
  add_paleo_test(tst_agent_tool_readiness LIBS paleo_workflow paleo_ai)
endif()
