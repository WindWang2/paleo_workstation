# H4：当前 Agent 作用域的持久 operation 发现与关系分页（paleo.operation_list）。
# 新增源码经 target_sources 挂入分层库（不动共享清单）；测试用最小链接集。
target_sources(paleo_workflow PRIVATE
  src/workflow/operationdiscovery.cpp
  src/workflow/capabilityrecovery_discovery.cpp
  src/workflow/aichattoolrunner_operationlist.cpp)
target_sources(paleo_ai PRIVATE
  src/ai/chat/operationdiscoverytools.cpp)
add_paleo_test(tst_h4_operation_discovery LIBS paleo_workflow paleo_services paleo_store)
add_paleo_test(tst_h4_operation_list_tool LIBS paleo_workflow paleo_ai)
