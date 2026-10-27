# 项目文件设计（PROJECT_FILE_DESIGN）

本文说明工程束、资料来源和坐标声明。GUI 当前入口是四步新建向导，
源目录与工程保存目录分开；命令行兼容入口允许就地建工程。

## 设计背景

此前工程锚点是裸 `.qgz`：新建工程只产一个 `.qgz`，打开收 `.qgz`，
「导入工区文件夹」是开口工程后的独立动作。`.qgz` 只能描述 QGIS 图层集，
管不住 catalog/manifest/gpkg/areaRules/commit_journal 这套工程束
（bundle）——成员缺失时 .qgz 照常开，坏在半截没人知道；也没有「指着
一个工区文件夹直接成工程」的入口。

## 设计：`project.paleo` 工程清单

工程根目录下 `project.paleo` = 工程的唯一入口与束清单（JSON，QSaveFile
原子写）。`.qgz` 退居「成员之一」——它仍然是 QGIS 图层集权威，但不再是
工程入口。

```json
{
  "format": "paleo-project",
  "formatVersion": 1,
  "name": "project_area",
  "projectId": "proj-…",
  "createdUtc": "…",
  "members": {
    "qgz":       "project_area.qgz",
    "catalog":   "artifacts/metadata/catalog.json",
    "manifest":  "project_area.qgz.project.sqlite",
    "gpkg":      "project_area.gpkg",
    "areaRules": "project_area.json"
  },
  "sourceArea": {
    "root": "/path/to/project_area",
    "importedUtc": "…",
    "stats": { "files": 60, "rows": 116, "failed": 0 }
  }
}
```

- **members** 全部是工程根相对路径——束可整体搬迁，清单不失链。
- **catalog/manifest/gpkg/areaRules** 允许缺席（工程早期没建 catalog 属
  正常）：打开时缺失成员如实进 `lastErrors`，不拦打开。
- **qgz** 缺席 = 工程束损坏，打开拒绝——唯一能拦开的成员。
- **sourceArea** 记录「从工区文件夹新建」的来源与导入统计，供追溯；
  手工新建的工程此节为空。

## 打开/新建契约

| 入口 | 行为 |
|---|---|
| 打开 `.paleo` | 读清单 → qgz 成员缺失/坏 → 拒开；其他成员缺失 → 如实报进 lastErrors 仍开 |
| 打开 `.qgz`（旁有 `.paleo`） | 打开 + 校验成员清单，缺失如实报 |
| 打开 `.qgz`（旁无 `.paleo`） | 打开 + **自动收养**：写一份 `project.paleo`（QGZ-only 老工程升级路径），记日志 |
| 新建工程 | 写 `.qgz` + `project.paleo`（name=文件名去后缀，projectId 随机） |
| GUI 新建 / 从工区文件夹新建 | 已有工程走打开/收养；无工程进入四步向导，在所选保存位置下创建同名目录，识别与确认资料后导入，stats 回填 `sourceArea` |
| 命令行兼容入口 | 保留源目录内建立 `<basename>.qgz` + `project.paleo` 的就地创建契约 |

### 就地工程与扫描守卫

命令行兼容入口允许**源目录==工程根**的就地工程；GUI 默认使用独立保存目录。`buildIngestPlan`
的扫描守卫相应改为：

- 源根 **在工程目录之内**（如 `artifacts/` 子目录）→ 仍拒（issue 如实报）。
- 源根 **==工程根** → 允许扫描，但束成员（`project.paleo` 声明的
  qgz/catalog/manifest/gpkg/areaRules + `project.paleo` 自身）与
  `artifacts/` 受管子树不出行；其余文件照常分类成项。
- 源根 **包住工程目录**（上级目录）→ 工程产物子树整体跳过（旧行为不变）。

`.preview_cache` 等隐藏目录天然不进表（枚举器不带 `Hidden`）。

## 文件契约

`src/metadata/paleoprojectfile.{h,cpp}` —— 纯 Qt JSON，无 QGIS 依赖：
`PaleoProjectFile`（formatVersion/name/projectId/createdUtc/members/
sourceArea）+ `readProjectFile`/`writeProjectFile`/`missingMembers`。
清单损坏或 formatVersion 超本实现 → 打开如实报错。

## 与既有机制的衔接

- `PaleoProjectStore::setProjectPaths` 的 qgz/gpkg/sqlite 约定不变——清单
  只是「指认并校验」这些既有位置，不另起路径体系。
- 现行 catalog 持久层是 `artifacts/metadata/catalog.sqlite`，JSON 是旧数据迁移入口。
  `members.catalog` 的旧字段及默认清单仍可能保留 `catalog.json`；成员检查同时识别
  SQLite、旧 JSON 与 `.migrated` 文件，不能仅凭清单该字段判断当前存储。`commit_journal/`、
  `.running`、崩溃转储等不重复登记进清单（它们是派生态不是成员）。
- 最近工程列表照存入口路径（`.paleo` 或 `.qgz` 均可——打开端自适应）。

## georeference 节（工程级地理配准，可选）

`kind=crs` 保存已知的原生 `sourceCrs` 声明，按原生投影转换；
`kind=similarity2d` 将无基准的局部米制网格配准到真实坐标。下面的参数示例仅用于 similarity2d。
有此节的工程，`DataImportService` 建井时把井口局部网格坐标换算成 WGS84
经纬度：`coordinateStatus="ok"`、`extra` 记 `projectLon/projectLat`
（`surfaceX/Y` 仍存原始网格）；地震 survey 冻结时
角点换算包围盒落 `extra.wgs84Bbox*`。

```json
"georeference": {
  "kind": "similarity2d",
  "targetCrs": "EPSG:4326",
  "anchor":     { "lonDeg": 108.05, "latDeg": 36.10,
                  "metersPerDegLon": 90049.7, "metersPerDegLat": 110960.8 },
  "params":     { "a": 1.000896, "b": 0.030276, "tE": -6182.24, "tN": -9871.26 },
  "formula":    "E=a*x-b*y+tE ; N=b*x+a*y+tN ; lon=lon0+E/mPerLon ; lat=lat0+N/mPerLat",
  "controlPoints": [ {"well":"A2","x":…,"y":…,"lon":…,"lat":…,"residualM":14.0}, … ],
  "maxResidualM": 48.7,
  "provenance":  "参数来源（拟合方法/日期/控制点出处）"
}
```

- 变换：`E = a*x - b*y + tE`、`N = b*x + a*y + tN`（米，a=s·cosθ、b=s·sinθ），
  再按锚点度米系数换算经纬度。`controlPoints`/`maxResidualM` 仅审查溯源，
  应用端不消费。
- 兼容：节缺席 = 无配准（现状行为，`formatVersion` 不动——旧读端逐键提取、
  多余键容忍）。节存在但缺键/含非有限值 → 读端置 `georeferenceError`，
  打开如实进 `lastErrors`，**不拦打开**（按无配准继续）。写端拒写不完整节。
- 分发：清单是权威；`QgisProjectService` 另把该节镜像进 QgsProject 自定义
  属性（scope `paleo`、key `georeference`）随 `.qgz` 持久化——直开 `.qgz`
  且旁无清单时作兜底。QGIS 地图、预览、联动和导出共用工程转换上下文。
- 序列化/反序列化复用 `paleoGeoreferenceToJson/paleoGeoreferenceFromJson`
  （清单与 `.qgz` 副本同一形状）。

## map 节（地图坐标系与离线底图，可选）

```json
"map": {
  "crs": "EPSG:3857",
  "basemap": {
    "enabled": true,
    "topo": "basemap/basemap_topo.mbtiles",
    "hillshade": "basemap/basemap_hillshade.mbtiles"
  }
}
```

- 界面入口为「文件 → 工程坐标与底图…」。表单编辑相似变换、WGS84 锚点、
  度米系数、参数来源、地图 CRS 与两份 MBTiles 路径。保存前校验参数、原生
  正反向范围转换和底图可读性，重新计算现有控制点残差，原子写入清单。
- 地质图层继续保留局部工程 CRS，地震/测井计算仍使用原始米坐标。
  仅在存在有效 `georeference` 时，QGIS 工程转换上下文注册显式操作并启用
  `map.crs`；缺少配准时保持无基准工程网格，不显示真实地理底图。
- 转换按配准公式得到 WGS84，再由 QGIS 原生投影操作转换到地图 CRS。
  测区、井位、层位栅格、剖面拾取和定位共用该上下文；地图拾取反算到局部
  网格后才进入地震计算。MBTiles 优先由 QGIS 原生 `wms` provider 加载；
  裁剪构建未提供该插件时，使用 QGIS 原生 `gdal` provider 读取同一份离线文件。
- 底图路径保存为工程根相对路径，图层声明在共享 `01_Base`，位于地质图层
  下方；全范围定位以测区/地质图层为准，底图的全国范围不参与。
- 底图是工程成员：选择工程外的 MBTiles 时原子复制到工程 `basemap/`，按
  SHA-256 命名避免重名覆盖；工程内已有底图保持原相对路径。保存旧工程时
  同样接管外部底图，清单与 `.qgz` 显示配置同步。缺失底图进入成员缺失检查，
  复制或配置写入失败不提交新路径。移动时携带整个工程目录（含 `basemap/`）；
  `.qgz` 中的原生底图数据源及附加路径均可在新目录恢复。
- `map` 缺席时默认 `EPSG:3857`、空底图路径，不改变旧工程网格行为。
  `.qgz` 的 `paleo/mapConfiguration` 保存显示配置副本；`canvasExtentCrs`
  标记保存视野的坐标系，旧视野默认按局部网格转换。
- 状态栏显示当前地图 CRS 和 WGS84 经纬度；无配准时保留「工程坐标 · 米 ·
  未投影」。底图版权来源读取 MBTiles metadata，并显示在地图界面。

## 非目标

- 不把数据文件清单写进 `project.paleo`（那是 catalog.json 的职责，
  清单只管束成员）。
- 不做多工程并行管理器（一次一个工程，QGIS app 同语义）。

## 新建向导与资料设置（2026-10-22）

GUI“新建工程”与尚无工程的“从工区文件夹新建”统一进入四步可跳过向导。
默认在选定保存位置下建立同名工程目录，资料源与工程位置分离；已有工程目录
仍走打开/收养。命令行 openPath 的既有就地创建契约保留。
sourceArea.stats 记录 points/wells/seismic/horizons/references/boundary、pointMapping
与井名冲突的显式归属选择 wellTargets，
供“工程资料设置”恢复。XLSX/点矢量的原件为 RAW，规范井表为带 parentVersionIds
的 DERIVED；井点与测区仍按 catalog 生成受管 GeoJSON，并由 manifest 持久声明。
georeference.kind=crs 时 sourceCrs 为原生投影声明；与 similarity2d 分开读取，
按精确投影操作绑定既有工程计算网格，不再拟合第二遍仿射。

现有古地理图来源保存为 sourceArea.stats.paleomaps；mapChoices 逐主件保存 included
与 horizon，normalizeMaps 保存自动三级相整理选项。引导按套预览文件数、校验缺件，
默认优先同名 GPKG 并取消预览图，允许逐套选择格式和修改层位。
执行复用 PaleoMapLibrary：完整 RAW 文件组与可选 DERIVED 分开保存，层位按确认值
传入，配套文件（含 `<主件>.qml`）一并校验；预览后增删或修改成员须重新识别。
图件在常规资料导入提交后串行处理，沿用任务会话、取消、去重和图层声明。
