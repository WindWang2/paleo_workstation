// 层：QGIS 封装
#include "constraintcrs.h"

#include "catalog/datacatalog.h"

#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>

#include <QObject>

namespace paleo::constraintcrs
{

QgsCoordinateReferenceSystem storeCrs()
{
  return QgsCoordinateReferenceSystem::fromWkt( DataCatalog::localGridCrsWkt() );
}

namespace
{
// 「同一 CRS」= QGIS 相等或 WKT2 全等。工程 CRS（ENGCRS/LOCAL_CS）没有 authid，
// 必须落到 WKT 逐字比较；两个不同的工程 CRS 文本不同即不同格网。
bool sameCrs( const QgsCoordinateReferenceSystem &a, const QgsCoordinateReferenceSystem &b )
{
  return a == b || ( a.isValid() && b.isValid() && a.toWkt() == b.toWkt() );
}
} // namespace

bool toStoreWkt( const QString &wkt, const QgsCoordinateReferenceSystem &source,
                 const QgsCoordinateTransformContext &context, QString *out, QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  if ( !out )
    return fail( QStringLiteral( "toStoreWkt: missing output" ) );
  const QgsCoordinateReferenceSystem target = storeCrs();
  // P0-1（pr457-closure）：等价只认「同一 CRS」——QGIS 相等或 WKT2 全等。
  // 工程坐标系即使单位同为米，原点/旋转/轴向也可能不同；绝不按单位推断等价，
  // 也绝不给无效来源静默指定本地网格（坐标含义无法核实就拒绝写入）。
  if ( !source.isValid() )
    return fail( QObject::tr( "来源坐标系无效：约束坐标含义无法核实，拒绝写入本地工程网格。"
                              "请提供画布/导入源的有效 CRS；无 CRS 的历史数据需先显式核定同一网格" ) );
  if ( sameCrs( source, target ) )
  {
    *out = wkt;
    return true;
  }
  QgsGeometry geometry = QgsGeometry::fromWkt( wkt );
  if ( geometry.isNull() )
    return fail( QObject::tr( "约束几何 WKT 无法解析" ) );
  const QgsCoordinateTransform transform( source, target, context );
  if ( !transform.isValid() )
    return fail( QObject::tr( "无法把约束几何从 %1 转换到本地工程网格：坐标系不同且工程没有对应的"
                              "坐标操作（未配准？）。单位同为米不代表同一局部网格" )
                     .arg( source.userFriendlyIdentifier() ) );
  try
  {
    if ( geometry.transform( transform ) != Qgis::GeometryOperationResult::Success )
      return fail( QObject::tr( "约束几何坐标转换失败（%1 → 本地工程网格）" ).arg( source.userFriendlyIdentifier() ) );
  }
  catch ( const QgsCsException &e )
  {
    return fail( QObject::tr( "约束几何坐标转换失败：%1" ).arg( e.what() ) );
  }
  *out = geometry.asWkt( 17 );
  return true;
}

StoreCrsAssignment assignStoreCrsIfMissing( QgsMapLayer *layer )
{
  StoreCrsAssignment result;
  if ( !layer || layer->crs().isValid() )
    return result;
  const QString id = layer->customProperty( QStringLiteral( "paleoLayerId" ) ).toString();
  const bool constraintLayer = id.startsWith( QLatin1String( "constraints." ) ) ||
                               layer->source().contains( QLatin1String( "layername=constraints" ) );
  if ( !constraintLayer )
    return result;
  // 历史 CRS-less 库：文件里没有坐标定义，指派只是「按工程上下文假定」。
  // 溯源写进图层属性，UI/诊断据此区分「已核实」与「假定」（不冒充已核实）。
  layer->setCrs( storeCrs() );
  layer->setCustomProperty( QStringLiteral( "paleoCrsProvenance" ), QStringLiteral( "assumed_from_context" ) );
  result.assigned = true;
  result.verified = false;
  return result;
}

QString layerCrsProvenance( QgsMapLayer *layer )
{
  if ( !layer || !layer->crs().isValid() )
    return QStringLiteral( "none" );
  const QString marked = layer->customProperty( QStringLiteral( "paleoCrsProvenance" ) ).toString();
  if ( marked == QLatin1String( "assumed_from_context" ) )
    return marked;
  // 无标记且 CRS 有效 = 提供方从文件读到 SRS（新库契约）。
  return QStringLiteral( "verified_from_file" );
}

} // namespace paleo::constraintcrs
