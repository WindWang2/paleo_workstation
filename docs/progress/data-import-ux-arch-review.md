# 数据管理导入 UX（PR #603）架构评审——最小必要复杂度维度

- 评审对象：worktree `C:\Users\wangj.KEVIN\.codex\worktrees\data-management-import-ux\paleo_workstation`，分支 `codex/data-management-import-ux`（9aba6893，79 文件 +6302/−966）
- 评审人：独立架构评审（只读 + 本报告）
- 评审日期：2026-10-20

## 逐题结论表

| # | 题目 | 结论 | 关键证据 |
|---|------|------|---------|
| 1 | 层次合规 | **合规** | 全部 src/ 新文件头三行有 `// 层：` 标记且归属正确；io 新文件无 QtWidgets/ui/；视图新增 include 无 io/、metadata/、algorithms/ |
| 2 | 重复造轮子 | **未造（XML）；文本分列为轻度自足实现** | XML 复用 outsourceworkbook 核；CSV 与 cuttingsdoc 各有内部实现，见下 |
| 3 | 抽象过度 | **不过度** | 新增公共函数全部有真实调用方；DTO 新字段全部被消费 |
| 4 | 接口收敛 | **真收敛，且顺手拆了两个存量旁路** | 本 PR 把 workflow 两处直连 LasParser::parse 改走 WellLogRead；存量旁路仅 selfcheck 性能基准 |
| 5 | 依赖与配置 | **无新依赖、无新顶层模块** | 仅两个 extra-*.cmake 挂进既有 paleo_io/paleo_ui；layering_vocab.json 无需更新 |
| 6 | 规模与分层债 | **基线只缩不涨；拆分是真降复杂度** | file-budget 删 1 行 + 降 2 值；ui-size 降 1 值；folderconfirm 单文件 670→406 |
| 7 | 与检查器一致 | **一致** | `check_layering.py --strict` 输出「检查通过：无层违规」；`git diff master -- tools/` 只有两个基线文件且只缩 |

## 分题证据

### 1. 层次合规

新文件层标记（全部在头三行，逐文件目检）：

- 数据层（io/）：`src/io/welllogtext.h:1`、`welllogtext.cpp:1`、`welllogtext_convert.cpp:1`、`welllogtext_internal.h:1`、`importclassification.{h,cpp}:1`、`importrecovery.{h,cpp}:1`、`dataimport_log.{h,cpp}:1` —— 均 `// 层：数据`。
- 视图层（ui/）：`src/ui/dialogs/folderconfirm_internal.h:1`、`folderconfirm_preview.cpp:1`、`folderconfirm_results.cpp:1`、`src/ui/pages/datalist_logsummary.cpp:1` —— 均 `// 层：视图`。
- `tests/importuxfixtures.h` 无层标记——层标记纪律只约束 `src/`（AGENTS.md 原文「每个 src/ 文件」），tests/ 不在检查器射程内，`--strict` 通过佐证。

io 新文件无 QtWidgets、无 ui/ include（grep 全量核对）。视图白名单：`git diff master -- src/ui/` 的全部新增 include 仅 catalog/、domain/、services/、ui 内部与 Qt 控件头，**零 io/ 引用**（连白名单内的 lasdoc.h 都没用到），metadata/、algorithms/ 零引用。

### 2. 是否重复造轮子

**XML：复用，未重写。** `src/io/welllogtext.cpp:157` 调 `paleo::io::readSpreadsheetMlWorkbook(path, bytes)`；该函数是本 PR 在 `src/io/outsourceworkbook.cpp:741-752` 新加的 12 行薄适配器，内部直接转调既有 `readSpreadsheetMl`（outsourceworkbook.cpp:93 起，同一套 QXmlStreamReader 核 + 单元格/大小安全检查）。welllogtext.cpp 里唯一的 XML 代码是 6 行的 `sniffSpreadsheetXml` 嗅探（welllogtext.cpp:131-136），不是解析器。`src/io/importclassification.cpp:18-33` 另有一处流式扫 Worksheet 表名——只读表名做分类决策，不解析数据，职责与解析器正交，可接受。

**文本 CSV：自足实现，存在轻度平行。** `welllogtext.cpp:38-65` 的 `Detail::split`（引号感知、单行、带列数上限）与 `src/io/cuttingsdoc.cpp:262-429` 的 RFC 4180 状态机在概念上重叠。但 cuttingsdoc 的解析器藏在匿名命名空间（cuttingsdoc.cpp:503/510），不导出、且支持跨行记录（井曲线文本不需要）；本 PR 选择写 25 行单行分列而非把 cuttingsdoc 内部状态机提升为公共组件，属于合理的局部性取舍。**记为「既有轻度重复未动」，非本 PR 新增债。**

### 3. 抽象是否过度

逐个核对公共 API 面与调用方（调用方计数均为 src/+tests/ 实调，不含声明/定义自身）：

| 导出函数 | 调用方 | 判定 |
|---------|--------|------|
| `inspectLog` | dataimport_file.cpp:210 | 用 |
| `fillLogPreview` | dataimport_folder.cpp:117,148 | 用 |
| `importInspectedLog` | dataimport_wells.cpp:181 | 用 |
| `logFileDigest` | dataimport_folder.cpp:502；dataimport_file.cpp:170,208 | 用 |
| `validateDuplicateLogTarget` | dataimport_folder.cpp:513；dataimport_file.cpp:275 | 用 |
| `writeImportIntent` | dataimport_session.cpp:293；dataimport_file.cpp:83,89；ingestplan.cpp:967 | 用 |
| `recoverImportIntents` | dataimportservice.cpp:104 | 用 |
| `classifyImportFile` | dataimport_file.cpp:167；ingestplan.cpp:175,510；folderimport.cpp:72 | 用 |
| `WellLogText::sniffText / sniffSpreadsheetXml / parse` | welllogread.cpp:44,50,21；importclassification.cpp:14 | 用 |
| `WellLogRead::parse`（本 PR 新增） | 9 处 src 调用（dataimport_log/crossplotsources/welllogset/petrophyscompute/previewdoc 链、propertymodelworkflow、wellpredictionservice、wellpredictionreview、mappingworkbench_predict、sectionworkbench、wellsectionworkflow） | 用足 |

DTO 字段核对：`FolderPreviewRow` 新增 9 字段（src/domain/importrows.h:37-45）全部被 fillLogPreview 写、folderconfirm 视图读；`ImportSession` 新增 7 字段（dataimportservice.h:423-432）全部接入提交/恢复链；`LogInspection`（dataimport_log.h:10-16）4 字段全用。**未发现为用不上的未来需求预留的字段或函数。**

两处备注（非阻塞）：

- `WellLogRead::parseCurves`（welllogread.h:59）当前零调用方，但 **master 上已存在**（master:src/io/welllogread.h:51），非本 PR 新增。
- `PaleoFolderConfirm::Hooks` 呈三档递进（`importAll` → `importAllWithForced` → `importAllWithWells`，folderconfirm.h:40-59），生产壳只接最后一档，前两档仅测试与 fallback 路径使用（folderconfirm_results.cpp:86-89）。注释明言 additive 兼容策略（folderconfirm.h:45-47）；属于有意的向后兼容面，建议后续 PR 在旧壳退场后收档，不记为本 PR 的债。

### 4. 接口收敛 vs 扩散

**收敛为真，且净拆除存量旁路。** master 上 `src/workflow/propertymodelworkflow.cpp:1150` 与 `src/workflow/mappingworkbench_predict.cpp:495` 直连 `LasParser::parse`；本 PR 分别改为 `WellLogRead::parse`（新 propertymodelworkflow.cpp:1153、mappingworkbench_predict.cpp:507）。当前 `src/` 内绕过 WellLogRead 的解析级直连只剩：

- `src/selfcheck/perfgroup.cpp:63,89,94` —— 测试壳层的 LAS 专项性能基准（注释明言「LasParser::parse 旧 QTextStream 路径」），既有、有意保留；
- `LasParser::depthUnitToMeters` 三处（petrophyscomputeservice.cpp:453-454、propertymodelworkflow.cpp:1170、mappingworkbench_predict.cpp:508）—— 静态单位换算工具，master 已有，非读取入口。

本 PR **零新增旁路**。WellLogRead 六个入口（detect/readWellInfo/parse/parseHeader/parseCurves/parseDoc）全部走同一 `detect()` 内容嗅探分派（welllogread.cpp:34-59），XML/文本/未知格式统一汇到 `parseAdditional`（welllogread.cpp:17-31），失败面如实给因，无吞错。

### 5. 依赖与配置

- `CMakeLists.txt` 仅 +2 行（`CMakeLists.txt:1564-1565`），把两个新 extra 文件挂进 `_paleo_extra_manifest`，符合「显式清单登记替代隐式 GLOB」纪律。
- `cmake/extra-well-logtext.cmake`、`cmake/extra-import-ux.cmake` 只向既有 `paleo_io`/`paleo_ui` 静态库加源文件、按 `add_paleo_test` 注册 4 个测试（LIBS 给最小链接集）。**无新第三方依赖、无新顶层模块。**
- 无新顶层模块 ⇒ `tools/layering_vocab.json` 无需更新，本 PR 未动它，正确。

### 6. 规模与分层债

- `tools/file-budget-baseline.txt`：删 1 行（`folderconfirm.cpp:fn:buildFolderConfirmDialog:346`，函数已拆解下线）、2 行降值（datapreviewtabwelllog 278→263；datalist_tree file 1094→1059、buildNavTreeSpec 676→638）。91→90 行，只缩不涨。
- `tools/ui-size-baseline.txt`：datalist_tree 1094→1059，只缩不涨。qgs-audit/qgis-spread/transitive 基线未动。
- **folderconfirm 拆分账**：master 单文件 670 行、含 346 行超限函数；拆分后 `folderconfirm.cpp` 406 行（装配）+ `folderconfirm_internal.h` 50 行（共享 DialogState）+ `folderconfirm_preview.cpp` 188 行（预览/选井）+ `folderconfirm_results.cpp` 172 行（执行/结果/重试），合计 816 行（净 +146）。+146 的去向：新增能力（数据预览窗、逐行选井下拉、取消导入、expectedHash 核验列、仍导入改判）+ 拆分固有成本（内部头、重复 include 面）。单文件峰值 670→406、超限函数条目从基线删除，职责按「装配/预览/执行」三段切开——**是真降复杂度，不是搬家**。datalist_tree 同理把日志摘要抽出为 `datalist_logsummary.cpp`（50 行），触发 ui-size 基线降值。

### 7. 与层契约检查器一致

- `C:/Users/wangj.KEVIN/paleo-qgis-deps/python.exe tools/check_layering.py --strict` → 输出 **「检查通过：无层违规。」**
- `git diff master --stat -- tools/`：仅 `file-budget-baseline.txt`（+3/−4）与 `ui-size-baseline.txt`（+1/−1），均为基线收缩，无检查器改动、无词表改动。

## 架构债增减清单

### 本 PR 消除的债

1. **读取入口旁路 ×2**：workflow 层两处直连 `LasParser::parse`（propertymodelworkflow、mappingworkbench_predict）收进 WellLogRead 门面。
2. **规模债 ×3**：file-budget 基线删 1 行 + 降 2 行；ui-size 降 1 行（folderconfirm 346 行函数消失、datalist_tree 跌破 1094）。
3. **格式假能力**：detect 对未知内容从「按 LAS 兜底猜」改为明确 `Unknown`（welllogread.cpp:47,58），导入面如实报不支持。

### 本 PR 新增的债（均为轻微）

1. **Hooks 三档递进兼容面**：`importAll` / `importAllWithForced` 只剩测试与 fallback 在用（folderconfirm.h:40-59）。有注释声明 additive 策略，待旧壳测试收敛后应删档收编。
2. **（观察项，非违例）** `FolderPreviewRow` 加 9 字段后接近「预览视图模型」上限，目前字段全被消费；再涨应考虑拆分。

### 故意没动的债（符合「最小必要复杂度」）

1. **CSV 解析轻度平行**：cuttingsdoc 匿名命名空间 RFC 4180 状态机 vs welllogtext 单行引号分列，未强行公共化——语义不同（跨行记录 vs 单行表格），公共化收益不抵改动面。
2. **`WellLogRead::parseCurves` 零调用**：master 既有，本 PR 不背。
3. **selfcheck 直连 LasParser 性能基准**：测试壳豁免 by design。
4. **`LasParser::depthUnitToMeters` 工具直连**：单位换算静态函数，非读取入口，master 既有口径。

## 总结论

PR #603 在架构维度做到了最小必要复杂度：新格式支持复用既有 XML 核、门面收敛真实发生且净拆存量旁路、公共 API 无死代码、基线只缩不涨、`--strict` 层检查干净。唯一建议跟进项是 Hooks 三档兼容面在旧测试壳退场后的收档（轻微，不阻塞合并）。
