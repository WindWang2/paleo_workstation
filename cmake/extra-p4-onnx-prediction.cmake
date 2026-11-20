# P4（2026-11-19）：原生 ONNX 预测准入、血缘与发布闭环。
# · onnxruntimeprobe：ORT C API 准入探测（首包装对象前），编入 paleo_ai_onnx
#   （无 ORT 构建该目标不存在——探测语义由编译期 PALEO_HAVE_ORT=0 表达，
#   target_sources 仅在有目标时生效）。
# · predictionpublication：预测发布协调（提交事实查询），编入 paleo_workflow。
# · tst_p4_onnx_prediction：两种构建都注册——无 ORT 构建断言「明确不可用」
#   （不是 QSKIP 也不是成功推理）。
if(TARGET paleo_ai_onnx)
  target_sources(paleo_ai_onnx PRIVATE src/ai/onnxruntimeprobe.cpp)
endif()
target_sources(paleo_workflow PRIVATE src/workflow/predictionpublication.cpp)
add_paleo_test(tst_p4_onnx_prediction LIBS paleo_workflow Paleo::paleo_data_service)
if(PALEO_HAVE_ORT)
  # 与 paleo-tests.cmake 的 onnx 族同口径：受控 ORT 运行库前置进
  # LD_LIBRARY_PATH（path_list_prepend 不吞 paleo-dev 注入的 deb 前缀）。
  set_tests_properties(tst_p4_onnx_prediction PROPERTIES ENVIRONMENT_MODIFICATION
    "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
endif()
