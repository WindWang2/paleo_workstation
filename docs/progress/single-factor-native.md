# 单因素图算法原生集成

状态：进行中。本文只记录已经在本机跑过的命令。未跑的 Oracle 不记为通过。

## 范围

- 目标分支：`goal/single-factor-native-20261002`
- 工作目录：`.worktrees/single-factor-native`
- 目标基线：`eaaf46def5ce20ed064e73c80853e67667b9407e`
- 参考：`WWX9/haiyou-visualization` `27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`
- 方案：`docs/designs/single-factor-native-integration-plan.md`

P0 可信分析场和 P1 制图工作场都还没有端到端闭环。当前代码有数值核、冻结点查询对拍、约束参数存储、样本计数、参数指纹，以及编图入口对制图工作场的拒绝。

## 已核对语义

- 硬屏障模型是 `grid_connectivity_v1`。它不计算参考工程的 `FaultPathMetric`。
- 旧 `break_line` 仍是硬屏障。`interpretive_boundary`、`contour_stop`、`cartographic_detour` 不进入旧 IDW 凸包。
- 缺省 `value_source` 的 `single_factor_raster` 仍可进入定量分析。`cartographic_work` 在数据层被拒绝。
- 产品默认不开启井群权重。对拍夹具把参考参数全部写成显式值。
- 井距使用全体最近邻均值。`spacingSubsampled=false`。n>400 时不复刻 NumPy 的 400 点抽样。

## 本机命令

构建目录是 worktree 内的 Ninja `RelWithDebInfo`，`QGIS_PREFIX` 指向本机 `vendor/superbuild/prefix`。

```text
ctest --test-dir build -R '^tst_singlefactor_(kernel|parity|contract|store|samples)$' --output-on-failure
python3 tools/check_layering.py --strict
```

2026-10-02 结果：上述 5 个测试通过，严格分层通过。`tst_singlefactor_parity` 比较 13 个点查询夹具，容差为 `1e-9×S` 与 `1e-10×S`。

`cmake --build build --target paleo_ui -j1` 退出码 0。并行编译时未改动的 `src/ui/dialogs/folderconfirm.cpp` 曾触发 GCC 16 内部错误，单线程重试后链接成功。这只说明改过的界面和编图文件能编进库，不是端到端通过证据。

## 尚未完成

- 原生 Processing 算法 `paleo:paleo_local_direction_idw` 与异步发布。
- 真实数值等值线来源、P1 工作场文件与消费守卫的集成测试。
- 单因素页参数区。开始改页面前要重读 `DESIGN.md`。
- `PALEO_REAL_PROJECT_AREA` 真工区和性能门。环境未设置时的跳过不能记为通过。
- 全量 ctest、i18n、UI 不变量、clang-tidy 和 PR。
