# wave/seismic-engine-deep — 地震引擎深化的新测试挂载点
# （源文件仍挂根 CMakeLists 模块表；新 vendor 源/测试集中此处，并行分支零冲突）

add_paleo_test(tst_seismic_engine) # sdk::Dataset Open 矩阵/取消无半成品/paged 续跑幂等/瓦片/体素/LOD/interpolate 回落/P5 道字约定/StorageProfile
target_compile_definitions(tst_seismic_engine PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area"
  SEGY_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")

# 主线7 性能闸门：make_segy_fixture.py --mb 合成 ≥200MB 生产形状体（一次性
# 生成缓存在构建目录，不入库）；QuickOpen/首剖面/时间片/任意剖面延迟预算。
add_paleo_test(tst_seismic_perf)
find_program(PALEO_PERF_PYTHON3 NAMES python3 python)
target_compile_definitions(tst_seismic_perf PRIVATE
  PALEO_SEGY_FIXTURE_TOOL="${CMAKE_SOURCE_DIR}/tools/make_segy_fixture.py"
  PALEO_SEISMIC_PERF_DIR="${CMAKE_CURRENT_BINARY_DIR}/seismic_perf"
  PALEO_PYTHON3="${PALEO_PERF_PYTHON3}")

# P5 Phase 0 基线实测：索引冷/热、切片冷/热、双通道转码、体素、离屏 3D
# 帧率、峰值 RSS（输出 = docs/seismic/BASELINE.md 数据源；共享夹具目录）。
add_paleo_test(tst_seismic_baseline LIBS paleo_ui)
target_compile_definitions(tst_seismic_baseline PRIVATE
  PALEO_SEGY_FIXTURE_TOOL="${CMAKE_SOURCE_DIR}/tools/make_segy_fixture.py"
  PALEO_SEISMIC_PERF_DIR="${CMAKE_CURRENT_BINARY_DIR}/seismic_perf"
  PALEO_PYTHON3="${PALEO_PERF_PYTHON3}")
