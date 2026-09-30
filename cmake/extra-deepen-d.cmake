# cmake/extra-deepen-d.cmake — wave/deepen-perf worker D（井综合/连井剖面）
# 新增源文件/测试挂载点（CMakeLists.txt 模块表禁改惯例；根 CMakeLists GLOB
# 自动 include 本文件）。交付：D1 派生登记 sink + 深度装配链；D2 剖面生命
# 周期回归；D3 打印对话框原生接线。

target_sources(paleo_ui PRIVATE
  src/ui/wellcomposite/derivedsink.cpp   # D1 derivedDocumentReady → catalog DERIVED + 井斜/时深喂表
)

add_paleo_test(tst_wellcomposite_shell LIBS paleo_ui paleo_io)
# 伞式（app 面全栈）：AppContext + PaleoMainWindow 真实壳驱动剖面生命周期
add_paleo_test(tst_sectionlifecycle)
