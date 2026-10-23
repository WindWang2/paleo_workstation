# 界面精简后的状态回归修复（2026-10-23）

基线：master `5abc061a`（#647）。本批修复 review 中已复现的三个状态回归，
并同步修正阻挡相关测试和 lint 的旧断言。

## 行为修复

- 数据列表的树、表、图标、虚拟表和分组树共用一份资产身份选区。视图切换和
  模型重载只镜像这份选区，批量命令不再合并隐藏控件的陈旧选中项。
- 选区仍支持跨分页保存；过滤后的有效选区与可见身份集合取交集，清除过滤后
  恢复原选区。可见身份集合随过滤结果缓存，徽标更新不再反复分配全量过滤集合。
- 替换过滤条件组时同步清空旧搜索/类型控件，避免空条件组被旧输入再次污染。
- 右侧参数 dock 纳入统一页面策略：起始页、地层对比和预览最大化时隐藏，返回
  普通工作流时按用户显隐意愿恢复；手动关闭的面板保持关闭。
- 版本导航复用实体导航的上下文设置入口，恢复真实实体的 CRUD 工具条，同时
  保留版本血缘图的单次选择保留语义。

## 检查与回归

- 新增隐藏图标旧选区、多视图选区同步、过滤清除恢复、首页往返显隐意愿、
  版本导航恢复工具条的回归用例。
- Ribbon 帮助覆盖改为逐页、逐命令身份检查，并覆盖有/无编辑目标，移除与
  按需显示设计冲突的“至少 90 个可见按钮”断言。
- 批量改型测试按稳定类型 ID 选择下拉项，兼容本地化标签；同步验证筛掉的
  资产不进入批量目标、清除过滤后恢复选择。
- `MappingReferencePanel::category` 声明和定义的参数名统一，修复已定位的
  clang-tidy 声明不一致问题。

规模基线只下调：`DataListPanel` 构造函数 478→459，`buildDataOpsUi` 337→332，
`entitypanel.cpp` 1619→1617（file-budget/ui-size 同步）。Qgs 直连、散布和层违规
基线不增加。

## 最终验证

- Windows / MSVC / Qt 6.11.2 / QGIS 4.2.3，RelWithDebInfo：应用 `paleo` 和
  相关测试目标构建成功（`-j8`）。迁移测试后重新生成了对应 Qt moc 文件。
- 12 套相关测试串行全绿，81.22 秒：`tst_panels_dataops`、`tst_mappingpages`、
  `tst_layoutshell`、`tst_previewmap_page`、`tst_assetpaging`、
  `tst_stratigraphicweb`、`tst_webviewpanel`、`tst_uxtheme`、
  `tst_seismic_sectionui`、`tst_shortcuthelp`、`tst_shortcuts_shell`、
  `tst_shell_chrome`。其中 DataOps 101 项、ShellChrome 19 项均通过。
- 12 项源码检查通过：layering strict / symbol / transitive（显式 QGIS_PREFIX）/
  qgs-audit、qgis-spread、ui-invariants、ui-size、file-budget、i18n、ui-tokens、
  QGIS versions、docs-drift。
- 单独启用 `PALEO_CATALOG_SCALE=10000` 的规模用例未通过：Windows 本机工作树下
  触发 QtTest 默认 5 分钟函数超时。诊断采样位于测试的旧全量基准循环，调用链为
  `scaleRenderingFilteringSortingSelection → DataCatalog::resolvedVersionPath →
  QFileInfo::canonicalFilePath → GetFileAttributesExW`。尚未获得完整对比结果，
  不报告提速倍数，也不将此用例计入上面的 12 套普通回归通过结果。后续需将基准
  阶段计时拆开，复核 Windows 路径规范化成本与规模用例的时间预算。

本批不将未复核的历史 CI 失败宣称为已解决；GitHub 全量 CI 与本地上述验证
分别记录。
