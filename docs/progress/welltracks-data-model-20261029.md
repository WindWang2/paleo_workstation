# 井道图消费工区数据模型

井道图原先按当前源文件组装；LAS 视图还从目录关联补取分层、岩性与照片。
已有模型预览也只读取同一 sourceId 内的曲线和层段，不能汇集同井的分散资料，
且图件数据不携带井坐标系／工程坐标系。

本批在 Data_service 增加 listSourceWells、readWellPlotData 和
readProjectCoordinates。按稳定井 ID 在 SQLite 只读快照中读取曲线组、层段道、
标准层和照片引用，保留来源 ID 与深度约定，不重新读入或校验原始数值文件。
逻辑行数预算默认八百万，支持取消；多井来源要求明确选井，不按文件夹猜归属。

宿主当前 RAW 测井版本通过 areaModelSourceId 直接请求后台模型预览，绕过原件
存在性检查和 SHA 扫描。模型失败如实展示，不能切回原件解析。数据列表仍是当前
来源的表，井道图汇集该模型井的资料。旧工程、独立文件与历史版本保留原入口。

井坐标、坐标系、单位和工程目标坐标系都来自数据库。界面分别标明两套约定，
缺失值显示缺失／未确认，不把工程 CRS 当作井源 CRS。ProjectDataFacade 也携带
模型投影出的井源 CRS 与单位。转换继续由 QGIS 执行，本批不隐式重投影。

井道主轴使用 MD 米：明确的英尺换算为米；未知类型、TVD 或未知单位跳过并列出
原因，不能把垂深混为测深。确认过的综合模板顶底深约定补入子模块；已缓存的旧
模型需要显式重导入以应用解析改进，本批不自动覆盖已编辑数据。

验证包括跨来源同井曲线、数据库样点修改、原件离线、井／工程 CRS 不同、零坐标、
未知井 CRS 不继承工程 CRS、预算、取消、只读重开及真实预览页装配。现有井导航
提供明确井上下文时，界面传递绑定的模型井 ID；无上下文的多井来源如实要求选井。

验证结果：主程序编译通过；宿主专项 8/8（tst_areamodel、tst_wellcomposite 和
6 个源码闸门），子模块专项 3/3。模型测试覆盖后台 XmlPreviewSession，井图测试
覆盖真实 DataPreviewTabs 页面在原件删除后的曲线及工程 CRS 显示。新建工程和
原生导入回归 tst_projectsetup / tst_nativeimport 也通过。
日志：build/model-welltracks-page-tests.log、build/model-welltracks-component-tests.log、
build/model-welltracks-sdk-final-tests.log。

完整 tst_datapreview 曾在原有 reselectFocusesExistingTab 的 QGIS 工程对象构造处
停住，185 秒无进展后终止；未将整套预览回归记为通过。新模型的后台会话、井图
组件和真实预览页在所属井图套件内通过；不据此宣称其他预览类型均已验证。

源码闸门还发现 master 的 datalist_tree.cpp 为 1006 行；将不变的节点计数 helper
移入既有内部头后降至 998 行，file_budget_strict 通过，未扩大任何基线。
