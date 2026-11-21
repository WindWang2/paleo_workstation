# G4：持久 operation 记录完整性、私有命名空间与恢复边界。
# 新增源码经 target_sources 挂入分层库（不动共享清单）；测试用最小链接集。
target_sources(paleo_store PRIVATE
  src/metadata/operationnamespace.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/operationprivatedir.cpp)
add_paleo_test(tst_agent_operation_namespace LIBS paleo_store)
add_paleo_test(tst_agent_operation_integrity LIBS paleo_workflow paleo_services paleo_store)
