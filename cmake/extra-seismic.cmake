# wave/seismic-engine-deep — 地震引擎深化的新测试挂载点
# （源文件仍挂根 CMakeLists 模块表；新 vendor 源/测试集中此处，并行分支零冲突）

add_paleo_test(tst_seismic_engine) # sdk::Dataset Open 矩阵/取消无半成品/paged 续跑幂等/瓦片/体素/LOD/interpolate 回落/P5 道字约定/StorageProfile
target_compile_definitions(tst_seismic_engine PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area"
  SEGY_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
