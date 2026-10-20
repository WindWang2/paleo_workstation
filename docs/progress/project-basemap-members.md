# 工程底图随工程保存与迁移（2026-10-19）

底图属于工程束，实际文件在工程 `basemap/`，权威角色路径在
`project.paleo` 的 `map.basemap`；`.qgz` 保存原生图层、显示配置与声明副本。
移动时携带整个工程目录，单个 `.paleo` / `.qgz` 不打包外部数据字节。

选择外部底图或保存旧外部引用时，将文件按 SHA-256 命名并原子复制到
`basemap/`。已有工程内文件保持相对路径。复制中核对摘要，失败不提交路径；
缺失底图纳入工程成员校验，禁用底图仍保留其资料成员。
`.qgz` 序列化时将附加底图路径也转为相对路径，随后恢复运行时绝对路径；
重开时按新工程目录解析。图层声明在资源接管后再保存。

当前 Windows vendored prefix 没有 `wms` provider，原 WMS 底图不能实例化。
离线底图工厂在未提供 WMS 或 WMS 拒载时使用已有 GDAL provider，保持原始
MBTiles、版权和工程坐标不变；声明相同但实例已失效时也允许原生替换。

验证：tst_basemap_project 检查外部接管、移走源文件后的同步/异步重开、
禁用但缺失的成员报告、失败不改变配置，以及旧工程普通保存迁移。
tst_projectsvc（20 项）、tst_basemap_project（5 项）、tst_layerservice 全部通过。
另以 paleo_comp_data 两份真实 MBTiles 的副本运行同一迁移测试并通过。
layering_strict、i18n 与 git diff --check 通过；不调整既有基线。
