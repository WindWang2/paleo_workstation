# 数据预览实际渲染修复（2026-10-20）

截图中预测成果已载入 20 个要素，主画布和鹰眼却都是白色。根因是
`FaciesHierarchyRenderer::apply` 收到空级别：图例默认解析为微相，分类
表达式仍以空字符串匹配，全部已知相编码落到没有符号的 unknown 分支。
统一解析默认级别后，井点、相名、鹰眼使用同一套分类。井点全图缩放
另为两行井名/相名预留边距，避免边缘标注被裁掉。

工区的八个层位只有 RAW DAT；原界面仅显示路径并提示选择尚不存在的
DERIVED。现在由 PreviewDocService 后台读取真实 XYZ/Inline/Crossline，
复用 horizonbinner 按文件测网装箱，用临时 GeoTIFF 显示原始 Z 值及其
范围、有效像元、拒绝数。临时文件随任务和图层存活，不登记为新工程版本。
关闭/重建页取消任务；失败显示原因和重试入口。生产任务使用 quiet 模式。

## 验证

- 新增井点渲染回归：修复前 0/20 可见，修复后逐井像素检查 20/20，
  同时检查顶部/底部留白；未依赖北箭头、标注或鹰眼判断地图有内容。
- RAW/DERIVED 版本切换，以及后台 RAW 准备、关闭页取消、重新打开。
- 真实工区：C3/C6/D53/D61/D62/D63/D71/D72 的八个 RAW 层位和八个
  最新预测成果均渲染；每个预测成果 20/20 个井点具有有效分类符号。
- 真实 SEG-Y：Inline 1520、Crossline 4485 解码并截图。
- 本地验收产物：`build/preview-acceptance/`，真实地图日志
  `build/preview-real-map-acceptance.log`，剖面日志
  `build/preview-real-acceptance.log`。截图启用正式浅色主题和内嵌字体。
- 实际数据通过当前目录库的 SQLite backup 快照读取，raw/derived/layers
  目录连接到原始工区；目录库只读，禁止验收静默退到旧备份。
- `tst_previewmap_assets`、`tst_previewmap_canvas`、`tst_datapreview` 串行执行。
  layering strict、i18n、ui_invariants、diff whitespace 检查通过。

既有闸门问题未扩增：file_budget 中 constraintpage_interpolation、
folderconfirm、datalist_tree 的冻结值问题；ui_tokens 中 identifypopup
间距及 compositebody 陈旧例外。此变更将 buildHorizonContent 冻结值
从 433 下调到 429，未上调任何基线。

真实工区验收入口是 `tst_previewmap_assets` 的
`actualProjectMapPreviews` / `actualProjectSeismicPreviews`；显式设置
`PALEO_PREVIEW_PROJECT` 和可选 `PALEO_PREVIEW_OUTPUT`，默认 CI 跳过
这两个依赖用户工区的验收项。普通渲染回归始终运行。

已部署到 `Downloads/paleo-portable-run/paleo.exe` 和
`build/portable/paleo.exe`，与 `build/paleo.exe` 的 SHA-256 一致：
`fb6359dbba3c7e7db23b8c431dfaa7cab30672db3eb8f0b20ae625f0ef2ba09f`。
旧程序留在 `build/preview-deployment-backup/`；没有终止用户进程。
