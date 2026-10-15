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
  // 与导入坐标门（acceptsLocalGrid）同口径：任意米制工程 CRS（LOCAL_CS / ENGCRS，
  // 无大地基准）都视作本工程局部网格——它们之间本来就没有坐标操作可用。
  const bool localEquivalent = source.isValid() && !source.isGeographic() &&
                               source.type() == Qgis::CrsType::Engineering &&
                               source.mapUnits() == Qgis::DistanceUnit::Meters;
  if ( !source.isValid() || source == target || localEquivalent )
  {
    *out = wkt;
    return true;
  }
  QgsGeometry geometry = QgsGeometry::fromWkt( wkt );
  if ( geometry.isNull() )
    return fail( QObject::tr( "约束几何 WKT 无法解析" ) );
  const QgsCoordinateTransform transform( source, target, context );
  if ( !transform.isValid() )
    return fail( QObject::tr( "无法把约束几何从 %1 转换到本地工程网格：工程没有对应的坐标操作（未配准？）" )
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

bool assignStoreCrsIfMissing( QgsMapLayer *layer )
{
  if ( !layer || layer->crs().isValid() )
    return false;
  const QString id = layer->customProperty( QStringLiteral( "paleoLayerId" ) ).toString();
  const bool constraintLayer = id.startsWith( QLatin1String( "constraints." ) ) ||
                               layer->source().contains( QLatin1String( "layername=constraints" ) );
  if ( !constraintLayer )
    return false;
  layer->setCrs( storeCrs() );
  return true;
}

} // namespace paleo::constraintcrs
