# 方向63（goal/shortcuts-help）——快捷键中央注册表 + 快捷键总表 + 帮助面。
# 挂点约定见 cmake/README.md：本方向不改 CMakeLists.txt 模块源列表。
target_sources(paleo_services PRIVATE
  src/services/shortcutregistry.cpp      # 数据层：条目/查重/上下文解析（无 QtWidgets）
)
target_sources(paleo_ui PRIVATE
  src/ui/shortcuts/shortcutcatalog.cpp    # 全仓键位唯一真源 + QShortcut/QAction 绑定入口
  src/ui/shortcuts/shortcutsheetdialog.cpp # F1 快捷键总表（分组/搜索/复制单条）
  src/ui/help/helpsurface.cpp             # 帮助菜单：总表 / 这是什么？ / 关于
  src/ui/help/whatsthiscatalog.cpp        # 核心控件 whatsThis 清单与回填
)

add_paleo_test(tst_shortcutregistry LIBS paleo_services) # 注册/查重/上下文优先级
add_paleo_test(tst_shortcuthelp LIBS paleo_ui)           # 目录零冲突+变异/R0 收编/总表/帮助面（offscreen）
target_compile_definitions(tst_shortcuthelp PRIVATE PALEO_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # 源码闸
add_paleo_test(tst_shortcuts_shell)                      # 真实主窗：键位行为回归 + F1/Shift+F1 + whatsThis 落点
