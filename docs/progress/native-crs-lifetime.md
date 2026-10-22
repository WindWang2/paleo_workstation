# 坐标系跨线程失效与新建工程修复（2026-10-22）

- 实际主窗口复现：打开 comp 工程副本后，通过引导扫描 paleo_comp_data，创建工程时报“无法建立原生坐标转换：EPSG:4326”，留下空工程。
- 诊断中 EPSG:32649 的 isValid() 仍为 true，但 toWkt() 为空，QgsDatumTransform::operations 返回空；主窗口之外的单线程导入测试不触发。
- 用 QThread 创建 CRS、主线程使用同一缓存对象、等待创建线程结束的测试可稳定重现。当前 QGIS 4.2 的 CRS TLS 清理移除共享缓存对象的 PROJ 主对象，仍被其他线程引用的缓存条目不能再投影。
- QgisRuntime 在初始化后、应用工作线程启动前关闭 CRS 实例共享缓存。坐标转换缓存保持启用；算法、坐标数值及原始资料不变。该兼容措施适用于未修复的二进制依赖，升级前需用同一线程退出测试确认上游生命周期修复，再恢复 CRS 缓存。
- 地图配准先在临时 QgsProject 中构建完整上下文，成功才提交到活动地图。失败保留原 CRS、转换操作和配准目标；原生操作注册也检查实际返回值。
- 新建工程在关闭旧工程和创建目录之前校验坐标配准，错误不再因此产生空工程。

本地真实资料与工程副本仅用于验证，不入库。

验证：
- 9 个相关套件共 135 项通过、无跳过：projectsetup 21（启用真实资料全批导入）、paleomaplibrary 36、folderimport 27、ingestplan 10、qgisruntime 5、projectsvc 20、basemap_project 5、basemap_rendering 4、offlinebasemap 7。
- 便携版依赖环境下实际操作：comp 工程一致性副本的原始 D61 相图仅通过树节点勾选即可显示；同一进程用实际资料新建 crs-fixed-qa，成功导入 20 井、3 份古地理图，并通过左侧勾选显示新导入的 D61，相面、井号、范围均正常。
- strict 分层、Qgs audit/spread、symbol/transitive、规模、界面不变量、i18n、tokens、docs drift 闸门通过；基线无增长。
