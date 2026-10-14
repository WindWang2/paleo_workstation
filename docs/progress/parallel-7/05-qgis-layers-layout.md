# 方向 05 ledger：QGIS 图层体系、样式制图与既有地图版式能力深化

- 分支：`goal7/qgis-layers-layout-20261014`；worktree：`../paleo-wt-goal7-qgis-layers`
- Base SHA：`b0520429c21f858a1c053b876f511680088564a9`（2026-10-14 执行时 origin/master 实测）
- 环境：Linux x86_64（16C/62G），cmake 4.4.4 + Ninja + ccache 4.14，Debug，QGIS_PREFIX=主仓 superbuild prefix（symlink 共享，只读不写），QT_QPA_PLATFORM=offscreen
- 全局构建锁：`/tmp/paleo_workstation_global_build_j4.lock`，构建/测试一律 flock -j4
- 实时边界复核：Open PR 仅 #437（80 文件只读，清单见 05-baseline.md §1）；Open Issues #352–#421 全归 Issue Agent

## R0（2026-10-14）基线与消重 —— 完成

- 阶段 0 盘点：fetch 后 origin/master = b0520429 与任务书核查基线一致；主 checkout 干净；无同名 worktree/分支；新建 worktree + vendor symlink 接线（superbuild/prefix + onnxruntime；本机无 vendor/prefix deb 闭包）；flock 内 configure 一次通过（QScintilla warning 属 BUILDING.md 允许降级）。
- 四路只读审计（S1 图层生命周期 / S2 符号样式 / S3 版式输出 / S4 渲染性能与 QA 基建）完成，主 Agent 对全部候选发现做了代码级抽查复核（qgislayerprofile.h:26-29 契约 vs applyPrunedTheme:283-323；geopatterns.cpp:43-85 NOOP vs :138-302 QObject::tr；layouttemplates.cpp:122-152 outcome 丢弃；mapbooklayout.cpp:38-71 硬编码字体；layertreepanel.cpp:683-759 循环内 declared()；qgislayerorganizer.cpp:153,217-232 双重复制——全部属实）。
- 产物：`docs/progress/parallel-7/05-baseline.md`（主账：写权限边界、已有能力、候选工作项 W1–W10、留档项、性能方案、风险）+ `05-baseline-{layers,symbols,layout,perf}.md`（四份分包审计全文）。
- 候选工作项（全部白名单内、与 #437/Issues 零重叠）：W1 page:* 主题重放保留无声明临时图层可见性；W2 refreshIndicators 声明回查提升出循环；W3 organizer declared() 复制消除；W4 i18n 上下文错配（NOOP vs QObject::tr）；W5 模板失败静默→ErrorHub+信号；W6 mapbooklayout 字体走 figureFontFamily；W7 地图册回读校验补 PDF 尾/PNG 尺寸/SVG 头；W8 exportLayoutAsync 启动失败补 exportFinished；W9 相花纹词表「三角洲」缺口；W10 守恒 Oracle 补强。
- 留档不做（超白名单或 #437 保护）：S1 A-1 双激活层位分叉（paleomainwindow_workbench.cpp，#437 文件）、S3 A4 mapbookqueue failed 计数（workflow）、S4 A2/A3/A4/A7（datapreview/decorations/paleomainwindow）——见 05-baseline.md §4。
- 基线构建/测试结果：见下轮记录（全量构建完成后补）。

## 基线验证（2026-10-14，b0520429 未改树）

- 全量构建：`cmake --build build -j4`（flock）完成，复跑 `ninja: no work to do`，退出 0。2129 条编译/链接边，首次全量 ~34min（ccache 部分命中）。
- 域测试基线（flock -j4，offscreen）：27/27 全绿，65.91s。套件 = tst_{layermanifest,layerservice,layerorganizer,layerplatform,layertreepanel,layerproperties,labelzorder,dragtop_render,layoutexport,layoutitempanel,layoutpalette,layoutpersistence,layoutshell,layouttemplatelibrary,layoutundo,layoutdesigner_full,standardelements,mapbook,horizonbatchexport,symbolpicker,geosymbols,semanticlegend,factorworkflow,mapping,mappingpages,mappingworkbench,shell_horizonstate}。
- 并发环境注记：兄弟方向 goal7-ui-shell 同时段持锁构建（flock 协调有效，本方向全程排队等待未越权并发）。

## 候选复核修正

- **W8（exportLayoutAsync 的 !task 分支不发 exportFinished）降级为不做**：实读 `PaleoTaskService::start`（paleotaskservice.cpp）——createTask 恒成功，start 永不返回 null，该分支不可达；且头注释契约「false=调用方自行呈现」在 runExportUi 有完整兑现（:283-290）。行为自洽，非缺陷。留档为「死分支防御代码」，不改动。

## 微循环记录

### 轮 4（2026-10-15）W2+W3 声明表热点：图层树指示器与归位的 sqlite 全表读放大 —— 本 commit

- **目标**：W2（S4 审计 P-1）LayerTreePanel::refreshIndicators 逐节点循环内 `m_layerService->declared()`——declared() 每次 = MetaStore::openConnection + CREATE TABLE IF NOT EXISTS + PRAGMA table_info + SELECT 全表（layermanifest.cpp:38-76,220），挂载 N 层时每层两路信号触发全量刷新 → O(层数²) 次查询。W3（S4 审计 P-2）QgisLayerOrganizer::placeLayer 单事件路径 2 次 declared()（自身 + desiredSlot 各一）、horizonGroup 建组时逐子节点再读、reorganize 全量 = 2N+3 次。
- **可观察现行行为**（S4 审计 §可观察清单 + 主 Agent 复核）：①归位 clone+insert+remove 保勾选态/引用（sameParentReorderKeepsLayersInProject 钉死）；②自定义绘制序归位后镜像树序自愈；③无声明手动层对摆放透明；④指示器灰显语义 = 非激活层位层带「未激活」标（tst_layertreepanel 既有用例）。
- **反例/探针（先量后改，qInfo 落盘）**：tst_layertreepanel::indicatorRefreshScalesWithDeclarations（150 声明+150 memory 层，量 mountMs/refreshMs + 灰显守恒断言 T1 无标/T2 有标）；tst_layerorganizer::placementScalesWithDeclarations（300 声明逐层实例化量 instantiateMs + 组收编守恒，新增 reorganizeMs 隔离全量规整路径）。宽门（30s/10s）只挡灾难回归，量级对比记账在此。
- **实测（flock -j4，offscreen，Debug，同机兄弟方向排队同锁）**：
  - W2：mountMs **14992 → 236**（63×），refreshMs **74 → 0**。修复前挂载期 O(层数²) 次 declared()（每层两路信号各触发一次全量刷新，每次刷新逐节点读表）是主峰。
  - W3：reorganizeMs **832 → 319/338**（2.5×，工程重开全量规整路径，声明表 603 次 → 3 次 all()）；instantiateMs 3233 → 2876~2961（~7-11%，逐层路径由 OGR 建 QgsVectorLayer(fixture.gpkg) 主导，声明表缩减被淹没——如实记录，不伪报加速）。
- **改动**：layertreepanel.cpp refreshIndicators 循环外一次取表建 layerId→horizon 哈希（同 layerId 取首个声明，与旧线性扫描 break 语义一致；信号接线不动——双信号+同步刷新是既有测试依赖语义）；qgislayerorganizer.{h,cpp} placeLayer/horizonGroup/desiredSlot 加声明表入参（默认 nullptr = 单事件路径现场取表，语义不变），reorganize/ensureHorizonGroups 一次取表批量复用，horizonGroup 逐子节点嵌套读表改 QSet 预建。src/metadata/layermanifest.cpp 不在本方向白名单——per-call schema 确保开销（CREATE TABLE+PRAGMA）留档给 metadata 所有者，不越界修。
- **验证**：探针两跑绿（含 stash 往返 before/after 对照）；守恒 tst_layerorganizer / tst_layertreepanel / tst_layerplatform 全套件绿。

### 轮 3（2026-10-14）W1 page:* 主题重放守恒无声明临时图层 —— commit 0d703d3b

- **目标**：W1（S1 审计 B-2）——`applyPrunedTheme` 重放 page:* 主题时，无声明层（paleoLayerId 空或声明已删的临时层）被主题里记录的未勾选状态压成不可见，且祖先组勾选链不恢复，与「无声明层对主题透明」契约相悖。
- **可观察现行行为**（S1 审计 §可观察清单 + 主 Agent 复核 qgislayerprofile.cpp）：①page:* 主题应用走 applyPrunedTheme 剪枝路径；②用户命名主题走另一分支不受影响；③清单读失败有豁免逻辑但只覆盖无 paleoId 层。
- **反例（先红后绿）**：tst_layerplatform 新增 pageThemeReplayKeepsUndeclaredTempLayers（主题快照含无声明临时层 → 重放后其自身勾选与有效可见性保持，含祖先组勾选链恢复）——修复前红，修复后绿。
- **改动**：qgislayerprofile.cpp applyPrunedTheme 快照/恢复两侧对无声明层（paleoId 空或声明已不在）守恒自身勾选+有效可见性；清单读失败豁免收窄到仅无 paleoId 层；用户命名主题路径不动。
- **验证**（flock -j4，offscreen）：tst_layerplatform 19/19 绿（新用例先红后绿）。

### 轮 2（2026-10-14）W5+W6+W7 版式簇 —— commit e3fc5c34

- **目标**：W5 模板动作失败静默（layouttemplates.cpp 12 处失败点 outcome.error 被丢弃）；W6 mapbooklayout 硬编码系统字体族绕过 figureFontFamily 工厂；W7 renderLayout 回读校验缺口（PDF 只验头/PNG 不验尺寸/SVG 零校验）。
- **可观察现行行为**（S3 审计 §可观察清单 1-8 + 主 Agent 复核 layouttemplates.cpp:122-152、mapbooklayout.cpp:38-71,338-372）：①导出失败有状态条+ErrorHub 双通道（对照面）；②qpt/qgz round-trip 有钉；③地图册 manifest 账实一致；④figureFontFamilyResolvesToInstalledFamily 钉死 vendored 优先（tst_layoutexport:566-577）。
- **反例**：W6 先红后绿（修复前 label 字体族 = "Noto Sans CJK SC" ≠ 工厂解析 "Noto Sans SC"，pinRenderEnvironment 下确定性红）；W5/W7 的校验/信号缝是新 API 面，无既有编译目标可红——缺口证据 = 代码审计调用链（模板 lambda 丢弃 outcome；renderLayout 无 SVG 分支）+ 新增负向用例（截断 PDF/尺寸错 PNG/假 SVG 在新校验下必败）。
- **改动**：mapbooklayout.{h,cpp}（labelFormat/northArrowSvg 收进 standardelements 工厂；validateExportedFile 析出并加 PDF 尾/PNG 尺寸/SVG 根校验；renderLayout 计算首页期望像素并调用）；layouttemplates.{h,cpp}（templateFailed 信号 + fail() 统一收口 12 处失败点 + ErrorHub warning 入账，无 hub 不弹窗）；standardelements.h/layoutexport.cpp 过时字体注释同步。
- **验证**（flock -j4，offscreen）：tst_layoutexport 19/19（+3 新用例）、tst_mapbook 18/18、tst_layoutdesigner_full 18/18、tst_layoutpersistence 3/3、tst_standardelements 9/9、tst_horizonbatchexport 6/6 绿。

### 轮 1（2026-10-14）W4+W9 符号词面翻译上下文 + 相词表别名 —— commit eaf54c16

- **目标**：W4（NOOP 登记上下文 GeoPatterns/QgisStyleService 与运行时 QObject::tr 上下文 "QObject" 错配，翻译永不命中）+ W9（catalog 标准词面「三角洲」落兜底桶）。
- **可观察现行行为**（开工证据，S2 审计 §可观察清单 1/2/3/6/7 + 主 Agent 复核）：①等值线双描边规格与 DESIGN.md 逐点对齐（factorstylewriter.h:39-44）；②井别 16 类 id+词面双桶；③符号覆盖经 customProperty 随工程往返；④花纹缺资源纯色兜底；⑤选择器三族 14/8/8。
- **反例（先红后绿）**：tst_symbolpicker 新增 translationContextsAreHonored（探针 QTranslator 在 GeoPatterns/QgisStyleService 上下文返回标记串，断言 titleFor/normalizeLithology/wellCategoryDefinitions 真查该上下文）+ faciesValueBucketsCoverCatalogTerms（delta 桶含「三角洲」且 deltaic 不误并）。修复前运行：2 FAIL（titleFor 返回源串「砂岩」而非 TEST_SANDSTONE；valueBuckets("delta") 不含「三角洲」），既有 3 用例绿。
- **改动**：geopatterns.cpp 8 处 QObject::tr→QCoreApplication::translate("GeoPatterns",…) + kFaciesAliases（「三角洲」→delta）+ valueBuckets 相分支消费别名；qgisstyleservice.cpp 27 处 QObject::tr→类内 tr（26 站点清单脚本核验全在成员函数内；替换数 27 因一行双处）；geopatterns.h valueBuckets 注释同步。
- **验证**：修复后 tst_symbolpicker 7/7、tst_geosymbols 10/10、tst_semanticlegend 6/6 绿（flock -j4，offscreen）。
- **影响面注记**：中文 UI 零变化（源串即中文）；.ts 中 GeoPatterns/QgisStyleService 上下文条目此前是死条目，修复后才真正生效；纯字面值串的 lupdate 上下文从 QObject 迁 QgisStyleService，lupdate 统一收口（不动 translations/ 本体）。

（后续每轮：目标 → 改动文件/函数 → Oracle → 失败/成功日志要点 → 耗时/RSS → commit ID → 排除事项）
