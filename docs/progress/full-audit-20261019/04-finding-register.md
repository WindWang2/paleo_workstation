# Finding Register（full-audit-20261019）

状态值：OPEN / IN-PROGRESS / FIXED <sha> / RECORD-ONLY / EXCLUDED_EXISTING_ISSUE / EXCLUDED_ACTIVE_WORK / FALSE_POSITIVE / BLOCKED_BY_EXTERNAL_WORK
来源：MA=主Agent独立复核；A/B/C/D/E=subagent。

## G1 簇 — master 门禁回红收敛（MA 发现，Phase 2 基线实测）

依据：BASE_SHA 干净检出上 `ctest -R "layering_qgs_audit|file_budget|ui_tokens|..."` 实测 5 红。
成因：2026-10-18 当天按「不等 CI」政策 admin-merge 的 PR（#549/#552/#565/#566）带入闸门违规。
处置原则（AGENTS.md 基线纪律）：收敛代码而非补登基线；视图原生控件走 qgs_audit_whitelist 显式裁决。

| ID | Sev | Conf | 位置 | 根因 | 修复策略 | 状态 |
|---|---|---|---|---|---|---|
| G1-01 | P2 | VERIFIED | src/ui/maptools/identifytool.{h,cpp} | #552 新增 QgsMapTool 子类直连 qgs* 头且未登记（qgs_audit 新增即红；同目录 8 个同类文件均在初冻基线） | 按批 3 纪律加入 layering_vocab.json qgs_audit_whitelist（带理由：QgsMapTool 子类=视图原生控件，与基线内 paleomaptools/paleoshapetools/sitingpicktool 同族） | OPEN |
| G1-02 | P3 | VERIFIED | src/ui/maptools/identifypopup.cpp:58 | setSpacing(2) 硬编码间距，绕过 DESIGN.md token 体系（spacingXs=4/sm=8/md=16） | 改 tokens.spacingXs（4px，符合设计尺度；变化 2→4px 属对齐设计体系） | FIXED b1076a62 |
| G1-03 | P2 | VERIFIED | src/ui/pages/datalist_tree.cpp（file 1100>1094；buildNavTreeSpec 679>676） | #566 在冻结面内净增 ~10 行（auxiliary sub/expanded 逻辑） | 提取「辅助资料分支」~75 行为 buildAuxiliaryNavSpec() 落新 TU（仿 buildPaleomapNavSpec 先例，fn/file 双收敛） | OPEN |
| G1-04 | P2 | VERIFIED | src/ui/dialogs/folderconfirm.cpp buildFolderConfirmDialog 363>346 | #549/#566 塞入 17 行文档焦点回落内联块 | 提取为文件内静态助手 pickDocumentFocusAsset()（fn 收敛，file 无冻结项） | OPEN |
| G1-05 | P2 | VERIFIED | src/ui/pages/constraintpage_interpolation.cpp updateEngineRows 230>200（新增超标） | #546/#548 在函数内持续追加行（协变量重建+方法可见性清单膨胀） | 提取 rebuildCovariateCombo()/方法可见性助手（目标 ≤200） | OPEN |

## EXCLUDED（主 Agent 复核中排除）

| ID | 位置 | 摘要 | 排除依据 |
|---|---|---|---|
| X-01 | src/workflow/paleomaplibrary.cpp / paleomapfiles / paleomapsource | copyDataset 孤儿文件+QFile::copy 目标冲突；worker 内建 QgsLayer；surveyArea /1e6 单位假定 | PR #568 活跃面（paleomaplibrary.cpp 51 行/paleomapsource.* 全改）→ EXCLUDED_ACTIVE_WORK |

## G1 补充（主 Agent 复核 E-01 后修正/扩展）

| ID | Sev | Conf | 位置 | 根因 | 状态 |
|---|---|---|---|---|---|
| G1-06 | P2 | VERIFIED | tools/transitive-baseline.txt | #564 把 heavy_tus 钉为 87 但其实测即 88（本树与 5a3cbe83 树 TU 集完全一致、edges 恒 52）；+1 TU 归因 #562 拆分文件 ui/paleomainwindow_paleomaps.cpp（#474 先例：拆分自然受染非新增债） | OPEN（带归因 re-pin 87→88） |
| G1-07 | P3 | VERIFIED | tools/file-budget-baseline.txt:63 | #564 将 attachConstraintPage 冻结值 718→**719**（该 PR 未触碰该 cpp；+1 行来自 #563 等自然增长）——「补登转绿」违约 | OPEN（压回 ≤718 并恢复冻结值） |
| G1-08 | P3 | VERIFIED | docs/ci/known-reds.json | E-06：5 条登记红实测已回冻结值内（STALE），与当前红集零交集 | OPEN（逐条复测后删条目） |
| MA-F01 | P3 | ~~HIGH~~ | src/qgis/previewidentify.cpp | **FALSE_POSITIVE（终审证实）**：canvas layers() 实为 index 0=顶层，旧代码本就正确；Fix-1 曾误修，R2-1 发现后已回退（eb39c31f）并钉死契约注释 | CLOSED（假阳性） |
| （修正）E-01 部分 | — | — | ui_qtwidgets_notes 7>6 为**已登记** known-red（#533 裁决挂起，transitive-baseline 头注在案），非未登记红 | EXTERNAL（#533） |

## Subagent Findings（修复池）

### Agent A（数据/IO，详情 /tmp/paleo-audit-r3/findings-agent-A.md）

| ID | Sev | Conf | 位置 | 一句话 | 状态 |
|---|---|---|---|---|---|
| A-01 | P2 | VERIFIED | src/io/dataimport_geodata.cpp:382-403 | geojson 图例字典 readAll 无大小闸（同族 FA-002/#387 口径遗漏） | FIXED 0dc5a1a4 |
| A-02 | P3 | HIGH | src/io/cuttingsdoc.cpp:560-580 | 岩屑 csv/txt readAll 无闸（xlsx 分支有），井剖面打开延迟引爆 | OPEN |
| A-03 | P3 | VERIFIED | src/io/ingestplan.cpp:538 | 哈希进度回调恒传总数违背「已见文件数」契约（#549 搬运旧错） | OPEN |
| A-04 | P3 | SUSPECTED | src/catalog/catalogstore_json.cpp:280-294 | 负 ord 链接行静默丢弃无告警（与同函数口径不对称） | OPEN |
| R-1..R-5 | — | — | — | horizon 整读峰值/Windows 大小写/entity id 路径段/坏库分叉/升级后缀 | RECORD-ONLY |

### Agent B（算法/服务，详情 findings-agent-B.md）

| ID | Sev | Conf | 位置 | 一句话 | 状态 |
|---|---|---|---|---|---|
| B-01 | P3（密井网可升 P2） | VERIFIED | seismicclusterlabels.cpp:33-35 + seismichorizoncluster.cpp:167-168 | #530 新链路 SMI 散点 extent=N·spacing 点栅格错配：井投票带整体偏半道间距、样本线上 y 错一整格、最小 y 边零票（TimeRaster 路径互逆无此问题；已 4×4 手算夹具验证） | OPEN |
| B-02 | P3 | VERIFIED | dataexport.cpp:22-34 | 崩溃暂存清扫白名单漏 `.mapbook-`/`.thickness-`/`.paleomap-import-` 三个现存根，永不清理 | OPEN |
| B-03 | P3 | HIGH | storagegovernance.cpp:85-119 vs paleomapfiles.cpp:22-24 | sidecar 后缀词表分叉（bundle 缺 7 项 dbf 族索引后缀）→ 导入生成索引被误报孤儿/误 purge | OPEN |
| B-04 | P3 | VERIFIED | sattrio.cpp:212 + crossplotsources.cpp:87 | SATR 取消谓词语义反转（靠调用方双负补偿），新调用方按标准约定即整读失败 | OPEN |
| B-05 | P3 | SUSPECTED | crossplotsamples.cpp:218-252 | 交会栅格采样无字节预算闸（#468 家族未覆盖） | OPEN（低优先） |
| B-06 | P3 | VERIFIED | seismichorizoncluster.cpp:212-215 | readInline 失败整条 inline 静默丢弃，无 QC 计数 | OPEN |
| B-07 | P3L | SUSPECTED | wellsiting.cpp:218-220 | 预算闸前 double→int 转换（被预算闸兜住） | RECORD-ONLY |
| B-08 | P3L | VERIFIED | welllogset.cpp:328 | 无轨迹时 MD 静默充当 TVD 无 warnings 留痕 | OPEN |

### Agent C（workflow/AI/联动，详情 findings-agent-C.md）

| ID | Sev | Conf | 位置 | 一句话 | 状态 |
|---|---|---|---|---|---|
| C-01 | P3 | VERIFIED | geologicalexport.cpp:201-204 + mapexport.cpp:86-99 | 空层位导出复用带 horizon 既有版本并清空其归属，下游按 horizon 归集错位 | FIXED eea57e46+c632b50f |
| C-02 | P3 | VERIFIED | onnxpredictionservice.cpp:774 | runTensor 失败路径锁外读 m_loaded（QString 竞争 + 冗余） | FIXED 0dc5a1a4 |
| C-03 | P3 | HIGH | horizonbatchexport.cpp:495-508 | 同资产多文件产物 dedup 缺口（#537 修复未覆盖 files>1 分支） | FIXED eea57e46 |
| C-05 | P3L | VERIFIED | mappingworkbench_predict.cpp:414-427 | Mock 井标定 95% 进度冻结 | RECORD-ONLY |
| C-06 | P3L | VERIFIED | horizonbatchexport.cpp:317-318 | registerMapPdfAsset 未传 horizon 参（与单图口径分叉） | FIXED eea57e46 |
| C-04 | P3 | SUSPECTED | folderimport.cpp:363-410 | preview 双胞胎无队列串行 | **EXCLUDED_EXISTING_ISSUE #560**（preview 双胞胎即其登记面；且 PR #569 活跃触碰该文件） |

### Agent D（qgis/ui，详情 findings-agent-D.md）

| ID | Sev | Conf | 位置 | 一句话 | 状态 |
|---|---|---|---|---|---|
| D-01 | P3 | VERIFIED | constraintpage.cpp:224-231 + constraintpage_wellfactors.cpp | #544 漏「单因素表换行勾选」路径（终审 FR-1/FR-2 更正：有保存版本路径由既有字段组合框链自愈，Fix-1 的 A-restore 在换勾时刻惰性；**无版本换勾路径的等值距建议残留仍在**——constraintpage.cpp:233 于清表前读旧值域，wellfactors 失败路径无重算者） | PARTIAL 051ed34d：A-restore 保留（口径对齐时有值）+ 终态钉子测试；残留 OPEN 待后续（重构两次破坏既有套件，见 07 试错记录） |
| D-02 | P3L | VERIFIED | mappingreferencepanel.cpp:193-196,257-258 | #547 删「至少一视图」保护留死参 setCanRemove；关最后视图无空态文案 | OPEN |
| D-03 | P3L | VERIFIED | datalist_paleomaps.cpp:21 | qMax(1,bundle_members) 对无记录图虚构「1 个配套文件」 | OPEN |
| D-04 | P3L | VERIFIED | paleomainwindow.cpp:598 | #565 删 lastPage 读取后 QSettings 永久死写 | OPEN |
| S-D1..S-D4 | — | SUSPECTED | — | 阈值 spin 截断/capture connect 无 context/首次悬停全量建索引/sidecar 写返回值 | RECORD-ONLY |

### Agent E（构建/测试/架构，详情 findings-agent-E.md）

| ID | Sev | Conf | 位置 | 一句话 | 状态 |
|---|---|---|---|---|---|
| E-01 | P1 | VERIFIED | 多处闸门 | master 闸门被 #546/#548/#552/#566 击穿 7 处未登记红（并入 G1 簇；ui_qtwidgets_notes 除外=已登记） | 并入 G1 |
| E-02 | P2 | VERIFIED | ci_triage_reds.py:285-302 + ci.yml | triage 对 lint 是 job 级黑盒：ci.yml 不传 lint 日志，job 级条目吞任何 lint 红 | OPEN |
| E-03 | P3 | VERIFIED | file-budget-baseline 头注/数值矛盾 | 并入 G1-07 | 并入 G1 |
| E-04 | P3 | VERIFIED | tests/tst_horizonbatchexport_diag.cpp | 582 行完整测试零注册（覆盖幻影） | OPEN |
| E-05 | P3 | VERIFIED | CMakeLists qgis_spread 族注册 | ctest 注册与 AGENTS 声明漂移（LABELS layering/缺 WORKING_DIRECTORY） | OPEN |
| E-06 | P3 | VERIFIED | docs/ci/known-reds.json | 5 条 STALE | 并入 G1-08 |
| E-07 | P3 | 张力 | 数据层 17 文件直连 qgs | by design 口径张力 | RECORD-ONLY |
| E-08 | P3 | VERIFIED | ui-token-exceptions.json | 2 条 STALE（compositebody） | 并入 G1-02 |
