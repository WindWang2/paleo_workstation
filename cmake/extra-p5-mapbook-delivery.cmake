# P5 mapbook-delivery（方向三：编图表达与成果交付）——井点数值表达与地图册
# 交付闭环。源文件与测试的唯一注册点（本任务独占，见
# .git/paleo-parallel-20261108/p5-mapbook-delivery-ownership.md）。
target_sources(paleo_qgis PRIVATE
  src/qgis/samplepointstyle.cpp   # P5-W1/W2：井点因子样点表达（井名+实际值双行标注）
  src/qgis/mapbookrender.cpp      # P5-W6：地图册建版/渲染/销毁 facade（QGIS 实体不出封装层）
)

add_paleo_test(tst_samplepoint_presentation LIBS paleo_qgis) # P5-W1/W2：数值/身份契约 oracle（含表达式同源断言）
add_paleo_test(tst_mapbook_delivery LIBS paleo_ui Qt6::Pdf)  # P5-W3/W4/W5：冻结输入/逐页状态/目录分页/多文件校验（PDF 页数核验用 QtPdf）
