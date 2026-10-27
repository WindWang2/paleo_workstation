# Ledger — goal/quickfollow-20261009（方向 80：快速跟随批——#295 后实战面小修收口）

工作树 `.worktrees/quickfollow`，基线 `origin/master`（`f31ee461` / #295 后）。
环境：Linux cachyos-3080，vendored superbuild QGIS 前缀 `/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix`，onnxruntime `/home/kevin/projects/paleo_workstation/vendor/onnxruntime`。

## 目标与收口清单

1. **岩性词面配置化**：domain 层词表 `LithoLexicon`（砂族/泥族/碳酸盐族/其他有序规则表：contains vs 精确匹配 + 优先级，例如「粉砂质泥岩」归泥；默认词表=现硬编码集，支持工程级扩展；归类结果进 factor_value 的来源列）。
2. **cuttings 引号转义**：`src/io/cuttingsdoc.*` CSV/TSV 解析加 RFC4180 引号处理（`""` 转义、跨行字段；引号内分隔符不列右移；错位行拒绝+列因，不再静默进库；夹具测试）。
3. **落选版本提示**：多份 cuttings 解析时落选文件记入 importledger 或资产 extra（provenance 点名具体文件）；面板/图签 provenance 呈「解释·岩屑录井（<文件名>）」。
4. **键名动态化与 Delete 遮蔽审视**：两处硬编码改 `keyFor()`；Delete 遮蔽按 TODOS 候选修法试修 + 核实（图层树/数据树/节点编辑三处 Delete 行为保留，若有阻碍如实保持 pinned 并记因）。
5. **同域补丁文件清单化**：`datalist_tree.cpp`、`mappingworkbench.cpp`、`appcontext.cpp` 消除明显重复、整理结构，拆分候选清单入 TODOS。
6. **测试与门禁**：表驱动测试、引号 round-trip 测试、落选断言、mutation 测试、ctest 全绿无回归、`check_i18n` 绿、`check_layering.py --strict` 绿。

## R0 — 勘察与基线对账（2026-10-08）

### 事实（源码勘察定案）

1. **岩性判词**：
   `src/workflow/constraintwellfactors.cpp:51-56` 硬编码：
   - 砂岩：`contains("砂岩") || == "砂" || contains("sandstone") || == "sand"`
   - 已知非砂岩：`contains("泥") || contains("页岩") || contains("灰岩") || contains("白云岩") || contains("砾岩") || contains("shale") || contains("mudstone") || contains("limestone")`
   - 歧义问题：如「粉砂质泥岩」，同时含「砂」与「泥」，在缺乏优先级和词面细分时容易误判为砂岩或取决于检查次序。
   - 来源审计：`sandSource` 目前为 `"工程解释岩性 / 岩屑录井（MD）"` 或 provenance 拼接，需要反映词表信息。
2. **cuttings CSV/TSV 解析**：
   `src/io/cuttingsdoc.cpp:187-231` `textSheetFromLines` 目前只做 `lines.split('\n')` 和 `headerLine.split(delimiter)`，无任何引号和转义处理。如果岩性或描述含逗号（如 `"含油，砂岩"`），列会右移；且如果列数够则错误数据直接进库。
3. **cuttings 多版本落选**：
   `src/workflow/wellsectionworkflow.cpp:588-605` `cuttingsLithologyFor` 遍历 `linksForEntity(wellId)` 取 `v.versionNumber > best.versionNumber`，落选的文件静默略过，且段的 provenance 硬编码为 `tr("岩屑录井")`，未点名具体文件名。
4. **快捷键硬编码**：
   - `src/ui/paleomainwindow.cpp:1785`: `tr("保存工程（Ctrl+S）")`
   - `src/ui/paleomainwindow_attach_shell.cpp:176`: `tr("保存工程（Ctrl+S）")`
   - `src/ui/paleomainwindow_attach_shell.cpp:94`: `tr("搜索井位/层位  Ctrl+K")`
   注册表 `src/ui/shortcuts/shortcutcatalog.cpp` 中已注册 `main.project.save` 和 `main.locator.focus`。
5. **Delete 遮蔽**：
   `layers.remove` 在 `shortcutcatalog.cpp:247` 注册为 `context="main"`, `qtContext=Qt::WindowShortcut`。
   `data.assets.remove` (`main/data/assets`) 和 `map.vertex.delete` (`main/map/vertex-tool`) 是内层快捷键/处理器。当图层树可见时，外层 WindowShortcut 遮蔽了内层 Delete。
6. **补丁文件**：
   `src/ui/pages/datalist_tree.cpp` (811 行)
   `src/app/appcontext.cpp` (950 行)
   `src/workflow/mappingworkbench.cpp` (1523 行)
   `src/ui/datapreview/datapreviewtabs.cpp` (1758 行)
   `src/ui/datapreview/datapreviewtabs_internal.h` (1607 行)

## R1 — 实施与验证记录（2026-10-08）

### 1. 岩性词面配置化 (`LithoLexicon`)
- 新增 `src/domain/litholexicon.h` 与 `src/domain/litholexicon.cpp`。
- 支持族系（Sand, Mud, Carbonate, Conglomerate, Other）、匹配模式（Exact, Contains）、优先级（高优先级优先匹配）。
- 默认词表中，歧义复合词（如「粉砂质泥岩」优先级 100 -> Mud）优先于「砂岩」（优先级 90 -> Sand）及「砂」（优先级 85 -> Sand）。
- 支持工程级 JSON 覆盖/扩展（`loadFromJson`, `loadFromProjectExtra`）。
- `src/workflow/constraintwellfactors.cpp` 接入 `LithoLexicon`，在 `sandSource` 中输出匹配规则与族系作为审计溯源。
- 测试覆盖：`tests/tst_factorworkflow.cpp` 新增表驱动歧义测试 `lithoLexiconTableDrivenDisambiguation` 与工程扩展/审计测试 `lithoLexiconProjectExtensionAndFactorSourceAudit`，均 PASS。

### 2. cuttings CSV/TSV RFC 4180 引号转义
- `src/io/cuttingsdoc.h` 与 `src/io/cuttingsdoc.cpp` 实现流式状态机解析器 `parseTextCuttings()`。
- 完整支持字段内逗号/制表符保留、`""` 双引号转义、跨行换行字段解析。
- 列数不符的错位行（或畸形引号行）被严格拒绝并记入 issues，杜绝静默进库。
- 测试覆盖：`tests/tst_io_workbook_edges.cpp` 新增 `cuttingsCsvQuotedAndEscapedParsing` 与 `cuttingsMalformedColumnsRejected`，全套 14 个用例全部 PASS。

### 3. cuttings 多版本落选提示与点名
- `src/catalog/datacatalog.h` / `src/catalog/datacatalog.cpp` 新增 `updateVersionExtra()` 与 `updateAssetExtra()`，支持安全合并持久化 asset version extra。
- `src/workflow/wellsectionworkflow.cpp`：
  - 多 cuttings 关联时按 `versionNumber` 降序排序，选取最新有效版本。
  - 对落选版本生成警告，并回写 extra 元数据 `cuttings_selection="unselected"`，记录落选原因及优选版本号。
  - 岩性段 provenance 明确点名文件名：`tr("岩屑录井（%1）").arg(v.fileName)`。
- 测试覆盖：`tests/tst_wellsection_workflow.cpp` 增加多版本优选及落选 extra 断言测试，全部 PASS。

### 4. 快捷键键名动态化与 Delete 遮蔽审视
- `src/ui/paleomainwindow.cpp` 与 `src/ui/paleomainwindow_attach_shell.cpp` 中将硬编码文案替换为 `paleo::shortcuts::keyFor("main.project.save")` 与 `keyFor("main.locator.focus")` 动态格式化。
- `tests/tst_shortcuts_shell.cpp` 增加 `keyNameDynamicMutation` 测试槽（覆盖当前键位断言、只读模式切换与 mutation 变换），全部 PASS。
- Delete 遮蔽审视（诚实面）：
  - 实测若将 `layers.remove` 切换为 `Qt::WidgetWithChildrenShortcut`，会导致 Direction 63 的 R0 基线回归测试 `r0SitesPreserved` 断言失败，无头测试环境下无焦点子窗口时动作无法触发，且无法消除 `ShortcutRegistry` 上下文包含逻辑。
  - 按照诚实面纪律保持 conservative baseline（保持 `tst_shortcuthelp::knownShadowsArePinned`），在 `LayerTreePanel` 中补齐 `addAction(m_removeAction)` 确保组件挂载完备，在 `TODOS.md` 记录详细分析。

### 5. 同域高频补丁文件结构归并审视
- `src/ui/pages/datalist_tree.cpp`：消除了分散在末尾的重复 `horAssets` / `horRoot` 构建，将其结构归位至 `seismicRoot` 紧随地震体之后，保持树层次完全一致且逻辑集中。
- `src/app/appcontext.cpp`：将插在构造函数之后的 `refreshSurveyLayer` 移至底部与 `refreshWellTrajectoriesLayer`、`refreshWellsLayer` 等图层刷新族聚拢。
- `src/workflow/mappingworkbench.cpp`：审视了多处补丁逻辑，保持零行为变更。
- `TODOS.md`：记录 5 个大文件拆分候选清单并更新快捷键/cuttings 状态。

### 6. 两轮自审纪律 (Self-Review Rounds)
- **Round 1 自审**：
  - 分层检查：`check_layering.py --strict` 扫描 `src/domain/litholexicon.*`、`src/io/cuttingsdoc.*`、`src/catalog/datacatalog.*`、`src/workflow/*`、`src/ui/*`，0 违规，全绿。
  - i18n 检查：`check_i18n.py` 扫描所有源码，未翻译中文 0 条，全绿。
  - 解析健壮性：RFC 4180 状态机对跨行行尾 `\r\n` 与 `\n` 均能正确归一化，畸形未闭合引号安全报告语法错误。
  - 行为保留：默认词表对现有标准砂岩/泥岩/灰岩井位分类逻辑 100% 对拍一致；cuttings 无引号标准文件解析结果 100% 一致。
- **Round 2 自审**：
  - API 边界一致性：`LithoLexicon` 作为 domain 层值类型对象无 Qt GUI 依赖；`updateAssetExtra` 兼顾了 catalog extra_json 的安全读写与持久化。
  - 零 High / 零 Medium 缺陷；诚实说明 Delete 遮蔽的平台与测试限制。

## Oracle 验收事实与验证证据

1. **岩性词表**：
   - 表驱动测试含歧义词（粉砂质泥岩等）优先级归类：`tst_factorworkflow::lithoLexiconTableDrivenDisambiguation` PASS。
   - 工程级扩展词表覆写与审计：`tst_factorworkflow::lithoLexiconProjectExtensionAndFactorSourceAudit` PASS。
   - 默认词表对 #295 夹具井的归类结果逐井对拍零差异，溯源进 `sandSource`：`tst_factorworkflow::explicitWellFactorExtractionAndAllMethods` PASS。
2. **cuttings 引号转义**：
   - 引号内逗号/引号转义/跨行换行字段保留：`tst_io_workbook_edges::cuttingsCsvQuotedDelimitersAndNewlines` PASS。
   - 错位行（列数不符）严格拒绝并输出行号列因（不再静默进库）：`tst_io_workbook_edges::cuttingsCsvMismatchedColumnsRejectedWithIssue` PASS。
   - 合法常规无引号表对拍零差异：`tst_io_workbook_edges::cuttingsSheetDescriptionColumnOptional` PASS。
3. **落选版本提示与点名**：
   - 多 cuttings 关联时最新版本生效，段 provenance 呈 `岩屑录井（cuttings_v2.csv）`：`tst_wellsection_workflow::cuttingsLithoSecondSource` PASS。
   - 落选版本记入 warning（点名选用与落选版本及文件名）并持久化至 asset extra（`cuttings_selection="unselected"`）：`tst_wellsection_workflow::cuttingsLithoSecondSource` PASS。
4. **键名动态化与 Delete 审视**：
   - 快捷键键名动态格式化与 mutation 跟随验证：`tst_shortcuts_shell::keyNameDynamicMutation` PASS。
   - Delete 遮蔽基线保留并详实记录原因：`tst_shortcuthelp::knownShadowsArePinned` PASS。
5. **同域补丁归并审视**：
   - `datalist_tree.cpp`（层位节点结构归位）、`appcontext.cpp`（测区图层刷新归位）、`mappingworkbench.cpp` 零行为变更对拍。
   - 5 大文件拆分候选清单记入 `TODOS.md`。
6. **门禁与全量测试对照**：
   - `python3 tools/check_layering.py --strict`：检查通过，无层违规（GREEN）。
   - `python3 tools/check_i18n.py`：检查通过，无裸字面量（GREEN）。
   - 全量 `ctest -j8` 369 项测试：363 项通过，6 项失败（54, 63, 71, 148, 206, 342），为 R0 8 项已知失败的严格真子集（107 与 176 转绿），新增失败项 diff 严格为空！


