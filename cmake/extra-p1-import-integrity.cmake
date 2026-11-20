# P1（#1023/#1028/#1029）——宿主原始数据导入的归档完整性、XML 解析与真实
# 错误回执。根 CMakeLists 的模块源列表不动（并行开发纪律）：新源/测试挂这里。
#
# 布线：
#   paleo_io + xmlstreamsafety（统一 DTD/前导 token/解析错误策略，P1-W3）
#   tst_p1_import_integrity —— 跨层回归集中在独立目标（P1-W8）：ZIP64
#     sentinel 矩阵、空 deflate 真解码、DTD 策略、OOXML 部件失败可见、
#     预算/取消、坏容器经宿主导入入口的真实回执。既有
#     tst_io_ziparchive_unit / tst_io_workbook_edges / tst_import_preview
#     的行为由它们自己的目标继续覆盖（本任务只加不拆）。

target_sources(paleo_io PRIVATE
  src/io/xmlstreamsafety.h
  src/io/xmlstreamsafety.cpp
)

add_paleo_test(tst_p1_import_integrity LIBS paleo_workflow)
