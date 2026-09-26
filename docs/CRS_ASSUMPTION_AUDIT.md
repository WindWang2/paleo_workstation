# CRS 假设审计：§17 单 CRS 前提在代码中的落点（只读审计）

状态：**审计快照 2026-09-26**（wave3/model-hardening；对应 TODOS P1「多 CRS
假设核查」）。只读审计——不改实现；所有断言带代码指针。

## 0. 结论先行

PALEO_QGIS_PLAN §17 要求 MapContext 携带唯一 CRS（extent/scale/CRS/horizon
跨页共享）。代码里的实现比计划更激进也更安全：**整个工作台被钉死在一个
无大地基准的工程直角米 CRS 上**（局部测网），「单 CRS」不是页面间共享的
约定，而是每次打开工程都重新钉一次的全局事实。当前真工区（井位/层位/
SEG-Y 道头坐标全部是同一局部测网米）与该假设完全自洽，**无现实冲突**。

真实项目「混合地震工区 CRS 与地图基准」的冲突点不在现有数据，而在**未来
入口**（§2 逐条列出），每条都给了建议。总体建议：维持单工程 CRS 脊线，
在入口处（导入/算法）加地理 CRS 拒绝/降级，而不是引入多 CRS。

## 1. 单 CRS 的实现落点（§17 假设的实际形态）

| 落点 | 行为 | 代码指针 |
|---|---|---|
| **CRS 定义**（唯一事实源） | WKT2 `ENGCRS`（工程基准、米、无椭球→无到 EPSG:4326 的坐标操作） | `DataCatalog::localGridCrsWkt()`，src/catalog/datacatalog.h:186-200 |
| **每次打开强制钉** | project CRS + canvas destinationCrs 都设为局部米 CRS；.qgz 里带来的任何 CRS 被覆盖 | `QgisCanvasController::applyLocalCrs`，src/qgis/qgiscanvascontroller.cpp:67-80（102-116：建 bridge 前先钉，防首层 CRS 快照泄漏） |
| **派生栅格 SRS** | 层位时间/厚度栅格写局部 ENGCRS | src/io/horizonbinner.cpp:320-324、src/workflow/mappingworkflow.cpp:252、src/workflow/workflows.cpp:296-313（ONNX 栅格：crs 参数无效时落局部） |
| **矢量导出** | 井位 GeoJSON 写 legacy `crs` 成员=ENGCRS WKT（不投 4326） | `DataCatalog::writeWellsGeoJson`，src/catalog/datacatalog.cpp:877-882 |
| **源 CRS 只是标签** | 导入面把源文件里的 EPSG:4326 记为标签（domain/types.h:42），UI 明示「源文件里的 EPSG:4326 只是标签，不会画到地图上」 | src/domain/types.h:42、src/ui/paleomainwindow.cpp:112 |
| **联动链无坐标变换** | map↔well↔seismic 选择联动传 id/画布坐标，不做任何 QgsCoordinateTransform——单 CRS 下正确 | src/linkage/selectioncontext.cpp、wellmaplink.cpp、threewaylocator.cpp（全文无 transform/crs 调用） |
| **唯一的显式变换** | 约束 IDW 的约束线与点层 CRS 不同时按 transformContext 重投影 | src/algorithms/paleoalgorithms.cpp:227-236 |

配套测试：`tst_import.cpp` `horizonDerivedRasterAndCrs`（authid 非 4326）、
`tst_segy.cpp`/`tst_segy_fixture.cpp`（角点=局部米）、
`tst_catalog.cpp`（GeoJSON crs 成员 round-trip）。

## 2. 混合 CRS 的冲突点（按风险排序）与建议

1. **外部 GeoJSON/参考图带真实地理 CRS 进图**（最大入口风险）
   现状：导入的 geojson 一律标 `georeferenced:false`（未配准，不生成地图
   图层，src/io/dataimportservice.cpp:668-671）——所以**今天没有通路**。
   冲突来自 D11（参考 GeoJSON 临时配准，Wave-2 包范围）：一旦 affine 进图，
   一个 4326 层与局部米层同图，QGIS 渲染靠 on-the-fly 尚可，但任何
   paleo:\* 算法把两者喂进同一 run（距离/插值）就会**度×米混算**。
   建议：配准层落地时**重采样进局部米栅格/重投影进局部米矢量**再入图，
   图层打「配准参考（显示级）」标记，算法入口拒绝该标记（见 2）。
2. **paleo:\* 算法不检查输入 CRS 是否地理坐标系**
   现状：constraint_idw 以输入点层的 CRS 写输出栅格（src/algorithms/
   paleoalgorithms.cpp:52-70）；喂 EPSG:4326 点层（如测试那样）也能跑——
   IDW 权重按「度」算距离，数值上有输出、物理上无意义（单 CRS 脊线下
   真数据不会触发，但 API 面无护栏）。
   建议（S）：算法入口对 `crs.isGeographic()` 的输入报错拒绝——「工程栅格
   一律工程米」，把假设从约定变成执行。
3. **未来 SEG-Y 带大地坐标（UTM/经纬度）道头**
   现状：角点从道头 72/76 原值冻结为「局部米」（segyreader.cpp:222-231）；
   结构门只查测网单调性，不查坐标量级。若某工区道头是 UTM 米，survey
   角点会落在离井网 1e5~1e6 米处，图层看似合法实则两套网格。
   建议（S）：survey 冻结时做一次量级一致性检查（角点包围盒 vs 已有井位
   包围盒，数量级偏离 → 导入确认对话点名，而不是静默接受）。
4. **井位 surface 坐标的 coordinate_status 已有护栏**（正面确认）
   `ok/untransformed/invalid/missing` 四态 + map 只在 `ok` 前一直读原始
   局部米（datacatalog.h:26-31 注释即契约）。真投影参数（工区真基准）
   出现之日，才有第一个真正的双 CRS 场景——届时应走「入库时一次性变换
   到局部米，源值留标签」，而不是让地图层各带各的 CRS。
5. **writeWellsGeoJson 的 legacy `crs` 成员**
   RFC 7946 废除了 GeoJSON 的 `crs` 成员（规范语义=永远 WGS84）。当前
   消费者只有自家 OGR 读回（认 named CRS），无冲突；但任何第三方严格
   RFC 7946 读取器会把它当 4326。
   建议：保持现状（OGR 链内自洽），在文件头注释/本文记录该偏差；若将来
   要交换给外部工具，改写 GeoJSON 之外的格式（gpkg）而非去掉 crs 成员。
6. **比例尺/量距装饰按米显示**（低风险）
   decorations/maptools 的 scale bar、量距以 mapUnits=米 为前提（单 CRS
   下恒真）。只要 CRS 钉死逻辑不撤，无冲突；撤销钉死则全部要重审。

## 3. §17 MapContext 与多页面共享的实现形态

- 单 canvas、单 project CRS：extent/scale/CRS 天然全局一致——§17 的
  「切页不丢上下文」由共享同一 QgsMapCanvas 实现（paleomainwindow/
  pagepanels），无每页独立 CRS 状态可漂移。
- 若将来真需要「地震工区 CRS ≠ 地图基准」（例如直接显示 UTM 道头的
  survey 而不重采样）：§17 的 MapContext 需要显式携带
  `crs + transformContext`，且 threewaylocator/maptools 的画布坐标↔图层
  坐标换算要全部过 QgsCoordinateTransform——这是放弃单 CRS 的真实代价
  （当前联动链零变换调用，靠的就是单 CRS）。
- 工程评审确认方向（建议）：**维持单工程 CRS 脊线 + 入口护栏**（§2 的
  1/2/3），多 CRS MapContext 列为「有真实混合数据工区时再评估」，
  不做预防性架构。

## 4. 与其他文档的衔接

- 单 CRS 的存储侧保障（catalog/栅格/GeoJSON 全落局部米）见
  `docs/PROJECT_AREA_PLAN.md` §3 坐标条款；本文只审代码落点。
- D11 临时配准（Wave-2 包）落地时必须回看本文 §2.1 的建议。
- 本文不改 `docs/PALEO_QGIS_PLAN.md`（对账归编排会话）。
