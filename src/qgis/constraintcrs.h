// 层：QGIS 封装
#pragma once

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>

#include <QString>

class QgsMapLayer;

// 约束库 CRS 契约（P1-C）。ConstraintStore 的 GPKG 层 "constraints" 建库时
// 不带 SRS（wkbUnknown、无 SRS，历史 schema 不迁移）；库内坐标的约定 CRS
// 是本地工程网格 DataCatalog::localGridCrsWkt()。所有写入（绘制/导入）先把
// 几何从来源 CRS 显式变换到该 CRS；读出的 QGIS 图层由 assignStoreCrsIfMissing
// 补上 CRS，画布再按工程转换上下文（配准工程的显式坐标操作）显示。
namespace paleo::constraintcrs
{

QgsCoordinateReferenceSystem storeCrs();

// 与库 CRS 同一（QGIS 相等或 WKT2 全等）→ 原样（已是库坐标）；来源 CRS 无效
// → 拒绝（坐标含义无法核实，不静默指定本地网格）；否则用 context 建
// QgsCoordinateTransform；无坐标操作（ENGCRS 无大地基准、工程未配准）或
// 逐点失败如实报错，绝不按原坐标静默写入。单位同为米不构成等价：不同原点/
// 旋转/轴向的工程坐标系之间没有安全的 identity。
bool toStoreWkt( const QString &wkt, const QgsCoordinateReferenceSystem &source,
                 const QgsCoordinateTransformContext &context, QString *out, QString *error = nullptr );

// 约束图层（paleoLayerId 以 constraints. 开头或数据源指向 constraints 层）
// CRS 无效时指派库 CRS；已有有效 CRS 不动。assigned=是否做了指派；
// verified=指派是否来自文件里的 SRS（本函数只在文件无 SRS 时指派，故恒为
// false——这正是「历史 CRS-less，按工程上下文假定」的标志，不冒充已核实）。
struct StoreCrsAssignment
{
  bool assigned = false;
  bool verified = false;
};
StoreCrsAssignment assignStoreCrsIfMissing( QgsMapLayer *layer );

// 约束图层 CRS 溯源：文件带 SRS（verified_from_file）还是运行时按工程上下文
// 假定（assumed_from_context）。无 CRS 返回 "none"。
QString layerCrsProvenance( QgsMapLayer *layer );

} // namespace paleo::constraintcrs
