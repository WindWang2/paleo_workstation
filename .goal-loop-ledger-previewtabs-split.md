# 方向 97 ledger：datapreviewtabs 家族拆分（cpp 1,758 + internal.h 1,607 双体解体）

- 基线：origin/master `f474069b`（fix(ui): 用户可见文案改为正式用语 #318）
- worktree：`.worktrees/previewtabs-split`，分支 `goal/previewtabs-split-20261010`
- 环境三坑（实测定案，勿再踩）：
  1. **必须用 pwsh 7** 跑 paleo-dev.ps1——Windows PowerShell 5.1 把无 BOM
     UTF-8 脚本当 ANSI 解析，中文串炸语法（lnk 证据
     `previewtabs-build-baseline.log` 首轮）。
  2. **PATH 前置 `C:/Qt/Tools/CMake_64/bin` + `/c/Qt/Tools/Ninja`**——VsDevCmd
     注入的 VS 自带 cmake 3.27.2 < 工程要求 3.28（BUILDING.md:168 口径）。
     实测前置后 Qt CMake 仍胜出（VsDevCmd 不前置其 cmake）。
  3. **vendor/onnxruntime 不随 worktree 复制**（gitignored）——缺了它
     PALEO_HAVE_ORT=FALSE，paleo_ai 走空壳分支，tst_background_open/
     tst_officepreview 链接 LNK2019（suggestHorizonTracking/runTileInference
     未解析——干净 origin/master 上即可复现，非本方向引入）。解法：从
     sibling worktree 复制闭包（419MB，tree-roles-incremental → 本 worktree）。

## R0 结构勘察（2026-10-10 实测，行号为 f474069b 口径，wc 与任务书略有
±（1,776/1,624——任务书 1,758/1,607 是 32851a1e 口径，f474069b 又进了 #318 文案）

### datapreviewtabs.cpp（1,776 行）构成盘点

| 区块 | 行区间 | 行数 | 归属定案 |
|------|--------|------|----------|
| includes + using | 1-118 | 118 | 按 TU 拆分后各取所需 |
| coordinateStatusText / setHorizonOnMap | 120-146 | 27 | 留主文件 |
| 构造/析构 + 空态 | 148-206 | 59 | 留主文件（门面） |
| details 宿主管理 | 208-236 | 29 | 留主文件 |
| 服务绑定（setImportService/setDocService/attachDoc/setTaskService/setProject） | 239-302 | 64 | 留主文件 |
| **测区全景** openSurveyArea + buildSurveyAreaContent | 304-677 | 374 | **→ datapreviewtabs_survey.cpp** |
| tab 生命周期（tabCount/assetIdAt/close/closeAll/isMissing/relocate） | 679-782 | 104 | 留主文件（门面+切换） |
| loadingLabel / failureState | 784-812 | 29 | 留主文件（多族共用） |
| focusWell / updateTabTitle / drainDeferred / rebuildAssetTab | 814-905 | 92 | 留主文件 |
| open 入口族（openAsset/versionIdAt/openVersion/openAssetForWell/openSeismicLine） | 907-1017 | 111 | 留主文件（门面+切换） |
| **buildContent 总分发**（缺失态/SHA 门/Office/wellfilter 分支/document PDF/aux 尾） | 1019-1325 | 307 | **→ datapreviewtabs_content.cpp**（wellfilterable 分支 1145-1233 析出为 buildWellFilteredContent 进 well TU） |
| **buildWellBody**（well_stratification/time_depth/well_head 三态正文） | 1327-1581 | 255 | **→ datapreviewtabs_well.cpp** |
| **同目录组图** addSiblingOverlayButton/Layer（D2.10） | 1583-1693 | 111 | **→ datapreviewtabs_overlay.cpp** |
| onSectionReady/onSectionFailed（地震挂起应用） | 1695-1742 | 48 | 留主文件（挂起态=门面状态） |
| onLasReady/onLasFailed（LAS 挂起应用） | 1744-1776 | 33 | **→ datapreviewtabs_well.cpp**（与 buildWellLog 两段式同族） |

主文件拆后预估 ≈ 650（≤700 目标 ✓）。

### datapreviewtabs_internal.h（1,624 行）符号→消费 TU 实测（rg 全量）

- **CurveData + CurvePanel（634-1287，654 行）**：真实消费仅
  datapreviewtabs.cpp（time_depth 正文）+ datapreviewtabwelllog.cpp
 （datapreviewtabseismic.cpp:5 只是注释提及）→ 拆 `previewcurvepanel.h`
 （650 行的单类组件，违背 internal.h「只放多分支共用的**短工具与小组件**」章程）。
- **SectionPanel（1289-1514，226 行）**：真实消费仅
  datapreviewtabseismic.cpp + datapreviewtabs.cpp（onSectionReady/Failed；
  geojson:107/horizon:105 是注释提及）→ 拆 `previewsectionpanel.h`。
  注：现 internal.h 版 SectionPanel 依赖包含方自带 nicestep.h——新头自带
  include（自包含修复，零行为变更）。
- 654+226 = 880 行出仓后 internal.h ≈ 746（≤800 目标 ✓）；internal.h
  继续包含两个新头（族内 9 TU 零 include 改动，等价组合关系）。
- 其余符号（caption8 8/9、qssHex 6/9、monoFont 6/9、stateLabel/warnLabel
  4/9、applyFaciesRendererToLayer 3/9、addRasterPyramidHint 3/9 等）确系
  多分支共用，留 internal.h；pickCurveColor/compositeCurve/lasBodyFor/
  catalogProjectDir（welllog 专用）、makeContourLayer（horizon 专用）等
  单 TU 消费符号本轮**不**再下沉（避免给既有 TU 加 include 改动面；
  internal.h 已达标，不为拆而拆）。

### 消费 TU 与 include 面

- datapreviewtabs_internal.h 代码消费 = 族内 9 个 cpp（tabs/seismic/welllog/
  horizon/geojson/image/mapproduct/workbook/xml）；workflow 侧仅注释提及
 （constraintworkflow_internal.h:22 记 LNK2005 历史教训——internal.h 内
  联函数只放 inline/static，拆分后新头沿用）。
- previewdoc.h 消费 = src 下 38 个文件（ui 30 + workflow 1 + services 自家
  等）；窄化评估见「include 面机会」节。

### 公共 API 冻结口径

- datapreviewtabs.h 公共区（signals/public 至 `private:` 前 + signals 区）
  diff 零变更；私有区仅**新增** buildWellFilteredContent 声明（buildContent
  wellfilterable 分支析出）；消费方零改动。

## 切分线定案（无人值守自决）

新 TU 四枚（均 `src/ui/datapreview/`，头三行 `// 层：视图`）：

1. `datapreviewtabs_content.cpp` — buildContent 总分发（~300 行）
2. `datapreviewtabs_survey.cpp` — 测区全景两函数（~420 行）
3. `datapreviewtabs_well.cpp` — buildWellBody + buildWellFilteredContent +
   onLasReady/onLasFailed（~450 行）
4. `datapreviewtabs_overlay.cpp` — D2.10 同目录组图（~160 行）

新头两枚：`previewcurvepanel.h`、`previewsectionpanel.h`（类定义逐字节搬移，
非 TU，无 600 行帽；wc 证据见验收节）。

## 批次执行记录

### 基线（R0）

- 全量构建绿（三轮：首轮 PS5.1 解析错弃；二轮缺 ORT 链接红；三轮补
  vendor/onnxruntime + 清 paleo_ai 陈旧 AUTOMOC 状态后绿）。
- 基线全量 ctest 红集合 = `previewtabs-test-baseline.log`（R0 对照面）。

### 批次 1：internal.h 瘦身（一次通过）

- `previewcurvepanel.h`（691 行）= CurveData + CurvePanel 逐字节迁出
  （对拍脚本 OK 654 行体 + 头部含 include/注释）；自带 Qt/paleotheme
  include 集。
- `previewsectionpanel.h`（258 行）= SectionPanel 逐字节迁出；自带
  nicestep.h（修掉旧「依赖包含方带 nicestep」的隐式耦合，零行为变更）。
- internal.h 1,625 → **747 行**（≤800 ✓）；组合导出两新头（族内 9 TU
  零 include 改动）；第 2 行 token 例外注释收口（振幅色带条目随迁）。

### 批次 2：门面 TU 拆分（一次通过）

- 新 TU 四枚（头三行 `// 层：视图`，同 internal.h using 引入先例）：
  - `datapreviewtabs_content.cpp` **238 行**：buildContent 总分发；well
    filterable 分支改调 `buildWellFilteredContent`（新私有方法，公共区
    零变更——头文件 diff 仅私有区 +7 行声明，对拍 `PUBLIC-REGION-IDENTICAL`）。
  - `datapreviewtabs_survey.cpp` **396 行**：openSurveyArea +
    buildSurveyAreaContent（token 例外注释随测区轮廓例外迁入）。
  - `datapreviewtabs_well.cpp` **396 行**：buildWellFilteredContent +
    buildWellBody + onLasReady/onLasFailed。
  - `datapreviewtabs_overlay.cpp` **121 行**：D2.10 同目录组图两函数。
- 主文件 1,776 → **690 行**（≤700 ✓；门面 + 切换 + onSection 挂起应用）。
- CMakeLists +4 TU（就近登记）；ui-token-exceptions.json 6 条例外改路径
  （survey 2 / curvepanel 1 / sectionpanel 3；条目总数 359 不变，ratchet
  天然绿）。
- **逐字节对拍**（python 函数级 diff，证据见会话）：openSurveyArea 49 行 /
  buildSurveyAreaContent 324 / buildContent 头 129 + 尾 92 / wellfilter
  if 体 83（去一层缩进外逐字节一致）/ buildWellBody 255 / onLasReady 13 +
  onLasFailed 19 / overlay 27+81 / onSectionReady 27 + onSectionFailed 18 /
  CurveData..CurvePanel 654 / SectionPanel 226 —— 全部 OK，
  `ALL-BLOCKS-VERBATIM`。

### 批次 3：include 面机会（评估→执行定案）

- **窄化不做**（记因）：previewdoc.h 经方向 59 已收敛（唯一重 include 是白名单
  lasdoc.h；重契约类型全走前向声明/拆细头），消费 TU 的「窄出参改窄」无可收
  面预览——本方向不再开新窄头（避免为拆而拆）。
- **死 include 清理（有编译证据）**：
  - 删 4 处全死：datapreviewtabmapproduct.cpp（经 internal.h 传递可见）、
    paleomainwindow_attach_compose/constraint/validate.cpp（经
    paleomainwindow.h/datalist 链传递可见）。
  - 保留 2 处「假死」：datalist_batch.cpp / datalist_table.cpp ——文本无
    `PreviewDocService` 字样，但 `m_doc->catalog()` 成员调用需完整类型
  （初判误删后 MSVC C2027 实锤，恢复并加注释）。教训：include 死活不能只
    rg 文本符号，还要看成员调用完整类型需求。
- **主 TU 死-by-split include 清理**：officepreviewwidget.h /
  officepreviewsession.h / paleoviewport.h / QFileDialog / QPdfDocument /
  QPdfView（计数=1 即 include 行自身；编译绿验证）。
- 主文件 690 → **684 行**。

## review 记录

### 第一轮（批次 1+2+3 后；diff 五维）

- **API 等价**：datapreviewtabs.h 公共区（`private:` 前）diff 为空
 （`PUBLIC-REGION-IDENTICAL` 实测）；私有区仅 +7 行声明
 （buildWellFilteredContent）。消费方改动 = 6 个 include 行级变更，零逻辑。
- **internal 归属**：CurvePanel/SectionPanel 逐字节迁出（函数级对拍
  ALL-BLOCKS-VERBATIM）；internal.h 组合导出，族内 9 TU include 零改动。
- **include 纪律**：`check_layering.py --strict` 绿（无层违规）；
  `check_ui_tokens.py --strict` 绿（violations 0 / staleExceptions 0，
  条目数 359 不变）。
- **TU 边界**：链接绿 = 单一定义；CMake 就近登记 4 TU。
- **无死代码**：主 TU 仅剩门面 + 切换 + onSection 挂起应用；grep 佐证无
  残留（openSurveyArea 等仅存在于 survey TU）。
- Low（记 PR body）：
  1. internal.h 组合导出插在 preview* 本地头与 qgis/ 头之间（风格位）。
  2. 主 TU 存在拆前即死的 Qt include（QTemporaryDir/QElapsedTimer/
     QMessageBox 等计数=1）——历史债，本方向不动（避免顺手改进越界）。
  3. previewcurvepanel.h 691 行 >600——它是头（类定义逐字节迁出），600
     帽按任务口径只约束 TU。

### 第二轮（全量对拍后收尾；最终 diff 复查五维）

- 复查批次 3 终态（4 删 + 2 承载注释 + 主 TU 死 include 6 处）无新问题；
  头/主/TU/内部头全部首行 `// 层：视图`。
- **结论：零 High / 零 Medium**（Low 三条见第一轮，入 PR body）。

## 验收证据（Oracle 对拍）

1. **类零变更**：datapreviewtabs.h 公共区 diff 为空（`PUBLIC-REGION-IDENTICAL`）；
   消费方改动 = 6 个 include 行（4 删 + 2 注释），rg 佐证无其他消费方变更
  （`git status` 全清单见 ledger 提交）。
2. **体量（wc 实测）**：主文件 **684**（≤700）；internal.h **747**（≤800）；
   新 TU：content **238** / survey **396** / well **396** / overlay **121**
  （各 ≤600）；previewcurvepanel.h 691 / previewsectionpanel.h 258（头，
   类定义逐字节迁出，600 帽按任务口径只限 TU）。
3. **行为等价**：
   - tst_previewdoc / tst_previewmap_canvas / tst_panels / tst_staleness /
     tst_relocate / tst_derivationgraph / tst_ui / tst_ui_blocking /
     layering 族：**零改动通过 ×2 遍**（previewtabs-test-split1b/split2.log，
     12 项中唯一红 = tst_datapreview 预存红）；最终全量轮再绿
    （previewtabs-test-final-full.log）。
   - tst_datapreview：41 passed / 1 failed / 1 skipped——与 R0 基线**同一用例
     同一断言**（unresolvedMultiWellTabShowsDeadEnd 期待 #318 前旧文案
     「还没有挂到任何井」；基线、拆分两遍、最终二进制直跑四点一致，
     previewtabs-test-datapreview-final.log）。
   - offscreen 抽查 ≥5 类：ps1 test 全程 QT_QPA_PLATFORM=offscreen；
     tst_datapreview 41 绿槽覆盖 everyTypeOpensContent（九类资产全开）+
     surveyAreaOpensQgisCanvasAndEmitsShowOnMain（测区全景）+
     closableTabsRestoreEmptyState / reselectFocusesExistingTab（关/切/重选）
     + multiWellTabComboAndTitle（井切换）——九类开/切/关全在 offscreen 下
     每轮回归。
   - tst_ui 一次 0xc0000409（负载抖动）：单跑绿 ×2（含 12 项集合复跑）。
4. **include 面**：4 死删 + 2 承载保留（注释）+ previewdoc 窄化评估「不做」
  （方向 59 已收敛，无可收面）——证据见批次 3。
5. **全量 ctest 红集合 diff**：稳定红 13 项两轮**完全一致**（officepreview/
   services2/workflows/composeworkflow/manifestproj/fspath/datapreview/
   mapping/startup_trace/crossplot_controller/cache_las/wellcomposite_perf/
   wellsection_ui）。摆动 3 项复跑全绿 = 抖动（tst_errorhub_storm 本轮红→
   复跑绿；tst_singlefactor_perf/tst_perfbudget 基线红→本轮绿——perf 阈值
   类，与本方向零接触面）。layering 三档（ctest 项 layering/
   layering_strict/layering_selftest）全量轮绿 + `--strict` 直跑绿；
   ui_tokens `--strict` 绿（violations 0 / stale 0 / 条目数 359 不变）。
6. **TODOS.md:21-31 候选清单**：第 1 项已划（留 ledger 指针）。

## include 面机会（评估记录）
- 见「批次 3」——4 删 2 留 1 不做，全部有证据定案。

## 验收证据（Oracle 对拍）
- （待补）
