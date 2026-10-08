# goal/python-scripting — 方向68：Python 脚本面（脚本运行服务 / JSON 行协议 /
# REPL 会话桥 / 控制台面板 / 结果落地通道）。契约与示例：
# tools/reference/scripts/README.md。
target_sources(paleo_services PRIVATE
  src/services/scriptrunner.cpp
  src/services/scriptprotocol.cpp
  src/services/pythonrepl.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/pythonconsolecontroller.cpp)
target_sources(paleo_ui PRIVATE
  src/ui/python/pythonconsolepanel.cpp
  src/ui/python/pythonreplpanel.cpp
  src/ui/paleomainwindow_attach_python.cpp)

add_paleo_test(tst_scriptrunner LIBS paleo_services)
target_compile_definitions(tst_scriptrunner PRIVATE
  SCRIPT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/scripts")
add_paleo_test(tst_scriptprotocol LIBS paleo_services)
add_paleo_test(tst_pythonrepl LIBS paleo_services)
add_paleo_test(tst_pythonconsole LIBS paleo_ui paleo_workflow paleo_services)
target_compile_definitions(tst_pythonconsole PRIVATE
  SCRIPT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/scripts")
# 方向81：configure 钉了 PALEO_TEST_PYTHON → 两个进程级测试断言 ctest 注入的
# PALEO_PYTHON 就是它且 findBasePython() 选中它（不静默退回 PATH / QSKIP）。
if(PALEO_TEST_PYTHON_NORM)
  foreach(_t tst_scriptrunner tst_pythonrepl)
    target_compile_definitions(${_t} PRIVATE PALEO_TEST_PYTHON="${PALEO_TEST_PYTHON_NORM}")
  endforeach()
endif()
