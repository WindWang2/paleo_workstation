# MAMCL「地震多属性智能分析」外部 Python 工具：内嵌 venv 环境管理 + 启动编排。
# vendor/mamcl/*.zip 为内置程序包，运行时解包到 AppDataLocation/external/mamcl。
# #142：程序包 SHA-256 编译进二进制（解包前核对）；同名 .requirements.lock 为
# uv pip compile --universal --generate-hashes 产物，pip 走 --require-hashes。
target_sources(paleo_services PRIVATE src/services/pythonenv.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/mamcltool.cpp)
set(PALEO_MAMCL_ZIP_NAME "MAMCL_software_W04_v0.2.1_20260924")
set(PALEO_MAMCL_ZIP_SHA256 "5650a172ce3054da17e9703c38822d85625843c9db4ce6d56b292017dfb8ce5a")
target_compile_definitions(paleo_workflow PRIVATE
  PALEO_MAMCL_PACKAGE="${CMAKE_SOURCE_DIR}/vendor/mamcl/${PALEO_MAMCL_ZIP_NAME}.zip"
  PALEO_MAMCL_SHA256="${PALEO_MAMCL_ZIP_SHA256}")
include(GNUInstallDirs)
install(FILES
  "${CMAKE_SOURCE_DIR}/vendor/mamcl/${PALEO_MAMCL_ZIP_NAME}.zip"
  "${CMAKE_SOURCE_DIR}/vendor/mamcl/${PALEO_MAMCL_ZIP_NAME}.requirements.lock"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/paleo/mamcl")
add_paleo_test(tst_pythonenv LIBS paleo_workflow)
if(PALEO_PYTHON3)
  add_test(NAME mamcl_lock COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_mamcl_lock.py)
  add_test(NAME mamcl_lock_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_mamcl_lock.py --selftest)
endif()
