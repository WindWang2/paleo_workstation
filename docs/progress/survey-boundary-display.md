# 测区范围空几何修复（2026-10-21）

## 原因与修复

测区图层在树中存在且已勾选，保存的范围也正确，但 GeoJSON Polygon
的 `coordinates` 被写成了点数组，缺少外层的环数组。GDAL 可从该文件
识别图层、要素数量和范围，逐要素读取却报 `invalid Point`，要素没有几何，
所以画布无边界。这不是页面显隐或图层顺序问题。

`DataCatalog::writeSurveyGeoJson` 中的 `QJsonArray{ring}` 调用了同类型的
拷贝构造，没有增加数组层级。改为显式 `rings.append(ring)`，保留原有
测区包络计算、闭合点、属性、CRS 与原子写入方式。

## 验证

- 新增 `tst_catalog::surveyGeoJsonContainsClosedPolygonRing`：检查完整环
  层级、闭合点、坐标，并让 OGR 解析几何、验证有效性和面积。
- 修改前在 `rings.size()` 断言失败（实际 5，预期 1）；修改后通过。
- 同时运行既有的井点 GeoJSON 写失败保旧文件测试；共 4 pass、0 fail、
  0 skip（含初始化与清理）。
- Windows 使用独立 worktree 构建目录，编译完整 `tst_catalog.cpp` 与本次
  修改的导出单元，链接主 checkout 已有依赖静态库；未改写主构建目录。
- layering strict、i18n、file budget strict 与 diff 空白检查通过。

当前工区的生成文件经原字节备份后补齐环数组，`ogrinfo -ro -al` 能读出
`POLYGON ((0 0,12793 0,12793 16406,0 16406,0 0))`，不再报 invalid Point。
没有修改原始地震资料、测区角点、工程配准或工程文件。

应用里的旧几何缓存需要重新加载；应使用包含此源码修复的程序，避免旧版
再次生成错误文件。本次未替换用户正在使用的可执行文件，未做桌面像素验收。
