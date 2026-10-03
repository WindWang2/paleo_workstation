// 层：数据
#include "faciescoveragereader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

namespace paleo::evolution
{

namespace
{

// OGR 环几何 → DTO Ring（点数不足的退化环跳过）。
bool readRing( OGRGeometryH ring, Ring *out )
{
  const int count = OGR_G_GetPointCount( ring );
  if ( count < 4 )
    return false;
  out->points.reserve( static_cast<std::size_t>( count ) );
  for ( int i = 0; i < count; ++i )
    out->points.push_back( Point2{ OGR_G_GetX( ring, i ), OGR_G_GetY( ring, i ) } );
  return true;
}

// 单个面几何（Polygon）→ DTO；嵌套在 MultiPolygon/Collection 里的递归展开。
bool readPolygon( OGRGeometryH geom, Polygon *out )
{
  const OGRwkbGeometryType type = wkbFlatten( OGR_G_GetGeometryType( geom ) );
  if ( type == wkbMultiPolygon || type == wkbGeometryCollection )
  {
    return false; // 由调用侧逐子几何展开
  }
  if ( type != wkbPolygon )
    return false;
  Ring exterior;
  if ( !readRing( OGR_G_GetGeometryRef( geom, 0 ), &exterior ) )
    return false;
  out->exterior = std::move( exterior );
  const int rings = OGR_G_GetGeometryCount( geom );
  for ( int i = 1; i < rings; ++i )
  {
    Ring hole;
    if ( readRing( OGR_G_GetGeometryRef( geom, i ), &hole ) )
      out->holes.push_back( std::move( hole ) );
  }
  return true;
}

void collectPatches( OGRGeometryH geom, int faciesCode, std::vector<FaciesPatch> *patches )
{
  const OGRwkbGeometryType type = wkbFlatten( OGR_G_GetGeometryType( geom ) );
  if ( type == wkbMultiPolygon || type == wkbGeometryCollection )
  {
    const int parts = OGR_G_GetGeometryCount( geom );
    for ( int i = 0; i < parts; ++i )
      collectPatches( OGR_G_GetGeometryRef( geom, i ), faciesCode, patches );
    return;
  }
  FaciesPatch patch;
  patch.faciesCode = faciesCode;
  if ( readPolygon( geom, &patch.geometry ) )
    patches->push_back( std::move( patch ) );
}

// PALEO_PROVENANCE → GridSpec。缺格网戳/字段不齐 → false（拒算面，不猜）。
bool gridFromProvenance( const QString &provenance, GridSpec *grid )
{
  const QJsonDocument doc = QJsonDocument::fromJson( provenance.toUtf8() );
  if ( !doc.isObject() )
    return false;
  const QJsonObject gridObj = doc.object().value( QStringLiteral( "grid" ) ).toObject();
  if ( gridObj.isEmpty() )
    return false;
  const QJsonArray gt = gridObj.value( QStringLiteral( "geotransform" ) ).toArray();
  if ( gt.size() != 6 )
    return false;
  const int cols = gridObj.value( QStringLiteral( "cols" ) ).toInt( -1 );
  const int rows = gridObj.value( QStringLiteral( "rows" ) ).toInt( -1 );
  if ( cols <= 0 || rows <= 0 )
    return false;
  // 北向上 north-up geotransform：[0]=originX，[3]=originY，[1]=pixelWidth，
  // [5]=pixelHeight（<0，行向南）。
  grid->cols = cols;
  grid->rows = rows;
  grid->originX = gt.at( 0 ).toDouble();
  grid->originY = gt.at( 3 ).toDouble();
  grid->pixelWidth = gt.at( 1 ).toDouble();
  grid->pixelHeight = gt.at( 5 ).toDouble();
  grid->crs = gridObj.value( QStringLiteral( "crs_wkt" ) ).toString().toStdString();
  grid->convention = "north_up_pixel_is_area";
  return grid->pixelWidth != 0 && grid->pixelHeight != 0;
}

} // namespace

CoverageReadResult readFaciesCoverage( const QString &sourceUri, const QString &horizon )
{
  CoverageReadResult result;
  const QString path = sourceUri.section( QLatin1Char( '|' ), 0, 0 );
  QString layerName = QStringLiteral( "facies_polygons" );
  const int nameAt = sourceUri.indexOf( QStringLiteral( "layername=" ) );
  if ( nameAt >= 0 )
    layerName = sourceUri.mid( nameAt + QStringLiteral( "layername=" ).size() );

  GDALDatasetH ds = GDALOpenEx( path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                nullptr, nullptr, nullptr );
  if ( !ds )
  {
    result.error = QObject::tr( "无法打开相多边形文件：%1" ).arg( path );
    return result;
  }

  const char *provRaw = GDALGetMetadataItem( ds, "PALEO_PROVENANCE", nullptr );
  const QString provenance = provRaw ? QString::fromUtf8( provRaw ) : QString();
  result.provenance = provenance;
  GridSpec grid;
  if ( provenance.isEmpty() )
  {
    GDALClose( ds );
    result.error = QObject::tr( "相多边形产物缺少 PALEO_PROVENANCE 口径戳（%1），无法证明其格网来源" ).arg( path );
    return result;
  }
  if ( !gridFromProvenance( provenance, &grid ) )
  {
    GDALClose( ds );
    result.error = QObject::tr( "相多边形口径戳里没有可用的格网（%1）——需用新版分相算法重 derive 后再对比" ).arg( path );
    return result;
  }

  OGRLayerH layer = GDALDatasetGetLayerByName( ds, layerName.toUtf8().constData() );
  if ( !layer )
  {
    GDALClose( ds );
    result.error = QObject::tr( "文件 %1 里没有图层 %2" ).arg( path, layerName );
    return result;
  }
  OGRFeatureDefnH defn = OGR_L_GetLayerDefn( layer );
  const int codeField = OGR_FD_GetFieldIndex( defn, "facies_code" );
  if ( codeField < 0 )
  {
    GDALClose( ds );
    result.error = QObject::tr( "图层 %1 缺少 facies_code 字段" ).arg( layerName );
    return result;
  }

  FaciesCoverage coverage;
  coverage.horizon = horizon.toStdString();
  coverage.grid = grid;
  OGR_L_ResetReading( layer );
  OGRFeatureH feat = nullptr;
  while ( ( feat = OGR_L_GetNextFeature( layer ) ) != nullptr )
  {
    OGRGeometryH geom = OGR_F_StealGeometry( feat );
    if ( geom )
    {
      collectPatches( geom, OGR_F_GetFieldAsInteger( feat, codeField ), &coverage.patches );
      OGR_G_DestroyGeometry( geom );
    }
    OGR_F_Destroy( feat );
  }
  GDALClose( ds );

  if ( coverage.patches.empty() )
  {
    result.error = QObject::tr( "图层 %1 没有可用的相面片几何" ).arg( layerName );
    return result;
  }
  result.coverage = std::move( coverage );
  result.ok = true;
  return result;
}

} // namespace paleo::evolution
