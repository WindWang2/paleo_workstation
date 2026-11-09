# UI 重设计 U0+U1（goal/ui-cmdregistry）——统一命令注册表 + 分派器 + 动作绑定。
# 设计：docs/designs/ui-command-registry.md。挂点约定见 cmake/README.md。
target_sources(paleo_services PRIVATE
  src/services/commandspec.cpp            # 数据层：CommandSpec 登记表（U0 盘点）+ 自检
)
target_sources(paleo_workflow PRIVATE
  src/workflow/commanddispatcher.cpp      # 功能层：状态/触发/Compute→CapabilityGateway 桥
)

add_paleo_test(tst_commandregistry LIBS paleo_services)                                # 登记表自检
add_paleo_test(tst_commanddispatcher LIBS paleo_workflow Paleo::paleo_data_service)  # 分派 + 网关桥
target_sources(paleo_qgis PRIVATE
  src/qgis/commandlocatorfilter.cpp       # QGIS 封装：Ctrl+K 命令搜索过滤器（禁用带原因）
)
target_sources(paleo_ui PRIVATE
  src/ui/commandactionbinder.cpp          # 视图层：QAction ↔ 命令（route / mirror / native 三种绑定）
  src/ui/paleomainwindow_commands.cpp     # 视图层：主窗命令处理器（工程/视图）+ Ctrl+K 命令过滤器挂接
)
add_paleo_test(tst_commandshell)                                                      # 真实主窗：入口全收编 + Ctrl+K + 源码闸
target_compile_definitions(tst_commandshell PRIVATE PALEO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
