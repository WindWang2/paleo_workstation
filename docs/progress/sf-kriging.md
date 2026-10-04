# 方向41：单因素原生算法深化（克里金接入 / 外委格式读取 / 策略词表）

## 范围

- 分支 `goal/sf-kriging-20261004`，worktree `.worktrees/sf-kriging`，基线
  `origin/master` `24daf8f`。
- 方案/账本：`.goal-loop-ledger-sf-kriging.md`（逐轮命令与证据）。
- 交付：
  1. `algorithms/singlefactor/krigingsurface.*` + `localidw.cpp` 克里金权重分支
     + `geostat/kriging.h` 的 `KrigingSolver`：本地方向插值面出真普通克里金；
  2. `io/ziparchive.*` + `io/sfpkgreader.*` + `io/outsourceworkbook.*` +
     `domain/projectclassifier.cpp` 词表登记：SFPKG 完整读取与外委 XML/XLSX 批量读取；
  3. `domain/singlefactorstrategy.*`：可枚举历史制图策略的参数包词表；
  4. 编排/UI 接线与诚实标签（`method_actual` / `engine_id` / 变差函数 QC）。

## 口径（关键取舍）

### 克里金复用与接入

- 变差函数与克里金求解器**复用方向18 的 geostat 核**，不重建：
  `geostat::KrigingSolver` 是唯一数值入口（去重样本 → 桶格最近 K → 加边 LU），
  `ordinaryKriging`/`ordinaryKrigingAt` 与 singlefactor 面共用同一个 `SolverCore`。
  这样不会出现两套邻域/方程口径。
- 单因素面按**硬屏障分量**各建一个 solver：克里金邻域不跨屏障（与 IDW 分量语义
  一致），成图域/井控标记/无井闭合区沿用同一插值面代码路径。
- `KrigingParams::maxPoints <= 0` = 分量内全部样本（远场平台/均值口径需要它）；
  正数仍封顶 64。
- **精确性口径**：本实现的变差模型取 `γ(0) = 0`（块金是 h→0⁺ 的跳变），
  因此普通克里金对**任意**块金都在采样点精确通过（`λ = eᵢ`、`μ = 0` ⇒
  估值 = 井值、方差 = 0）；块金只体现在井点之间。测试对 `nugget = 0` 与
  `nugget = 0.75` 两种都断言 1e-9 精确通过（`exactAtSamples`）。
- **远场均值只在不相关时才精确**：当所有井对与查询点的 γ 都饱和（纯块金，或
  变程远小于井距）时 OK 权重精确等于 1/n，估值 = 算术均值；变程与井距同量级时
  远场进入「平台」而不是均值。测试按这两种情形分别断言，不把平台写成趋均值。
- 各向异性：显式 `azimuth ≥ 0` 时按方向双拟合 `ratio = clamp(沿/垂, 1, 8)`，
  与 geostat 编排同口径。

### 诚实面（不冒充克里金）

- 有效样本 < 8（变差函数欠定）、拟合失败、零信号（块金+拱高 = 0）、
  全场无一格解出 → **整面回落 IDW**：`resolved.methodActual = "local_direction_idw"`、
  `fallbackReason` 写原因、`issues` 加 `kriging_fallback: ...`、
  `surfaceFallbacks = 1`；回落结果与直接跑 IDW **逐位相同**（测试断言向量相等）。
- 单格没解出（方程奇异/病态，**或半径邻域不足**）→ 该格用**同一参数**的 IDW 权重，计数进 `idwFallbackCells`
  并出 issue；回落后仍无值的格保持 nodata，不计数（不把 nodata 说成回落）。
- 方向线/软边界/井群权重**不参与克里金权重**（v1 语义）：逐条进 `issues`
  （`direction_guide_not_used_by_kriging` 等），不静默忽略。
- 血缘只认实际执行的引擎：QC `algorithm_id` = 实际引擎（克里金或回落后的 IDW），
  `extra.engine_id` = 请求的引擎，`extra.method_actual` = 实际方法。
  UI 下拉新增「克里金（局部方向约束）」与既有「克里金（各向异性）」作用域不同，
  文案与真实算法一一对应。

### 外委格式读取

- ZIP 读面自持（`io/ziparchive.*`，zlib raw inflate），**不落临时文件**、
  不依赖 `QMimeDatabase`：`.sfpkg`/`.xlsx` 都不是 `.zip` 扩展名，
  `QgsZipUtils::isZipFile` 在缺 mime 数据的机器上会把它们判成非 ZIP（实测）。
  ZIP64/加密/超 512 MiB 明确报因。
- `.sfpkg`：manifest 逐字段 + `raw` 原样保留（「完整字段」不靠枚举穷举）、
  9 个 NPZ 数组（含 fortran_order 转置、整数/布尔 dtype）、SHA256 校验、
  禁 pickle。字段级坏数据进 `issues`，包级失败 `ok=false` + `error`。
- 外委工作簿：SpreadsheetML 2003 与 OOXML 两种容器；`ss:Index`/`ss:MergeAcross`
  与 `r="B3"` 列引用都补齐空列；坏行/坏格逐条带行号列因；层段行数值字段的空值、
  `-`、非数值都报因（不静默当 0）。
- 格式识别进分类器词表：`.sfpkg` → `single_factor_package`、`.xlsx` →
  `outsource_workbook`（都是 input）。旧 `.xls` 不给假能力（仍是 unknown），
  `.xml` 的内容嗅探保持既有判据（SpreadsheetML 判为测井类，既有断言不变）。

### 制图策略词表

- 三张表：曲面方法包、提线方法包、制图工作场包。每条包显式声明映射到哪个引擎、
  哪些上游参数本仓**未实现**（`notImplemented`，与 `consumedParameters` 互斥）。
- 未知包 id 返回 `nullptr`——不回退、不猜；UI 未暴露这些包（不做 GUI 全复刻）。

### FaultPathMetric 边界

- 已在（`singlefactor/faultpath.*`，方向18 也在 geostat 侧落地）；解析解断言在
  `tst_singlefactor_faultpath`（绕墙端点折线长度 ±1e-6）。与 `grid_connectivity_v1`
  的分工：后者只做硬屏障连通域隔离、不算绕行距离；两者不混称（`localidw.h`
  头注释 + QC `hard_barrier_model`）。

## 本机命令

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix \
  "-DCMAKE_PREFIX_PATH=C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" \
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
ctest --output-on-failure -j4 -R '^(tst_io_sfpkg|tst_io_outsource|tst_singlefactor_kriging|tst_singlefactor_strategy)$'
python tools/reference/singlefactor/make_outsource_fixtures.py --check   # 9/9 OK（.gitattributes 保证夹具不做换行转换）
python tools/check_layering.py --strict
python tools/check_i18n.py
python tools/check_ui_invariants.py --strict
```

运行时 PATH 需让 `paleo-qgis-deps/Library/bin`（Qt 6.11，QGIS 二进制就是按它编的）
排在 `C:/deps/Qt/6.8.0/msvc2022_64/bin` 之前，否则加载期 `ENTRYPOINT_NOT_FOUND`。

2026-10-04 结果：新测试 4 项全绿（kriging 13 用例 / sfpkg 12 / outsource 11 /
strategy 词表），同批回归（kernel、geostat×4、faultpath、parity、contract、
samples、contourlevels、projectparsers）全绿；三个门禁脚本通过。
本沙箱拒绝对测试子进程的临时目录写入，命中该约束的既有测试失败/崩溃 12 项，
已用 `git stash` 基线对照证实与本方向无关（详见账本「基线对照」）。

## 递延（未做，如实）

- 协同/泛克里金、把方向线/软边界耦合进克里金权重（协克里金/带约束 OK）。
- 变差函数逐硬隔断分量拟合；交互式变差拟合 UI。
- SFPKG 写出、ZIP64、shp 边车几何解析（当前只列条目名）。
- 外委曲线统计（mean/median/min/max + 深度区间）与因素自动发现。
- 制图策略包进 UI、真实工区绝对耗时门、UI 真人点击（双主题/窄 dock/高 DPI）。
- `--check` 还没有 ctest 门禁（当前是文档里的手写命令）。
- 本机 CMake/Ninja 不记头文件依赖（`deps = gcc`）：改头文件后必须删
  `build/CMakeFiles/**/*.obj` 再整编，否则会出现旧布局对象混链的假失败。
