# P2（2026-11-19）— SEG-Y 索引可信恢复与时间切片缓存一致性。
# 索引侧（SegyIndexAudit/验证器）随 extra-io-perf 的 segyindexstore 同库
# 登记于 extra-io-perf.cmake；本 fragment 只挂时间切片 writer/摘要的
# io 层回归（真实 GDAL，无mock）。服务层缓存对协议回归在既有的
# tst_seismicattrscan（paleo-tests.cmake）内扩展。

add_paleo_test(tst_p2_seismic_cache LIBS paleo_io)
