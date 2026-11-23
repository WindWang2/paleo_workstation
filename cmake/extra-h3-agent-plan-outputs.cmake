# H3｜版本化 DAG 的显式多成果（v2 plan exports）。
# 源码进 paleo_workflow；测试走 add_paleo_test 的最小链接集
#（与 tst_capability_recovery_native 同口径，含 catalog/lock 夹具所需模块）。
target_sources(paleo_workflow PRIVATE
  src/workflow/capabilityplan_outputs.cpp
  src/workflow/capabilityplanexecution.cpp
  src/workflow/capabilitynativeplan_v2.cpp
  src/workflow/capabilityhost_plan_v2.cpp)
add_paleo_test(tst_h3_plan_outputs LIBS paleo_workflow paleo_services paleo_store paleo_algorithms)
add_paleo_test(tst_h3_plan_publication LIBS paleo_workflow paleo_services paleo_store paleo_algorithms)
