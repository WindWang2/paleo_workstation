# wave/batch-jobqueue —— 批处理与作业队列编排（方向 33）。
# 挂载点纪律见 CMakeLists.txt「并行开发挂点」段：源文件与测试一律放本文件，
# 禁止直接改 CMakeLists.txt 的模块源列表（并行分支在此汇合零冲突）。
#
# 编排层落 src/workflow（层：功能）——它要 include paleo_services 的
# jobrunner 三段式并调既有工作流；src/services 禁引 workflow/*（层契约）。
target_sources(paleo_workflow PRIVATE
  src/workflow/batchjobqueue.cpp)

# 批次进度面板（视图层，只观察编排层信号）。
target_sources(paleo_ui PRIVATE
  src/ui/batchjobpanel.cpp)

add_paleo_test(tst_batchjobqueue LIBS paleo_workflow)  # 幂等/恢复/隔离/取消/熔断/报告
add_paleo_test(tst_batchjobpanel LIBS paleo_ui)          # 面板只观察信号：行/汇总/可见性轮询
