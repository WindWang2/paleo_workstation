# S4 渲染性能与无屏 golden/QA 基建 R0 审计（任务域 F 大规模图层渲染 + G 无屏验证和图面 QA）

审计基线：worktree `paleo-wt-goal7-qgis-layers`，分支 `goal7/qgis-layers-layout-20261014`，基线 b0520429。全程只读。

## 链路与现状（调用图/状态机/数据流，逐环 file:line 证据）

**渲染对拍基建（无 golden 像素对拍框架，全部自写）**
- 全仓唯一渲染 golden：`tests/golden/wellcomposite/golden_syn_full.png`，对拍实现是 `tst_wellcomposite_visual.cpp:625-728` 的 `testGoldenImageSampling`：固定 900x600 离屏 `panel.render(&img)`（643-645），8x6 网格抽样、每点 7x7 均值（666-681），逐通道容差 22/255、允许 4/48 点超差（694-696, 727）。**未用 QgsRenderChecker**——全仓只有 `docs/PALEO_QGIS_PLAN.md:1264` 文档提及该思路，代码零实例。
- golden 更新流程（`tst_wellcomposite_visual.cpp:647-661`）：基线缺失即 QFAIL；`PALEO_UPDATE_GOLDEN=1` 时把当前渲染写进构建目录 `golden-out/`（绝不写源码树，#151），人工审阅后复制进 `tests/golden/`；失败现场落 `golden_syn_full.actual.png` + 逐像素 `diff.png` 到构建目录供 CI artifact（709-724）。
- 环境钉死：`PaleoTheme::pinRenderEnvironment()` 定义于 `src/ui/paleotheme.cpp:642-648` = `applyLightTheme()` → `applyTheme`（444-458）：`ensureApplicationFonts()`（vendor 字体注册，`paleotheme.cpp:245`）+ Fusion 样式 + 主题 palette + `bodyFont()`。16 个测试套件在 `initTestCase` 调它（tst_layoutexport:48、tst_wellcomposite_visual:221、tst_decorations:37 等）。
- DPR 基线：无全局 DPR 钉死，先例是 `tst_seismic_highdpi.cpp:163-170` 断言 ctest offscreen 环境 dpr 必为 1（「golden/快照测试全部在该基线跑」）；dpr=2 用例以子进程重入实现（5-9 行）。
- 其他对拍手法：`tst_layoutexport.cpp:242-256` 按 mm→px 换算 ROI 数暗像素（>20）；`tst_boot.cpp:43-48` 渲染非均匀性；`tst_labelzorder.cpp` 像素遮盖断言但自带字体交叠注释（79-81，「#138：标注盒宽度随字体变」说明字体敏感点仍在）；`tst_dragtop_render.cpp` 只验引用存活不渲染像素。

**渐进渲染/缓存链路**
- `PreviewRenderCache`（`src/qgis/previewrendercache.cpp`）：内存 LRU 16（41 行）+ 磁盘 PNG 64（42 行）；`makeKey` 按 1e-3 m / 8px 量化（42-59，定点 `'f',0` 防公里级坐标碰撞 + `sanitizeKey` 防 Windows 禁字符 #396）。
- 写缓存：`previewmappage.cpp:286-311`——renderCompleted 后 300ms singleShot settle 定时器，超时同步 `canvas()->grab().toImage()` 落缓存（#376 已占用：当前已是去抖实现）。读缓存：`primeRenderCache`（871-883）命中即 `showPreviewOverlay`，真渲完成 `hidePreviewOverlay`（previewmapcanvas.cpp:70-77）。

**画布重绘链路（② 合并/防抖现状）**
- 主画布：`QgsLayerTreeMapCanvasBridge` 挂树与画布之间（`qgiscanvascontroller.cpp:124-126`；preview 桥接 `previewmapcanvas.cpp:525`），注释声称「defers canvas-layer updates onto the event loop」（121-123 行注释，QGIS 上游实现，本仓不可见）。Paleo 侧在 `profileApplied` 后再补一次显式 `canvas()->refresh()` 兜底（`paleomainwindow.cpp:128-137`）。
- 预览画布（`previewmapcanvas.cpp:227-242`）：任何 setLayers/addLayer/removeLayer/moveLayer/setLayerVisible 都立即 `stopRendering()` + 全量 `setLayers(shown)`，**无防抖、无事务口**；调用方 `previewmappage.cpp:354-375` 每个 TOC 信号（visibility/order/opacity/blend）各自再补一次显式 `canvas()->refresh()`。
- 装饰：`PaleoDecorationManager` 在 QGIS4 无 `addDecorationItem`，装饰全部画在 viewport 上的 overlay widget（`paleodecorations.cpp:407-415` 注释），overlay 自挂 `mapCanvasRefreshed`/`extentsChanged` → `update()`（389-404）。但每个 setter 仍调 `m_canvas->refresh()`（417-471），`PreviewMapPage::setDecorationEnabled` 又无条件 refresh 一次（`previewmappage.cpp:815-824`）。

**主题应用链路**
- 页切换/层位 chip → `QgisLayerProfileService::applyPageProfile`（`qgislayerprofile.cpp:105-149`）→ 首建时 `stageTreeVisibility`（160-216，O(L) 遍历 + decl 哈希表）逐层 `setItemVisibilityChecked`；已有主题走 `applyPrunedTheme`（283-323，修剪死记录 + `collection->applyTheme` 批量改勾选）。每个勾选变化经 bridge 到画布；完成后 shell 再显式 refresh（paleomainwindow.cpp:134-137）。
- organizer 归位：`QgisLayerOrganizer` 挂 `legendLayersAdded`/`layerInstantiated`（`qgislayerorganizer.cpp:90-107`），`placeLayer` 每次复制全量 `declared()`（153 行）+ `desiredSlot` 重建 declById O(D)（228-232）+ 树扫描 O(L)；`moveNode` clone+insert+remove（64-74）。

**复杂度静态分析（③ 1/100/1000 层 cliff）**
- `LayerTreePanel::refreshIndicators`（`layertreepanel.cpp:683-737`）：全量先清后建，内层对每层节点循环 `m_layerService->declared()`（722 行）→ 单次 O(L·D)。挂接处：constructor 同时连 `legendLayersAdded`（140-145）和 `layersAdded`（146-151）——单数 `addMapLayer` 两个信号都会发 → **每加一层跑两遍 O(L·D)**；`QgisLayerService::instantiate` 是逐层单数 add（`qgislayerservice.cpp:186`）→ 批量实例化 N 层总成本 O(N²·D)。L=D=1000 时单信号 1e6 次 decl 比较，挂载一轮 ~2e9。
- `expandNewLayerNodes`（747-759）每层 `rootGroup()->findLayer` O(L)，同样挂双信号 → O(N²)。
- 预览 TOC 批量操作：N 次信号 = N 次 stopRendering+全量 setLayers+refresh（无合并点）。

**无屏（offscreen）先例（④）**
- `QT_QPA_PLATFORM=offscreen` 在测试 main 里默认设置（`tst_layoutexport.cpp:718-719`）；QgisRuntime 初始化模式普及（tst_previewmap_perf、tst_perfbudget 等）；`noFileDialogs()` 以 offscreen 为闸跳过 QFileDialog（`layertreepanel.cpp:52-55`）。限制：offscreen 无真 GPU/GL 路径（3D 用看门狗回退件）、无窗口系统交互、dpr 恒 1；TODOS 记录「CI 无底图资产——离线底图渲染测试需合成单瓦片 MBTiles 夹具」（TODOS.md:551 递延③）、「D3.12 pinch zoom offscreen 不可测」（TODOS.md:487 递延）。

**perf 标签测试与本方向口径（⑤）**
- `tst_previewmap_perf.cpp`：首开 <300ms×3 放宽（48-69）、缓存二开命中（71-107）、renderSnapshot(320) <100ms×3（109-135）、identify 索引复用（137-167）、200 点采样 <100ms（169-197）；测量值 qInfo 落盘「selfcheck 口径：能看到数字，而不是只有红绿」（24-28 行注释）。
- `tst_perfbudget.cpp`：工程打开 100ms、文件夹枚举 500ms、save 100ms（171/217/251 行），sanitizer 档 ×3 经 `perfbudget_relax.h`（13-25）；系数契约由 tst_sanitizer_budget 钉住。
- `tst_perf.cpp`：§41.6 工程打开 10s + 并发上限 max(2, cores/4)（17-24 行）。其余 perf 套件（crossplot/geostat/singlefactor/seismic/las/welllog 等 20+）与本方向弱相关。

## 可观察现行行为清单

1. **预览二开同资产同范围必中缓存**——`tst_previewmap_perf::renderCacheHitOnReopen`（71-107）直接可跑；实现 `previewrendercache.cpp:61-79` + `previewmappage.cpp:871-883`。
2. **TOC 单层可见性切换 = 中止在飞渲染 + 全量 setLayers + 整图 refresh 三连**——`previewmappage.cpp:354-358` → `previewmapcanvas.cpp:187-193, 227-242`。验证：QSignalSpy 数 renderStarted 次数 / 对单层 toggle 测 renderCompleted 计数。
3. **每个装饰开关触发 2 次整图重渲**——setter 内 refresh（`paleodecorations.cpp:428,442,456`）+ `previewmappage.cpp:823` 无条件再 refresh。验证：spy `QgsMapCanvas::renderStarting` 计数应为 2/次开关。
4. **页切换/层位 chip 切换 = 主题批量改勾选 + bridge 更新 + shell 显式 refresh 兜底**——`qgislayerprofile.cpp:130-147`、`paleomainwindow.cpp:133-137`（注释自述「再补一次显式 refresh 兜底」）。
5. **golden 基线缺失即红，更新必须人工审阅**——`tst_wellcomposite_visual.cpp:659-661`；`PALEO_UPDATE_GOLDEN=1` 只写构建目录（651-657）。
6. **实例化失败的声明层不进树，warning 卡点名**——`qgislayerservice.cpp:287-293` 收集 failures → `layertreepanel.cpp:123-133` ErrorHub 呈现。
7. **预览状态条「N 层 · M 图元 · X ms」来自 renderCompleted 实测**——`previewmapcanvas.cpp:70-77`（estimateElements 493-514：矢量 featureCount + 栅格 w×h，栅格层按全图幅像元数计）。
8. **offscreen 下文件对话框硬跳过**——`layertreepanel.cpp:52-55`，导出/导入样式动作成为无操作（559-595）。

## 发现分组

### A. 新发现（经核实未被占用）

**A1. `LayerTreePanel::refreshIndicators` 批量实例化 O(N²·D) 复杂度 cliff —— M**
现象：indicator 全量重建内层循环 declared()（O(L·D)），且 `legendLayersAdded` 与 `layersAdded` 双信号各触发一次；`expandNewLayerNodes` 同样双倍。QgisLayerService 逐层单数 addMapLayer（`qgislayerservice.cpp:186`），批量实例化 1000 层/1000 声明约 2e9 次 decl 比较，且每次都先清全树 indicator 再重建（模型行刷新风暴）。
证据：`layertreepanel.cpp:140-157`（双挂接）、683-737（O(L·D) 重建）、747-759；`qgislayerservice.cpp:274-295`（instantiateHorizon 逐层）。
影响面：层位切换（release+instantiate 全层位批次）与工程首开；100 层开始可感知、1000 层显著。
验证方式：单测 seed 1000 声明 + `instantiateHorizon`，QSignalSpy/计数器断言 refreshIndicators 调用次数并量墙钟；或给 refreshIndicators 加 declById 预建 + 单信号挂接后对比。
规模：M。→ 与 S1 A-3 为同一发现的两路独立命中，本方向立项 W2。

**A2. 预览 TOC 批量可见性/顺序变化无合并口，N 次操作 N 次全量 setLayers+refresh —— M**
现象：`PreviewMapCanvas` 每个变更 API 都同步 `stopRendering()+setLayers`（`previewmapcanvas.cpp:227-242`），previewmappage 对每个 TOC 信号再补显式 refresh（`previewmappage.cpp:354-375`）。无 beginBatch/endBatch 事务口；对比主树路径有 bridge 事件循环延迟合并，预览路径完全没有。
处置：消费方 `src/ui/datapreview/previewmappage.cpp` 超出本方向白名单 → 留档（见 05-baseline.md §4）。

**A3. 装饰开关双重整图重渲 —— S**
现象：`PreviewMapPage::setDecorationEnabled` 在 setter 内部已 refresh 后无条件再 refresh（`previewmappage.cpp:815-824`）；且 QGIS4 装饰纯 overlay 自绘（`paleodecorations.cpp:407-415` 自述），setter 里的 `m_canvas->refresh()`（417-471）本身对 overlay 装饰就是多余的整图重渲，双重叠加 = 每开关 2 次全图渲染。
处置：`src/ui/decorations/`、`src/ui/datapreview/` 超白名单 → 留档。

**A4. 页切换主题应用后 shell 显式 refresh 兜底与 bridge 更新叠加 —— S**
现象：`profileApplied` 后无条件 `m_canvasCtl->canvas()->refresh()`（`paleomainwindow.cpp:128-137`），注释自述是兜底。处置：`paleomainwindow.cpp` 超白名单 + 删除有「画布不刷新」回归风险 → 留档（正确方向是条件化，且需先有 renderStarted 计数测试）。

**A5. organizer 归位每次 placeLayer 全量复制 declared() + 重建哈希 —— M**
现象：`placeLayer` 每次 `m_layerSvc->declared()` 按值复制 QVector（`qgislayerorganizer.cpp:153`），`desiredSlot` 再重建 declById（228-232）；挂接 `legendLayersAdded`（逐层信号）+ `layerInstantiated`（102-106）→ 批量挂载 N 层 O(N·(D+L))，且每次 moveNode（clone+insert+remove）各触发一次树模型 + bridge 更新。
证据：`qgislayerorganizer.cpp:90-107, 144-182, 217-232`。
影响面：工程首开/层位切换批量实例化的主线程开销（与 A1 同栈叠加）。
验证方式：1000 声明夹具量 `instantiateHorizon` 墙钟；声明缓存到 service 侧可消复制。
规模：M。→ 本方向立项 W3。

**A6. `PreviewMapCanvasSelftest` friend 声明无定义 —— S**
现象：`previewmapcanvas.h:164` 声明友元测试类，全仓 grep 无任何 `class PreviewMapCanvasSelftest` 定义——死友元（历史遗留），误导读者以为有自测直查内部栈的测试。处置：微清理，R7 阶段顺手。

**A7. `PaleoDecorationTheme::gDark` 文件级可变静态 bool 无同步 —— S（归属风险标注）**
现象：`paleodecorations.cpp:52-57` 命名空间内 `bool gDark` 全局可变、setDark/isDark 无锁。当前调用面疑似仅 GUI 线程（未证实有跨线程调用），与 Issue #355 同族模式但不同文件；#355 明确占用 qgiscanvascontroller，#419 明确为 src/qgis 五项低危，本文件属 src/ui 且不在排除清单——若 Issue Agent 的 #355 修复范围泛化到「全局可变静态」则归 B。处置：`src/ui/decorations/` 超白名单 → 留档给 Issue Agent。

### B. 已占用（只列归属）

- **Issue #419**：previewrendercache QTemporaryDir 泄漏——基线已修（`previewrendercache.h:40`、`previewrendercache.cpp:38-40` unique_ptr 出参）；「src/qgis 五项低危」归 Issue Agent。
- **Issue #355**：qgiscanvascontroller 文件级 s_guards 全局 QHash——基线已修为实例成员 `BroadcastGuard m_guard`（`qgiscanvascontroller.h:65-73`，注释自述「实例成员而非全局表」）。
- **Issue #376**：预览 renderCompleted 同步 grab 落盘——基线已修为 300ms settle 定时器（`previewmappage.cpp:286-311`）。
- **Issue #369**：图层树栅格面运算右键菜单——`layertreepanel.cpp:336-345`，只读参考。
- **Issue #352**：协克里金工作线程调 `QgisLayerService::instantiate`——qgislayerservice 领地。
- **PR #437**：80 文件只读（qgisprojectservice.cpp、mappingartifactwriter、evidencemapadapter、mapsnapshot、mapversionstore、mappingworkbench、mapversioncontroller 等）。
- **TODOS 已递延**：pinch zoom offscreen 不可测（TODOS.md:487）；CI 无底图资产/合成 MBTiles 夹具（TODOS.md:551 ③）；「性能测试」已落地（TODOS.md:390）、「渲染对比测试稳定性」已落地（TODOS.md:391）——不得当新发现。
- 已合并交付：`tst_dragtop_render`/`tst_labelzorder` 仅只读参考；wave/layer-platform page:* 主题、layervocabulary 词表、renameMapTheme、geopatterns/semanticlegend、standardelements/mapbooklayout 不在本方向重报。

### C. 已修复/与文档不符但不构成缺陷

- `docs/PALEO_QGIS_PLAN.md:1264` 称渲染回归「参考 QGIS 自身的 QgsRenderChecker 思路」——代码零使用，实际为自建抽样/暗像素计数；TODOS.md:391 已记「渲染对比测试稳定性已落地」，文档-实现对偏差属历史决策记录，非缺陷。
- 缓存键定点格式碰撞、Windows 禁字符落盘静默、磁盘层无限增长——均已在代码内修复并内联注释（`previewrendercache.cpp:45-59, 93-98, 99-106`，#396/#376/#419）。
- `qgiscanvascontroller` 的 `mapConfigurationChanged` 已做视野 CRS 变换保留（`qgiscanvascontroller.cpp:140-156`），非裸 refresh。
- 渲染预算墙钟断言统一带 ×3 CI 放宽 + qInfo 落盘（`tst_previewmap_perf.cpp:24-28, 66-68`），口径一致。

### D. 低价值不建议做

- **给预览画布引入按图层脏区域/增量渲染**：QGIS4 QgsMapCanvas 无此 API，要动 vendored 渲染管线，工作量 L 收益不可证。
- **删除 paleomainwindow 的兜底 refresh（A4）**：注释明确是防漏语义，删除可能引入「主题应用后画布不刷新」回归；正确方向是条件化，且需先有 renderStarted 计数测试才能动。
- **把装饰 setter 的 refresh 全删（A3 的一部分）**：grid 装饰 label 依赖 `mapSettings.mapToPixel()`（`paleodecorations.cpp:210`），虽画在 overlay 上但 extentsChanged 才重绘——需先证实 overlay 更新足够，不建议无条件删。

## 建议的非回归验证场景（无缺陷时的守恒验证）

1. **渲染缓存与预算守恒**：`tst_previewmap_perf` 全套（首开 300ms、二开缓存命中、快照 100ms、identify 索引复用、采样预算）+ `tst_perfbudget` 三项 + `tst_sanitizer_budget` 系数契约——任何 S4 改动后跑这三套，qInfo 实测值与基线比对可发现相对劣化。
2. **树操作后渲染不空白/勾选不丢守恒**：`tst_dragtop_render`（组/层 InternalMove 引用存活 + 勾选态）+ `tst_labelzorder`（标注 z 序像素遮盖）——大规模图层改造（A1/A5）后必跑。
3. **无屏 golden 环境守恒**：`tst_seismic_highdpi::dpr1BaselineIdentity`（dpr=1 红线）+ `tst_wellcomposite_visual::testGoldenImageSampling`（48 点抽样容差）——验证渲染环境（Fusion/vendor 字体/palette/offscreen）未被改动波及。
4. **主题应用守恒**：`tst_layoutexport`（导出尺寸/页范围/异步导出）+ 页切换相关现有套件——A4 若做条件化，用页切换后画布非空白 + renderStarted 次数上界双重断言。
