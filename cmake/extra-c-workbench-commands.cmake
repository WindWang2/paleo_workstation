# C｜工作台命令资格、页面反馈与绑定生命周期（codex/paleo-c-workbench-commands-20261120）
# 新增源文件与专属测试挂点：单份动作目录、统一准入适配器与命令一致性回归。
target_sources(paleo_ui PRIVATE
  src/ui/workbench/workbenchactioncatalog.cpp
  src/ui/workbench/workbenchadmission.cpp
)

add_paleo_test(tst_c_workbench_commands LIBS paleo_ui)
