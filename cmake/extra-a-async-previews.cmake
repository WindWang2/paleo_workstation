# A｜异步预览与交会数据读取（#961/#974）：闭环测试（预算适配器随
# seismictaskservice.cpp 编进 paleo_services，无单独源文件）。
add_paleo_test(tst_a_async_previews LIBS paleo_services paleo_qgis)
target_compile_definitions(tst_a_async_previews PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
