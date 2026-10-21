# PR #603 修复轮独立复审（Review–Fix–Review 闭环，只读）

对象：worktree `codex/data-management-import-ux`，HEAD = 42c6e6e7；
复审目标 = 工作树未提交改动（`git diff` vs HEAD，18 文件，+416/−39）。
基线报告：`docs/progress/data-import-ux-security-review.md`（第六轮，F1/F2/F4/F5 高危）。
方法：只读静态审查——逐条对照安全报告的缺陷证据核对修复代码真实路径；
未编译、未运行测试；跑了 7 个只读源码闸门。本报告是唯一写入产物。

## 逐条修复有效性结论

### F1 执行期重扫（预览≠执行计划）——有效

- 差分真实跳过未预览路径：`src/io/dataimport_folder.cpp:403-431`
  （`produceFromPlan` 入口，`previewedPaths` 非空时按路径集过滤 `plan.items`，
  多出的路径落 `Skipped` 行并提示「该文件在预览后才出现……请重新生成预览」）；
  过滤发生在 typeOverrides/forceImport 改判之前，晚到路径无法被任何确认表
  操作复活。预挂行在 `sess.rows += executedRows`（:466-467）得以保留。
- 空 `previewedPaths` 完全保持旧行为：唯一闸门是 `:406` 的
  `!sess.previewedPaths.isEmpty()`；服务级 importFolder/importFiles、
  importFolderRow、runInlineImport 缺省参数 `{}` 路径零行为变化
  （`src/workflow/folderimport.cpp:374,436,449,512` 全链默认值贯通）。
- 确认表传入预览全集：`folderconfirm_results.cpp:88-95`（beginImport 收集全部
  preview 行 path，含 skipped/decision=skip 行）→ Hooks（folderconfirm.h:58）→
  paleomainwindow_project.cpp:82-90/148-157 两处接线 → workflow 双路径
  （异步 job lambda folderimport.cpp:465-471/528-534 + 同步 runInlineImport:385）。
  预览里 decision=skip 的行在 previewedPaths 内，仍在 plan 中按原逻辑跳过——
  不误伤。
- 结果表无预览行的 Skipped 行显示进 errors 区：`folderconfirm_results.cpp:120-130`
  （`row < 0 && outcome == Skipped` → unreviewed 清单）+ `:148-158`（追加进 errors
  区，不覆盖已有失败清单）。F1 行恒为 Skipped 故必然被显示；「预览后消失的文件」
  不产生结果行，预览行保持默认 `Outcome::Skipped`（importrows.h:56）——与旧行为
  一致，不误伤。plan.skipped（枚举期跳过）晚到项同理落入 unreviewed 提示且本就不
  入库，F1 承诺（未审阅文件零入库）对两条路径都成立。
- commit 面零副作用：预挂 Skipped 行不进 journal；failSession 只翻
  Imported/Unresolved 行（dataimport_session.cpp:393-401），Skipped 行不受影响。

### F2 真哈希（ShaCache 陈旧误去重）——有效

- importOneFile 全类型改 `logFileDigest`：`src/io/dataimport_file.cpp:163-172`；
  全函数已无 ShaCache 读（仅剩 :177 的 invalidate——expected 不符时自愈）。
  dedup（:272 `versionBySha256(sourceSha)`）与 AlreadyStored 出口现在只吃真哈希，
  井表流式复验（:262 streamHash vs sourceSha）两侧同为新鲜读，误报面同步消除。
- plan 期命中复核：`src/io/ingestplan.cpp:570-587`——只在缓存哈希命中库内版本
  （`!vid.isEmpty()`）时做 `logFileDigest` 真复核；失真则换真值重查 + invalidate
  自愈。未命中不触发全读，plan 期哈希循环（:535-548）仍走 ShaCache——**没有引入
  逐文件全读回退**，性能口径保持（200MB 闸 kPlanHashLimitBytes 不变）。
  `vid` const→可变：仅在 fresh≠cached 分支内重查赋值，后续 duplicateOf 标记逻辑
  逐字不变，语义保持。
- produceItem 失配 invalidate：`dataimport_folder.cpp:535-540` ✓。
- 全类型行带 sourceSha256：`dataimport_folder.cpp:205-207`（previewRowsFromPlan
  先赋 plan 期 sha）；well_log 行随后被 `fillLogPreview`（dataimport_log.cpp:128）
  以真哈希**覆盖**——调用顺序为 previewRowsFromPlan → fillLogPreview
  （dataimport_folder.cpp:117,148），覆盖在后，无冲突，well_log 仍拿最强指纹；
  非 well_log 行首次获得执行期指纹闸。

### F4 新建井撞名——有效（附两条行为说明，见发现 N2/N3）

- io 层：`dataimport_log.cpp:170-183`——`new:` 前缀先扫全部 well 实体做
  `name.trimmed().compare(name, Qt::CaseInsensitive)` 精确匹配，命中即如实拒绝
  （文案指引改选已有井）。口径与 ensureWell 快速并入条件
  （dataimport_log.cpp:38-40 同一 compare）完全一致——ensureWell 会并入的情形
  先被预检拦截，静默并入通道关闭；与 validateDuplicateLogTarget（:219-240）的
  拒绝语义现在一致（同源 compare）。
- UI 层：`folderconfirm_preview.cpp:51-66`——「新建其他井…」输入撞名时
  `setCurrentIndex(existIdx)` 改选已有井 + errors 区提示。重入分析：
  setCurrentIndex 触发 currentIndexChanged → 重入体读到的 currentData 是已有井
  id（非 `__new__`）→ 直接落 wellTargets 后返回，不再触碰索引——**单层重入，
  无死循环**；外层随后只写提示文案即 return，不覆盖重入写入的 target。
- 组合框初始为占位项（folderconfirm_preview.cpp:30，data 为空），wellTargets 只在
  用户主动改选时写入——默认不动 combo 的自动归井路径（ensureWell 内容井名）
  完全不受本次修复影响。

### F5 坐标回填——有效

- `CatalogOp::UpdateEntity` 确实追加在枚举末尾（datacatalog.h:112-114，在
  UpdateVersionExtra 之后），注释说明了 ABI 混链原因；journal 为内存结构
  （m_journal，datacatalog.cpp:138-145），无磁盘序列化口径问题。
- `updateEntity`（datacatalog_entities.cpp:40-72）逐字镜像 addEntity 模式：
  checkWriteThread → ensureOpen → id 校验 → entityType 不一致拒绝 → 就地换行 →
  dirty → **save 成功才 recordOp**；save 失败恢复旧行（:68-71）。行位与 (id,type)
  键不变，索引无需重建——entityType 守卫对 m_idx.entityRowsByType 是必要的，
  对「就地更新」语义是充分的。
- applyJournal 重放：datacatalog.cpp:312 case 已接；重放整体包在
  beginBatch + 全量快照 restore 内（:280-300），失败原子回滚。staging 副本上的
  recordOp 位置正确（save 之后），commitImport 取 `s.cat->journal()`
  （dataimport_session.cpp:438）重放——链条闭合。
- 回填逻辑（dataimport_wells.cpp:134-161）与新建井分支（:166-196）的坐标/
  georeference/extra 处理逐字一致（ok/untransformed、projectLon/Lat）。
  回填条件 `unknown || !hasSurface`：全仓 coordinateStatus 写点核对
  （log 建井 unknown+无 hasSurface；wellhead 建井/回填 ok|untransformed+
  hasSurface；wellsiting untransformed+有坐标）——不存在 hasSurface=true 且
  status=unknown 的活状态，**不会误覆盖有坐标的井**；同文件多行命中同一井时
  首行回填后条件即转否，不会重复更新。

### UX 两项——有效

- showFailures 兜底文案（folderconfirm_results.cpp:17-20）+ importFolderRow 兜底
  （folderimport.cpp:418-424，带文件路径）——正对应 UX 评审 F1 点名的两个位置，
  空 message 场景不再有「相对路径：导入失败」式空文案。
- retryRow 后 showUnresolved 刷新（folderconfirm_results.cpp:196-201）——按
  results 全量重算 Unresolved，与 applyResults 的 `unresolved` 累加口径一致
  （默认填充行 outcome=Skipped 不产生假未决），正对应 UX 评审 F3 建议的修法。

### 顺手项——有效

- ensureWell 规范化后为空拒绝（dataimport_log.cpp:30-36）：normalizeWellName
  只去空白/`-`/`_` 并小写（datacatalog_entities.cpp:102-113），CJK 字符保留——
  不会误拒中文井名；只有 `-`、`___` 这类纯符号名被拦（即 F6 记录的场景）。
- importFolderRow 头注释（dataimportservice.h:119-122）：写明「无预览指纹复核」，
  正是安全评审 F3 残留面建议的声明。

### 测试——4 个新故障测试均真实钉住目标行为

- `lateArrivingFilesAreSkippedNotImported`（tst_import_ux.cpp:410-453）：预览 1 行 →
  落盘晚到文件 → produceFolder 断言 2 行、first=Imported、late=Skipped+「预览后才
  出现」文案、commit 后 B1 井不存在/资产 1/raw 1。修复前晚到文件会被 Imported，
  断言必红。✓
- `staleCacheDoesNotSuppressChangedBytes`（:455-500）：先构造真陈旧缓存
  （invalidate→prime→同尺寸换内容→setFileTime 拨回 mtime；QFile 截断写保 inode，
  POSIX/Windows 指纹均不变），并先断言缓存确实陈旧（stale==旧哈希≠真哈希）再重导——
  断言 Imported、新 assetId、资产 2、同名曲线两来源。修复前走 ShaCache 必中
  AlreadyStored，必红。✓（钉的是 importOneFile 真哈希；ingestplan 命中复核分支
  无直接测试，见 N4。）
- `explicitNewWellCollidingExistingNameIsRejected`（:502-536）：`new:A1` 撞已有 A1 →
  Failed+「已存在」+ commit 后 A1 井 1/资产 1/B1 未建/raw 1。修复前 ensureWell
  静默并入，必红。✓
- `wellheadImportBackfillsLogCreatedWellCoordinates`（:538-570）：先断言建井后
  unknown+无 hasSurface，再导井位表（「井位」段 → well_head 分类经
  arearules.cpp:336-339 segmentKeywords 确认；SMI 格式与 tst_import.cpp:2560 既有
  夹具同构）断言 x/y/kb/td 全值 + 状态转非 unknown，**reopen 后再断言持久化**——
  journal 重放链也被钉住。修复前无 updateEntity，必红。✓
- tst_panels.cpp 7 处 lambda：6 处纯增参适配；第 7 处（:497-507）把 `previewed`
  透传进 workflow.importFiles，同步验证了确认框→workflow 新参数链路可编译可运行，
  仍属机械适配范畴。

### 回归风险——未发现

- 全量 diff 逐文件过：18 个文件全部对应 F1/F2/F4/F5/UX/顺手项/测试/文档
  （docs/import-well-logs.md 新增三段用户向说明与实现行为一致），无无关改动。
- 只读闸门（本 worktree 实测）：`check_layering.py --strict` 通过；
  `check_i18n.py` 通过；`check_ui_invariants.py` clean；`check_ui_size.py` clean。
  file_budget / qgs-audit / qgis-spread / ui_tokens 四项各有 +1/+2/+2/+1 的
  基线差，但涉案文件（constraintpage_interpolation.cpp、maptools/identifypopup.cpp
  等）均不在本 diff、且修复 diff 零 qgs include——属分支 HEAD 既有状态，非本轮
  引入（见 N5）。

## 新发现问题清单

| 序 | 严重度 | 问题 | 证据 |
|----|--------|------|------|
| N1 | 低 | `QString shaErr;` 声明残留未使用（F2 改动删掉其唯一消费者）——编译告警，不挡构建（仅 -Werror=narrowing） | src/io/dataimport_file.cpp:169 |
| N2 | 低 | F4 行为变更未钉测试/未写文档：同批两个文件都被用户显式选「新建井：X」时，第二行执行期被拒（修复前静默并入新建井 X）。默认自动归井路径不受影响（组合框初始为占位项，folderconfirm_preview.cpp:30）；拒绝文案「请重新预览并选择『已有井』」在重新预览后井已存在、流程可走通，方向安全，但建议补一条测试或文档声明 | src/io/dataimport_log.cpp:170-183 |
| N3 | 低 | UI 层 F4 撞名改选分支无自动化测试（tst_panels 未覆盖 folderconfirm_preview.cpp:51-66）；安全评审测试表第 3 项的「UI 层撞名禁用」以「改选+提示」语义实现，满足「或显式合并提示」，但缺钉住 | src/ui/dialogs/folderconfirm_preview.cpp:51-66 |
| N4 | 低·行为说明 | 「缓存陈旧但未命中库内版本」情形：预览指纹取缓存旧值 → 执行期真哈希不符 → 行 Failed「文件内容与预览时不同」+ invalidate 自愈，用户需重新预览一轮。fail-safe 方向、零数据损失，属修复的已知保守后果（plan 期复核只覆盖缓存命中库内版本的分支）；该 ingestplan 复核分支本身亦无直接测试 | src/io/ingestplan.cpp:570-587 |
| N5 | 信息 | 分支 HEAD 既有闸门基线差（非本轮引入，本轮 diff 未触碰涉案文件、零 qgs include）：file_budget +1（constraintpage_interpolation.cpp:updateEngineRows 230>200）、qgs-audit +2、qgis-spread +2、ui_tokens 1 违规（maptools/identifypopup.cpp:58 setSpacing(2)）——CI lint 车道会报，作者需知晓 | tools/*-baseline.txt 实测输出 |
| N6 | 低·性能 | importOneFile 全类型真哈希后，单文件导入 >200MB 文件（如 SEG-Y）新增一次全量流式读（plan 期 200MB 闸不覆盖 importOneFile）。有界一次性开销，与导入本身的复制开销同阶，可接受 | src/io/dataimport_file.cpp:170 |

## 总结论

F1/F2/F4/F5 四项高危修复**全部有效**，实现路径与安全报告的建议口径一致，
防御层位置正确（io 层兜底 + UI 层提示），空参/旧入口零行为变化；两个 UX 项与
两个顺手项均如实落地；4 个新测试逐个验证过「修复前必红」的针对性。新发现问题
6 条，最高仅「低」：1 个未使用变量残留（N1，建议顺手删掉）、F4 同批双「新建井」
行为变更缺测试/文档（N2）、UI 撞名改选分支缺测试（N3），余为行为说明与既有
闸门状态通报。建议：合入前顺手处理 N1、评估是否补 N2/N3 测试；其余可记入
TODOS。本轮复审未编译、未运行测试；执行验证责任仍在作者。
