# 方向63（goal/shortcuts-help）——快捷键中央注册表 + 快捷键总表 + 帮助面。
# 挂点约定见 cmake/README.md：本方向不改 CMakeLists.txt 模块源列表。
target_sources(paleo_services PRIVATE
  src/services/shortcutregistry.cpp      # 数据层：条目/查重/上下文解析（无 QtWidgets）
)

add_paleo_test(tst_shortcutregistry LIBS paleo_services) # 注册/查重/上下文优先级
