# 地震预览接入（2026-10-30）

## 交付范围

- 普通 IL／XL 剖面、任意测网路径剖面、2D 时间切片统一由 Data_service
  读取。已登记来源按工区数据库检索和校验；未登记文件复用 SDK 的文件读取
  入口与同一 segyio Reader，不依赖 3D 索引或分页工作区。
- 数据服务保留原始采样、延迟时间、缺失值与明确的值数预算；仅在源头明确
  声明 m／ft 时换算物理距离。工区来源校验失败直接报错。
- 3D 浏览沿用 SeismicF3Viewer 同源的 `vendor/sbm`，先以经过规则验证的
  内存快照显示有限分辨率切片，后台加载缓存或完整扫描道头。完整索引就绪后
  自动采用原始分辨率，保留相机、IL／XL 和时间采样号；过期任务按世代丢弃。
- 3D 粗预览与高清状态显式显示。普通剖面和时间切片独立加载，关闭预览页会
  取消未完成的相关读取和索引任务。

## 缓存与来源

完整索引沿用库的路径、文件大小、修改时间、采样指纹、版本和载荷校验，以及
原子发布。临时规则快照只驻留内存；只接受完整扫描索引作为高清依据。断点
文件使用来源路径唯一键，支持取消和续建，强制重建忽略旧断点。缓存写入失败
仍可使用本次内存索引，诊断保留在任务详情。文件变化后拒绝当前体继续读取，
内存切片缓存和 Dataset 注册表的键也包含来源状态及工区绑定状态。

数据服务 SDK 新增独立文件的检查／剖面／时间切片入口和原生测网路径剖面，
共享既有读取器；四角坐标来自实际角点道头。SDK 说明见
`vendor/Data_service/docs/segyio-guide.md`；同源库本地差异见
`vendor/sbm/PATCHES.md`。

## 验证

本地 Windows、vendored Qt／QGIS、串行 ctest，构建并行度不超过 8：

- `tst_seismic_index`、`tst_seismic_engine`、`tst_seismic_sectionui`、
  `tst_seismic_3dui` 最终复核全部通过。覆盖粗到高清、缓存复用／损坏／
  来源变更、已登记来源失败不回退、中文路径、延迟时间、精确振幅、预算和取消，
  以及升级时保持相机与切片选择。
- `tst_datapreview`、`tst_previewdoc`、`tst_dataservice_hd`、`tst_areamodel`、
  `tst_stratigraphy_editing`、`tst_seismic_3d`、`tst_seismic_section`、
  `tst_seismic_welltie` 完整测试均通过。另复核数据预览的六个地震场景。
- offscreen 环境跳过两项需要真实 OpenGL 上下文的可选用例；本次未声明
  实机 GPU 视觉验收。
- layering strict／symbol／Qgs、QGIS spread、UI invariants、i18n、UI tokens、
  docs drift 检查通过。函数规模冻结值仅收缩 `buildSeismicContent` 742→654。
  现存 `datalist_tree.cpp` 1006 行超过 1000 行的 file-budget／ui-size 红项
  与本次改动无关，未扩大或补登基线。

架构不变：数据服务与索引任务位于 services／SDK，视图只发起请求并展示结果。
3D 接入说明见 [3D](../seismic/3D.md)，普通取数说明见
[剖面](../seismic/SECTION.md)。
