# 方向 98 ledger — wellsection 家族深化（goal/wellsection-deep-20261010）

基线：origin/master `f474069b`（提示词按 32851a1e 口径，master 已前进；
scene 实测 1,921 行而非 1,762，任务面不变）。worktree：
`.worktrees/wellsection-deep`，分支 `goal/wellsection-deep-20261010`。
（worktree 目录复用自方向 26——旧 ledger 已移出归档，方向 26 成果早随
PR #273 系列入库。）

## 环境事实（R0 实测）

- `paleo-dev.ps1` 需 PowerShell 7（`pwsh`）；Windows PowerShell 5.1 解析失败。
- PATH 默认 cmake 3.27 < 3.28 门槛；前置 pip cmake 4.4.2
  （`~/AppData/Roaming/Python/Python313/site-packages/cmake/data/bin`）后过。
- 裸 `cmake --build` 缺 MSVC INCLUDE 环境（type_traits 找不到）——构建/测试
  一律走 `paleo-dev.ps1` 包装。
- **无 vendored onnxruntime 时 aiassistworkflow 必链接失败**（runTileInference/
  suggestHorizonTracking 无 stub 无守卫，仅 ORT 分支编译 horizonsuggest.cpp）→
  tst_background_open/tst_singlefactor_* 等全红且不在方向 81 环境债名单，
  `paleo-dev.ps1 test` 的 env_redset 门直接不放行。处置：从同机 mkproject-spread
  worktree 复制钉哈希 vendor/onnxruntime（同源闭包，免 bootstrap 下载），
  touch CMakeLists 重配后 ORT 上线（cache 口径与 mkproject-spread 一致）。
- tst_deviation 对拍测试首轮发现自身缺陷：单站 md=0 的退化表 totalDepth=0
  → 线性网格步长 0 → 无限追加至 bad_alloc（39s 后 C++ 异常）。已加
  `total > 0` 守卫（站点/边界采样仍覆盖该面）。

## 定案（无人值守决策，逐条记据）

1. **scene 地震缝行级 LUT 绕开：删**。治本后逐行直调 tvdToMd 为精确反解
   （每行 O(段数+100·log 站数)），LUT 的亚米~米级内插误差与 ~45 行复杂度
   （knot 建立/尾块分数）一并删除。理由：精确性优先，性能已非瓶颈。
2. **fence spacing 存储契约：sqlite 可空列（NULL=未存档）**。NOT NULL
   DEFAULT 'EQUAL' 无法区分「旧档缺列」与「显式等距」——目标 ② 要求旧文件
   缺字段回落 + 如实标注，可空列是唯一诚实口径。depth_domain 列沿方向 69
   既有契约（NOT NULL DEFAULT 'MD'）不动。
3. **fence rebuild 加 persist 参数（构造/换库 = restore-only）**。否则旧档
   缺 spacing 的纯读回会立即触发首写（Equal 落库）——回落标注被抹掉、版本
   无用户编辑也推进，违背「版本只随真实编辑推进」精神。
4. **多解释选择语义：不造合并算法**（按目标定位）。显式选择 well→asset
   表（workflow 内存态 + store `well_litho_selection` 表持久化），默认仲裁
   不变（最新版本号）；多份候选时消费/落选对象点名（告警 + provenance 后缀
   「· <displayName>（vN）」——TODOS:75 落选无提示收口）；单份候选零改动
   （对拍不变）。选择失效（资产删链）→ 回落默认 + 告警。
5. **fence 剖面内不放解释来源入口**。选择态语义面在主剖面 workflow + 工程
   库（fence 各剖面独立 workflow 实例，选择态不共享——栅内接半套会成死 UI/
   假联动）；fence 面板 `setLithoSourceButtonVisible(false)` 隐藏。递延：
   fence 消费侧跟随主选择（需选择态跨 workflow 共享，另立方向）。
6. **主剖面（default 节）与 fence 节同用 spacing 字段**：主面板
   spacingModeChanged 挂 saveSectionEdits 钩子（域同款）；恢复路径
   `rec.spacing` 有值才回读（缺档保持面板现状）。
7. **perf 比率门放 tst_deviation 常规套件**（不打 perf 标签）：比率断言
   （≥15×，实测见下）不吃绝对毫秒，运行 ~百 ms 级，进默认回归面。

## 逐轮记录

| 轮 | 改动 | 验证 | 结果 |
|----|------|------|------|
| R0 | 勘察 + 基线构建 | paleo-dev.ps1 build（改前状态） | cmake 门槛红→PATH 前置后绿；ONNX vendor 缺失补齐 |
| R1 | ① deviationsurvey 治本 + tst_deviation 对拍/比率门 + scene LUT 删除 | 构建 3 轮（QPixmapCache include / 测试 bounded 双参误用 / 对拍 total=0 死循环守卫） | 见 Oracle |
| R2 | ② store spacing 列 + fence 读回/标注 + mainwindow 主节间距 | 构建 + 测试 | 见 Oracle |
| R3 | ③ workflow 仲裁 + panel 按钮 + store 选择表 + mainwindow 对话框 + ②③④ 测试 | 修 hasWarning 编码（QLatin1String→fromUtf8）+ lambda QCOMPARE | 见 Oracle |
| R4 | ⑤ scene 拆分 5 TU + internal.h + cmake | wc 全达标；代码行集合 diff 仅 LUT 删除；highlightColor 补 inline（头文件多 TU） | 见 Oracle |
| R5 | token 例外表 repoint（拆分后两处例外随代码搬新 TU） | ui_tokens/ui_tokens_strict 100% 绿 | ✓ |
| R6 | 全量对照 | R0 19 红 vs R1 16 红：新增红 0；R1 少的 5 红（perf 族）为波动转绿；共同红 14 全为本机既有环境红 | ✓ diff 为空 |

## Oracle 证据（实测，2026-10-10）

1. **tvdToMd 逐值对拍**：tst_deviation 13/13 绿（tvdToMdParityWithLegacy：
   形状库 10 + 种子随机表 8 × pointAt/tvdToMd 网格逐位一致，含 NaN/表前后/
   掉头/最深外）。首轮 total=0 死循环为**测试自身缺陷**（网格步长 0），守卫
   后绿——治本代码未动。
2. **比率门**：tvdtMdSpeedRatioGate **实测 33.6×**（门槛 ≥15×；N=3000 站
   ×150 目标，基准=旧实现逐字拷贝，nsecsElapsed 同进程背靠背，只断比率）。
3. **绕开点处置**：行级 LUT 整体删除（含 knot 建立/尾块分数/内插 ~45 行），
   seismicgap TU 现 mdAtRow 逐行精确反解 w.mdOf()——处置证据 = diff +
   seismicgap.cpp:22-27 注释。
4. **fence round-trip**：tst_wellsection_workflow::sectionStoreSpacingContract
   绿（TVD+Proportional 存读、nullopt=NULL、覆盖+版本推进、Equal↔nullopt
   语义恒定）；tst_wellsection_ui::fenceStoreDomainSpacingRoundTrip 绿
   （旧档缺 spacing：域回读生效+间距回落+标注可见+纯读回不落库版本不动；
   用户动作后落库+标注撤下；同库新实例 round-trip；栅内按钮隐藏断言）。
5. **多解释**：lithoSourceSelectionArbitration 绿——默认对拍不变（v2 泥岩
   200-250 逐值）+ 告警点名消费/落选 + provenance「· 解释2（v2）」；显式选
   v1（细砂岩 100）+ 信号；失效回落默认+告警；清空回默认；store 表 round-trip
   + 恢复消费一致 + 删行幂等；单份候选零改动（无告警、无后缀）；枚举降序。
6. **端到端**：wellFaciesCompletedPublishE2E 绿——本机 HTTP 服务（models+
   predict 同步 completed）驱动 configure→setData→run→completed：缓存写入
   （「结果已保存」）+ publish（「地质成果已保存」）+ DERIVED 资产唯一 +
   per-well interpretation 链接 + 剖面消费（河口坝 1000-1016，provenance
   = welllogfacies 测试微相 v1）。
7. **scene**：wc -l 实测——主 631 ≤800、columns 566 ≤600、seismicgap 397、
   images 243、faults 126、internal.h 17。测试族零改动 ×2 遍：tst_wellsection/
   tst_wellsection_workflow/tst_wellsection_imagetrack/tst_wellfacies/tst_deviation/
   layering/ui_tokens 全绿 ×2（pass1 12 项 + pass2 15 项含 ui_tokens）；唯一红
   tst_wellsection_ui::screenshots 为本机既有环境红（R0 实测同红——/tmp 路径
   QImage::save 失败，与本方向无关；该套件其余 35 函数全绿）。
8. **全量对照**：R0（stash 实测）19 红：officepreview/singlefactor_perf/
   services2/workflows/composeworkflow/manifestproj/fspath/datapreview/mapping/
   startup_trace/perfbudget/crossplot_controller(SEGFAULT)/correlation_async/
   cache_las/cache_segyindex/perf_segyindex/perf_regress/wellcomposite_perf/
   wellsection_ui。R1 16 红（token 例外 repoint 后）：R1⊆R0 **新增红 0**；
   R0 独有 5 红（cache_segyindex/perf_segyindex/perf_regress/perfbudget/
   correlation_async）为 perf 族波动转绿；crossplot_controller R0=SEGFAULT vs
   R1=0xc0000409（同测试既有崩溃，退出码模式漂移）。**对照 R0 红集合 diff
   为空（无新增红）**。env_redset 分类与方向 81 名单一致（wellsection_ui
   qsettings=已修同款既有环境红，perf-host 3 项豁免）。

## 环境债备注（不入方向账）

- tst_wellsection_ui::screenshots：QImage::save 到 "/tmp/..." 在本机 Qt 路径
  归一下失败（目录可写、native probe 可写——疑 Qt 文件名归一层）；R0 同红，
  建议后续方向把截图产物路径改 QTemporaryDir 或入 CTest 资产目录。
- tst_deviation 直跑（不经 paleo-dev.ps1）0xc0000139：wrapper 注入的 DLL
  路径缺失所致，wrapper 下 100% 绿——直跑口径不作数。
