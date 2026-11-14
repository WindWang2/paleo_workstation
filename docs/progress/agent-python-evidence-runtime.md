# PY4：无 GIS 宿主的真实证据链

`evidenceRecord` 仍使用生产 Catalog、输入版本、计算父版本及领域证据构造器，
软件环境从当前宿主实际装配的信息取得。QGIS 版本由 `QgisRuntime::initialize`
成功执行真实 `initQgis()` 后写入进程内部属性 `paleo.qgis.version`，shutdown 前清除。
未装配 QGIS 的宿主不写 `software.qgis`，不生成替代版本或空的版本声明。

此处只移除证据组装对 QGIS 库的直接依赖；没有替换 Catalog 提交、恢复、计算适配器
或 Agent 解码器。原生 Core 验证使用相同生产实现。

规模与依赖基线按实际条目收缩：`qgs-audit-baseline.txt` 删除
`src/workflow/evidencerecord.cpp` 一行，135 → 134；`qgis-spread-baseline.txt`
同样删除一行，170 → 169。其余基线只在检查器确认可收缩时下调。

新增 `tst_evidenceruntime` 检查未装配时不声称 QGIS、显式版本保留、移除后不残留，
并保留既有证据对象的值语义。`tst_runtime` 在真实 QGIS 初始化后核对属性与
`Qgis::version()` 一致，随完整应用车道验收。

原生 Windows / MSVC 14.38 / Qt 6.8.0 的独立 Core 构建已编译生产 Catalog、
领域、元数据、Host、Recovery、Agent 解码器、发布与进程原语，链接真实 SDK
`9d80d1f6` 的静态库；`tst_evidenceruntime` 3/3，通过且无跳过（30 ms）。
该构建没有替代 Registry、执行或提交函数；不包含原生 GIS 适配器，也不作为
Windows 完整 QGIS 应用或生成代码全链验收。最终 MOC 清单不再含无反射成员的
`CapabilityHost`，保留 QObject 基类和已有的 QPointer 生命周期行为。

本批严格层级、Qgs 审计、QGIS 散布、全四指标 transitive 与文件规模检查通过；
transitive 为 func=0、ui=4、heavy_tus=36、include_tu=41，没有可收缩的剩余条目。
