# 单因素原生集成 ledger

未完成项不能记为通过。下面只写本轮实际命令。

## R0 — 基线与参考锁

- 目标基线：`eaaf46def5ce20ed064e73c80853e67667b9407e`（worktree `goal/single-factor-native-20261002`）。
- 参考 SHA：`27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`，只读目录 `/home/kevin/projects/_refs/haiyou-visualization`。
- 参考环境：`/tmp/sf-ref-venv`，CPython 3.14.7。`numpy==2.5.3`、`scipy==1.18.1`、`shapely==2.1.2`，wheel sha256 见 `tools/reference/singlefactor/requirements.lock`。`numpy==2.2.6` 没有 cp314 wheel，源码构建失败，不进入锁。
- 本轮提交：`4caa1af1e3ce5a080b0dc988558594f5c88647d6`。这是数值核与消费守卫切片，不是全文完成。
- 未完成：真工区字段盘点、性能负载生成器冻结、`DESIGN.md` 在改单因素页之前重读。

## R1 — 局部方向核与点查询对拍

- 变更：`src/algorithms/singlefactor/` 的曲线核、局部 IDW、软边界、井群、硬屏障连通和样本过滤。Processing 包装尚未注册。
- 夹具：`tests/fixtures/singlefactor/kernel_parity.json`，由 `tools/reference/singlefactor/generate_reference.py` 调用参考工程 `LocalInterpolator` 生成。13 个点查询。不含硬屏障。
- 命令：

```text
cmake --build .worktrees/single-factor-native/build --target tst_singlefactor_kernel tst_singlefactor_parity tst_singlefactor_samples -j4
ctest --test-dir .worktrees/single-factor-native/build -R '^tst_singlefactor_(kernel|parity|samples)$' --output-on-failure
```

- 结果：三个测试退出码 0。parity 使用 `maxAbs <= 1e-9×S`、`RMSE <= 1e-10×S`，mask 先比较。
- 对应 Oracle：O1/O2/O3 的点查询对拍通过。O4 只在 C++ 栅格连通测试中通过，没有对 Python 可见性图对齐。O5 的计数、0 值、百分数换算和 CRS 模式通过；几何库预处理尚未做。
- 数值：Gauss–Legendre 48 节点与 NumPy `leggauss(48)` 的核质量相对差约 `4e-15`。未放宽容差。
- 未完成：Float32 落盘、网格对拍、空间索引。当前查询循环是线性的。

## R2 — 约束参数存储

- 变更：`ConstraintStore::appendExtended` 与 `updateParameters` 写入 `params_json`、`schema_version`。旧 `append` 仍可用。只读队列和缺失文件不创建 GPKG。
- 命令：`ctest --test-dir build -R '^tst_singlefactor_store$' --output-on-failure`
- 结果：退出码 0，`tst_singlefactor_store` Passed。
- 对应 Oracle：O6 的重开、幂等更新、缺 id 回滚、只读不建库通过。旧工程全量 `tst_constraintstore` 尚未在这次改动后重跑。

## R3 — 消费边界与参数指纹

- 变更：`src/domain/singlefactorrequest.*`。旧 IDW 按 `legacyConstraintRole` 跳过三种新类型。编图 polygonize、compose、等值线入口和页面按钮拒绝 `cartographic_work`。
- 命令：`ctest --test-dir build -R '^tst_singlefactor_contract$' --output-on-failure`；`python3 tools/check_layering.py --strict`
- 结果：合同测试退出码 0。严格分层输出「检查通过：无层违规。」
- 对应 Oracle：O8 的定量拒绝只覆盖现有编图入口，没有 DERIVED 发布测试。
- 构建：`cmake --build build --target paleo_ui -j1` 退出码 0，`libpaleo_ui.a` 已链接。并行编译时未改动的 `folderconfirm.cpp` 曾触发 GCC 16 内部错误，单线程重试后消失。

## 图层挂钩 — 单因素、综合编图、验证

- 不重建图层树，不按地层改组。子组 `04_SingleFactor/Contours` 与 `04_SingleFactor/Cartographic` 跟随根组出现在单因素页和综合编图页。
- `PaleoMainWindow::revealDeclaredLayer`：等值线、融合栅格、等厚链、相多边形成功后实例化并勾选声明图层。验证定位先勾选问题行的清单图层，再交给三方定位缩放。
- 制图图层 id 约定为 `cartographic.<层位>.<因素>`。融合清单只收组名恰好是 `04_SingleFactor` 的分析栅格。`fuseFactors` 与 `deriveFaciesPolygons` 拒绝制图图层。
- 命令：`ctest --test-dir build -R '^tst_(singlefactor_contract|panels|workflows|layerplatform)$' --output-on-failure`；`python3 tools/check_layering.py --strict`
- 结果：四个测试退出码 0。`tst_panels` 在补上验证按钮的事件等待后通过。严格分层通过。
- 未完成：这些挂钩还没有新的本地方向场或制图工作场文件。P0 发布链和 P1 工作场仍未做。

## 发布链 — 本地方向分析场与制图工作场

- 提交：`d7bf6dc887c0b6a7a071174564af814946a7065e`。注册 `paleo:paleo_local_direction_idw` 与 `paleo:paleo_cartographic_work`。缺省 method 仍走 `paleo:paleo_constraint_idw`。
- 分析场声明 `factor.<层位>.<因素>`，组恰好是 `04_SingleFactor`。旁路是支撑标记和 QC，不另建图层。制图工作场声明 `cartographic.<层位>.<因素>`，组 `04_SingleFactor/Cartographic`。成功声明后壳用 `revealDeclaredLayer` 勾选该层。
- 没有单独成图多边形时，输出范围矩形写入成图域。空域不能进入网格求值。
- 命令：

```text
cmake --build build -j2 --target tst_factorworkflow paleo_ui
ctest --test-dir build -R '^tst_factorworkflow$' --output-on-failure
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
git diff --check
```

- 结果：第一次测试失败，报「成图域为空」。补上范围矩形后 `tst_factorworkflow` 退出码 0，6.17 秒。严格分层输出「检查通过：无层违规。」i18n 与 UI 不变量通过。`git diff --check` 无输出。
- 证据：EPSG:3857 井点发布分析场，`value_source=analysis`，参数指纹为 64 位十六进制，有限像元大于 0，支撑栅格没有 nodata 标记，其 SHA-256 与目录记录一致。之后追加 `contour_stop` 与无 `params_json` 的 `hard_barrier`，再生成级别 2/4/6 的工作场。分析场 SHA-256 不变。工作场 `value_source=cartographic_work`。`hard_barrier` 记为 `unknown_type`。`fuseFactors` 拒绝该图层，文案含「解释性制图」。EPSG:4326 在 `localGrid=true` 时仍拒绝，文案含「经纬度必须先投影」，且不声明图层。无坐标系、未开局部网时拒绝。打开局部网后 `poro` 成功，`crs_mode=local_engineering`。`method=kriging` 拒绝且不声明。
- 当时未完成：单因素页参数区、异步任务、产品等值线固定级别、`addConstraint` 仍走旧 `append()`、真工区、性能、全量 ctest、clang-tidy、PR。这些项的后续证据见下一节，不把本节 retroactively 记成通过。

## R4 — 页面、异步发布、固定级别与解释性等值线

- 目标基线：`eaaf46def5ce20ed064e73c80853e67667b9407e`。参考 SHA：`27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`。
- 本轮代码提交：`2e293d57f965956a54f2af8a894f3b331844515a`。账本在随后的 docs 提交里指向这个 sha。
- 变更：单因素页增加方法、覆盖、高级参数、等值线来源和逐线参数保存，已知语义走 `appendExtended`。`local_direction_idw` 在 `PaleoTaskService` 上计算，只发布代数匹配的一代。固定级别和分析等值线仍是 `contours.<层位>.<因素>`。解释性等值线是 `cartographic.<层位>.<因素>.contours`，组 `04_SingleFactor/Cartographic`，`value_source=cartographic_work`。严格模式下肩部仍穿线则不发布。分析栅格字节、SHA 和版本不随等值线级别改变。
- 命令：

```text
cd .worktrees/single-factor-native/build
QT_QPA_PLATFORM=offscreen ctest --output-on-failure -j8
QT_QPA_PLATFORM=offscreen ctest --output-on-failure -R '^tst_startup_trace$' -j1
cd .worktrees/single-factor-native && python3 tools/check_tidy.py
```

更早、同一未提交树上还单独跑过 `tst_factorworkflow`、`tst_constraintstore`、`tst_mappingpages`、`tst_singlefactor_kernel`、`tst_singlefactor_parity`、`tst_singlefactor_contours`、`tst_singlefactor_perf`，以及 `check_layering.py --strict`、`check_i18n.py`、`check_ui_invariants.py --strict`、`git diff --check`。全量 ctest 覆盖了这些测试的通过结果。

- 结果：全量 181 项里 180 项通过，`tst_startup_trace` 在 `-j8` 下失败（`qgis_init_share_max` 中位 0.2254 > 门 0.2200，样本 0.214、0.225、0.233；注劣化份额 0.644 < 3×健康 0.218）。单独空载重跑退出码 0，11.56 秒。新算法注册在 `qgis_app_ready` 之后，不在该段内。未改门。`check_tidy.py` 对相对 `origin/master` 的 17 个 TU 退出码 0。
- 性能（`tst_singlefactor_perf`，RelWithDebInfo，空载，预热后中位）：S=2004 ms（门 ≤5000），同井同约束把成图域扩到 1024 方格后 scale1024=6885 ms，像元比 3.44（门 ≤6）。L=14334 ms（门 ≤15000；L 的方向线 8 条、软边界 4 条，不拿 L/S 当像元比）。wells200=3129 ms，井数比 1.56（门 ≤3.5）。取消中位 1 ms、最大 2 ms。RSS 增量 15328 KiB（门 ≤512 MiB）。全量 ctest 里同一测试再通过，耗时 173.08 秒。GUI 事件循环 p95 和等值线 20 秒预算没有测。
- Oracle 对账：O9 的常量场、斜面残差、nodata 孔、开放端、圆锥和非法级别在 `tst_singlefactor_contours` 通过（全量中 0.30 秒）。O10 的穿线绕行、远线不改场、级别变更不改分析 SHA、双血缘和融合拒绝在 `tst_factorworkflow` 通过；肩部斜线未解决穿线被严格模式拒绝，文案含「未解决穿线」。O6 的类型化参数重开在 `tst_constraintstore` 通过。O8 的分析场与制图场声明沿用上一轮，本轮补了等值线身份。O1/O2 点查询对拍仍通过。O7 只有世代不匹配时丢弃发布这一条，没有文件写入、GDAL flush、目录提交、清单声明和进程杀死的故障注入。O11 的离屏页面测试点了方法、等值线来源和忙碌态；两套主题、窄 dock、高 DPI 的真人点击没有做。解释性等值线和等间距等值线仍在界面线程上生成。O12 未做：本轮没有设置 `PALEO_REAL_PROJECT_AREA`。已知只读工区井点属性只有 `coordinate_status`、`id`、`name`，没有数值井点字段。测试在环境未设置时 QSKIP，跳过不是通过。没有编造井值。
- 兼容：缺省 method 仍是 `paleo:paleo_constraint_idw`。旧 `break_line` 仍是硬屏障。无 `params_json` 的未知 type 列出为 `unknown_type`，不进凸包。`cartographic_work` 不能进融合、相带分类和厚度统计。P2 已在 `TODOS.md` 顶部，本轮不实现。
- 分支比 `origin/master` 多上述提交，并落后 1 个提交 `4f35600`（`tst_ui_blocking` 预热）。按方案不合并 master。

## 等值线离界面线程、故障注入与 O13 界面/提线门

- 基线：`origin/master` `56d643c5b394898bdf4bc0497c970e9884b0f5b2`（PR #109 已合并）。本轮代码提交：`38e4ed29f51a8a5feb1a37d8694b73b245ab42e7`。账本在随后的 docs 提交里指向这个 sha。
- 变更：等间距等值线和解释性等值线拆成 prepare / compute / publish。有 `PaleoTaskService` 时，界面在任务里计算，只在 finished 回调里发布。`generateCartographicWork` 仍同步。校验失败的 prepare 不增加发布代数。目录没有 `removeVersion`，成功提交后的文件不删。
- 命令（worktree `.worktrees/single-factor-native`，Ninja RelWithDebInfo，`-j2` 构建，测试 `QT_QPA_PLATFORM=offscreen`，`-j1`）：

```text
cmake --build build -j2 --target tst_factorworkflow tst_singlefactor_asynccontour tst_singlefactor_faults tst_singlefactor_contourbudget tst_singlefactor_guilatency
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure -V -j1 -R '^(tst_factorworkflow|tst_singlefactor_asynccontour|tst_singlefactor_faults|tst_singlefactor_contourbudget|tst_singlefactor_guilatency)$'
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
git diff --check
python3 tools/check_tidy.py
```

- 结果：构建退出码 0。五个测试退出码 0。`tst_singlefactor_contourbudget` 3 通过、0 跳过：预热 1 次后 3 次中位 66 ms、最大 67 ms，512²、10 级、8 条约束，`modified=14408`，门 ≤20000 ms。`tst_singlefactor_guilatency` 3 通过、0 跳过：与 `tst_singlefactor_perf` 相同的 S/L 负载各跑 1 次，10 ms 计时。S p95=11 ms、maxGap=11 ms、wall=2103 ms、gaps=209。L p95=11 ms、maxGap=11 ms、wall=14618 ms、gaps=1460。门 ≤100 ms。这不是分析耗时的 5 次中位。`tst_singlefactor_faults` 6 通过、0 跳过：无法识别的栅格不声明等值线；制品目录只读时文案含「无法创建派生产物目录」，原因素图层还在，没有新等值线图层；清单只读时声明失败且没有新图层；没有 `addVersion` 的孤立 tif 重开后不是版本也不是声明图层，文件仍留在磁盘上。`tst_singlefactor_asynccontour` 5 通过、0 跳过：固定级别发布、过期代数丢弃、解释性穿线后分析 SHA 不变。`tst_factorworkflow` 18 通过、1 跳过。跳过文案是 `PALEO_REAL_PROJECT_AREA is unset. This skip is not an O12 pass.`。严格分层、i18n、UI 不变量和 `git diff --check` 通过。`check_tidy.py` 对本分支相对 `origin/master` 的 2 个 TU（`paleomainwindow_attach.cpp`、`workflows.cpp`）退出码 0。
- 真工区复核：`PALEO_REAL_PROJECT_AREA` 未设置。`/home/kevin/projects/paleo_project/data/project_area/artifacts/layers/wells.geojson` 有 20 个点，属性只有 `coordinate_status`、`id`、`name`，全是字符串。没有编造井值。
- 没有重跑 `tst_singlefactor_perf`，也没有重跑全量 ctest。上一轮空载中位仍是 S=2004 ms、scale1024=6885 ms、L=14334 ms、wells200=3129 ms、像元比 3.44、井数比 1.56、取消中位 1 ms、最大 2 ms、RSS 增量 15328 KiB。本轮 GUI 测试里 L 的单次任务墙钟是 14618 ms，口径不同，不替换那组中位。

## 分析等值线发布核对分析场字节

- 代码提交：`8dff163a1d3d272e9adecfd981780fcde5e4ba62`。账本在随后的 docs 提交里指向这个 sha。
- `prepareAnalysisContourJob` 在增加发布代数之前记下分析栅格 SHA。`publishAnalysisContourJob` 在 stage 前和 commit 前再算一次。不一致则删除临时等值线，不声明 `contours.<层位>.<因素>`，文案是「分析场在等值线期间被改写，丢弃这次等值线」。成功发布的 extra 带 `analysis_sha256`。哈希失败不增加代数。
- 命令：

```text
cmake --build build -j2 --target tst_singlefactor_asynccontour tst_factorworkflow
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure -V -j1 -R '^(tst_singlefactor_asynccontour|tst_factorworkflow)$'
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
git diff --check
python3 tools/check_tidy.py
```

- 结果：构建退出码 0。`rewrittenAnalysisDropsContourPublish` 通过：改写栅格后发布失败，`contoursGenerated` 为 0，没有 `contours.T1.sandthick`。`tst_singlefactor_asynccontour` 6 通过、0 跳过。`tst_factorworkflow` 18 通过、1 跳过，跳过仍是 O12。严格分层、i18n、UI 不变量、`git diff --check` 通过。`check_tidy.py` 2 个 TU 退出码 0。

## 发布核对因子图层源，解释性计算离开 Processing

- 代码提交：`587cd797fae264840f91a9334d3e691462ef7896`。账本在随后的 docs 提交里指向这个 sha。
- `publishAnalysisContourJob` 和 `publishInterpretiveContourJob` 在原有 SHA 核对之外，再解析 `factorLayerId` 的当前声明路径。路径与任务里的栅格不一致时丢弃，包括字节相同的新文件。分析等值线文案仍是「分析场在等值线期间被改写，丢弃这次等值线」。解释性文案仍是「分析场在制图期间被改写，丢弃工作场」。不声明等值线，也不声明制图工作场。
- `declareFactorResult` 和 `runConstraintIDW` 在真正 `declare` 之前增加发布代数。`runConstraintIDW` 改的是 `factor.<层位>.idw`。等厚、距离和没有 method 的生成走 `declareFactorResult`，改的是 `factor.<层位>.<因素>`。
- 非本地方向的 `generateFactor` 和旧插值请求在单因素任务进行中拒绝，文案是「已有单因素计算在进行」。插值按钮忙碌时禁用，提示「正在计算，可取消」。
- 解释性准备在调用线程打开分析栅格取 CRS，并解析约束线。计算线程把分析栅格复制到 `paleo-sf-carto-<uuid>`，只跑 GDAL 提线和制图核，不构造 `QgsMapLayer`，不调用 Processing。同步算法 `paleo:paleo_cartographic_work` 仍走 Processing，数值写入与计算线程共用 `writeCartographicWorkFile`。
- 命令：

```text
cmake --build /home/kevin/projects/paleo_workstation/.worktrees/single-factor-publish-guard/build -j2 --target tst_singlefactor_asynccontour tst_factorworkflow tst_mappingpages
cd /home/kevin/projects/paleo_workstation/.worktrees/single-factor-publish-guard/build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure -j1 -R '^(tst_singlefactor_asynccontour|tst_factorworkflow|tst_mappingpages)$'
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
git diff --check
python3 tools/check_tidy.py --build-dir build
```

- 结果：构建退出码 0。ctest 三个目标 100%。`tst_singlefactor_asynccontour` 9 通过、0 跳过，其中包括 `retargetedFactorDropsAnalysisContourPublish`、`retargetedFactorDropsInterpretiveContourPublish`、`interpretiveComputeOnWorkerThreadPublishes`。`tst_factorworkflow` 18 通过、1 跳过，跳过文案仍是 `PALEO_REAL_PROJECT_AREA is unset. This skip is not an O12 pass.`。`tst_mappingpages` 里 `FactorPageTests` 13 通过、0 跳过；`PredictPageTests` 12 通过、1 跳过（`build lacks ORT`，原有）；`ComposePageTests` 9 通过、0 跳过。严格分层、i18n、UI 不变量和 `git diff --check` 通过。`check_tidy.py` 5 个 TU 退出码 0：`localdirectionalgorithm.cpp`、`constraintpage.cpp`、`paleomainwindow_attach.cpp`、`paleomainwindow_workbench.cpp`、`workflows.cpp`。
- 没有重跑 `tst_singlefactor_perf`、`tst_singlefactor_contourbudget`、`tst_singlefactor_guilatency` 和全量 ctest。上一轮空载中位和 GUI p95 不改写。
- `addConstraint` 仍不增加发布代数。忙碌时绘制按钮仍可用。约束线在准备阶段已解析；准备之后再改约束不在这次发布核对里。

## 未完成项

- O7 覆盖了 GDAL 打开失败、制品目录不可写、清单声明失败、孤立文件不进产品、代数丢弃、分析栅格改字节后不发布，以及因子图层改指向到字节相同的新文件后分析和解释性发布都拒绝。没有注入 GDAL flush 中途失败，没有杀掉正在跑的进程，孤立文件没有自动回收。关停和单调进度没有单独断言。
- O11：解释性计算在 `QThread` 上跑过 `computeInterpretiveContourJob`，再回到测试线程发布。计算线程不打开图层、不调用 Processing。任务服务为空时仍走同步包装，`tst_factorworkflow` 测的是这条同步包装。双主题、窄 dock、高 DPI 没有真人点击。
- O12 跳过不是通过。井点 GeoJSON 没有数值字段。
- O13 的分析中位、GUI p95 和 512² 提线/绕行中位沿用上一节已经记下的数。`587cd79` 没有重测。
- O3/O4 没有单独的 `tst_singlefactor_constraints`。硬屏障仍是 `grid_connectivity_v1`，不对齐参考工程 `FaultPathMetric`。
- `addConstraint` 不增加发布代数。忙碌时绘制按钮仍可用。约束线只在准备阶段解析，准备之后再改约束不在发布核对里。
- P2 见 `TODOS.md`。不把本轮记成方案全文通过。
