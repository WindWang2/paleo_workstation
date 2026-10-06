# 并行方向挂点：goal/ai-assist-20261006（方向51）
# 远端预测路由接线（替身 Mock 退场）+ 传输异步化。
#
# 约定见 CMakeLists.txt「并行开发挂点」：新增源文件/测试一律在这里登记，
# 不直接改根 CMakeLists 的模块源列表（并行分支在此汇合零冲突）。
target_sources(paleo_ai PRIVATE
  src/ai/remotepredictrouter.cpp
  src/ai/remotepredictconfig.cpp)
# ORT 本地降级引擎：TU 里用到 PaleoOnnxService 的符号，只在 ORT 构建里编。
if(PALEO_HAVE_ORT)
  target_sources(paleo_ai PRIVATE src/ai/onnxlocalpredictor.cpp)
endif()
target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paleo_ai PUBLIC Qt6::Network)

# 装配根：远端预测装配（唯一的装配入口，产品与测试同源）。
target_sources(paleo_app PRIVATE src/app/aiwiring.cpp)

# 装配断言：用 paleo_app 最小链接集（伞式 paleo_core 在部分宿主上死于
# 0xc0000139，见 ledger「宿主红项」）。
add_paleo_test(tst_aiwiring LIBS paleo_app)
