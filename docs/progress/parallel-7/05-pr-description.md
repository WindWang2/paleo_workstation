# refactor(05): QGIS 图层体系、样式制图与既有地图版式能力深化

方向 05（QGIS 图层/样式/版式）goal-loop 迭代包。只对既有功能做行为守恒的完善：符号词面翻译上下文、相词表别名、版式失败反馈与回读校验、page 主题重放守恒、声明表查询热点去放大。无新业务面、无新依赖、无 schema/持久化口径变化。

- Base：`b0520429c21f858a1c053b876f511680088564a9`（origin/master）
- 分支：`goal7/qgis-layers-layout-20261014`
- Ledger：`docs/progress/parallel-7/05-qgis-layers-layout.md`；R0 审计：`05-baseline{,-layers,-symbols,-layout,-perf}.md`；独立 review：`05-s5-review.md`

## 改动清单（7 个 commit：R0 审计文档 + 轮 1-4 + R7 + S5 minor 闭环）

### 轮 1（eaf54c16）符号词面翻译上下文 + 相词表别名（W4+W9）
- `geopatterns.cpp`：8 处 `QObject::tr` → `QCoreApplication::translate("GeoPatterns",…)`——lupdate NOOP 登记上下文与运行时查找上下文此前错配，GeoPatterns 上下文的 .ts 条目是死条目；修复后真正生效（中文 UI 零变化，源串即中文）。
- `geopatterns.cpp`：`kFaciesAliases` 新增「三角洲」→ delta，valueBuckets 相分支消费——catalog 标准词面不再落兜底桶。
- `qgisstyleservice.cpp`：27 处 `QObject::tr(` → 类内 `tr(`（26 站点，一行双处）。
- 红测先行：`tst_symbolpicker` +2 用例（探针 QTranslator 上下文命中、delta 桶含「三角洲」且 deltaic 不误并），先红后绿；守恒 tst_geosymbols 10/10、tst_semanticlegend 6/6。

### 轮 2（e3fc5c34）版式簇：模板失败反馈、地图册字体收口、回读校验加固（W5+W6+W7）
- `layouttemplates.{h,cpp}`：新增 `templateFailed(path,error)` 信号，`fail()` 收口全部 12 处失败点并入 ErrorHub warning（无 hub 不弹窗）——此前 outcome.error 被静默丢弃。
- `mapbooklayout.{h,cpp}`：labelFormat/northArrowSvg 改调 `PaleoStandardElements` 工厂（删硬编码 "Noto Sans CJK SC" 与重复实现）；`validateExportedFile` 析出为公开静态校验：PDF 验头+尾 1KiB %%EOF、PNG 验签名+像素尺寸 ±2px、SVG 验 `<svg` 根；renderLayout 计算首页期望像素并调用。
- 先红后绿：W6（CJK SC ≠ 工厂解析 SC，pinRenderEnvironment 下确定性红）；W5/W7 新 API 面配负向用例（截断 PDF/尺寸错 PNG/假 SVG 必败）。
- 守恒：tst_layoutexport 19/19（+3）、tst_mapbook 18/18、tst_layoutdesigner_full 18/18、tst_layoutpersistence 3/3、tst_standardelements 9/9、tst_horizonbatchexport 6/6。

### 轮 3（0d703d3b）page:* 主题重放守恒无声明临时图层（W1）
- `qgislayerprofile.cpp::applyPrunedTheme`：page:* 主题快照/恢复对无声明层（paleoLayerId 空或声明已删）守恒自身勾选+有效可见性（含祖先组勾选链恢复）；清单读失败豁免收窄到仅无 paleoId 层；用户命名主题路径不动。
- 红测先行：tst_layerplatform::pageThemeReplayKeepsUndeclaredTempLayers 先红后绿，19/19 全绿。

### 轮 4（32ab6185）声明表热点去放大（W2+W3，性能）
- 根因：`QgisLayerService::declared()` 每次调用 = `MetaStore::openConnection` + `CREATE TABLE IF NOT EXISTS` + `PRAGMA table_info` + `SELECT` 全表（layermanifest.cpp:38-76,220）。
- `layertreepanel.cpp::refreshIndicators`：逐节点循环内 declared() → 循环外一次取表建 layerId→horizon 哈希（同 layerId 取首个声明，与旧线性扫描 break 语义一致；信号接线不动）。
- `qgislayerorganizer.{h,cpp}`：placeLayer/horizonGroup/desiredSlot 加声明表入参（默认 nullptr = 单事件路径现场取表，语义不变）；reorganize/ensureHorizonGroups 一次取表批量复用；horizonGroup 逐子节点嵌套读表改 QSet 预建。
- 实测（flock -j4，offscreen，Debug，探针 qInfo 落盘，stash 往返对照）：
  - W2（150 声明+150 层挂载）：mountMs **14992 → 236**（63×），refreshMs **74 → 0**。
  - W3（300 声明）：reorganizeMs **832 → 319/338**（2.5×，工程重开全量规整路径，all() 调用 603 → 3 次）；instantiateMs 3233 → 2876~2961（~7-11%，逐层路径由 OGR 建层主导，声明表缩减被淹没——如实记录）。
- 守恒：tst_layerorganizer 14/14、tst_layertreepanel 22/22、tst_layerplatform 19/19。

### R7（baaa2efb）死友元清理
- `previewmapcanvas.h`：删除全仓零引用的 `friend class PreviewMapCanvasSelftest`。

### S5 minor 闭环（f04b8190）
- `qgislayerprofile.cpp`：读失败路径注释纠错（如实描述 fail-safe 守恒语义）+ m_layerService 为空时 page:* 重放空转的 qWarning 诊断。
- `tst_layoutexport.cpp`：mapbookLabelsUseFigureFontFactory 补「expected 非空且已安装」断言（防同义反复，工厂退化必红）。
- 复验：tst_layerplatform 19/19、tst_layoutexport 19/19 绿。

## 所有权与冲突排除（反向验证）

- 全 diff 26 文件：`git diff origin/master...HEAD --name-only` 逐一核对——
  - vs PR #437 实时清单（84 文件，`gh pr view 437 --json files`）：交集 **0**（qgisprojectservice.cpp / qgisconstrainteditsession.* / evidencemapadapter.* / mappingartifactwriter.cpp / mappingfeatureidentity.* 全程只读）。
  - vs 兄弟 PR #445（方向 06 ui-shell）/#446（方向 02 well-sections）：交集 **0**。
  - vs Issue 指纹：#419（src/qgis 低危 5 项）文件与事项均无交集；#352-#421、#438-#444 未认领、未修复、未关闭（详见 ledger「反向验证」节与 05-baseline.md §3/§4）。
- 白名单符合性：src/qgis 13 文件均非保护文件；src/ui 仅 layers/+layout/；tests 仅 tst_layer*/tst_layout*/tst_symbol*；docs 仅 parallel-7/05-*。
- CMakeLists.txt 零改动（在 #437 清单内）——新测试用例全部加入已注册测试文件，无新测试二进制。

## 验证

- 门禁：`git diff --check` 绿；`tools/check_layering.py --strict` 绿；ctest 门禁 7/7（layering/layering_strict/ui_invariants/ui_invariants_strict/ui_tokens/ui_tokens_strict/i18n）。
- 全量构建：`cmake --build build -j4`（flock）退出 0。
- 全量测试：`ctest --test-dir build -j4 --output-on-failure`（394 项）两轮。第一轮 387/394、第二轮 388/394 绿；两轮红项为同一集合（±tst_singlefactor_kriging 第二轮转绿），全部在本方向改动面之外且均有基线/环境证据：
  - `tst_ui`：高负载下 IO 竞态（sqlite3_open 失败 + 索引缓存写后校验字节不符）→ `MappingWorkbench::errorOccurred` → 既有模态告警（paleomainwindow_workbench.cpp:1012，#437 文件）在 offscreen 无人点框 → 看门狗杀。**纯净 master（b0520429）同条件复跑同样红（340s）**，前一方向 ledger（qgis-edit-compose-deepening-20261013）亦载 master 自带红；本分支隔离单跑该用例 697ms 绿。
  - `tst_singlefactor_perf`（envdebt_exempt）/`tst_singlefactor_guilatency`/`tst_perf_segyindex`/`tst_perf_regress`/`tst_inversion_perf`：ratios.json 标定比率的宿主机敏感门（docs/perf/BASELINE.md），七方向同机并发下超时/超比率；均在 algorithms/io/workflow 域，与本 diff 零文件相交。`tst_singlefactor_kriging` 隔离复跑已转绿。
  - 本方向域测试（layer/layout/symbol 27 套件）两轮全绿。
- S5 独立评审：**PASS-with-minors**（无 blocker/major；评审人实测构建+5 套件+门禁全绿，五契约逐 hunk 守恒）。minor 1/2/4 已修复并复验（commit f04b8190），minor 3 留 lupdate 周期，nit 不处理。报告：`docs/progress/parallel-7/05-s5-review.md`。

## 留档不做（超界/无据，转交对应所有者）

- `layermanifest.cpp` per-call schema 确保开销（CREATE TABLE+PRAGMA per declared()）——metadata 域，建议加进程内 schemaEnsured 缓存。
- S1 A-1 双激活层位分叉（paleomainwindow_workbench.cpp，#437 文件）。
- S3 A4 mapbookqueue failed 计数（workflow 域）；S4 A2/A3/A4/A7（datapreview/decorations/paleomainwindow，方向 06/#437 域）。
- W8 exportLayoutAsync 死分支：`PaleoTaskService::start` 恒非空，契约自洽，非缺陷。
- 以上全部未改动，细节见 `05-baseline.md` §4 与 ledger。

## 已知风险

- 探针宽门（30s/10s）只挡灾难回归，量级对比以 ledger 记账为准；Debug 构建数字不代表 Release 绝对值（比值有效）。
- 轮 4 性能数字在同机兄弟方向并发排队环境下测得；before/after 均同条件两跑，方向一致。
