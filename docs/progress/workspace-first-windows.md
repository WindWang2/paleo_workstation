# Windows 原生工作区改版验证

本次改动落在 Paleo 的 Qt 主窗口。地图与参考图使用 QGIS 画布，井道、地震剖面和资料预览使用工程中已有的控件。设计与原型见[工作区改版方案](../designs/WORKSPACE_FIRST_UI_PLAN.md)。

2026-11-26 在 Windows 上构建 Release 版工作站和 12 个相关测试程序。六套完整 Qt 回归与其余专项用例合计 **145 个不同的测试用例通过，0 个失败或跳过**。计数剔除了初始化、清理和重复执行的用例。源码检查 39 项通过，启动器的六项 QGIS、GDAL 和坐标系统自检通过。

## 实际操作

| 操作 | 结果 |
| --- | --- |
| 打开一张相图，切换顶部五类工具 | 中央文档保持打开。地图标题显示当前图件及版本，资料预览显示所选文件。图层搜索常驻，搜索框右侧可以展开排序、分组等操作。 |
| 在已登记图件与人工工作副本间切换 | 图件显示对应的版本，工作副本在标签上明确标注，悬停可查看其关联的已登记版本。 |
| 按 F8，再按 F8 或 Esc | 进入与退出专注。退出后恢复原停靠布局、活动文档和焦点。Esc 优先取消地图捕获或关闭弹层，空闲输入框也能用 Esc 退出专注。 |
| 打开未配准的本地相图，改变工作区大小 | 图片按比例居中，随文档宽高重新适配。预览一次解码，缩放或进入专注复用同一张图。配准入口在图下方。 |
| 在浏览模式点击已登记簇面的一块图斑 | 右侧显示当前微相、井证据与目标微相。用户主动收起后，后续点选沿用这项偏好。 |
| 在“单图斑”和“整簇图斑”间切换 | 先显示作用数量与轮廓；应用到草稿后可撤销、重做。保存新版本时复核工程、源版本、层段与写入权限。 |
| 对照当前图件 | 中央打开只读参考图，优先显示工作副本的来源版本，用户也可以手选其他来源。宽窗左右并排。中央宽度小于 780 逻辑像素且只有一份参考时，两个标签共用同一区域；参考标签显示层位及版本，展开“参考设置”可更换图件、解除联动或添加视图。 |
| 从功能区打开地震剖面或连井剖面 | 同一个剖面视图进入中央文档。关闭标签或从布局菜单调回原面板时，井段、切片与编辑模型随视图保留。 |
| 后台任务启动或向 Agent 提问 | 任务进度显示在状态栏，单击后查看详情。Agent 每次发送时记录当时的工程、层段、版本及读写状态。 |

## 测量口径

Windows 11、MSVC Release、仓库管理的 Qt 与 QGIS。测试工程、截图和操作记录保存在 D 盘独立验证目录。旧版使用本次改动的直接基底 `be0283b5`，两版打开同一套可重建的编图样例，显示同一图层，并按相同地理范围定位。

地图面积按 `QgsMapCanvas::viewport()` 的宽乘高除以主窗口客户区的宽乘高计算。尺寸使用 Qt 逻辑像素。工具条和停靠面板计入客户区，窗口外框不计入。覆盖在地图上的地质图例仍属于 viewport。先在浏览状态测量，按 F8 进入专注再次测量，再退出专注、打开对照。对照记录两幅实际地图的 viewport。窄屏依次切到主图和参考图，等布局稳定后读取各自的宽高，并断言默认收起“参考设置”时宽高分别相等。

在同一 1440 × 900 客户区，旧版浏览为 **782 × 514（31.0%）**，新版为 **1212 × 745（69.7%）**。进入专注后为 **1434 × 771（85.3%）**。并排对照的主图为 603 × 720，参考图为 563 × 600。测试还校验了主图版本 `ver-3` 与来源版本 `ver-2` 的来源关系、主编辑目标和活动地图工具。

| 目标客户区（设备像素） | 模拟缩放 | Qt 逻辑客户区 | 浏览地图 | 专注地图 | 参考布局 |
| --- | --- | --- | --- | --- | --- |
| 1366 × 768 | 100% | 1366 × 768 | 1138 × 613 | 1360 × 639 | 并排 |
| 1366 × 768 | 125% | 1093 × 614 | 865 × 425 | 1087 × 485 | 并排 |
| 1366 × 768 | 150% | 911 × 512 | 683 × 323 | 905 × 349 | 原位切换 |
| 1920 × 1080 | 100% | 1920 × 1080 | 1692 × 925 | 1914 × 951 | 并排 |
| 1920 × 1080 | 125% | 1536 × 864 | 1308 × 709 | 1530 × 735 | 并排 |
| 1920 × 1080 | 150% | 1280 × 720 | 1052 × 565 | 1274 × 591 | 并排 |

在 1366 × 768、150% 缩放的场景，切换前的主图和切换后的参考图均为 **683 × 293** 逻辑像素。参考版本标在标签上。点“参考设置”可换来源、解除范围联动或增加另一幅参考图，收起后恢复完整绘图区。

缩放矩阵在同一台 Windows 主机上运行。每种组合单独启动使用 Windows 平台插件的 Qt 进程，以 `QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough` 配合 `QT_SCALE_FACTOR` 调整缩放倍率。该主机原始缩放为 150%，因而目标 125% 的进程系数为 125/150。请求的逻辑客户区为目标设备像素尺寸除以目标倍率，取整到最近的逻辑像素。记录窗口实际报告的 `devicePixelRatioF()`（1、1.25、1.5）及客户区尺寸。浅色和深色均使用原生窗口渲染，各次运行使用独立用户配置。物理显示器间拖移窗口时的动态 DPI 切换未纳入本轮验证。

[旧版浏览](../evidence/workspace-first-qt/before.png) · [新版浏览](../evidence/workspace-first-qt/browse.png) · [专注](../evidence/workspace-first-qt/focus.png) · [双图对照](../evidence/workspace-first-qt/comparison.png) · [深色主题](../evidence/workspace-first-qt/dark.png) · [原生窗口操作短片](../evidence/workspace-first-qt/workspace.mp4)

[窄屏单参考](../evidence/workspace-first-qt/narrow.png) · [图斑检查器](../evidence/workspace-first-qt/inspector.png) · [逐场景尺寸、地图占比与像素比](../evidence/workspace-first-qt/measurements.json)

## 原生回归入口

| Windows 测试 | 覆盖的行为 |
| --- | --- |
| `tst_shell_chrome` | 五类工具、文档切换、F8/Esc、用户停靠意愿、布局恢复和后台任务。 |
| `tst_ui` | 同一地震/连井控件进出中央标签、图层及编辑目标、780 像素边界、参考跨工程恢复。 |
| `tst_mockseismicunit`、`tst_seismicclusterpatches` | 用同簇两个实际矢量要素验证单斑和整簇作用域、撤销、只读保护、失败后的重试、源文件校验值、已保存结果和地图范围。 |
| `tst_mappingworkbench` | 当前副本与来源版本的选择、用户手选参考、参考只读、两份图例以及已有参考的恢复。 |
| `tst_datapreview`、`tst_seismic_sectionui` | 井道与文件标签的绑定、当前文件版本、资料表和地震剖面控件。 |
| `tst_previewmap_assets`、`tst_paleomaplibrary` | 已配准与未配准图像的预览入口、图层顺序、复用既有图件原件。另以 D 盘本地相图导入独立工程，验证专注前后的文档身份与实际居中效果。 |
| `tst_g5_conversation_workbench` | 在两个不同的工程上下文中发问，校验模型收到的工作区快照与历史消息。 |

在已备齐受控依赖的 Windows 工作树根目录，用 PowerShell 构建 Release，并串行复核本次按整套执行的已注册测试。仓库脚本负责设置 MSVC、Qt 和 QGIS 环境。[Windows 构建说明](../../BUILDING.md)

```powershell
$env:PALEO_CMAKE_BUILD_TYPE = 'Release'
$env:PALEO_JOBS = '8'
.\paleo-dev.ps1 build
$env:PALEO_CTEST_ARGS = '-j1 --no-tests=error -R ^(tst_shell_chrome|tst_dockmanager|tst_layertreepanel|tst_seismicclusterpatches|tst_g5_conversation_workbench|tst_seismic_sectionui)$'
.\paleo-dev.ps1 test
```

源码检查可用同一仓库入口复跑。额外传入实际使用的 QGIS 头文件目录，覆盖跨层依赖检查的全部指标。

```powershell
$env:PALEO_CTEST_ARGS = '-j1 --no-tests=error -LE core|perf'
.\paleo-dev.ps1 test
& $env:PALEO_PYTHON .\tools\check_layering.py --transitive --qgis-include .\vendor\osgeo4w\apps\qgis\include
```

复现窄屏原位切换与图斑检查器时，可在上述构建环境中直接运行相应的 QtTest 用例。下例会在 D 盘保存范围、草稿与新版本截图。默认构建目录为 `build`，使用独立构建目录时替换为其实际路径。

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:PALEO_WORKSPACE_INSPECT_EVIDENCE = 'D:\PaleoWorkstation-validation\workspace-inspector'
New-Item -ItemType Directory -Force $env:PALEO_WORKSPACE_INSPECT_EVIDENCE | Out-Null
Push-Location .\build
try {
    .\tst_mappingworkbench.exe workingMapComparisonUsesSourceAndKeepsManualReference
    .\tst_mockseismicunit.exe rawClustersPublishExactNativeGridAndReopen
} finally {
    Pop-Location
}
```

复核中央画布可运行 `tst_shell_chrome::workspaceFocusRestoresDocumentLayoutAndIntent`。设置 `PALEO_WORKSPACE_QA_DIR` 可保存原生浏览与专注截图。复核赋相流程可运行 `tst_mockseismicunit::rawClustersPublishExactNativeGridAndReopen`。设置 `PALEO_WORKSPACE_INSPECT_EVIDENCE` 可保存范围、草稿与新版本截图。

整簇数量在后台计算。高亮最多展示 256 个图斑，预览会注明截取，作用数量包括全部匹配图斑。显式保存新版本时仍同步复制 GeoPackage 并计算校验值，本轮未测生产规模大图件的保存耗时。
