# cmake/extra-surfaceops.cmake —— goal/gridding-surface-ops（网格化算法与面运算）
# 本方向新增源文件/测试的挂载点（见 CMakeLists.txt「并行开发挂点」段；
# 禁止直接改根 CMakeLists 的模块源列表）。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/gridsolver.cpp       # 最小曲率/连续曲率张力样条网格化核 + 守卫 + CV
  src/algorithms/mincurvature.cpp     # paleo:paleo_min_curvature Processing 包装
  src/algorithms/rasteralgebra.cpp    # 栅格代数核（NaN 传播/零除→null 语义）
  src/algorithms/surfacevolumes.cpp   # 面间体积/基准面上方体积量算核
)

add_paleo_test(tst_gridsolver LIBS paleo_algorithms)     # 解析面 RMS + CV + 守卫 + 取消/进度
add_paleo_test(tst_rasteralgebra LIBS paleo_algorithms)  # 代数语义（null/零除/条件）
add_paleo_test(tst_surfacevolumes LIBS paleo_algorithms) # 楔/锥/棱柱解析体积 + 等厚端到端

target_sources(paleo_workflow PRIVATE
  src/workflow/surfacegridding.cpp    # 层位散点网格化 + 等厚/体积面运算编排
)

add_paleo_test(tst_surfacegridding_thread LIBS paleo_workflow) # #122 worker 登记回 catalog 所属线程

target_sources(paleo_ui PRIVATE
  src/ui/dialogs/griddingdialog.cpp     # 网格化/面运算参数表 + 体积报告（视图件）
)

add_paleo_test(tst_gridding_realarea LIBS paleo_algorithms paleo_io) # 真机 8 层位耗时（PALEO_REAL_PROJECT_AREA 门控，未设跳过）
