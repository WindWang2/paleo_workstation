# cmake/extra-wellcomposite.cmake — wave/wellcomposite-deep 新增源文件/测试挂载点
#（CMakeLists.txt 模块表禁改惯例；本文件由根 CMakeLists GLOB 自动 include）。
#
# 深度升级交付：D1 道系统框架 / D2 深度交互 / D3 编辑套件 / D4 视觉输出 /
# D5 多井对比 / D6 深度变换 / D7 打磨可达性 / D8 测试与文档。

target_sources(paleo_ui PRIVATE
  src/ui/wellcomposite/trackregistry.cpp        # D1.1/D1.2 道规格与类型注册表
  src/ui/wellcomposite/trackops.cpp             # D1.5/D1.9 CSV 导出/复制/注入
  src/ui/wellcomposite/wellcompositestore.cpp   # D1.8 会话 + sidecar 持久化
  src/ui/wellcomposite/hiddentrackbar.cpp       # D1.10 隐藏道管理条
  src/ui/wellcomposite/trackconfigdialog.cpp    # D1.6/D1.7 道配置/多曲线编辑器
  src/ui/wellcomposite/depthtools.cpp           # D2.1/D2.7/D2.11/D2.12/D6.3
  src/ui/wellcomposite/intervalstatistics.cpp   # D2.2 区间统计
  src/ui/wellcomposite/editstack.cpp            # D3.8/D3.9 undo 栈
  src/ui/wellcomposite/editsession.cpp          # D3.3/D3.10/D3.11/D3.15 编辑会话
  src/ui/wellcomposite/topseditor.cpp           # D3.7 批量 TOPs 导入
  src/ui/wellcomposite/intervaleditor.cpp       # D3.4/D3.5/D3.13 区间编辑
  src/ui/wellcomposite/stratassignment.cpp      # D3.6 地层单元显式指派
  src/ui/wellcomposite/chronostratcolors.cpp    # D4.1 国际年代色标表
  src/ui/wellcomposite/exportengine.cpp         # D4.4–D4.10 导出引擎/图例
)
target_sources(paleo_ui PRIVATE
  src/ui/wellcomposite/patterncatalog.cpp     # D4.2/D4.3 花纹库 + JSON 相映射
  src/ui/wellcomposite/depthtransform.cpp     # D6.x MD↔TVD/KB/TWT/采样/LOD
  src/ui/wellcomposite/multiwellview.cpp      # D5.x 多井对比容器
)

# ---- D8.x 测试注册 ----
add_paleo_test(tst_wellcomposite_framework LIBS paleo_ui)
add_paleo_test(tst_wellcomposite_depth LIBS paleo_ui)
add_paleo_test(tst_wellcomposite_editing LIBS paleo_ui paleo_io)
add_paleo_test(tst_wellcomposite_visual LIBS paleo_ui)
target_compile_definitions(tst_wellcomposite_visual PRIVATE
  PATTERNS_JSON_PATH="${CMAKE_SOURCE_DIR}/src/ui/wellcomposite/resources/faciespatterns.json"
  WELLGOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/wellcomposite")
add_paleo_test(tst_wellcomposite_multiwell LIBS paleo_ui)
add_paleo_test(tst_wellcomposite_perf LIBS paleo_ui)
add_paleo_test(tst_wellcomposite_a11y LIBS paleo_ui)
