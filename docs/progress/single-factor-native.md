# 单因素图算法原生集成

状态：PR #109 已合并进 `56d643c`。后续代码提交 `38e4ed2` 把等间距等值线和解释性等值线拆到任务里，并补了故障注入、提线预算和 S/L 事件循环 p95。未跑完的 Oracle 不记为通过。O12 仍是跳过。这仍不是方案全文通过。

## 范围

- 目标分支：`goal/single-factor-native-20261002`
- 工作目录：`.worktrees/single-factor-native`
- 目标基线：`eaaf46def5ce20ed064e73c80853e67667b9407e`
- 参考：`WWX9/haiyou-visualization` `27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`
- 方案：`docs/designs/single-factor-native-integration-plan.md`

`method=local_direction_idw` 走 `paleo:paleo_local_direction_idw`，把分析栅格、支撑标记和 QC 写入 DERIVED，并声明 `factor.<层位>.<因素>`。缺省 method 仍走 `paleo:paleo_constraint_idw`。`paleo:paleo_cartographic_work` 另写 `cartographic.<层位>.<因素>`，组为 `04_SingleFactor/Cartographic`，不改分析场字节。没有单独成图多边形时，输出范围矩形就是成图域。单因素页可以选择该方法、覆盖方式和等值线来源；本地方向计算走 `PaleoTaskService`。解释性等值线图层 id 是 `cartographic.<层位>.<因素>.contours`。真工区仍未验收。

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

页面、异步和等值线补丁 `2e293d5` 另跑：

```text
cd .worktrees/single-factor-native/build
QT_QPA_PLATFORM=offscreen ctest --output-on-failure -j8
QT_QPA_PLATFORM=offscreen ctest --output-on-failure -R '^tst_startup_trace$' -j1
cd .worktrees/single-factor-native && python3 tools/check_tidy.py
```

全量 180/181 通过。`tst_startup_trace` 在并行负载下失败，空载单独重跑通过（11.56 秒）。未放宽启动比率门。clang-tidy 17 个改动 TU 通过。`tst_singlefactor_perf` 空载结果：S=2004 ms，scale1024=6885 ms，L=14334 ms，wells200=3129 ms，像元比 3.44，井数比 1.56，取消中位 1 ms、最大 2 ms，RSS 增量 15328 KiB。像元比比较的是同一批井和同一批约束，只把成图域扩成 1024 方格。L 的方向线和软边界更多，不用 L/S 充当像元比。`tst_singlefactor_contours` 通过常量场、斜面、nodata 孔、开放端、圆锥和非法级别。`tst_factorworkflow` 通过穿线绕行后分析 SHA 不变、严格模式拒绝剩余穿线，以及融合拒绝解释性等值线。`tst_mappingpages` 的离屏用例覆盖方法、等值线来源和忙碌态。

等值线任务补丁 `38e4ed2` 在 `56d643c` 上另跑：

```text
cmake --build build -j2 --target tst_factorworkflow tst_singlefactor_asynccontour tst_singlefactor_faults tst_singlefactor_contourbudget tst_singlefactor_guilatency
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure -V -j1 -R '^(tst_factorworkflow|tst_singlefactor_asynccontour|tst_singlefactor_faults|tst_singlefactor_contourbudget|tst_singlefactor_guilatency)$'
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
git diff --check
python3 tools/check_tidy.py
```

五个测试通过。`tst_factorworkflow` 18 通过、1 跳过，跳过文案写明这不是 O12 通过。提线预算预热 1 次后 3 次中位 66 ms、最大 67 ms（512²、10 级、8 条约束，改写像元 14408，门 ≤20000 ms）。GUI 用与性能测相同的 S/L 负载各跑 1 次：S p95=11 ms（wall 2103 ms），L p95=11 ms（wall 14618 ms），maxGap 都是 11 ms，门 ≤100 ms。故障测试覆盖无法识别的栅格、只读制品目录、只读清单和未登记的孤立文件；孤立文件重开后不是产品，文件本身还在。clang-tidy 通过本分支相对 master 的 2 个 TU。严格分层、i18n 和 UI 不变量通过。没有重跑 `tst_singlefactor_perf` 和全量 ctest。

`8dff163` 补上分析等值线的分析场 SHA。prepare 在增加代数之前记 SHA；publish 在 stage 前和 commit 前再核对。栅格被改写则不声明等值线，文案含「分析场在等值线期间被改写」。`tst_singlefactor_asynccontour` 的 `rewrittenAnalysisDropsContourPublish` 通过（6 通过、0 跳过）。同一轮 `tst_factorworkflow` 仍是 18 通过、1 跳过。严格分层、i18n、UI 不变量和 clang-tidy 的 2 个 TU 通过。

## 尚未完成

- O7 还没有 GDAL flush 中途失败、杀掉正在跑的进程，也没有自动回收孤立文件。关停和单调进度没有单独断言。
- O11：有任务服务时等值线计算离开界面线程。任务服务为空时仍同步。双主题、窄 dock、高 DPI 没有真人点击。
- O12：`PALEO_REAL_PROJECT_AREA` 未设置。只读工区井点 GeoJSON 仍只有 `coordinate_status`、`id`、`name`。跳过不是通过，也没有编造井值。
- O13 的分析中位仍是上一轮空载结果。本轮测的是 GUI p95 和提线/绕行中位。
- P2（克里金、完整 SFPKG、外委 XML/XLSX、历史制图策略、时深转换、监督分类、打印排版、Python GUI、`FaultPathMetric`）在 `TODOS.md`，不在本分支实现。
