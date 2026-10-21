# [med] 3 条待定性发现（medium，复核未复现）：相面几何单/多类型入库、零要素相面建副本门、批量导出失败仍发布

## 问题

2026-10-18 swarm 评审（范围 366aef97..8db58138）产出 3 条 medium 发现，独立复核员未能复现其用户可见后果（缺 Qt/QGIS/GEOS 运行期环境），按「保留并标注」纪律在此跟踪，建议先补测试再定性。三条均已核实代码事实部分，未复现的是后果推断。

1. **覆盖绘制可能把单部件 Polygon 存进 MultiPolygon 相面图层**：`src/qgis/faciespartitioneditor.cpp:99` add 路径对 `QgsWkbTypes::isMultiType` 的几何做了 `convertToMultiType()`，但 :112-113 的 changed 路径直接 `v->changeGeometry(f.id(), geometry)` 未做同一规范化；:270 `pieces.size() == 1` 时 remainder 为单部件 Polygon。若成立，GeoPackage 的 MULTIPOLYGON 几何列会混进 POLYGON 行。现有测试 tests/tst_faciespartition.cpp:77-93 走该 changed 分支但只断言面积/数量/编码，不断言存库 WKB 类型。
2. **零要素相面矢量版本建不出编辑副本**：`src/workflow/mappingworkbench_edit.cpp:188-190` 新增「相面必须有非空冻结 footprint」前置门（footprint() 对空图层 fail「无法建立相图覆盖范围」），可能堵死「从零手绘相面」工作流，且报错不指明治本原因。
3. **批量出图 worker 报错仍发布外部目录**：`src/workflow/horizonbatchexport.cpp:459-472` 的发布守卫只判 `journal.isEmpty() && !error.isEmpty()`，:429/:440 早退路径跳过 :436-441 的 metadata.json 写出——失败的导出可能「发布」出没有清单的半成品目录。同族 geologicalexport.cpp:214 / mapexport.cpp:287-288 的发布条件更严（Succeeded 才发布）。

## 证据

三条的代码事实部分（上述 path:line）均已由独立复核员逐行核实；未复现的部分：QGIS changeGeometry 是否保 multi 性、零要素层是否真的建不出副本、worker 报错时 journal 是否非空——复核员无 Qt/QGIS/GEOS 源码与运行环境，按纪律标注 unconfirmed 而非断言。

## 影响

若三条全部成立：1 违反 GeoPackage 规范（几何列混类型），2 阻断合法工作流，3 半成品交付；若均不成立则仅为测试盲区。共同根因是相关路径缺断言级测试。

## 建议

按优先级补测试定性：① tst_faciespartition 增加 changed 特征 wkbType() 仍为 MULTIPOLYGON 的断言（对应第 1 条，成本最低）；② 零要素多边形层走 copyForEditing 的用例（第 2 条）；③ worker 报错时断言外部目录不被发布且 metadata.json 存在（第 3 条）。测试红 → 转正式 bug issue；测试绿 → 关闭本条并在测试里留痕。

## 验证

上述三条测试用例落库并给出确定结论；第 1 条若成立，同步在 apply() 的 changed 循环补 convertToMultiType()（与 :99 add 路径对齐）。

## 关联

- 来源：2026-10-18 swarm 评审（5 维度 + 逐条独立复核，31 条发现中 28 条确认、3 条即本单）；#521 / #522 / #523 相面编辑与导出链。
