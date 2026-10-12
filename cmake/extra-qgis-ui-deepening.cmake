# cmake/extra-qgis-ui-deepening.cmake — goal/qgis-ui-deepening-20261012 挂载点
#（CMakeLists.txt 模块表禁改，见 cmake/README.md）。
# B 任务（QGIS/UI 工作流深化）：层位状态一致性壳级回归
#（AppContext + PaleoMainWindow + attachWorkflows/attachMapping 真实壳）。

add_paleo_test(tst_shell_horizonstate) # 伞式（app 面全栈，tst_shell_projectlifecycle 模式）
