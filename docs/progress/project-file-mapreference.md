# project-file-mapreference — 工程束清单与工程坐标配准（实战系补账立账）

本文为补账文档：`src/metadata/paleoprojectfile.{h,cpp}`（#261 新建 +238、
3e8988cd 再扩 +32）与 `src/qgis/projectmapreference.{h,cpp}`（3e8988cd
+212）落地时只随提交带了设计文档增量（docs/PROJECT_FILE_DESIGN.md
georeference/map 两节），progress 主文档缺失。行号为 origin/master
`f31ee461` 口径。

## What（交付面）

| 面 | 内容 |
|----|------|
| 工程束清单 | `project.paleo`（kFileName，paleoprojectfile.h:68）：qgz 唯一硬成员 + catalog/manifest/gpkg/areaRules 相对路径成员（:77-81）+ 溯源（sourceAreaRoot 等 :83-85）+ georeference/map 两节（:89, :92-96）。纯 Qt JSON 无 QGIS 依赖，QSaveFile 原子落盘（头注释 :15） |
| 打开契约 | 读 .paleo → qgz 缺失/坏拒开；其余成员缺失如实报 lastErrors 仍开；裸 .qgz 收养写一份清单（头注释 :17-19；收养实现在 qgisprojectservice_async.cpp:181-194） |
| 配准模型 | `PaleoGeoreference` 相似变换 7 参数（a/b/tE/tN + WGS84 锚点 + 度米系数，paleoprojectfile.h:22-38）；纯函数 `applyGeoreference`（:57，实现 paleoprojectfile.cpp:184-198）；控制点/残差仅溯源不参与变换（头注释 :26） |
| PROJ 注册 | `mapreference::configure`（projectmapreference.cpp:53-130）：局部 ENGCRS → {EPSG:4326, EPSG:3857, mapCrs} 三目标的显式坐标操作注册 |
| 底图接入 | `offlineBasemap`/`mbtilesUri`（projectmapreference.cpp:157-173）：MBTiles 走 wms provider `type=mbtiles` URI；attribution 从 mbtiles metadata 表读（:33-50），画布角标聚合（:175-183） |

## 口径（语义决策）

1. **仿射保持线性单位**：PROJ 管线 = `+proj=affine`（系数先乘
   R·π/180 折算等距圆柱米制）+ `+inv +proj=eqc +R=6378137` 输出弧度；
   目标 4326 时补 `unitconvert rad→deg`（projectmapreference.cpp:20-31
   注释「直接 affine → 度会把角度单位传播到工程坐标输入」）。
2. **三目标注册 + 记忆回收**：每次 configure 先按上一轮
   `localTransformTargets` 删除旧操作再注册新操作（:59-66, :128），
   工程其它 CRS 的原生转换不受影响；非 4326 目标在 affine 段后拼接
   QGIS 实例化出的投影段（去掉其首个度→弧度步骤，:86-104）。
3. **正反向往返校验闸门**：注册后即时验证——点 (0,0) 正变换有限、
   范围 (0,0,1000,1000) 正反向变换有限、单点反变换回投偏差
   `hypot ≤ 0.001`（工程米制单位，:108-121）。任何一环失败即 configure
   整体失败——不留半注册状态。
4. **georeference 节兼容语义**：节缺席 = 无配准（现状行为，formatVersion
   不动）；节在但坏 → `georeferenceError` 进 lastErrors 不拦打开；写端
   拒写不完整节（paleoprojectfile.h:87-90；PROJECT_FILE_DESIGN.md:113-115）。
   isComplete 闸：EPSG:4326 目标 + 有效锚点 + 正度米系数 + 非零缩放
   （paleoprojectfile.cpp:172-182）。
5. **同形状双持久化**：清单内嵌节与 .qgz 自定义属性（scope `paleo`、
   key `georeference`）共用同一 JSON 形状（paleoprojectfile.h:60-61；
   异步打开时的回填/搬移在 qgisprojectservice_async.cpp:165-177）。
6. **map 节独立于地质网格**：mapCrs（默认 EPSG:3857）与 basemap
   topo/hillshade 开关属显示配置，不改变图层与计算的局部米网格语义
   （paleoprojectfile.h:92-96；PROJECT_AREA_PLAN.md:8-13 约束注记）。

## 证据

- JSON 往返 + applyGeoreference 纯函数 + 坏节报错测试：
  tests/tst_projectsvc.cpp:219-299（projectFileRoundTripsGeoreference）。
- configure 的 PROJ 管线注册测试锚：tests/tst_projectsvc.cpp:301-339
  （mapreferenceConfigureRegistersPipeline，本方向补）——三目标注册成功 +
  QgsCoordinateTransform 结果与 applyGeoreference 纯函数在控制点上
  一致（两条独立实现互证）。
- 底图链路证据：vendor/basemap/README.md（产物/复现/合规节）+
  fetch-tiles.py 头注释（JOBS 计划表、JPEG magic 校验、TMS y 翻转）。

## 对账与递延

- 与方向 16 catalog-sqlite：**正交、同束协作**——project.paleo 只存成员
  路径引用（catalog 字段指向 artifacts/metadata/catalog.sqlite 或迁移期
  catalog.json），实体/资产/版本的权威在 DataCatalog/CatalogStore
  （docs/progress/catalog-sqlite.md「改的是盘不是内存」）。无 schema 交集。
- 与方向 12 时深（depthtransform/timedepth）：**正交**——配准管平面
  (x,y)↔(lon,lat)，时深管 TWT↔深度；井震联合显示时两层各自换算，
  无共享状态。
- 递延：map 节的 basemap 相对路径只在清单层定义，MBTiles 文件本身的
  分发/缓存治理（vendor/basemap 产物不入库）依赖开发机自跑
  fetch-tiles.py——CI 无底图资产，离线底图渲染测试只能跳过（记 TODOS
  可选补：合成单瓦片 MBTiles 夹具）。
