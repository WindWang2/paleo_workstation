# 方向02 ledger：测井、井轨迹、连井剖面与现有交会分析闭环

- 分支：`goal7/well-sections-crossplot-20261014`
- 工作树：`../paleo-wt-goal7-well-sections`
- base SHA：`b0520429c21f858a1c053b876f511680088564a9`（== origin/master，2026-10-14 复核）
- 唯一写 owner：主 Agent（本 ledger）；子代理只读侦察 ≤3 并发 + 最终独立 review 1 个。

## R0 基线（2026-10-14）

### 环境事实

- 仓库状态：master 干净；4 个兄弟 worktree（mapping/data-io/seismic/architecture，2026-09-30/10-01 建于
  别的分支）不与本分支冲突；无同名 goal7 分支。
- 网络：`~/.gitconfig` 的 `http.https://github.com/.proxy=socks5h://127.0.0.1:7890` 已死（代理进程不在）。
  gh CLI 直连正常；`git fetch/push` 需按命令覆盖 `http.https://github.com/.proxy=`。
- **构建环境破损（本轮已修复）**：Arch 系统 Qt 已升 6.12.0，兄弟 worktree 时代的 wp2-qt-addon SerialPort
  （6.11.2 私有 ABI）与系统 Qt 混链 → 测试目标链接失败（`QIODevicePrivate@Qt_6_PRIVATE_API` 未定义）。
  硬链 seismic 兄弟的旧闭包（Qt 6.10.2）也不完整（缺 qt6-5compat）。**处置**：按当前
  `vendor/deb-closure.lock` 全量拉取 vendored 闭包（glibc 2.44 ≥ 2.43 达标），所有 Qt6 组件/工具包
  用 `cmake -C /home/kevin/project/paleo-goal7-build-init.cmake` 显式钉进闭包（zsh 不做 $FLAGS 词拆分，
  命令行 -D 串会静默失效——教训入账）。
- 测试 runner：ctest 无 PATH 项，用
  `/home/kevin/.local/opt/cmake-3.30.5-linux-x86_64/bin/ctest`；`ninja` 用户别名含 `-j40`，一律
  `cmake --build ... -j4` 绕过。
- 全局互斥：`/tmp/paleo_workstation_global_build_j4.lock`（configure/build/ctest/fetch 全走）。

### 指纹排除表（执行时核实，禁止本方向触碰）

PR #437（80 文件只读；含 `src/workflow/wellpredictionreview.cpp`——虽然匹配本方向 `*well*`
白名单模式，但保护区优先）：完整清单以 `gh pr view 437 --json files` 为准，已核对 2026-10-14。

Open Issues（#352–#421，70 项）中与本方向写入范围相交的缺陷指纹：

| Issue | 文件:行 | 缺陷 | 处置 |
|---|---|---|---|
| #412 | wellcompositecanvas.cpp:618-622 + compositebody.cpp:324-331 | Esc 取消拖拽不回深度 | 禁修，留给 Issue Agent |
| #412 | trackcurves.cpp:210-258 + wellcompositemodel.h:47 | depths/values 尺寸守卫缺失 | 禁修 |
| #412 | editstack.cpp:25-30,70-78 | savedIndex 漂移误标 clean | 禁修 |
| #412 | derivedsink.cpp:69-78 | livePanels 死条目 | 禁修 |
| #412 | wellsectionscene.cpp:65-67 | 单样本 trace NaN | 禁修 |
| #368 | exportengine.cpp CurvePenGuard | 导出线宽恢复错 track | 禁修（含 exportengine 的分页/SVG 比例尺问题也一并避让该文件） |
| #389 | petrophyscomputeservice.cpp loadWellCurves | 驱动 LAS 非单调不校验 | 禁修 |
| #407 | tst_wellfacies.cpp:691 | percent 竞态 | 禁修 |
| #415 | sectionworkbench.cpp + projectlayerrefresh.cpp | 合并请求后多跑一轮 | 禁修（且不在白名单） |
| #418 | app/crossplotcontroller.cpp:24-25 | bottomTabs null 泄漏 | 禁修（src/app 不在白名单） |
| #387 | 井文本/层位散点导入超限 | 导入无 guard | 禁修（io 侧，方向01） |

### 领域底账（S1/S2/S3 侦察 + 主 Agent 复核）

数据流：LAS → WellLogSet/LasCache → 三条消费线（PetroPhysTaskService 合并计算、
WellSectionWorkflow.loadCurveBodies 剖面曲线、SectionWorkbench 时深叠加）；井斜 →
WellDeviationSurvey（最小曲率）→ wellsection::Well tvdOf/mdOf 显示期换算；井数据 MD 为真源，
显示域切换不改数据。

已核实的候选缺口（主 Agent 已亲核 G1/G6，其余以子代理 file:line 证据为准，改动前逐一复核）：

- **G1** wellsectionworkflow.cpp:354-367 单位别名表只认 FT/F/M：METER/METRES/FEET/空单位 → 整文件
  曲线丢弃；而 wellnumeric.h:57-66 词表收 M/METER/METRES/FT/F/FOOT/FEET。行为分裂，反例可构造。
- **G6** petrophys.cpp:170-205 curveStats one-pass 方差消碳：{1e9,1e9+1,1e9+2} → stddev=0（真值 1）。
  Welford 替换，tst_petrophys.cpp 已有统计测试段可扩。
- **G2** wellcompositemodel.cpp:21 lower_bound 假升序无守卫；io 按文件序 append（io 侧不可写，
  修 domain 消毒点）。
- **G3/G8** welltopsedit.cpp validate/mergeDiff 键不含井名 → 跨井行集误报（现调用方预过滤，潜伏）。
- 交会图通道多选被 refreshSources 重建清空（crossplotpanel.cpp:456-463）。
- CorrelationOverlay 仅 relayout 时 setGeometry，MultiWellView 无 resizeEvent（multiwellview.cpp:164）。
- applySpecsIncrementally 不恢复曲线配置（paneltracks.cpp:287-309）；loadWidthSet 零生产调用。
- wellsectionworkflow 岩性/相 JSON 每请求重读（:443-490,645-707,926-943）。
- 比例尺读数 resize 后陈旧（wellcompositecanvas.cpp:588-592）。
- 交会图 <10 万点逐点 drawEllipse + 套索拖动全量重绘（crossplotpanel.cpp:124-131,157-166）。

优先级排序：数值正确性（G6）→ 行为完整性（G1）→ 状态闭环（选择保留/恢复）→ UI 几何（overlay/
读数）→ 性能（解析缓存/增量绘制）。每轮先写可失败测试（能先红后绿的尽力），再改生产代码。

## 轮次账本

### 轮次 0（2026-10-14）：环境 + 基线
- fetch origin 失败（死代理）但 origin/master 已在基线值；gh API 盘点完成。
- worktree/分支建立；wp2-prefix 配置下 paleo target 构建成功（exit 0），但 tst 目标链接失败
  （SerialPort 6.11.2 addon × 系统 Qt 6.12 私有 ABI）。
- **环境最终方案（复合 vendored deb 闭包栈）**：Arch 无 apt/dpkg-deb，自写
  `/home/kevin/project/paleo-goal7-fetch-debs-arch.sh` 忠实复刻 fetch-deps.sh 的下载/校验/
  解包段（ar+tar、SHA256、snapshot 回退、curl --retry）；snapshot.ubuntu.com 间歇 5xx 导致
  全量拉取过慢（164/864）后改走**硬链复用并行 seismic-ai 方向已实证的闭包**（cp -al，对源
  树只读）+ qt6-5compat 头显式补齐。所有 Qt6 组件/工具包/GDAL/GEOS 用
  `/home/kevin/project/paleo-goal7-build-init.cmake`（cmake -C）显式钉进闭包。
  **构建/测试命令必须前置** `LD_LIBRARY_PATH=<worktree>/vendor/prefix/usr/lib/x86_64-linux-gnu:
  <worktree>/vendor/prefix/usr/lib`——否则 vendored lrelease/moc 运行时混载系统 libQt6Core
  （GNU_PROPERTY_1_NEEDED_INDIRECT_EXTERN_ACCESS 报错）。教训：`cmd | tail` 管道会吞真实
  退出码（本轮一度把失败构建误读为成功）；zsh 不做 $FLAGS 词拆分，多 -D 必须走 cmake -C。
- 侦察 3 子代理完成（S1 数据层 / S2 剖面井道 GUI / S3 交会图 workflow），候选缺口入 baseline。

### 轮次 1（2026-10-14）：数值可信度——curveStats 方差消碳（G6）
- 缺陷：petrophys.cpp curveStats 用 one-pass `sumSq − n·mean²`，大基数小差值消碳
  （{1e9, 1e9+1, 1e9+2} → stddev=0，真值 1）。QC 统计失真，无既有 Issue 指纹。
- 修复：Welford 累推（mean/m2），NaN 跳过语义不变，min/max/nullRate 不受影响。
- 测试：tst_petrophys testCurveStats 增加大基数断言（mean 1e-3、stddev=1 within 1e-6）。
- 红绿证据（独立程序 /tmp/welford_check.cpp 对拍新旧算法）：
  `OLD: mean=1000000001 stddev=0`（断言必红）/ `NEW: mean=1000000001 stddev=1`（绿）。
- 状态：代码就位，待构建验证。

### 轮次 2（2026-10-14）：行为完整性——剖面单位别名表统一（G1）
- 缺陷：wellsectionworkflow.cpp loadCurveBodies 只认 FT/F/M；METER/METERS/METRE/METRES/FOOT/FEET
  的 LAS 整文件丢弃。与 wellnumeric.h 权威词表、sectionworkbench/PetroPhysTaskService 接受面分裂。
- 修复：改用 `paleo::wellnumeric::depthScaleToMetres()`（空单位/未知词表仍拒绝——领域契约「不从
  空单位推断」保持不变，只扩大规范别名覆盖）。
- 测试：tst_wellsection_workflow 夹具加 METER 文件（well-1 DEN 曲线），断言装载 + 深度按米 +
  无「单位未知」告警；availableMnemonics 期望集更新（+DEN）。
- 状态：代码就位，待构建验证。

### 轮次 3（2026-10-14）：状态闭环——交会图通道选择跨重建保留
- 缺陷：refreshSources 每次切回底栏 tab / catalog 变更（150ms 去抖）重建 QListWidget 且清空选择，
  用户已选通道丢失须重选。
- 修复：crossplotpanel setSources 重建前收集选中 id（Qt::UserRole），同 id 恢复选中。
- 测试：tst_crossplotpanel sourceSelectionSurvivesRebuild（同集重投/收窄/全新列表三态）。
- 状态：代码就位，待构建验证。

### 轮次 4（2026-10-14）：UI 几何——CorrelationOverlay resize 跟随 + 比例尺读数刷新
- 缺陷 A：MultiWellView 无 resizeEvent，overlay（不在布局内）容器 resize 后几何陈旧 →
  correlation 线裁剪/错位。修复：resizeEvent 内 setGeometry(rect())；overlay 设 objectName。
- 缺陷 B：wellcompositecanvas resizeEvent 自适应分支判据用 m_scaleRatio（已被 setScaleRatio
  换成计算串 "1:N"）→ 分支死代码，resize 后 ppm 变而道头/图例比例尺文本陈旧。
  修复：判据改 m_baseScaleRatio，重算后 refreshScaleTracks + 发 scaleRatioChanged
  （消费方只更新文本/图例，无几何反馈，不自激）。zoom 档位保持不重置。
- 测试：tst_wellcomposite_multiwell testOverlayGeometryFollowsResize；
  tst_wellcomposite_depth testAdaptiveScaleRatioRefreshOnResize（ppm 减半 + 信号 + 文本一致）。
- 状态：代码就位，待构建验证。

### 轮次 5（2026-10-14）：状态闭环——wellcomposite 会话曲线配置恢复
- 缺陷：TrackConfig 曲线集/量程写入 spec.params 并持久化，但 applySpecsIncrementally 恢复
  width/visible/print/grid 而不注入曲线 → 重开回退到按助记名分组的默认道。
- 修复：applySpecsIncrementally 对 CurveTrack 且 spec.curveNames() 非空时调
  TrackOps::injectCurvesFromWellData（restoreSessionState 在 m_data 填充后调用，名字可解析）。
- 测试：tst_wellcomposite_framework testSessionCurveConfigRestore（DEN+RT 注入 + DEN 量程覆盖）。
- 状态：代码就位，待构建验证。

### 轮次 6（2026-10-14）：性能——岩性/相解析跨请求缓存
- 缺陷：CatalogWellLithologyProvider 每请求新建（m_parsed 随实例销毁）；attachFaciesSegments
  每请求整文件重读 JSON。换曲线/换井的重复请求重复读盘 + 重复告警。
- 修复：WellSectionWorkflow 持有 `mutable QHash<QString, InterpParseCacheEntry> m_interpParseCache`
  （键 = resolvedVersionPath + '|' + 文件 mtime —— 内容替换自然失效；仅 GUI 线程计划步触达，
  无锁；loadCurveBodies 任务线程不触达）。provider parse() 与 attachFaciesSegments 共用；
  解析失败同样缓存（不重复告警）。
- 测试：tst_wellsection_workflow lithoSegmentsAttached / faciesSegmentsAttached 各加第二次请求
  断言（结果一致 + 岩性无效段告警不重复）。
- 状态：代码就位，待构建验证。

### 轮次 7（2026-10-14）：验证闭环 + 独立 review 采纳

**构建与测试实测（复合 vendored 栈 + offscreen + QT_PLUGIN_PATH/QGIS_PREFIX_PATH/LD_LIBRARY_PATH）：**
- 全量构建（2042 步）`BUILD_EXIT=0`；review 修复后增量重建（164 步）`INC_EXIT=0`。
- 六个微循环目标 ctest：7/7 通过（tst_petrophys 16/16、tst_crossplotpanel 7/7、
  tst_wellsection_workflow 24/24、tst_wellcomposite_multiwell 11/11、
  tst_wellcomposite_depth 20/20、tst_wellcomposite_framework 19/19）。
- 领域子集（50 项）两遍全绿；全套 384 项一遍：377 过 / 7 失败。
- 7 个失败（singlefactor×5/catalog/algorithm_harness）与本方向文件零交集；
  tst_catalog 直跑 44/0 全绿、tst_singlefactor_contourbudget 失败在夹具 writeGrid
  （IO 层）——定性为 -j4 并行负载波动 + 环境夹具问题，串行复跑结果见 PR 描述。
  纯净基线复核：这些测试不触及 curveStats/loadCurveBodies/setSources/
  applySpecsIncrementally/resizeEvent/interpCache 任何改动面。

**独立 review（S5，general-purpose 子代理，非实现者）：**
- A 资源泄漏/UAF、B 线程归属、C Qt 信号、D 缓存失效、E 行为回归、F 数值、
  G 测试质量、H 层合规、I 保护区/指纹 九项全 PASS；**blocker = 0**。
- 采纳建议：S5-1 读失败不再缓存钉死（保持旧的重试语义）；S5-2 缓存上限 64 条
  （超限先腾位再插，防逐出刚插入的键）；S5-3 request()/lithologiesFor() 加
  Q_ASSERT 线程归属（RelWithDebInfo 下编译为空，纯防线）。
- 登记：S5-4 本 diff 使 #412 在 wellcompositecanvas.cpp 的指纹行号漂移约 +6 行
  （Esc 拖拽旧 618-622 → 约 624-628，Issue Agent 定位时注意）；
  S5-6 `WellLogSet::readCurveTvd`（services/welllogset.cpp:304-314）仍是窄词表
  且空单位按 M 接受（与「不从空单位推断」相反），当前无生产调用方——留给
  Issue Agent 统一到 wellnumeric 词表。
- 不采纳（无害、保持最小 diff）：S5-7 自适应 resize 双发 viewportChanged /
  setSources 重复 id 双选。

### 留给 Issue Agent / 下一阶段的发现（本方向不改，附证据）

与本方向写入范围相交但属既有 Issue 指纹的（全部禁修，仅登记复核）：
- #412 wellcomposite 五项（Esc 拖拽/尺寸守卫/editstack 漂移/derivedsink 死条目/单样本 NaN）。
- #368 exportengine CurvePenGuard（本方向连 exportengine 的分页漏带/SVG 比例尺两个邻接缺陷
  也一并避让，S2 报告有 file:line 证据可复用）。
- #389 petrophyscomputeservice 驱动 LAS 非单调（`depthUsable()` 只校验次级）。

本方向核实过但本轮不做（无 Issue 占用、优先级让位于已交付六轮）：
- G2 wellcompositemodel valueAtDepth 假升序（io/wellcompositexml.cpp:121-146 按文件序 append，
  0 处 sort；domain 侧只读函数难廉价修乱序，正确修法在 io 装载层——方向01 领地）。
- G3/G8 welltopsedit validate/mergeDiff 键不含井名（现调用方预过滤故潜伏）。
- 交会图 <10 万点逐点 drawEllipse + 套索拖动全量重绘（crossplotpanel.cpp:124-166）。
- 栅格交会内存无上限（crossplotsamples.cpp:218-252，10000² GeoTIFF → 数 GB）。
- CurveTrack::paintBody 无 LOD（decimateForLod 死代码）+ hover 全量重绘。
- loadWidthSet 死代码（D1.4 宽度记忆只写不读）。
- S3 报告的剖面计划步 GUI 线程同步 IO（wellsectionworkflow.cpp:198-335）——涉任务化架构，
  需要方向07 任务 API 配合，建议下一 PR。


