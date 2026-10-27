# QGIS 坐标交互统一移植与优化报告

日期：2026-10-27  
分支：`codex/qgis-coordinate-inputs-20261027`  
基线：`origin/master` @ `9fdc7bcf`

## 1. 目标与评审结论

本次处理的是项目内所有**地理空间位置、CRS、坐标转换和地图范围**入口，不是把深度、时间、图版毫米、3D 相机角度都误当成经纬度，也不是复制 QGIS 主程序源码。

评审发现：工程基准与井点来源 CRS 使用裸文本框；布井 X/Y 隐含局部米制；主图与预览分别手拼坐标；地图册只有四个范围数值框；缺少统一的点转换与定位入口。上述方式不能利用 QGIS 的 CRS 搜索、坐标格式、基准操作、范围拾取与转换上下文，且容易造成“声明坐标系被误认为完成转换”的风险。

QGIS 4.2 的公共 core/gui API 已提供所需能力。QGIS `src/app` 的专有状态栏/坐标输入实现及地理配准插件不是可直接链接的公共控件，因此采用**原生公共控件 + `src/qgis` 薄封装**，不复制 QGIS app 私有实现，不另建一套 PROJ 坐标引擎。

## 2. 入口覆盖与优化方案

| 入口 | 评审问题 | 实施方式 |
|---|---|---|
| 新建工程 / 工程资料设置 | 基准与来源 CRS 为裸文本，无有效性反馈 | `PaleoCrsSelector` 包装 `QgsProjectionSelectionWidget`；支持 QGIS 搜索、最近使用、自定义 CRS 和“自动识别／未设置”；保留扫描 revision 与资料映射契约。已有工程基准只读。 |
| 工程坐标与底图 | 原生地图 CRS 已有；局部配准锚点缺少统一坐标输入 | 复用公共 CRS 选择器；WGS84 锚点使用 `PaleoCoordinateEditor`，支持经纬度格式与范围校验。原生投影工程禁止在此清除计算基准。 |
| 布井辅助 | X/Y 裸文本，来源与转换边界不明确 | 公共点位编辑器，明确来源 CRS 与格式；地图拾取和手工输入均先转换到工程计算坐标，成功后才调用 `WellSitingWorkflow`。 |
| 主地图状态栏 / 文件菜单 | 手拼坐标，缺少统一定位与复制 | QGIS 格式化与精度；“QGIS 坐标工具”支持输入/拾取、源/目标 CRS、正反转换、基准操作、复制与定位；可复制当前地图 CRS 或 WGS84。 |
| 预览地图 | 独立手拼读数，复制形式单一 | 复用同一格式化和坐标工具；分别复制当前预览 CRS 与 WGS84，使用预览自己的转换上下文。 |
| 地图册 AOI | 不能取当前视图、图层或框选范围 | `QgsExtentWidget` + `PaleoMapExtentWidget`，支持手工范围、当前视图、实际图层与画布框选；保持 `PaleoMapBook::Area` 和导出契约。 |
| 数据管理实体创建 / 编辑 | 井口 X/Y 为数字微调框，来源 CRS 与工程存储 CRS 边界不清 | 公共 `PaleoCoordinateEditor`；默认按当前工程存储 CRS，输入可显式选择来源 CRS，保存前转换到工程存储；非法 CRS、非法经纬度和转换失败均不写入实体。 |
| GeoJSON 临时仿射 | 坐标偏移、缩放与旋转混排 | 独立 `PaleoProvisionalRegistration::prompt` 原生对话框；明确米制平移、仿射参数和范围预览；不改变“手工仿射、非权威坐标”语义。 |

## 3. 公共实现边界

新增公共代码均在 `src/qgis`，头三行标记 `// 层：QGIS 封装`：

- `coordinateformat`：`QgsCoordinateUtils` / `QgsCoordinateFormatter` 的格式化、十进制度与度分/度分秒解析、经纬度范围检查、显式转换和读数。
- `crsselector`：`QgsProjectionSelectionWidget` 的选择、自动/未设置状态、EPSG/WKT 序列化和米制基准校验。
- `coordinateeditor`：点位编辑、格式选择、地图回填、来源到目标的显式转换、工程存储 CRS 绑定及就地错误；视图侧只使用 `QPointF`、`QString` 与服务指针，不直接散布 QGS 坐标类型。
- `coordinatetoolsdialog`：源/目标 CRS、点转换、`QgsCoordinateOperationWidget` 的基准操作与 fallback 状态、复制、定位。
- `coordinatemaptool`：基于 `QgsMapToolEmitPoint` 的吸附拾取，左键确认、Esc/右键取消、单次退出。
- `mapextentwidget`：`QgsExtentWidget`、实际图层范围与当前画布转换上下文。

QGIS 封装层不引用 `ui/` 或 PaleoTheme。主题、JetBrains Mono 数字字体和 QGIS 图标由视图层按 `DESIGN.md` 提供。主窗坐标壳接线和预览坐标壳接线分别位于 `paleomainwindow_coordinates.cpp` 与 `previewmapcoordinates.cpp`。

## 4. 坐标安全与业务守恒

- 未知 CRS 不猜 EPSG、带号或轴序；未知来源必须明确选择，不能借用工程 CRS 冒充已知来源。
- 声明 CRS 不等于转换。原始资料与既有导入/派生链路不变，转换按明确来源和目标执行。
- 输入固定使用 easting/northing（经度/纬度）顺序，不按数值大小交换轴；经纬度分别检查 ±180/±90，局部大坐标不套用该范围。
- 十进制度、度分、度分秒和 N/S/E/W 后缀走 QGIS 解析；非数值、NaN/Inf、非法 CRS、转换失败和非有限结果均阻止提交。
- 定位结果先反算到当前画布 CRS 再移动地图；未配准工程只显示局部网格，绝不把局部数值标成经纬度。
- 基准操作只作用于当前工具的转换上下文。缺失网格的显式操作不静默近似；若 QGIS 默认路径发生 fallback，结果中明确标注。
- 不改变 `ProjectSetupSources`、`PaleoGeoreference`、`PaleoMapBook::Area`、`EntityOverride` 的持久化/导出契约，不改变计划井、评估、地图册和实体目录业务算法。
- 深度、TWT、图版毫米、3D 相机和网格间距不是本次地理坐标输入，不套用经纬度控件。

## 5. 规模与分层纪律

临时仿射、布井坐标区和地图册装配分别拆为独立职责。函数规模实测：`buildGeoJsonContent` 488→407、`WellSitingPanel` 253→225、`attachShellSurfaces` 548→477，冻结值只下调不上调。

`qgs-audit` 从基线删除 3 个已收敛文件，`qgis-spread` 删除 4 个；检查时为 133 / 167，新增直连均为 0。没有通过新增豁免或上调基线放行。主窗家族新增 `_coordinates.cpp`，`docs/UI_LAYER_PLAN.md` 已同步为 30 TU。

## 6. 验证方案与证据

测试使用真实 `QgisRuntime`、原生 Qt 控件树与合成坐标点，不依赖私人工区：

- `tst_coordinateinputs`：CRS/WKT、格式与范围、已知点正反转换、基准操作、地图拾取与取消、原生范围、窄面板和主题截图。
- `tst_projectsetup` / `tst_projectcoordinates`：工程 CRS 声明、自动识别、锚点校验与既有导入/转换回归。
- `tst_wellsiting_panel`：手工转换后写库、未知 CRS 不增加计划井、既有布井工作流。
- `tst_mapbook`：原生范围、输出 CRS、格序列与导出参数契约。
- `tst_panels_dataops`：实体创建/编辑使用工程存储 CRS、输入来源 CRS 显式转换，非法经纬度与未知 CRS 不写入且保留原坐标。
- `tst_ui`：主窗口真实的转换、定位、复制路径。
- 源码闸门：layering、qgs-audit、qgis-spread、ui_tokens、ui_invariants、ui_size、file_budget、i18n、docs_drift 与 `git diff --check`。
- 视觉证据：浅色/深色、1×/2×、原生 CRS/坐标工具、布井、地图册、配准、实体编辑与主窗口。证据目录 `docs/evidence/qgis-coordinates-20261027/`，截图和 SHA-256 由 `manifest.json` 索引。

构建使用项目 `paleo-dev.ps1` 与要求的 CMake 3.29.3 / MSVC / QGIS 环境，编译和测试并行度不超过 8。Windows 测试使用独立注册表沙箱与树内临时目录；Python 闸门使用 UTF-8。

### 最终验证记录

- 构建：`paleo-dev.ps1 build` 全量链接成功（`paleo.exe` 与相关测试目标均生成）。
- 聚焦回归（均在本分支最终代码上跑过）：`tst_coordinateinputs` 13 项通过；`tst_wellsiting_panel` 6 项通过；`tst_projectsetup` 41 项通过、7 项跳过；`tst_projectcoordinates` 12 项通过、2 项跳过；`tst_mapbook` 20 项通过；`tst_previewmap_page` 27 项通过；`tst_panels_dataops` 全量通过，其中新增实体坐标用例单独通过；`tst_ui` 的 `coordinateMenuTransformsLocatesAndCopies` 单独通过。主窗完整 `tst_ui` 仍有 1 个与坐标无关的既有失败（`testSimultaneousWellsAndSurveyAreaVisibility`）。
- 源码闸门：layering / layering_strict / layering_selftest / layering_symbol_audit / layering_transitive / layering_qgs_audit / qgis_spread 族 / ui_invariants / ui_size / file_budget / i18n / ui_tokens / qgis_versions / docs_drift / cpp_mainline 共 30 项全部通过。
- 带 `QGIS_PREFIX` 的 `--transitive` 检查仍为 `ui_qtwidgets_notes: 6 > baseline 5`；同一检查在 `origin/master` 临时工作树上同为 6/5，属存量传递债。本分支没有新增该指标，也不上调基线。
- 全量 CTest：使用 `PALEO_CTEST_ARGS=-j8` 跑完 485 项，结果为 449 通过 / 36 失败。该轮次暴露并已修复两类本分支问题：符号级词表把 `class QWidget;` 前向声明误当 UI Q_OBJECT 类（检查器已修并加自检）、状态坐标帮助文案丢掉“已配准”契约（已恢复）；另补齐了一个可见 Ribbon 命令缺失的帮助文本。剩余失败集中在在线底图/PDF、工程与文件锁、性能预算和并行竞态等环境项，不属于坐标输入、CRS 选择、坐标转换或地图范围功能路径，因此本报告不宣称全量 CTest 全绿。
- 截图复核：1× 与 2× 浅/深截图已实际读取复核；范围输入采用 QGIS 展开式控件，坐标工具显示源/目标 CRS、基准操作和转换结果，实体与布井保存前显示“输入 → 工程存储”的转换边界。
