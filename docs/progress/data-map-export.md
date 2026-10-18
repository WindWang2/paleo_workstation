# 数据与地质图导出

日期：2026-10-18。对应方案：`docs/EXPORT_PLAN.md`。

## 使用入口

- 数据管理列表的“导出数据…”、资产右键的“导出数据／制作源数据…”及命令面板中的同名命令。
- 智能编图的“导出地质图…”；沿用既有按钮/信号标识，界面不再限定为厚度图 PDF。
- 默认工作台保留该 ribbon 动作；“文件 → 导出工程地图／地质图…”也进入同一流程。
  井位等工程地图不要求先选择层位。
- 版面设计器的 PNG/PDF/SVG 导出继续保留，增加按运行时能力启用的 TIFF 动作。
- 既有“按层位批量出图”增加格式、DPI 和导出父目录选择。

## 数据输出

导出界面可选择 RAW 原件、当前/历史版本、成果及制作源数据，逐资产选择明确版本。
开始前在任务池检查来源、文件大小和缺失项；显示可读取项数、源文件估算大小及问题。
原格式复制、自动交换格式、GPKG、GeoJSON、Shapefile、CSV、数值 GeoTIFF 分开选择。
RAW 模式保持原格式。专业文件没有通用格式转换器时，应选择原格式复制。

源数据扫描复用 catalog 图谱，读取计算输入、编辑基线、回滚来源与解释关系。
图斑人工证据复用 evidenceindex 的原有 GeoPackage 解析器，以只读方式读取
`facies_evidence`，不写证据索引，不把当前最新版本替换为历史输入。
已登记图件的 `map_exports` 记录也参与来源追溯；同一内容有不同来源批次时报告歧义。
普通参考资料可选，已解除的图斑关系跳过；缺失、循环、证据损坏或截断均有明确结果。

服务分块复制并核对 SHA-256，保留 Shapefile 等配套文件。
转换由功能层调用 QGIS 写出器，数据服务不反向 include QGIS。
数值 GeoTIFF 使用仅含数据 provider 的管线，保留类型、波段、NoData、CRS 和网格，
不把渲染后的颜色当作数值。GeoPackage 保留子图层；CSV 导出属性和 WKT。
GeoJSON 仅在可建立经纬度变换时输出，未配准数据使用 GeoPackage。
新写出的 CSV 使用 UTF-8，并附 CSVT 保留数值列类型；已有 CSV 的原格式副本保留其编码与配套文件。

每批写到新目录，附 `metadata.json` 和 `README.txt`，记录版本、关联实体、来源边、
文件摘要、字段说明、已有单位/参数和失败项。失败项使 `complete=false`，
取消清理未提交暂存目录，已有目录不覆盖。原件副本不重复登记 RAW。

## 地质图输出

主流程读取当前可见地图、范围、旋转和具体数据版本，不再要求先生成厚度栅格。
用户可选择井位图、单因素图或沉积相图骨架，编辑图名、层位/时代说明、图签、
A4/A3 横竖、比例尺与 DPI，在原生 QgsLayoutView 预览后多选输出格式。

图件复用既有 QGIS 标准元素工厂：主图、真实渲染器图例、比例尺、方向、网格、
图框、标题、图签、CRS 说明及沉积相图位置插图。局部工程坐标注明未配准及网格北；
自定义投影不因没有 EPSG 编号而被误当作工程坐标。未保存矢量编辑须先保存。

PNG/PDF/SVG 使用同一布局快照，TIFF 探测编码器；提供透明纸面、world file/CRS、
PDF/SVG 文字转轮廓或保留文字、按后端能力启用的 GeoPDF。
GeoPDF 额外要求已配准 CRS。透明输出使用布局副本，不改原纸面。

图件任务在私有 QGIS 工程上执行，检查必要图层，逐格式回读，登记实际格式的 OUTPUT。
绑定输入在绘制前后核对摘要，输入文件变化时不登记为历史版本图件；说明文件也冻结输入版本的已有单位、参数等元数据。
新目录包含所有图件、QPT 及版本/摘要说明；可附带当前地图图层的数值 GPKG/GeoTIFF
与 QML，并可附带已绑定版本的制作源数据。未绑定的图层不猜来源，图件说明列出缺口。
只有实际导出的 PDF 才进入原有 PDF 发布条件，PNG 不冒充 PDF，也不自动发布。

批量流程保留版面恢复和工程世代检查，登记、交付全部页面，输出逐层位清单；
取消保留此前已完成的层位并如实列出未完成项。任务完成后释放暂存目录和工程快照，
不让任务历史长期占用文件与内存。

## 验证

本机构建通过：`cmake --build build -j1 --target paleo ...`，使用既有 vendor QGIS 4.2.2。
相关 18 项 CTest 以 `-j4` 全部通过，包含：

- 新增 `tst_dataexport`、`tst_exportpipeline`。
- `tst_layoutexport`、`tst_horizonbatchexport`、`tst_layoutdesigner_full`、`tst_layoutpalette`、`tst_layoutshell`、`tst_mapbook`、`tst_standardelements`。
- `tst_panels_dataops`、`tst_datagraph`、`tst_evidenceindex`、`tst_mapping`、`tst_versions`、`tst_release`。
- `layering`、`layering_strict`、`layering_selftest`；`git diff --check` 通过。

新增回归覆盖历史输入去重、图斑解释来源、原件摘要一致、篡改/缺失/取消、
GPKG 与 CSV 数值/空值、CSVT 数值列、栅格数值/NoData/网格回读、预检不产生导出文件、
主窗菜单入口、三种格式及实际资产类型、工程切换丢弃、批量多页、透明纸面与 world file、
TIFF 能力检测，以及方形/狭长范围下图框和比例尺不得越界、长图例不得静默裁切。

产物目视检查发现并修复原模板的 `setExtent()` 改变地图框高度问题：标准图框及批量
换内容改用保持纸面框尺寸的 `zoomToExtent()`，重新预留坐标与页脚空间，比例尺完整置于地图内。
坐标注记使用 DESIGN.md 的 JetBrains Mono 9pt，并避免左右长坐标被页面裁切。

本机示例位于 `build/export-qa/standard-C6-final/`，包括 PNG、PDF、SVG、QPT、GPKG/QML 和说明文件。
PNG 与 PDF 光栅化预览已目视检查；PDF 为 A4 横向单页，SVG 通过 XML 解析。
另以 `QT_SCALE_FACTOR=2` 执行格式/像素验证通过，输出尺寸保持由纸张和 DPI 决定。
以上示例为测试数据，用于检查文件与制图要素，不代表真实工区的解释成果。

## 当前边界

- VRT 不仅是单个文件；当前明确要求先保存为独立 GeoTIFF，避免导出只有声明而缺数据的包。
- 有活动 WAL/回滚日志的 GeoPackage 不做原格式裸复制，须先完成写入；当前地图的图层数据可以走 QGIS 写出。
- GeoJSON/Shapefile 具有各自的坐标/字段限制；无损空间交换优先使用 GPKG。
- 远程底图记录来源，不下载离线瓦片。
- QPT 是版面模板，需要先加载相应图层；本轮不生成可移植 QGZ，也不生成 ZIP。
  导出的 QML 可能引用 Paleo 的地质符号资源或扩展渲染器，不能据此宣称可在任意 QGIS 安装中无依赖还原。
- 本轮不补造历史工程缺失的来源、单位、验证结论或算法参数。实际真实工区仍需结合使用者工程验收。
