# UI 重设计 U0+U1（goal/ui-cmdregistry）——统一命令注册表 + 分派器 + 动作绑定。
# 设计：docs/designs/ui-command-registry.md。挂点约定见 cmake/README.md。
target_sources(paleo_services PRIVATE
  src/services/commandspec.cpp            # 数据层：CommandSpec 登记表（U0 盘点）+ 自检
)

add_paleo_test(tst_commandregistry LIBS paleo_services)                                # 登记表自检
