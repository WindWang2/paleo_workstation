# I2（agent-unit-onnx-atoms，2026-11-24 批次）：地层单元 RMS → 本地 ONNX
# 推理原子（unit.onnx_predict v1 / paleo.unit_onnx_predict）。
# 专属实现文件（contract/operation/service/tools/wiring）+ 专属测试都挂这里；
# 共享注册缝只有根 manifest 一行与本文件内的 target_sources 追加。
#
# · paleo_workflow_unitonnx：冻结计划/物化/推理（数据+ai 依赖，两种构建
#   都编——无 ORT 构建里相关入口返回 ort_unavailable，测试断言结构化不可用）。
# · unitonnxservice.cpp 编入 paleo_workflow（需要 capabilityhost.h；与
#   paleo_workflow_unitonnx 的依赖方向一致）。
# · unitonnxtools.cpp 编入 paleo_ai（工具表族）；unitonnxwiring.cpp 编入
#   paleo_app（组装根）。
add_library(paleo_workflow_unitonnx STATIC
  src/workflow/unitonnxcontract.cpp
  src/workflow/unitonnxoperation.cpp
)
target_include_directories(paleo_workflow_unitonnx PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paleo_workflow_unitonnx PUBLIC paleo_services)
if(PALEO_HAVE_ORT)
  target_link_libraries(paleo_workflow_unitonnx PUBLIC paleo_ai_onnx)
else()
  target_link_libraries(paleo_workflow_unitonnx PUBLIC paleo_ai)
endif()
target_link_libraries(paleo_workflow PUBLIC paleo_workflow_unitonnx)
target_sources(paleo_workflow PRIVATE src/workflow/unitonnxservice.cpp)
target_sources(paleo_ai PRIVATE src/ai/chat/unitonnxtools.cpp)
target_sources(paleo_app PRIVATE src/app/unitonnxwiring.cpp)
target_sources(paleo_services PRIVATE
  src/services/unitonnxcapabilityspec.cpp
  src/services/algorithmspec_unitonnx.cpp
)

# 契约/物化/推理的单元级回归：真实 SeismicUnitInput + 受控 ORT 夹具模型，
# 不依赖 QGIS 视图/宿主。两种构建都注册（无 ORT 构建断言 ort_unavailable）。
add_paleo_test(tst_unit_onnx_operation LIBS paleo_workflow_unitonnx Paleo::paleo_data_service)

# Agent 级回归：真实工具解码 → 宿主适配器 → 持久 operation 链
# （prepare/compute-only/publish/重放/冲突/取消/恢复；documentsourcefixture
# + 单元/地震夹具，走生产 bindChatToolRunner 装配）。
if(PALEO_HAVE_ORT)
  add_paleo_test(tst_unit_onnx_agent LIBS paleo_app paleo_ai_onnx Paleo::paleo_data_service)
  set_tests_properties(tst_unit_onnx_operation tst_unit_onnx_agent PROPERTIES ENVIRONMENT_MODIFICATION
    "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
else()
  # 无 ORT 构建档：就绪委托与有宿主的 readiness 路径按阶段给结构化不可用
  # （publish/列表不设门）；工具表保留静态路由。同 tst_agent_noort 口径。
  add_paleo_test(tst_unit_onnx_noort LIBS paleo_workflow)
endif()
