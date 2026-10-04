# Goal Loop ledger — geological-symbols（地质符号库与花纹体系）

- 分支：`goal/geological-symbols-20261004`，基线 `dda2faa`（origin/master）
- worktree：`.worktrees/geological-symbols`；构建 `cmake --build build -j8`
- 语义全文：`goal-loop-prompts/31-geological-symbols.md`
- 素材基线：`resources/geology/`（qrc prefix `/geology`，编进 `paleo_domain`）
  ——textures/ 26 项可平铺岩性纹理（Q/HS 1011—2016 附录 F.1/F.2/F.3）、
  strata/*_fill.svg 8 项相花纹（附录 O.1/O.2）、faults/ 34、wells/ 34、
  boundaries/ 36；`catalog.json` 312 条为图录权威词面源。

## 轮 0 — 口径

- **单点**：符号语义注册表新文件 `src/qgis/geopatterns.{h,cpp}`（GeoPatterns
  namespace）；样式入口统一挂 `QgisStyleService` 静态方法，UI 只读词表不碰
  Qgs 符号代码。qrc 路径解析复用 `FaciesCatalog::resourcePath()`（资源激活
  单点，先例 mappingartifactwriter.cpp:345 已用 `QgsSVGFillSymbolLayer(qrc, 10)`）。
- **词表 id 全局唯一**（lithology/facies/line 三族共一个 id 空间），词面
  `QT_TRANSLATE_NOOP("GeoPatterns", …)`（对齐 QgisStyleService 井别词表惯例）。
- **断层齿**：程序化 marker line（QgsMarkerLineSymbolLayer + triangle/cross
  glyph）首版，不用 faults/ 图例卡做平铺（那是整图符号不是齿素材）。
- **DESIGN.md 授权**：DESIGN.md:118 地图域数据符号归 QGIS 样式系统管理，
  不属 UI token；图例文字 8pt（DESIGN.md:33）在图例轮生效。
- **i18n**：词面走翻译面（lupdate 词条 NOOP 惯例）。
- **测试**：新 `tests/tst_geosymbols.cpp`（LIBS paleo_qgis paleo_domain），
  样板照 tst_services2.cpp。

## 批 1/2 — 花纹词表 + 入口 + 井别扩展（commit 2dbe651）

- 岩性 14（textures/ 附录 F）/相 8（strata _fill 附录 O）/线型 8 词表；
  applyLithologyPatternStyle/applyFaciesPatternStyle/applyFaultLineLayerStyle
  三入口；井别 12→16（开发区块 4 类，行业惯例近似——**出处注**：表 K.1
  只覆盖探井，生产/注水/采水图式按油田惯用图版近似，色源
  catalog.json 井型表）+ 全类 @map_scale 数据定义尺寸（1:25万 6mm→
  1:250万 3mm）。
- tst_geosymbols 9 项绿；tst_services2 13 项绿（井别 16 类断言同步）。
- 修复：include 相对路径（../domain/faciescatalog.h）；QgsSimpleLine
  在 qgslinesymbollayer.h；bigSymbolPreviewImage V2 歧义；layerType
  字符串改 dynamic_cast 判定。

## 批 3 — 图例 builder（commit 57d32d5）

- SemanticLegendBuilder：layerOrder 顶部层在前的树显示序（与画布 z 序
  同源）；分类渲染器逐类目（label/semanticId/swatch），single 走
  legendSymbolItems 回退；layersAdded/Removed + rendererChanged/
  nameChanged → legendChanged。
- tst_semanticlegend 6 项绿。修复：range-for 悬空引用（build() 临时
  容器先落局部——SIGSEGV 教训）；QgsLegendSymbolList 在 QGIS 4 是
  QgsLegendSymbolItem 类列表（非 QPair）。

## 批 4 — 符号选择器（commit ec66d48）

- SymbolPickerPanel（src/ui/symbols/）：三族 IconMode 网格 + 词面/id
  包含过滤；patternPicked 语义 id 出口（视图只发信号）。缩略同图例
  管线。GeoPatterns::lineSymbolFor 线型统一构造口。
- tst_symbolpicker 5 项绿（QTest::mouseClick 真事件出信号）。
- **遗留**：主窗口 dock/页面接线未做（宿主归属方向 24/25 的页面 UX
  决策，panelHost 是 per-page stack）。

## 批 5 — 样式版本语义（commit c853161）

- paleo/symbolSemantics = {family,id,version}（symbolTableVersion=1）；
  applySymbolOverride familyOf 精确族校验 + 几何匹配；restore 幂等。
- tst_geosymbols 增 round-trip（Oracle 1）：path/宽度/旋转逐字段、qrc
  锚定、断层两层结构、拒绝三分支、重放幂等——一次过。

## 自审轮 1（六维）

- 无 High/Medium。Low 修复 2：注释引用不存在的 wellScaleExpression
  （改为就地描述）；restoreSymbolOverride 的 QVariant::type() deprecated
  （改 userType()==QMetaType::QVariantMap）。补 Oracle 1 旋转字段断言。
- 分层（layering×3 绿）/样式收口单点（QgisStyleService 唯一入口，
  GeoPatterns 为其实现件）/资源打包（零新增 SVG，全复用 qrc 既有素材，
  无 PATCHES.md 事项）/语义映射完整（词表-归一-桶-覆盖四层一致）/
  i18n（NOOP 词面 + tr；i18n×2 绿）/DESIGN.md（数据符号条款合规，
  色不进 UI token）。

## 自审轮 2

- 全文件复读：geopatterns 构造/词表无新发现；tst 断言与文档口径一致。
- ledger 本轮补记。

## 收尾 — 全量测试结论（Oracle 4）

- 本 diff 面全绿：tst_geosymbols 10、tst_semanticlegend 6、tst_symbolpicker 5、
  tst_services2 13、tst_layertreepanel 1、tst_ui 1；layering×3、
  ui_invariants×3、i18n×2、git diff --check 全过。
- 全量 ctest（-j8）偏差 3：tst_singlefactor_perf、tst_startup_trace、
  tst_catalog。
  - **均非本 diff 因果**（两路证据）：①LIBS 面不含改动库
    （paleo_algorithms / paleo_services），nm 验证两测试二进制零新符号
    （GeoPatterns/symbolpickerpanel 0 命中）；②主仓基线 build（src 无本
    diff）同样失败：tst_startup_trace 基线 healthy=0.205 degraded=0.570
    （x2.8 < 3.0 门，与 worktree 同一断言红）、tst_singlefactor_perf 基线
    同红（L median 超 15000ms 预算门）——性能门对机器当日负载敏感。
  - tst_catalog 串行重跑即过（flaky，非稳定失败）。
  - 结论：性能门红为基线既有/环境性，归属 perf 方向收口，本 PR 注明。
