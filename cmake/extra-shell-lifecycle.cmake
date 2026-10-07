# cmake/extra-shell-lifecycle.cmake — issue 批次：工程生命周期 × 组装根接线
# （#275 UIS-07 / #282 UIS-09 / #236 工程切换残留批）壳级回归。
# 伞式（app 面全栈）：AppContext + PaleoMainWindow + attachWorkflows 真实壳驱动。
add_paleo_test(tst_shell_projectlifecycle)
