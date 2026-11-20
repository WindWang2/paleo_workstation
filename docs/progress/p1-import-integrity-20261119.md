# P1｜归档与表格导入完整性（2026-11-19 开工）

任务：宿主原始数据导入的归档完整性、XML 解析和真实错误回执（#1023/#1028/#1029）。
分支 `codex/paleo-p1-import-integrity-20261119`，基线 `origin/master = 08ca3e52`。

## Scope ledger（开工核对结论）

| 项 | 结论 | 依据 |
|---|---|---|
| #1028 ZIP64 选择性字段 | 基线上仍存在：中央目录 `0x0001` 按固定 +4/+12/+20 读取，`repaired` 不证明必需字段完整 | `src/io/ziparchive.cpp`（基线行 274–309，静态核查） |
| #1029 空 deflate | 仍存在：`uncompressedSize==0` 早退跳过 inflate 与 CRC | `zipExtractEntry`（基线行 450–456） |
| #1023 DTD 策略 | `readSpreadsheetMl` 主循环已拒；流式入口 `readNextStartElement()` 越过前导 token、OOXML 辅助部件（rels/sharedStrings/styles）、workbook.xml/sheet 循环、井斜/时深 `collectSheetRows`、分类扫描与身份探测均无策略 | `outsourceworkbook.cpp`、`wellcompositexml.cpp`、`importclassification.cpp`、`sourceprobe.cpp` |
| 宿主导入闭环 | 复用：`classifyImportFile → readImportWellInput → importOneFile`，produce/commit、staging、rollbackFailedRow、ImportSession.cancelled、台账既有链不动 | `dataimport_file.cpp`、`ingestplan.cpp`、`folderimport.cpp` |
| 并行边界 | P6 占 mapversion*；P2/P3/P5 未见本任务文件冲突；开放 PR = 0 | gh pr list（2026-11-19），协调目录 paleo-parallel-20261119/ |
| 原生 importDirectory 边界 | 明确不覆盖：`ProjectSetupWorkflow::importAreaSources` 走 Data_service 原生模块，本任务只修宿主解析器路径 | `projectsetup_area.cpp`（只读） |

## 依赖与环境

- 受控前缀：deb 闭包路线（`vendor/prefix/usr`，P2 任务并行 fetch；superbuild 前缀早于 #1048 收据机制、无 `.paleo-superbuild-base.sha256`，按纪律不重建共享前缀）。
- 本 worktree：`vendor/superbuild/prefix`、`vendor/onnxruntime`、`vendor/ranuts-document` 以主仓绝对路径 symlink；`vendor/Data_service` 按 gitlink `905b5c1` 独立检出。
- 构建：Ninja RelWithDebInfo，`-j4` 上限（批次纪律），测试 `ctest -j1`。

## 实现记录（W1–W8）

### W1 ZIP64 sentinel 顺序消费（#1028）
- `ziparchive.cpp`：新增 `consumeZip64Extra()`——中央目录 `0x0001` 块按 APPNOTE 6.3.10 §4.5.3 选择性字段顺序消费（usize/csize/offset 各自仅在主字段为哨兵时在场；offset 在场附带盘号，非零盘=跨盘明确拒绝）。必需字段缺失/块截断/子块长度越界/重复 0x0001 块全部确定失败并带条目名与字段上下文；保留既有无符号减法判界/目录范围闸。
- 测试：`tst_p1_import_integrity` A 组——独立手工夹具矩阵（仅 offset 哨兵、仅 usize、仅 csize、带洞混合、全字段、缺块、截断、重复、子块越界、跨盘）。

### W2 空 deflate 真实解码（#1029）
- `zipExtractEntry`：usize==0 不再早退——256 字节探测缓冲真实 inflate：`Z_STREAM_END` 且零输出才算合法空流；截断/损坏/声明空但产数据失败；CRC 契约（空数据 CRC32==0）照验；失败时 `*out` 保持原值（与非空路径同一约定，测试断言）。
- `out == nullptr` 探测语义保留（只做结构校验，不冒充内容已验证；生产代码无此用法，注释写明）。

### W3 XML 前导 token 策略统一（#1023）
- 新增 `src/io/xmlstreamsafety.{h,cpp}`（QtCore-only）：`rejectDtdToken`（令牌级）、`advanceToRootElement`（代替裸 `readNextStartElement`，DTD/解析错误如实拒绝）、`parseErrorMessage`（源+行列，不回显内容）。
- 接入：SpreadsheetML 主循环（原防线改走统一策略）、流式 `streamSpreadsheetMlWorkbook(QIODevice*)`（修复前导 DTD 被静默消耗）、`readXlsx` 的 workbook.xml 与 sheet 循环、三个辅助部件读取器、井斜/时深 `collectSheetRows`（新增 DTD 拒绝 + hasError 终检——坏 XML 中途截断此前会把部分行当完整结果交回）、`classifyImportFileDetailed` 流式扫描、`sourceprobe` 身份探测。
- 诚实边界：拒绝 DTD 是项目解析策略，不宣称消除所有 XML 资源风险；Qt 自身解析限制保持原样。

### W4 OOXML 部件失败可见
- `readRelationships/readSharedStrings/readDateStyles` 改返回 `PartResult<T>`（完整成功〔允许合法空〕/解析失败〔部件名+行列〕），rels/sharedStrings/styles 在场但损坏 → 整簿拒收（理由：坏 rels 使工作表定位不可信；坏 sharedStrings 把整列字符串格当越界置空；坏 styles 静默改变日期/数值解释）。部件缺失语义不变（rels 缺失记根因 issue；sharedStrings/styles 缺失按空表用，t="s" 格按格报因）。
- `TargetMode="External"` 关系不解析成工作表目标（不触网/本地文件读取），用它的表按「r:id 没有对应目标」如实报因。
- 工作表部件损坏保留兄弟表，issue 带表名+行列（来源可追溯）。

### W5 预算/取消语义
- 预算面保持：previewXlsxWorkbook 的 2 MiB/4 MiB/128 条目/200 行、#810 的 classify 有界回退、4M 格预算均在实际消费处检查；「受限/损坏/成功」三态可辨（limited=true 且 error 空 = 受限；error 非空 = 损坏）。
- 取消面：`ReadCancelCheck`/`WellTableCancelCheck`（默认空，兼容补充）——`readWorkbook/readSpreadsheetMlWorkbook/readWellTable/vectorOrText/readImportWellInput` 行粒度检查；`executeIngestPlan` 把批次 progress 包装成 `ImportSession.cancelCheck` 注入（项结束即清理）；`ImportOptions.cancelled` 供单文件路径。取消结果：`WorkbookReadResult::cancelled`/`WellTable::cancelled` 置位、错误以「已取消」开头，与解析失败严格区分。

### W6 分类→井表→导入贯通
- `WellTable::containerBroken`（`tableFromWorkbook` 在 `!wb.ok` 且非取消时置位）；`classifyImportFileDetailed` 返回 `{cls, containerError, cancelled}`——坏容器（坏 ZIP/XML、超格数预算）携带真实拒收理由；`classifyImportFile` 旧签名语义不变。
- `importOneFile`：分类改走详情版；`containerError` 非空 → 以真实原因 fail（不静默落入通用文件导入/外委簿「有效」状态）；取消 → fail「已取消」+ `s.cancelled=true`。合法非井表工作簿照常 outsource_workbook（回归保护）。

### W7 提交/失败/重试回执
- 复用既有 produce/commit、staging、`rollbackFailedRow`、`ImportSession.cancelled` 协议；本任务把解析失败/取消接入该链（见 W5/W6），未新增平行事务框架。测试覆盖：致命解析失败不返回资产 ID、取消不新增提交、同输入重试成功、wellParseReport（接受行计数）入库、台账行文案（既有 tst_io_importledger 继续覆盖）。
- 原生 `importDirectory` 覆盖边界如实记录：本任务不触碰 Data_service 子模块。

### W8 回归证据
- 新目标 `tests/tst_p1_import_integrity.cpp`（cmake/extra-p1-import-integrity.cmake + 根清单一行）：A–F 六组（详见文件头）。全部走真实生产 API（zipListBytes/zipExtractEntry/readWorkbook/streamSpreadsheetMlWorkbook/parseDeviationSurvey/previewXlsxWorkbook/classifyImportFileDetailed/importProjectFileEx）。
- 夹具独立构造（手工 ZIP/ZIP64 中央目录、raw deflate 字节、SpreadsheetML/OOXML 变异），不依赖本项目 writer 的全字段写法。

## 验证记录

环境：Linux（CachyOS，内核 7.3.4），Qt 6.11/QGIS 4.2.3/GDAL/PROJ 全走受控 deb
闭包前缀（与 P2 worktree 共享只读前缀 `/home/kevin/projects/paleo-p2-seismic-cache-20261119/vendor/prefix/usr`；
本机 `vendor/superbuild/prefix` 早于 #1048 收据机制、无 `.paleo-superbuild-base.sha256`，
按纪律未重建共享前缀）。构建类型 Release（避免 -g 全量 199G 撑爆盘），`-j4/-j2`，
批次纪律内（P4/P5 并行编译期间出现资源争抢，已逐项复证）。

| 验证 | 命令（worktree 内） | 结果 |
|---|---|---|
| 新套件 | `tst_p1_import_integrity` | **36 passed / 0 failed** |
| ZIP 既有 | `tst_io_ziparchive_unit` | 13 / 0 |
| 工作簿既有 | `tst_io_workbook_edges` | 19 / 0 |
| 推导既有 | `tst_io_importrows_derivation` | 4 / 0 |
| 外委簿既有 | `tst_io_outsource` | 15 / 0 |
| 预览既有 | `tst_import_preview` | 24 / 0（1 skipped，既有） |
| 台账既有 | `tst_io_importledger` | 21 / 0 |
| 队列进度 | `tst_importqueue_progress` | 8 / 0 |
| 导入主栈 | `tst_import` | 63 / 0（2 skipped，既有） |
| 导入 UX | `tst_import_ux` | 55 / 0（1 skipped，既有） |
| 数据预览 | `tst_datapreview` | 43 / 0（1 skipped，既有） |
| 目录导入 | `tst_folderimport` | 28 / 0 |
| 版本/工程/映射族 | tst_versions / tst_projectsetup / tst_mapping / tst_mappingworkbench / tst_paleomaplibrary / tst_projectsvc / tst_impactrecompute | 46/0、46/0、17/0、120/0、36/0、22/0、10/0 |

源码闸门（全绿）：`check_layering.py`（普通 + `--strict` + `--selftest`）、
`--symbol-audit`、`--transitive`（显式 `QGIS_PREFIX`，exit=0）、
`check_qgis_spread.py`（新增 0）、`ui_invariants`、`ui_size`、`file_budget`
（新增 0）、`i18n`、`ui_tokens`、`docs_drift`。ctest 无标签闸门项已全量构建并
执行（566 项）。

### 已知限制（如实记录）

1. **first_configure 在本机需显式 triplet**：deb 闭包前缀布局为
   `lib/<triplet>/qt6/...`，本机 CMake 不自动探测 `CMAKE_LIBRARY_ARCHITECTURE`；
   不带 `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu` 时 G1 子构建报
   `lib//qt6/...` 缺失。这是主机环境特性（P2/P4/P5 同样处理），非本任务代码
   问题——单独带该参数复跑 G1 通过（QGIS_INCLUDE_DIR 命中受控前缀、
   compile_commands.json 生成）。
2. **并行构建期的门禁抖动**：P4/P5 并行重型构建期间，`ctest -LE 'core|perf'`
   出现 10 个 300s 超时/段错误（tst_mappingworkbench/tst_projectsvc/
   tst_datapreview/tst_folderimport/tst_paleomaplibrary/tst_versions/
   tst_projectsetup/tst_mapping/tst_impactrecompute + first_configure）；
   资源空闲后逐项单独复跑全部通过（见上表），无一是本任务引入的行为变化。
3. **原生 importDirectory 覆盖边界**：本任务只修宿主解析器路径
   （classifyImportFile → readWellTable/readImportWellInput → importOneFile）；
   新建工程的 `ProjectSetupWorkflow::importAreaSources` 走 Data_service 原生
   模块，本任务不触碰、不断言其已修复。
4. **取消覆盖边界**：行粒度取消已覆盖导入主链（分类 → readImportWellInput →
   readWellTable → readWorkbook/readXlsx/vectorOrText → readSpreadsheetMl）与
   单文件路径（ImportOptions.cancelled）；`streamSpreadsheetMlWorkbook(QIODevice*)`
   的流式读取、井斜/时深 collectSheetRows 与 CSV 快速路径未接取消谓词（非批量
   导入热点，属增量增强）。
5. **DTD 策略边界**：拒绝 DTD 是项目解析策略，不宣称消除所有 XML 资源风险
   （未验证实体炸弹/SSRF/超大内存占用）；Qt 自身解析限制保持原样。

## 独立复审记录

Reviewer（未参与实现的独立 Agent，读真实 diff）结论 **MINOR**：
- MINOR-1 `welltableimport.cpp` 重复死代码行 → 已删；
- MINOR-2 `ziparchive.cpp` 写面注释失实（写面实际不含盘号，28 为整块长度）
  → 已改为准确表述。
- 覆盖边界（流式/井斜时深/CSV 未接取消）已记录在上述限制 4。
- 其余关键点（ZIP64 逐字节推演、空 deflate 外部 zlib 独立验证、DTD 全入口
  覆盖、PartResult 三分、取消生命周期、回归签名、测试可证伪性、分层纪律）
  均经 reviewer 逐条推演通过。复证：修复后 `tst_p1_import_integrity` 36/0、
  `tst_io_importledger` 21/0。
