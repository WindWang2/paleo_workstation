# 资产覆盖对照（P2 D2.x · wave/preview-map-canvas）

每个可地图化资产类型的预览形态与交付项对照。测试证据：
`tst_previewmap_assets`（27 函数）+ `tst_datapreview`（31 函数，兼容面全保）。

## 覆盖矩阵

| 资产类型 | 预览形态 | 交付项 |
|---|---|---|
| horizon | 元数据卡 + PreviewMapPage（伪彩栅格） | D2.1 等值线 overlay（FactorContourService→GPKG→线层+标注）；D2.2 拉伸（全值域/2%-98%/均衡/手动）× 5 档色带（TOC 快调即时应用）；D2.3 直方图小图（分箱可调，拉伸界联动）；D2.9 RAW/DERIVED 版本下拉即时切换（RAW→散点信息卡，如实不造假地图）；D2.11 >50MB 无金字塔提示；D5.1–5.3 剖面线→采样→剖面图→导出 PNG/CSV；D5.4 多剖面叠绘；D5.5 统计页；D5.6 峰/洼橡皮带开关+清单；D5.7 等值线间距/标注密度参数化；D1.7 损坏栅格原因页 |
| geojson / boundary.geojson | PreviewMapPage（分类渲染）+ 属性表双视图 | D2.4 图例侧栏（渲染器 category 读回）；D2.5/D7 identify（点+框）；D2.6 名称标注开关；D4.4 分类字段/单色快调（相色逻辑在视图层回调）；D11 临时配准入口（原样保留）；同目录叠加（D2.10） |
| image_reference | 配准 → 栅格上图；未配准 → 查看器 | D2.7 world file 探测（.wld/.pgw/.jgw/.tfw/.hpw/.bpw）；托管副本旁无边车、源目录有 → 成对搬 QTemporaryDir 再上图（GDAL 只认数据文件旁的边车）；未配准 → 图片查看器 + 「去配准」引导（world file 用法 + GeoJSON 临时配准两条路）；D2.11 大图提示 |
| well_head | 信息卡 + 井位图 | D2.6 全部井位打点 + 当前井高亮（role=highlight）+ 名称标注开关（`wellHeadMapCanvas`） |
| well_stratification（DC.dat） | 分层表 + 井位落图 | D2.8 行带 X/Y → 分层顶点打点（`topsMapCanvas`），无坐标不画 |
| （测区全景） | PreviewMapPage（项目图层树桥接） | 框架全套 + 既有 objectName 兼容面（btnSurvey*/surveyMapCanvas/surveyAreaDecorManager/surveyAreaRubberBand/surveyAreaExtentLabel） |
| seismic | **P5 领地，分支函数体未动** | —（本波未触碰，见文件领地约束） |
| well_log / time_depth / document | 曲线/柱状/PDF（非地图资产） | 回归保护测试：documentTabNotMapFramework（不挂地图框架件） |
| 未知类型 | 统一「不支持预览」态 | D2.12 类型名 + 可支持类型清单（`previewUnsupportedPage`）；XML 综合柱状图优先（原行为保留） |

## 横切能力

- **D2.9 版本切换**：`previewVersionCombo`（≥2 版本才显示）；选择经
  `m_chosenVersionOfAsset` 记忆，`rebuildAssetTab` 即时重建（关闭标签清记忆）。
- **D2.10 同目录组图**：`PreviewMapStates::siblingMappableAssets`（geojson/
  带配准图片/层位栅格；版本绝对路径注入解析器）；「同目录叠加」菜单逐项
  叠加（相图自动挂相渲染器；未配准图片如实跳过）。
- **D4.x TOC**：见 CANVAS.md；状态随资产记忆（D4.7，`PreviewStateMemory`
  会话级——加层不落盘、用户变化才存、移除单删，防增量加层期间整表重写
  清掉未加层记忆）。
- **D6.x 性能**：见 PERFORMANCE.md。

## 私有层约定（不变式）

预览层（栅格/矢量/内存点层/等值线 GPKG 层）一律：
1. 不注册 QgsProject——画布只引用裸指针；
2. 所有权挂预览页宿主（`setParent(host)`），页销毁随删；
3. `PreviewIdentifyCore` 的空间索引缓存在层析构时自动清（`destroyed` 钩子）。
