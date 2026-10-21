# 数据管理导入 UX / 设计系统独立评审（PR #603）

评审人：独立 UX/设计系统评审（只读）。对象：worktree `data-management-import-ux`，HEAD `9aba6893`（`feat(io): 统一测井数据读取与智能导入`），基线 origin/master `e6837e14`。日期：2026-10-20。
依据：`DESIGN.md`（token/错误呈现/动效）、`docs/UI_LAYER_PLAN.md`（W2 确认表契约、三层边界）。

## 逐条结论

### 1. 三步流程 —— 达标

代码路径：`runFolderImportAt`（`src/ui/paleomainwindow_project.cpp:60-112`）→ `previewFolderAsync` 任务池扫描 → `PaleoFolderConfirm::buildFolderConfirmDialog`（`src/ui/dialogs/folderconfirm.cpp:319`）→ `DialogState::buildPreview`（`folderconfirm_preview.cpp:65`）→ `updateAvailability`（`folderconfirm_preview.cpp:144`）→ `beginImport`（`folderconfirm_results.cpp:60`）→ `applyResults` 逐行回写（`folderconfirm_results.cpp:93`）。

无歧义场景（类型全部识别、井名唯一匹配或文件内自报新井）实测操作步数：

1. 选目录/文件（系统文件对话框）；
2. 确认表内检查（自动选中首行并显示内容预览，`folderconfirm_preview.cpp:122-125`；类型下拉已预填，无需触碰）；
3. 点「确认导入」，结果回写原表，「取消」变「关闭」（`folderconfirm_results.cpp:133`）。

**导入 = 3 步（选择→预览→确认），必填字段 0 个**——类型由分类器预填、目标井在能唯一判定时不需要任何输入（`dataimport_log.cpp:131-150`）。仅当井身份无法判定时每行多一次下拉选择（确认键禁用并带 reason tooltip「请为 %1 项测井数据选择目标井」，`folderconfirm_preview.cpp:179-186`，符合 DESIGN.md 禁用带 reason 的 §35 契约）。

### 2. 技术概念泄漏 —— 基本达标，两处轻微

- 确认表、估算文案、结果摘要、数据树新增文案均无 staging/journal/session/sha256/内部 id 字样；「暂存」仅出现在兜底错误「导入暂存尚未准备完成，请重试。」（`dataimportservice.cpp`，中文自然语）。
- 「高级选项」：新对话框**没有**新增高级选项区，逐行类型下拉/「仍导入」是主流程控件，不存在应折叠而未折叠的区块。
- 轻微泄漏 1：数据树 tooltip 显示「版本：v%2」（`src/ui/pages/datalist_logsummary.cpp:36-37`）。DESIGN.md AI 卡片节认可「节点悬停显示版本信息（tooltip，同数据页）」，属既有口径，可接受，但建议版本号只在 tooltip、不进主文本（当前已如此，维持）。
- 轻微泄漏 2：`folderTypeLabel` 对词表外类型**裸显内部 id**（`folderconfirm.cpp:47`，`kLabels.value(type, type)`）。当前分类器词表全覆盖，属防御性兜底；建议兜底显示「未知」而非英文 id。

### 3. 错误文案质量 —— 良好，1 处空文案残留

抽查 10+ 条（`src/io/dataimport_log.cpp`、`src/io/welllogtext.cpp`、`src/io/welllogtext_convert.cpp`、`src/ui/dialogs/folderconfirm_results.cpp`、`src/workflow/folderimport.cpp`）：

| 文案 | 位置 | 文件名/位置 | 恢复建议 | 判定 |
|---|---|---|---|---|
| 「%1：没有可用的索引和测井曲线，请检查数据表头。」 | dataimport_log.cpp:57 | 有 | 有 | 好 |
| 「%1：第 %2 个索引不是有限数值，请检查源文件。」 | dataimport_log.cpp:66 | 有（精确到采样点序号） | 有 | 好 |
| 「预览期间文件内容发生变化，请重新选择文件生成预览。」 | dataimport_log.cpp:125 | 行内 | 有 | 好 |
| 「与已有曲线同名：%1。确认后保留两份并标明来源，不覆盖已有数据。」 | dataimport_log.cpp:147 | 有（曲线名） | 有（后果说明） | 好 |
| 「%1，第 %2 行：%3。请检查文件后重试。」 | welllogtext.cpp:31 | 有（文件+行号） | 有 | 好 |
| 「缺少明确索引列 DEPTH/DEPT/MD/TVD/TVDSS/TIME/深度」 | welllogtext_convert.cpp:108 | 有 | 有（列出合法表头） | 好 |
| 「曲线名称重复：%1；请区分来源后重试」 | welllogtext_convert.cpp:117 | 有 | 有 | 好 |
| 「无法识别测井格式。请提供 LAS/DLIS/LIS、综合柱状图 XML，或带明确索引表头的 CSV/TXT/TSV。」 | welllogread.cpp（新增） | 有 | 有（列出受支持形态） | 好 |
| 「已识别测井曲线，但无法确定所属井。文件名仅作线索，请选择目标井或新建井。」 | dataimport_log.cpp:157 | 有 | 有 | 好 |
| 失败行兜底「导入失败」（message 为空时） | folderconfirm_results.cpp:18、folderimport.cpp:420 | 只有行路径 | **无** | **空文案残留** |

结论：10 条抽查中 9 条含文件名/位置/恢复建议且为自然中文；`「相对路径：导入失败」`式空文案兜底仍在（两个位置），见发现清单 F1。

### 4. 设计系统符合性 —— 达标

- 新 UI 文件全部走 `PaleoTheme`：错误红用 `PaleoTheme::tokens().errorText`（`folderconfirm.cpp:379-381`），次级说明用 `PaleoTheme::mutedCaptionStyleSheet()`（`folderconfirm.cpp:339,368`），预览等宽字体用 `PaleoTheme::monoFont()`（`folderconfirm_preview.cpp:71`），重试行内边距用 `tokens().spacingXs`（`folderconfirm.cpp:215`）。
- 全量 grep 新文件无 `setStyleSheet` 硬编码、无 `QColor(...)` 字面量、无 px/pt 魔数；预览高度由 `fontMetrics().lineSpacing()*8` 推导（`folderconfirm_preview.cpp:72`）；对话框 `resize(860,520)` 与既有对话框同口径（`geologicalexportdialog.cpp:106` 1100×720、`storagegovernancedialog.cpp:67` 900×560）。
- 数据树未决节点用 `PaleoTheme::setItemTextColor(..., ItemTextColor::Warning)` token（`datalist_tree.cpp:737-740`）。
- Accessibility：确认表 `accessibleName`/`accessibleDescription`（`folderconfirm.cpp:344-347`）、重试钮「重试导入 %1」（`folderconfirm.cpp:220-221`）、仍导入钮（`folderconfirm_preview.cpp:103`）、目标井下拉「%1 的目标井」（`folderconfirm_preview.cpp:28`）、内容预览（`folderconfirm_preview.cpp:69`）、查看未决（`folderconfirm.cpp:391`）。确认/取消由按钮文本承担可访问名，符合 Qt 惯例。

### 5. 交互一致性 —— 达标，1 处行为-文案耦合

- 按钮布局：`QDialogButtonBox` AcceptRole「确认导入」(setDefault) + RejectRole「取消」+ ActionRole「查看未决」（`folderconfirm.cpp:384-392`），与 `geologicalexportdialog.cpp:90-95`（Save|Cancel）同角色语义；完成后取消键改「关闭」（`folderconfirm_results.cpp:133`）。
- Esc/关闭 → `rejected` → `cancelImport`（`folderconfirm.cpp:398-400`），与 UI_LAYER_PLAN「对话框中途关闭结果弃置（QPointer guard）」契约一致（`folderconfirm_results.cpp:84`）。
- 数据树双击/右键未改动既有行为（双击走 `itemActivated` 单通道，`datalist.cpp:426-430` 注释明确防重发；右键 `showAssetContextMenu`，`datalist.cpp:909-911`）；本次树改动只是把 43 行内联渲染抽成 `describeWellAssetNode`（`datalist_tree.cpp:181`）+ 测井节点摘要增强（`datalist_logsummary.cpp`）。
- 不一致点：「同名曲线」引导语靠**翻译文本子串匹配**触发（`folderconfirm_preview.cpp:85`，`issues.join().contains(QObject::tr("同名"))`）——行为耦合在译文上，换语言/改措辞即静默失效，见 F4。

### 6. 反馈与可恢复 —— 基本达标，3 处缺口

- 进度：批量导入走 `PaleoTaskService` 任务（扫描段跑马灯+文件名 detail、执行段 `reportBytes` 行进度，`folderimport.cpp:190-204`），符合 DESIGN.md Motion「任务进度走底部任务面板」；对话框内同步给「正在导入；完成后逐项显示结果。」提示（`folderconfirm_results.cpp:79`）。
- 取消：即时可见——取消钮导入中保持可用，点击即关对话框并经 `hooks.cancelImport` → `PaleoTask::requestCancel` 中止；取消/worker 失败整批不提交，会话报「已取消，本批数据未入库」。
- 重试就地可达：失败行内嵌「重试」钮（`folderconfirm.cpp:210-227`）；整批失败（服务未就绪等）错误原文展示并恢复可编辑态可再次确认（`folderconfirm_results.cpp:96-104`）。
- 缺口 1：**行级重试在 GUI 线程同步执行**（`folderimport.cpp:370-424` `runInlineImport` 不经任务池），大文件重试会冻结对话框，见 F2。
- 缺口 2：「查看未决」只在 `applyResults` 按当时结果露出（`folderconfirm_results.cpp:132`）；`retryRow` 把最后一个未决行修好后**不刷新该钮可见性**（`folderconfirm_results.cpp:140-171`），露出条件与实际未决数据失同步，见 F3。
- 缺口 3：导入中关对话框即弃置结果且无确认提示（设计契约如此，但用户无预警），建议性发现 F6。

### 7. i18n —— 达标

- `C:/Users/wangj.KEVIN/paleo-qgis-deps/python.exe tools/check_i18n.py` → **通过**（「文案入口无裸字面量」，exit 0）。
- 新文案全部走 `tr()` / `QCoreApplication::translate("WellLogImport"/"WellLogText"/...)`（io 层无 QObject 基类，用 translate 符合层惯例）；静态表用 `QT_TR_NOOP`（`datalist_importbuttons.cpp:10-16`）；`folderconfirm.cpp:29-31` 注释正确说明了静态表首次调用求值的翻译时机。
- 中英混排符合仓库惯例（「LAS/DLIS/LIS、综合柱状图 XML，或带明确索引表头的 CSV/TXT/TSV」与 DESIGN.md 2026-10-09 决策条同款写法）。
- 附带发现：`DataImportService::offThreadError()` 是 `QStringLiteral` 裸串且含内部 API 名「beginImport/produce*/commitImport」（`src/io/dataimport_session.cpp:257-261`，**master 既有**），本 PR 新增的 fail 路径（`folderimport.cpp:221`、`:379`）把它带进了确认表错误区，见 F5。

## 发现清单

| # | 严重度 | 证据 | 建议 | 处置 |
|---|---|---|---|---|
| F1 | 中 | 失败兜底空文案「导入失败」：`folderconfirm_results.cpp:18`、`folderimport.cpp:420` | 兜底至少说明「未返回具体原因，请查看错误历史或重试」；更优是 producer 侧保证 Failed 必带 message | 建议（本 PR 收敛） |
| F2 | 中 | 行重试 GUI 线程同步：`folderimport.cpp:370-424`（`importFolderRow`→`runInlineImport`），大文件解析/复制冻结对话框 | 行重试接入 `PaleoTaskService`（复用 `makeImportJob` 单行口径），或在重试期间置忙+禁交互 | 建议（可后续 PR） |
| F3 | 中 | 「查看未决」露出条件不随重试刷新：`folderconfirm_results.cpp:132` 只在 `applyResults` 设置；`retryRow`（:140-171）未重算 | `retryRow` 末尾按 `results` 重算 unresolved 并 `showUnresolved->setVisible(...)` | 建议（本 PR 收敛，一行级修复） |
| F4 | 低 | 行为依赖译文子串：`folderconfirm_preview.cpp:85` `contains(QObject::tr("同名"))` | 用结构化标记（issue category/布尔字段）替代文本匹配 | 建议 |
| F5 | 低 | `offThreadError()` 裸串+内部 API 名泄漏：`dataimport_session.cpp:257-261`（master 既有，本 PR 新增导入失败路径把它显示进对话框） | 文案改走 translate 并用户化（如「导入服务当前不可用，请重试」），API 名留 qWarning | 建议（另开 issue，非本 PR 引入） |
| F6 | 低 | 导入中 Esc/关闭即弃置结果、无确认：`folderconfirm.cpp:398-400` + `folderconfirm_results.cpp:84` | 导入中把关闭语义改为「取消导入并关闭」的文案提示（hint 或 cancel 钮 tooltip 明示） | 建议 |
| F7 | 低 | 词表外类型裸显内部 id：`folderconfirm.cpp:47` | 兜底显示「未知」/「其他」中文文案 | 建议 |
| F8 | 低 | 行 tooltip 大小裸字节（`folderconfirm_preview.cpp:78-79`「大小 %1 字节」），与估算行的人类可读 MB/GB（`folderconfirm.cpp:291-303`）不一致 | 复用 humanSize 格式化 | 建议 |

**无必须修（blocking）项**：七条评审面无设计系统违规、无分层违规（新 UI 文件头三行层标记齐全，i18n/裸字面量闸门通过）、无技术词主文案泄漏。F1/F3 建议本 PR 内顺手收敛（均为小改动），F2/F5 可递延。
