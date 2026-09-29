# MAMCL「地震多属性智能分析」外部 Python 工具：内嵌 venv 环境管理 + 启动编排。
# vendor/mamcl/*.zip 为内置程序包，运行时解包到 AppDataLocation/external/mamcl。
target_sources(paleo_services PRIVATE src/services/pythonenv.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/mamcltool.cpp)
target_compile_definitions(paleo_workflow PRIVATE
  PALEO_MAMCL_PACKAGE="${CMAKE_SOURCE_DIR}/vendor/mamcl/MAMCL_software_W04_v0.2.1_20260924.zip")
add_paleo_test(tst_pythonenv LIBS paleo_workflow)
