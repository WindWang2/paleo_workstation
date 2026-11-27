# goal/b-runtime-lifecycle — 任务 B（#959/#960）：用户脚本/REPL 的 Windows
# 创建时进程树所有权（scriptprocessscope，Unix 为无操作保形层）与受控
# Python 的认证私有执行视图/租约/恢复（pythonsandbox_windows_view）。
target_sources(paleo_services PRIVATE src/services/scriptprocessscope.cpp)
# 与 pythonsandbox_windows.cpp 同口径：文件内自带 Q_OS_WIN 守卫与空翻译
# 单元，非 Windows 平台仅保住挂点、不产符号。
target_sources(paleo_services_pythonruntime PRIVATE
  src/services/pythonsandbox_windows_view.cpp
  src/services/pythonsandbox_windows_view_logic.cpp)
add_paleo_test(tst_scriptprocessscope LIBS paleo_services)
add_paleo_test(tst_pythonsandbox_view_journal LIBS paleo_services_pythonruntime)
