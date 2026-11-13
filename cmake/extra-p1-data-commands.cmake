# P1（data-commands 2026-11-08）：数据操作编排服务化与可信执行回执。
# 功能层命令服务 + 无 QtWidgets 的真实存储/故障注入测试。
target_sources(paleo_workflow PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src/workflow/dataopscommandservice.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/workflow/dataopscommandservice_undo.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/workflow/dataopscommandservice_catalog.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/workflow/dataopscommandservice_sidecars.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/workflow/dataopscommandservice_entities.cpp)
add_paleo_test(tst_dataops_commands LIBS paleo_workflow paleo_store)
add_paleo_test(tst_dataops_receipts LIBS paleo_workflow paleo_store)
add_paleo_test(tst_dataops_ui_receipts LIBS paleo_ui)  # P1-W8：原生数据页交互级回执验证
