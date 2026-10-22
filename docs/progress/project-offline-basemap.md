# 工程离线底图导入

2026-10-22。

- 数据页、数据 ribbon、文件菜单增加工程离线底图导入对话框。选择用途为地形／影像
  或地形阴影，可选择导入后显示；未配准工程默认仅保存，并说明坐标设置入口。
- 支持 MBTiles、GeoTIFF、IMG、可直接读取的栅格 GeoPackage 和已配准 PNG／JPEG。
  保留原文件名及扩展名，定位、坐标、样式配套文件一并保存。不支持外部引用 VRT；
  无坐标系栅格报错，不默认赋予经纬度。
- 通过 PaleoTaskService 后台复制及 SHA-256 核验，使用临时目录及内容寻址目录；
  重复导入核验已有副本，替换用途选择时保留旧文件。取消、切工程、只读状态、
  导入期间底图设置改变均阻止旧任务发布。
- 复用 project.paleo 的 topo／hillshade 和现有图层声明，不引入第二套工程底图配置。
  启用新底图时关闭在线选择，主图置底；现有测区／井位预览按工程底图复制显示。
- 工程保存使用相对路径；非 MBTiles 栅格走 GDAL，避免当作 SQLite 瓦片读取。
  原有 MBTiles 保存及迁移方式保持兼容。

验证（Windows / vendored QGIS / Qt offscreen）：

- 主程序及四个相关套件独立构建，最多 -j8；未替换日常运行目录。
- offlinebasemap 7、basemap_project 5、wellbasemap 8、paleomaplibrary 34，
  共 54 通过，0 失败、0 跳过。最后的发布清理和实际渲染调整另经 15 项定向验证。
- 实际生成 GeoTIFF／MBTiles／PNG+aux.xml，核对后台导入、独立用途、重复导入、
  替换保留旧文件、取消、切工程及只读拒绝。复制工程、删除外部源文件后重开底图
  仍有效；QGIS 渲染结果确有像素内容，原有井点图层保持存在及可见。
- 新按钮及文件菜单均打开正确表单，空路径禁用导入，错误留在表单；截图复查通过。
- layering strict／qgs-audit／symbol-audit／transitive strict、qgis_spread strict、
  ui_invariants／ui_size／file_budget strict、i18n、ui_tokens、docs_drift 通过，
  基线无新增。
