# 单因素图算法原生集成

状态：进行中。提交 `4caa1af` 是数值核切片。提交 `d7bf6dc` 把本地方向插值发布进已有因子图层，并另声明制图工作场。未跑的 Oracle 不记为通过。这仍不是全文 P0/P1。

## 范围

- 目标分支：`goal/single-factor-native-20261002`
- 工作目录：`.worktrees/single-factor-native`
- 目标基线：`eaaf46def5ce20ed064e73c80853e67667b9407e`
- 参考：`WWX9/haiyou-visualization` `27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`
- 方案：`docs/designs/single-factor-native-integration-plan.md`

`method=local_direction_idw` 走 `paleo:paleo_local_direction_idw`，把分析栅格、支撑标记和 QC 写入 DERIVED，并声明 `factor.<层位>.<因素>`。缺省 method 仍走 `paleo:paleo_constraint_idw`。`paleo:paleo_cartographic_work` 另写 `cartographic.<层位>.<因素>`，组为 `04_SingleFactor/Cartographic`，不改分析场字节。没有单独成图多边形时，输出范围矩形就是成图域。单因素页参数区、异步任务、真工区和性能门还没有做。

## 已核对语义

- 硬屏障模型是 `grid_connectivity_v1`。它不计算参考工程的 `FaultPathMetric`。
- 旧 `break_line` 仍是硬屏障。`interpretive_boundary`、`contour_stop`、`cartographic_detour` 不进入旧 IDW 凸包。
- 缺省 `value_source` 的 `single_factor_raster` 仍可进入定量分析。`cartographic_work` 在数据层被拒绝。
- 图层树仍按成果类型分组。`04_SingleFactor/Contours` 与 `04_SingleFactor/Cartographic` 跟随 `04_SingleFactor`，在单因素页和综合编图页可见，预测页和验证页不带上整组。
- 等值线、制图工作场、因子融合、等厚链和相多边形成功后，壳会实例化对应声明图层并勾选。验证定位对问题行的 `layerId` 做同样的勾选，再缩放。
- 制图成果的图层 id 使用 `cartographic.<层位>.<因素>`，组为 `04_SingleFactor/Cartographic`。它不进入融合清单，融合和转面会拒绝它。分析栅格仍是 `factor.<层位>.<因素>`。
- 产品默认不开启井群权重。对拍夹具把参考参数全部写成显式值。
- 井距使用全体最近邻均值。`spacingSubsampled=false`。n>400 时不复刻 NumPy 的 400 点抽样。

## 本机命令

构建目录是 worktree 内的 Ninja `RelWithDebInfo`，`QGIS_PREFIX` 指向本机 `vendor/superbuild/prefix`。

```text
ctest --test-dir build -R '^tst_singlefactor_(kernel|parity|contract|store|samples)$' --output-on-failure
python3 tools/check_layering.py --strict
```

2026-10-02 结果：上述 5 个测试通过，严格分层通过。`tst_singlefactor_parity` 比较 13 个点查询夹具，容差为 `1e-9×S` 与 `1e-10×S`。

图层挂钩补丁另跑：

```text
ctest --test-dir build -R '^tst_(singlefactor_contract|panels|workflows|layerplatform)$' --output-on-failure
python3 tools/check_layering.py --strict
```

`tst_singlefactor_contract`、`tst_workflows`、`tst_layerplatform` 通过。`tst_panels` 第一次在 `validatePage_populatesAndLocates` 段错误：运行验证把刷新排到下一事件回合，测试当时仍读到空态行。补上 `QTest::qWait(1)` 后 `tst_panels` 通过。严格分层再次输出「检查通过：无层违规。」这不是端到端成图通过证据。

`cmake --build build --target paleo_ui -j1` 退出码 0。并行编译时未改动的 `src/ui/dialogs/folderconfirm.cpp` 曾触发 GCC 16 内部错误，单线程重试后链接成功。这只说明改过的界面和编图文件能编进库，不是端到端通过证据。

发布链补丁 `d7bf6dc` 另跑：

```text
cmake --build build -j2 --target tst_factorworkflow paleo_ui
ctest --test-dir build -R '^tst_factorworkflow$' --output-on-failure
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
```

第一次 `tst_factorworkflow` 失败，算法报「成图域为空」。处理包装把输出范围矩形写入成图域后，同一测试通过（6.17 秒）。严格分层、i18n 和 UI 不变量通过。EPSG:3857 分析场落在组 `04_SingleFactor`，制图工作场落在 `04_SingleFactor/Cartographic`，分析场 SHA-256 不变，融合拒绝文案含「解释性制图」。`type=hard_barrier` 且没有 `params_json` 被记为 `unknown_type`。EPSG:4326 即使 `localGrid=true` 也拒绝，文案含「经纬度必须先投影」。无坐标系且未开局部网时拒绝；打开局部网后 `poro` 成功，`crs_mode=local_engineering`。`method=kriging` 拒绝且不声明图层。未设置 `PALEO_REAL_PROJECT_AREA`，真工区不记通过。

## 尚未完成

- 单因素页参数区。开始改页面前要重读 `DESIGN.md`。
- 异步 `PaleoTaskService`、产品等值线的固定级别接口、`addConstraint` 仍调用旧 `append()`。
- `PALEO_REAL_PROJECT_AREA` 真工区和性能门。环境未设置时的跳过不能记为通过。
- 全量 ctest、clang-tidy 和 PR。
