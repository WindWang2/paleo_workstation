# Ledger — 方向 38：连井剖面 TVD 域 + 真实井距 + 解释岩性道

> **⚠ 方向 69 修订重发（2026-10-07）：最新口径见文末「方向 69」节。**
> 本文件方向 38 节保留作历史记录；其中以下决策已被 69 修订：
> 无链接语义（「直井显式语义」→ NoSurvey 如实标注）、岩性生产者递延
> （消费先行 → wellfacies 落 DERIVED 资产为首个生产者）、资产契约
> （无 provenance → schema 1 + provenance + per-well interpretation 链接）、
> 岩性消费口径（全工程最新版 → 按井链接消费）、题注口径（「GR 推断」→
> 「推断·GR 截断」+ 解释来源词）、遗留销记（无生产者一项已销记）、
> depthWindow anyTops 边界（保持递延）、分支名
> （goal/wellsection-tvd-20261004 → goal/wellsection-tvd-20261007）、
> 及 69 新增三项（深度域入剖面状态 / 无测斜如实标注 / 岩性 provider 化）。

分支：`goal/wellsection-tvd-20261004`（基线 24daf8f）

## 勘察结论（2026-10-04，勿再勘察）

- **TVD 数据源存在**：catalog 角色 `trajectory`（井斜轨迹，primary 生效）；
  `ProjectDataFacade::trajectoryFor(wellId)` → `paleo::WellDeviationSurvey`
  （`tvdAt`/`tvdToMd`，最小曲率，契约 #126/#168 已测）。无链接 = 直井显式
  语义（TVD≡MD，不是伪造）；坏表 → nullopt + lastError。
- **解释岩性资产不存在**：catalog 井上唯一解释角色资产是
  `well_facies_intervals`（交会分类 classId，代码注释明言 "not geological
  facies claims"）。wellcomposite 的综合井 XML（真岩性名井道）不走 catalog
  （单文件直载）。→ 消费优先：定义新契约 `well_litho_intervals`
  （{schema, intervals:[{wellId, top, base, litho}]}），attach 侧消费，
  生产者/导入器递延（TODOS）。
- **SpacingMode 已存在**（方向 33 落了 domain `gapWidthsFor` + 面板接线），
  但缺坐标段走「中位距离补」——oracle 要求缺坐标井**不参与比例轴**且有名录。
  → 契约 v2：未知段固定等距宽，已知段按真实距离分摊剩余预算；名录进状态行。
- **断层投绘 z 本就是垂深**（z 向下正）：MD 域显示它本是一级近似，TVD 域
  反而是精确映射——FaultOverlay 公式两域同式，无需改。

## 设计决策

1. **TVD = 渲染域映射，不改数据**（拉平不变量延续）：MD 是唯一真源；
   `RenderState` 加 `displayOf/mdOfDisplay`（域映射 + 基准偏移），全部
   painter 走 `yForMd` 自动换域。深度窗/基准偏移/CSV 在 domain 层加域参数。
2. **域切换走世代**：面板切域发 `dataRequested`（同 mnemonics 变更路径）
   → workflow 新世代 → 在途旧结果按 currentGeneration 丢弃。
3. **岩性道优先级**：`well_litho_intervals` 资产段（解释）> GR 推断二分
   （回落，题注「GR 推断」保留）。解释段花纹走 wellcomposite 工程图式
   `PatternCatalog::lookupLithology`（30+ 花纹，中文词面命中），未命中
   灰底 + 原词面（不吞数据）。
4. **三处一致**：主面板域/井距用户动作 → 信号 → 壳层推给 fence 各剖面
   面板；band 是地理真坐标，与显示域无关（核对：域切换不改井径，band 不动）。

## 三处一致性核对表（逐处）

| 改动 | 剖面 panel | fence 栅状图 | map band | 断层投绘 |
|------|-----------|--------------|----------|----------|
| TVD 域 | RenderState 域映射 | 各剖面 panel 同构（传播信号） | 无涉（地理真坐标） | z=垂深，TVD 域精确映射 |
| 真实井距 | gapWidthsFor v2 | 同左（panel 内置） | 无涉 | pathFractions 已按真距 |
| 解释岩性 | Lithology 道优先解释段 | 同左（panel 内置） | 无涉 | 无涉 |

## 验收对照（Oracle）

1. TVD round-trip：单测（tvdOf/mdOf 对站表闭式）；workflow 测试种 trajectory
   链接断言 survey 挂载；UI 测试断言切域后 topLineY 按 TVD 排布 +
   CSV 多 TVD 列；坏表井 warning 如实。
2. 真实井距：gapWidths v2 单测（悬殊距离非均匀、缺坐标段等距不搅比例、
   全缺退化）；UI 测试状态行名录。
3. 解释岩性：workflow 测试种 well_litho_intervals 断言挂载；UI 测试
   有资产井画解释段（渲染断言）/无资产井回落 GR + 题注「推断」。
4. 一致性：fence 同步测试；世代测试（切域作废在途）。
5. 空态：零井/无 TVD/无坐标文案测试。

## 进度

- [x] 勘察（两个 Explore 报告：TVD 链路 / 岩性资产）
- [x] 基线构建绿（RelWithDebInfo，ninja -j8；GNU ld 2.47 并行链接偶发
  段错误，串行重试即过——环境项，非代码问题）
- [x] domain 层（DepthDomain/Well.survey/tvdOf·mdOf/LithoSegment/域感知
  datumOffset·depthWindow·topsTable·gapTwtMs/gapWidthsFor v2 + unpositionedWellNames）
- [x] workflow 层（trajectoryFor 取数 + attachLithoSegments 含告警）
- [x] 视图层（RenderState 域映射/深度轴标/岩性道解释段/面板域菜单与
  名录/fence 同步/壳层双向接线/地震缝 TVD 行级 LUT）
- [x] 测试全绿（wellsection×3 + deviation/tvd/trajectory + layering×3；
  ui_tokens 与 tst_services2 为 origin/master 基线先存红——stash 验证；
  tst_startup_trace/tst_cache_las -j8 并行下 flaky、串行绿）

## Review 记录

- **轮 1**（general-purpose 评审）：4 Medium（曲线道 int(NaN) UB / hover
  「MD nan」/ trajectoryFor lastError 同串漏记 / 地震缝逐行 tvdToMd 热路径）
  + 5 Low，全部修复：
  - NaN 断笔守卫（paintCurveTrack）、hover 诚实文案、GR/facies/深度刻度守卫；
  - `trajectoryFor(wellId, error=nullptr)` 出参（无链接=空串；三失败路径
    写明）+ 共享坏文件双井回归测试；
  - paintSeismic TVD 行级 LUT（8 行粒度精确反解+线性内插，MD 域精确直通）；
  - attachLithoSegments 告警（读失败/schema/无有效段/无效段计数）；
  - setDepthDomain 不再 clearStrip（TWT 域缝数据保留，位图缓存走 layoutVersion 失效）；
  - litho 花纹 QPixmapCache；ui-token-exceptions.json 挪行同步。
- **轮 2**（终审）：8 项轮 1 修复全部判定正确；新出 2 Medium + 6 Low：
  - Medium：GapItem 连线/层段带/高亮带/列内高亮带 NaN 几何无守卫（已修：
    linkPath 端点非有限→空路径，带构造前判空跳过，列内高亮带 isfinite 门）；
  - Medium：岩性四类告警零断言（已修：lithoSegmentsAttached 补「2 个无效
    段」断言 + schema 拒读/读失败两个场景回归网）；
  - Low 修复：0xD9DEE4→十进制+例外登记（token 平价 35=基线）；测试格式
    误并；mdAtRow 死计算清理；LUT 注释口径修正（极小 px/m 时米级）；
    ledger 进度勾选。Low 未修（备忘）：depthWindow anyTops 全局判定的
    边界（唯一有顶井恰为坏表井时退 {0,100}——结构先存，坏表使其新可达）；
    TVD×Depth 坏表井深度道刻度仍绘（全局轴口径，可辩护）。
  - 追加冒烟：tvdBrokenWellHonesty 补 renderImage 全路径冒烟。
- **轮 3（增量终验）**：轮 2 两条 Medium 修复逐点验证到位（linkPath 空路径
  的全部消费点安全、hitLink NaN 自然不可命中、岩性告警断言非恒真且
  resolvedVersionPath 对 managed=false 直用 v.path 的语义核实）；杂项
  （十进制灰底 + 例外逐字一致、mdAtRow 分支语义、renderImage 冒烟）全过；
  diff 范围 18 文件全在特征面内。**结论：零 High/Medium 成立**。余 3 Low：
  LUT 尾段分数（已随后精确到实际行距）、测试风格孤例（已归一）、ledger
  需 git add（提交时处理）——均已收口。

## 遗留（Low 记 PR body）

1. well_litho_intervals 无生产者/导入器（消费先行，TODOS 已记）。
2. deviationsurvey::tvdToMd O(站数)+百次二分治本递延（wellsection 侧 LUT
   绕开；TODOS 已记）。
3. depthWindow anyTops 边界（上文轮 2 Low 未修项）。
4. GNU ld 2.47 并行链接偶发段错误——环境项，构建循环重试即可。

---

## 方向 69（2026-10-07 修订重发）

分支：`goal/wellsection-tvd-20261007`（基线 origin/master 7aa71c04；分支
7 提交：a875996e → f76e8475 → b0a80e2e → 0eb8f10a → 5c7cc745 → 39e18364
→ b8fa0b7e）

### 重发背景

- 方向 38 于 2026-10-04 完成于 `goal/wellsection-tvd-20261004`
  （8515e2e9 + c2c5395e，三轮 review 零 High/Medium），但未提 PR 未合并。
- 69 以 origin/master 7aa71c04 为基线 cherry-pick 移植：16/17 文件干净，
  唯一冲突 `tools/ui-token-exceptions.json` 按既定解法处理（计数挪行同步）。
- 移植之上做三处口径修订（见下「设计决策」②③④ 与「38 节修订对照」）。

### 设计决策（69 定案）

1. **保留 38 的正交 `DepthDomain{MD,TVD}` 渲染域映射架构**——偏离 69
   任务书字面的「DatumMode 增 Tvd」；理由：Flatten 在 TVD 空间仍可用、
   MD 唯一真源不变量延续、38 已评审。`readCurveTvd` 为平行消费面（按列
   读 TVD 数据体），剖面渲染走 `displayOfMd` 不经它。
2. **`TvdStatus` 三态**（Surveyed / NoSurvey / BrokenSurvey）：NoSurvey
   几何恒等按 MD 绘制 + 顶部角标 + 状态行名录 + hover 如实，取代 38 的
   「直井显式语义」。不告警（非数据损坏），但 TVD 域必须如实标注不可用。
3. **岩性 provider 形态定案**：domain `WellLithologyProvider` 接口 +
   `LithoSegment{topMd,baseMd,litho,source,provenance}` +
   `LithoSource{Interpreted,Inferred}`；GR 截断包装为 domain fallback
   provider；workflow `CatalogWellLithologyProvider` 按
   `linksForEntity(wellId)` + interpretation 链接 + 类型过滤消费（废弃
   38 的全工程最新版粗口径——A 井资产不漏给 B 井）。
4. **首个生产者**：`WellFaciesWorkflow::publishLithoAsset` 落 catalog
   DERIVED `well_litho_intervals`（schema 1 +
   provenance{source,modelName,modelVersion,jobId} + per-well
   interpretation 链接）；失败路径不写资产、如实进状态行。
5. **深度域入剖面状态**：`well_section_edits.depth_domain` 列（ensureColumn
   前向升级，缺省 MD）；恢复仅 `rec.valid()` 才覆盖面板（QSettings 偏好
   做无记录缺省，R1 M1 定案）；导出 PNG/SVG/PDF 经版头题注单源携带
   口径词。
6. **cuttings 第二解释源**（PR #266 在审批次）：解释岩性优先级按井
   「有该井段的 well_litho_intervals 解释资产 > cuttings（role==
   "cuttings" 链接，多份取 currentVersion 最新）> 无（GR 回落）」——
   解释资产读失败（已告警）或资产内无该井段时按井兜底 cuttings
   （增量评审 M1 修复，第四场景钉死）。io 新增 `cuttingsdoc.h/.cpp`：
   `parseCuttingsSheet` 纯表解析（必需列 顶深/底深/岩性 方言表头——
   去空白/括号单位后大小写不敏感；描述列可选；坏行跳过进 issues 带
   物理行号），`readCuttingsFile` 按扩展名分派（.xlsx/.xml 经
   readWorkbook 取首个可解析 sheet；.csv/.txt/.tsv 自动探测分隔符转
   WorkbookSheet 走同一解析）。cuttings 段 source=Interpreted、
   provenance=「岩屑录井」（题注自然呈「解释·岩屑录井」，不新增
   LithoSource 枚举）；文件读失败/零有效段/跳过行如实进告警（带
   fileName）。

### gapWidthsFor v2 随移植捆绑的说明

69 任务书称井距「已实现勿重做」指 SpacingMode UI 切换（master 42b9a57）；
v2（缺坐标段等距不搅比例 + 名录）是 38 已评审实现的行为修正，随移植
整体落地，非重做。

### R0 基线与终验

- R0：主 checkout（origin/master 7aa71c04）全量串行 ctest 361 项，红集合
  = {tst_entityview（稳定红，复跑仍红），tst_errorhub_storm（flaky，单跑
  绿）}。
- 终验全量对照（已执行）：工作树（本分支 HEAD）全量串行 ctest 361 项
  （`build/final-ctest-dir69.log`，360/361 通过），红集合 =
  {tst_entityview}——即 R0 稳定红原样复现；R0 的 flaky 项
  tst_errorhub_storm 本轮通过。**相对 R0 新增红 = ∅，对照成立。**

### Review 记录

- **R1（五维独立评审：分层 / TVD 数学 / 诚实 / 渲染 / 文档各一评审员）**：
  分层、TVD 数学、渲染零 High/Medium；诚实面与文档共同命中 1 Medium——
  restoreWells 无记录强制 MD 覆盖 QSettings 偏好、注释与实现矛盾——
  + 若干 Low。修复批 5c7cc745：M1 改 `rec.valid()` 守卫 + hover 诚实 /
  告警归因 / 单井名录 / 拉平题注 / 版头钳制 / 注释清理 / TODOS 补登。
- **R2（五维复审）**：M1 关闭确认，五维零 High/Medium；新出 4 Low——
  3 条注释类（fencewidget.h 落库表述失实、panel 状态行注释口径、
  workflow/domain 三处「直井显式语义」残留）+ 1 条 hover 测试缺口
  （scene hover 三态文案无断言）。
- **收口批（本批，A/B 任务）**：注释口径五处对齐 69 三态口径；hover 文案
  构造提为自由函数 `hoverReadoutText` 单源（View 的 hoverChanged 与面板
  测试钩子 `hoverReadoutTextFor` 共用）+
  `tvdHoverReadoutHonesty` 三态断言（Surveyed 出 TVD 数值 / NoSurvey 出
  「TVD 不可用（无测斜，按井深绘制）」 / BrokenSurvey 出「井斜不可用，
  TVD 域无深度读数」）。
- **增量评审（cuttings 批，394271e3）**：1 Medium（M1：`lithologyFor`
  只在完全无解释资产链接时落 cuttings——井有链接但资产读失败/无该井段
  时 cuttings 被短路，与按井优先级语义不符）+ 若干 Low。M1 修复：
  改按井兜底（有该井段的解释资产 > cuttings），`cuttingsLithoSecondSource`
  加第四场景（资产只含他人段 → cuttings 生效、无新增告警）钉死；Low 中
  2 条登记 TODOS（CSV 无引号转义列右移、多份取最新落选无提示），余记
  下方 Low 清单。

### Oracle 逐条对照（附测试名证据）

1. **TVD 域合成测斜对比** → `tvdDomainMapping` / `topsTableTvdColumn` /
   `gapTwtTvdDomain`（domain 闭式）+ `tvdDomainSwitch`（UI 30° 合成井、
   CSV TVD 列对 `survey->tvdAt`）+ `tvdDepthCaptionSync`（题注/单位口径）。
2. **无测斜降级** → `tvdNoSurveyWellHonesty`（三井角标/名录/几何/
   renderImage 冒烟/MD 域无标记）+ `tvdBrokenWellHonesty`；hover 侧
   `tvdHoverReadoutHonesty`（收口批新增）。
3. **岩性道** → `lithoTrackInterpreted` 扩展（解释题注+来源词/推断·GR/
   混合井如实）+ `grCutoffLithologyProvider` +
   `wellFaciesLithoAssetRoundTrip`（A 井资产不漏 B 井、source/provenance
   断言）+ `cuttingsLithoSecondSource`（cuttings 第二解释源：解释资产
   优先级胜出/仅 cuttings 生效且 provenance「岩屑录井」/删文件读失败
   告警带 fileName/解释资产无该井段按井兜底 cuttings 且无新增告警）与
   io 纯表解析 `cuttingsSheet*`（标准/方言/缺列/
   坏行 issues/描述可选五例）。
4. **切换状态保持** → `tvdSwitchPreservesSectionState`（井序/flattenTop/
   选中/dataRequested）+ `sectionStoreDepthDomain`（store round-trip +
   旧库迁移 + 无记录有效性别）+ `fenceViewPrefSync`。
5. **回归** → 主回归面 27/27 绿 + MD 域对拍（`lithoTrackInterpreted`
   回落帧 QCOMPARE、`tvdDepthCaptionSync` MD 像素级复原）。
6. **全量 ctest R0 对照** → 已执行：终验 360/361，红集合 {tst_entityview}
   = R0 稳定红；相对 R0 新增红 = ∅（证据 `build/final-ctest-dir69.log`，
   R0 证据主 checkout `build/r0-ctest-dir69.log`）。方向 38 任务书已加
   「已修订重发为方向 69」注记（`goal-loop-prompts/38-wellsection-tvd.md`
   头部块引用）。

### Low 清单（记 PR body 同款）

gapWidthsFor minGap 夹取超总宽（v1 既有）；地震缝 LUT 无直接回归测试；
断层投绘 z-down 既有口径；fencewidget saveToStore err 静默；main.cpp
工厂栈引用；tvdDomainMapping/topsTableTvdColumn 两对同源断言；题注取
首段 provenance；store 恢复不回写 QSettings；fence 纯域改动不落 fence
节；QPixmapCache 键 alpha/DPR 边界；fence 读回只取井序（TODOS 已登记）；
同井多解释资产取最新无合并（TODOS 已登记）；completed→publish 无端到端
测试（TODOS 已登记）；新增 tr 串未进 paleo_zh_CN.ts（l10n 闸门绿，词面
随 lupdate 流程）；cuttings xlsx 分支经 readWorkbook 既有测试覆盖、本
方向测试走 CSV+合成 WorkbookSheet（cuttings 专属 xlsx fixture 未造）；
cuttings CSV/TSV 无引号转义、词面含分隔符列右移不报警（TODOS 已登记）；
同井多份 cuttings 取最新版、落选无提示、provenance 不点名文件（TODOS
已登记）。

### 38 节修订对照（R1-5 评审 10 条分歧点逐条标注）

| # | 分歧点 | 38 口径 | 69 修订 |
|---|--------|---------|---------|
| 1 | 无链接语义 | 直井显式语义（TVD≡MD，非伪造） | NoSurvey 三态：几何恒等按 MD 绘制 + 角标/名录/hover 如实标注 |
| 2 | 生产者 | 消费先行，生产者/导入器递延 | wellfacies `publishLithoAsset` 已落（首个生产者；余置信度曲线/导入器仍递延） |
| 3 | 资产契约 | `{schema, intervals:[...]}` | schema 1 + provenance{source,modelName,modelVersion,jobId} + per-well interpretation 链接 |
| 4 | 题注口径 | 「GR 推断」保留 | 「推断·<曲线> 截断」+ 解释段带资产来源词 |
| 5 | 消费口径 | 全工程最新版粗口径 | 按井链接 + interpretation 链接 + 类型过滤（A 不漏 B） |
| 6 | 遗留销记 | 遗留 1 = 无生产者（TODOS） | 已销记（TODOS 已更新为仍递延的置信度曲线/导入器） |
| 7 | 三处一致 | 主面板 → fence 单向 + band 无涉 | 壳层双向接线（fence 动作 → 主面板）核校，`fenceViewPrefSync` 断言 |
| 8 | depthWindow 边界 | 轮 2 Low：anyTops 全局判定（唯一有顶井恰为坏表井时退 {0,100}），结构先存 | 未修，随移植保持递延（69 坏表井 NaN 跳过使该边界在 TVD 域更可控，仍按 38 口径记录） |
| 9 | 分支名 | goal/wellsection-tvd-20261004 | goal/wellsection-tvd-20261007 |
| 10 | 69 新增三项 | —（38 无） | 深度域入剖面状态（depth_domain 列 + rec.valid() 恢复守卫）/ 无测斜如实标注 / 岩性 provider 化 |
