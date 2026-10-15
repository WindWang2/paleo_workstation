# S2 符号/标注/样式 R0 审计（任务域 C 既有矢量工具/样式 + D 专业图式与 DPI）

## 链路与现状（调用图/数据流，逐环 file:line 证据）

**等值线双描边 + 注记（DESIGN.md:122/124 的落点）**
- 入口统一：`QgisStyleService::applyContourLayerStyle` 仅转调 `FactorStyleWriter::applyContours`（src/qgis/qgisstyleservice.cpp:605-608）。
- 实现：双描边 = 白 0.55mm 底层 + 深墨 #24303E 0.25mm 上层（src/qgis/factorstylewriter.cpp:158-164；常量 src/qgis/factorstylewriter.h:39-44）；注记字段 `level` 优先、`ELEV` 兜底（factorstylewriter.cpp:165），`format_number(field, 2)` 两位小数（:168），Curved 沿线 + repeatDistance 60mm（:170-172），minimumFeatureSize 10mm（:173），JetBrains Mono 9pt 墨字 + 白缓冲 0.6mm（:174-177）。
- 生产调用方：src/qgis/qgislayerservice.cpp:183、src/workflow/mappingworkbench.cpp:930、src/ui/datapreview/datapreviewtabmapproduct.cpp:316。

**单因素色带（栅格）**
- `FactorStyleWriter::applyTo/writeStyleQml/rampFor`（src/qgis/factorstylewriter.cpp:72-152）：7 个因子预设色带（:46-65），地图域数据符号硬编码 hex，src/qgis 全目录零 `PaleoTheme` 引用（grep 实证为空）→ 浅/深主题下不跟随 UI token，与 DESIGN.md:115/118 一致。
- 落盘调用方：constraintfactorjobs_factor.cpp:1017、_contours.cpp:739、_interpretive.cpp:580；内存直配：mappingworkbench.cpp:920、paleomainwindow_workbench.cpp:336、datapreviewtabmapproduct.cpp:105。

**相色标/相面渲染**
- `FaciesHierarchyRenderer::apply`（src/qgis/facieshierarchyrenderer.cpp:70-162）：先 `MappingArtifactWriter::applyFaciesStyle`（#437 占用件）→ 矢量换 QgsCategorizedSymbolRenderer（面则包 QgsMergedFeatureRenderer，:132-135）+ `applyFaciesLabels`（:137，#437 占用）；栅格走 QgsPalettedRasterRenderer 重着色（:141-156）。相色来源 `FaciesCatalog::appearance`（src/domain/faciescatalog.cpp:70-109），数据驱动、无 UI token。
- catalog.json 完整性：`FaciesCatalog::library()` 运行时跳过文件缺失条目（faciescatalog.cpp:53-55）——对账防线存在。实测 317 条 catalog.json `file` 全部在磁盘且全部在 geology.qrc 有 alias（脚本对账，0 缺失）。

**GeoPatterns 词表-符号注册表（src/qgis/geopatterns.cpp）**
- 词表：岩性 14（:42-57）/ 相 8（:62-71）/ 线型 8（:76-85），资源经 `FaciesCatalog::resourcePath` 锚 qrc（:162），缺资源 → 纯色兜底（:160-171，诚实降级不吞语义）。
- 样式入口：`applyLithologyPatternStyle/applyFaciesPatternStyle/applyFaultLineLayerStyle`（qgisstyleservice.cpp:883-957）——**生产零调用方，仅测试调用**（grep 实证：仅 tst_geosymbols/tst_semanticlegend）；`applySymbolOverride/restoreSymbolOverride/restoreAllSymbolOverrides`（qgisstyleservice.cpp:971-1033）同样无生产调用方。
- 视图侧 `SymbolPickerPanel`（src/ui/symbols/symbolpickerpanel.cpp）发出 `patternPicked` 语义 id——**无生产宿主**（全仓 grep 仅测试实例化）。层契约合规（视图只发信号；include 仅 `qgis/geopatterns.h`，无 algorithms/io 违规）。

**井别符号（表 K.1 16 类）**
- `applyWellCategoryStyle`（qgisstyleservice.cpp:757-788）+ 符号构造 `wellCategorySymbol`（:97-159，@map_scale 数据定义尺寸 :156-157）；生产接线：src/app/appcontext_layers.cpp:138-148（well_class/well_category/category/类别 四字段探测）。

**标注 z 序**
- `QgisLabelZOrder`（src/qgis/qgislabelzorder.cpp）：构造时给全工程图层钉 `rendering/labelsWithLayer=true` 并清旧 `renderAboveLabels` 残留（:57-75），layersAdded/readProject 重打（:35-45）；未打补丁构建优雅降级 + qInfo（:22-31）。宿主：src/ui/paleomainwindow_docks.cpp:378-388（含状态栏降级提示）。补丁实体：vendor/superbuild/patches/qgis-4.2.3-labels-with-layer.patch（定义 `QGIS_PALEO_LABELS_WITH_LAYER`，改 qgsmaprendererjob）。

**语义图例**
- `SemanticLegendBuilder`（src/qgis/semanticlegend.cpp:82-127）：layerOrder 树序（顶部层在前），分类渲染器逐类目、其余走 legendSymbolItems 回退；rendererChanged/nameChanged/layersAdded/Removed 联动。**无生产消费方**——semanticlegend.h:20-21 自述回收条件（方向 24/25 收口后仍无消费则删除）。

**布局导出 DPI**
- `PaleoLayoutExport::exportLayout`（src/qgis/layoutexport.cpp:124-306）：dpi 直接交给 QgsLayoutExporter::Image/Pdf/SvgExportSettings（:193/:208/:220/:234/:246/:260），版面单位全 mm/pt，无任何手动 DPR 乘除；预览缩略 `PreviewMapCanvas::renderSnapshot` 按像素比同步缩 outputDpi（src/qgis/previewmapcanvas.cpp:432-436），mm/pt 尺寸守恒。DESIGN.md:124「不随屏幕 DPR 手动缩放」实证满足。

## 可观察现行行为清单

1. 等值线图层换渲染器后必为双层线符号（层0 白 0.55mm、层1 墨 0.25mm）；字段缺失时只换线型不标注。验证：memory line 层 + `ELEV` 字段跑 `applyContours` 检查 symbolLayer(1)->width()==0.25——已有断言 tests/tst_factorworkflow.cpp:551-562（缺层0 白 0.55 与缓冲 0.6mm 的断言）。
2. 井别渲染器对每个规范 id 与中文词面各登记一个分类桶（qgisstyleservice.cpp:777-784），16 类共 32 桶 + all-other 通用探井桶；未知值落通用符号不吞数据。验证：memory point 层 + 类别字段，统计 renderer 类目数。
3. 符号覆盖随工程持久化：customProperty `paleo/symbolSemantics={family,id,version:1}`（qgisstyleservice.cpp:1006-1010），重开工程 `restoreSymbolOverride` 幂等重放（:1014-1024）。验证：已有 round-trip 测试 tst_geosymbols.cpp:321-335。
4. labelsWithLayer 属性在工程每次 readProject 后全表重钉（qgislabelzorder.cpp:42-45），旧 renderAboveLabels 残留被清除（:68-72）。验证：像素回归 tests/tst_labelzorder.cpp（底层标注被中间不透明面遮盖的像素断言）。
5. 语义图例节序 = 图层树显示序（顶部层在前，semanticlegend.cpp:88），分类类目 semanticId = 类目数据值（:108）。验证：tests/tst_semanticlegend.cpp:34-69（井位节在首、岩性类目全收）。
6. 花纹缺 qrc 资源时降级为纯色填 + 细灰边（geopatterns.cpp:160-171），且词表仍可从 customProperty 恢复——图面不空窗。验证：构造不存在的 patternId 调 `lithologyFillSymbol` 得 QgsSimpleFillSymbolLayer。
7. 选择器过滤只做词面/id 子串匹配（symbolpickerpanel.cpp:132-134），三族网格计数 14/8/8。验证：tests/tst_symbolpicker.cpp:22-54。

## 发现分组

### A. 新发现（未占用）

**A1. NOOP 词面翻译上下文与运行时 tr 上下文不匹配（i18n 潜伏缺陷，S）**
- 现象：`QT_TRANSLATE_NOOP("GeoPatterns", …)` / `QT_TRANSLATE_NOOP("QgisStyleService", …)` 登记的词面，运行时却用 `QObject::tr()` 取译——QObject 的 tr 上下文是 `"QObject"`，翻译文件里这些词条在 GeoPatterns/QgisStyleService 上下文下，永远查不中。
- 证据：登记 geopatterns.cpp:43-85（NOOP）vs 取用 geopatterns.cpp:138、151、220-224、255、296、302；qgisstyleservice.cpp:704、718、780、944。ts 文件确仅有 GeoPatterns/QgisStyleService 上下文（translations/paleo_zh_CN.ts:7926、21660），`QObject` 上下文无这些词条。仓内正确惯例是 `QCoreApplication::translate("RealizationSet", …)`（src/catalog/realizationset.cpp:13 等 453 处）。
- 影响面：当前中文 UI 无感（源串即中文）；英文（及任何未来翻译）locale 下 30 个花纹/线型词面 + 16 个井别词面 + 归一化比对保持中文，翻译加了也不生效。normalizeLithology/normalizeWellCategory 的 `v == tr(title)` 比对在同一缺陷上。
- 验证方式：装一个 QTranslator（qm 里把 GeoPatterns 上下文「砂岩」译为测试串），构造 SymbolPickerPanel 断言词面变化；或更直接断言 `GeoPatterns::titleFor("sandstone")` 在翻译装载后返回测试串。修复 = 两处 `QObject::tr(x)` 改 `QCoreApplication::translate("GeoPatterns"/"QgisStyleService", x)`。
- 规模：S（约 10 处调用点 + 测试）。

**A2. 相花纹词表与 catalog.json 权威词面存在缺口（S，低危）**
- 现象：geopatterns.h:17 声称「词面出处 catalog.json」，但 catalog.json 地层沉积相有「三角洲」（strat_delta.svg），GeoPatterns kFacies 只有 `delta`（词面「三角洲朵体」）与 `deltaic`（「三角洲相」）；相词面无同义词层（valueBuckets 相分支只加 id+词面，geopatterns.cpp:301-302），数据字段写「三角洲」→ 不命中任何桶 → 落「未划分相」。
- 证据：catalog.json（地层沉积相 29 条含「三角洲」）、geopatterns.cpp:62-71、286-304。
- 影响面：相花纹分类渲染/图例对标准 catalog 词面出兜底桶；不吞数据但图面语义损失。
- 验证方式：memory polygon 层 facies 字段写「三角洲」，applyFaciesPatternStyle 后 symbolForFeature 落 all-other；修复 = kFacies 增别名或 valueBuckets 相分支加同义词。
- 规模：S。

**A3. 选择器过滤占位文案与实现不符（S，cosmetic）**
- 现象：占位文案「词面或语义代码」（symbolpickerpanel.cpp:79），实现只匹配 title 与 id（:132-134）；真正的语义代码（catalog.json `symbol_code`，如 TEX-F1-1H/C）不在 GeoPatterns 词表字段里（geopatterns.cpp:134-143），输代码永远 0 命中。
- 处置：低价值且「语义代码」可解读为 GeoPatterns id（id 参与匹配），留档不做。

### B. 已占用（只列归属）

- `MappingArtifactWriter::{applyFaciesStyle, applyFaciesLabels, applyFaciesEdge}`、mappingartifactwriter.cpp 全部——PR #437（只读，未审计为发现项）；其中 syncFaciesAttributes 嵌套读另属 Issue #419 第 5 项。
- `SymbolPickerPanel` 无生产宿主——已记录遗留，归属方向 24/25 页面 UX 决策（.goal-loop-ledger-geological-symbols.md 批 4「遗留」）；`applySymbolOverride/restoreAllSymbolOverrides` 无生产调用方同属该链路（批 5 交付的待接线语义）。
- `SemanticLegendBuilder` 无生产消费方——semanticlegend.h:20-21 自述回收条件（方向 24/25 图例收口后裁决），不算新发现。
- qgislabelzorder 依赖 vendored 补丁（含「只与本层标注避让」的跨层避让损失）——已知设计，qgislabelzorder.h:10-19 文档化。
- 补丁文件名漂移（goal-loop-prompts/71-dep-unify.md:8 写 qgis-4.2.2，实为 vendor/superbuild/patches/qgis-4.2.3-labels-with-layer.patch）——方向 71（dep-unify）领地。
- TODOS.md P3「表 K.1 十二类探井符号地质评审」——已立 TODOS（TODOS.md:344-351），人类评审项，不重复立。
- Issue #419 五项（previewrendercache/qgislayerservice/qgisprocessingservice/qgiscanvascontroller/mappingartifactwriter）——全部非本域，仅确认不冲突。

### C. 已修复/与文档不符但不构成缺陷

- DESIGN.md 等值线规格零漂移：0.25/0.55mm、9pt JetBrains Mono、0.6mm 白缓冲、60mm 重复、<10mm 不注记、两位小数、level/ELEV 双识别——逐一对应 factorstylewriter.h:39-44 与 factorstylewriter.cpp:158-179；回归断言在 tst_factorworkflow.cpp:551-562。
- 数据符号色不随主题：src/qgis 全目录无 Paleotheme include/引用（grep 实证）；色带/相色/井别/约束线色全部硬编码 hex。
- 高 DPI：符号尺寸全为 mm/pt 渲染单位，代码无任何 DPR 乘除（全 src 仅 previewmapcanvas.cpp:436 按缩略比例同步 outputDpi——mm/pt 守恒的正确做法）；布局导出 dpi 直通 QGIS exporter（layoutexport.cpp:185-266）。
- 资源对账：catalog.json 317 条 file 全部存在且全部进 qrc（脚本对账 0 缺失）；geopatterns 22 条花纹 texture 全部在 qrc 且 tst_geosymbols 有存在性断言（tst_geosymbols.cpp:53-61）。
- factorstylewriter.h:16 重复的「// 层：QGIS 封装」行与 symbolpickerpanel.cpp:2,4 重复 include paleotheme.h——排版瑕疵，D 类顺手项。

### D. 低价值不建议做

- labelsWithLayer 加逐层 opt-out：当前「全层统一钉标」是文档化设计，无用户诉求；补丁属性本身就是全局机制。
- catalog_*.md 与 catalog.json 逐条机器对账：md 是人类图录文档，catalog.json 已有运行时防线（faciescatalog.cpp:53-55 跳过缺文件条目），再做静态对账收益极低。
- qrc 中存在 catalog 未登记的 SVG（strata _fill、micro_*_fill、fig_* 图版、个别 boundaries/reservoirs）——其中 _fill/micro 被 geopatterns 与 FaciesCatalog 代码路径消费，fig_* 是规范图版，均非孤儿。

## 建议的非回归验证场景

1. **等值线规格守恒（补两条断言）**：memory line 层（ELEV double 字段）→ `FactorStyleWriter::applyContours` → 断言 symbolLayer(0) 白 0.55mm（现只断言层1 的 0.25）、label buffer 0.6mm 白色（现未断言缓冲）；再切 level 字段优先于 ELEV 的分支断言 `format_number("level", 2)` 表达式。防 DESIGN.md:122 规格静默漂移。
2. **符号库完整性守恒（tst_symbolpicker 白名单内可加）**：三族网格计数 14/8/8 + `GeoPatterns::textureResourcePath` 全 id 非空（等价于 tst_geosymbols 的资源断言，在选择器侧守住「缩略所见=图面所得」）；外加 A1 的翻译上下文断言（装测试 QTranslator 后词面变化）。
3. **标注 z 序像素守恒（tst_labelzorder 已有）**：保持「底层标注被中间不透明面遮盖、顶层标注可见」像素回归常绿，即守住了 labelsWithLayer 打标 + vendored 补丁的组合不退化（未打补丁环境下属性打标断言同样守降级路径）。

注：tst_geosymbols/tst_semanticlegend 不在本方向测试白名单，新增断言均落在 tst_symbolpicker/tst_factorworkflow/tst_labelzorder 白名单文件内（factorworkflow/labelzorder 仅作既有断言参照，不改动）。
