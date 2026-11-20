# P5 edit-lifecycle —— 约束编辑生命周期、复制身份与撤销一致性
#（docs/progress/p5-edit-lifecycle-20261119.md）。
# 新增测试的挂载点（见 CMakeLists.txt「并行开发挂点」段；不直接改根
# CMakeLists 的模块源列表）。生产代码改动全部落在既有文件，无新增源文件。

# P5 专属生命周期回归：单一撤销归属（#1009）、批量导入有界性（#1013）、
# 编辑器一次绑定（#1015）、失败分支一致性（#1017）、复制身份映射（#990）。
# 真实 GPKG + 生产 session starter + ClosureEditStack + 计数 oracle。
#（Paleo::paleo_data_service 同 tst_workflows：workflow 头面所需的链接集。）
add_paleo_test(tst_p5_edit_lifecycle LIBS paleo_workflow Paleo::paleo_data_service)
set_tests_properties(tst_p5_edit_lifecycle PROPERTIES TIMEOUT 600)
