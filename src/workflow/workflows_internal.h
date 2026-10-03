// 层：功能
#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVariantMap>

#include <gdal.h>

class QgisLayerService;
class QObject;
class DerivedAssetRegistrar;

#if PALEO_HAVE_ORT
// onnxAreaGrid/resolveOnnxGrid/writeOnnxRaster 的 inline 体使用全型——
// 头部自含（IWYU），不再靠各 .cpp 恰好把这些头排在本头之前。
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <cstring>
#include <cpl_conv.h>
#include <ogr_spatialref.h>
#include <qgis.h>
#include <qgscoordinatereferencesystem.h>
#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../qgis/qgislayerservice.h"
#endif

namespace paleo::workflow_detail {

/// 三段式各段统一的失败文案落点：*error 非空才写。
inline void setError( QString *error, const QString &text )
{
  if ( error )
    *error = text;
}

/// 结果图层 id ↔ 资产 id 的互链（写在 QgsMapLayer 的自定义属性上）。
/// QgisLayerService/QgsMapLayer 只在 .cpp 侧可见，故这里只前向声明。
void stampLayerAssetLink( QgisLayerService *layers, const QString &layerId,
                          const QString &assetId );

/// 派生产物登记通道（catalog 与 projectDir 走 QObject 动态属性，见 workflows.h）。
/// 返回值是按值的 registrar，需要完整类型，故定义在 workflows.cpp。
DerivedAssetRegistrar derivedRegistrarOf( const QObject *wf, QString *error );

/// OUTPUT 是约定的目的地键；为空时回落到首个字符串值的结果，让目的地命名
/// 不同的算法也能声明产物。
inline QString outputPathOf( const QVariantMap &results )
{
  QString out = results.value( QStringLiteral( "OUTPUT" ) ).toString();
  if ( out.isEmpty() )
  {
    for ( const QVariant &v : results )
    {
      if ( v.typeId() == QMetaType::QString && !v.toString().isEmpty() )
      {
        out = v.toString();
        break;
      }
    }
  }
  return out;
}

/// 已声明的源在「基底（去掉 "|layername=" 之类后缀）是本地文件系统路径」时算
/// file-backed。内存伪 URI 与远程/VSI 源无法用 QFile 检查，跳过。
inline bool isFileBackedSource( const QString &source )
{
  if ( source.isEmpty() )
    return false;
  const QString base = source.section( QLatin1Char( '|' ), 0, 0 );
  if ( base.isEmpty() )
    return false;
  if ( base.startsWith( QStringLiteral( "memory" ), Qt::CaseInsensitive ) )
    return false;
  if ( base.contains( QLatin1Char( '?' ) ) )          // memory provider URI ("Point?crs=...")
    return false;
  if ( base.contains( QStringLiteral( "://" ) ) )     // remote URI
    return false;
  if ( base.startsWith( QStringLiteral( "/vsi" ) ) )  // GDAL virtual filesystem
    return false;
  return true;
}

/// 紧凑 UTC 时间戳——让结果图层 id 逐次运行唯一。
inline QString stamp()
{
  return QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyyMMdd-hhmmss-zzz" ) );
}

// ---------------------------------------------------------------------------
// ONNX 推理面的工区网格辅助（方向20 轮4 从 workflows.cpp 迁入）。
//
// 跨 Prediction 与 Constraint 两段共用（前者 4 处、后者 7 处），故随 Constraint
// 段迁移前先搬进这个共享头——留在 workflows.cpp 的匿名命名空间里会让
// predictionworkflow.cpp 链接不到（且本机未启用 ORT 时察觉不到）。
// ---------------------------------------------------------------------------

#if PALEO_HAVE_ORT
// project_area 工区网格（PROJECT_AREA_PLAN §3 + autoplan eng trap）：ONNX 结果
// 落在标定层位的工程栅格上（默认 D61 411×641，北向上 geotransform
// (0, 12793/640, 0, 16406, 0, -16406/410)）。清单里已声明的
// horizon.<target>* 栅格优先提供 geotransform 与 SRS，读不到时用这组常量；
// 行列数硬要求 = AreaRules::onnxGrid（工程配置），不是就失败。
// 注：geotransform 常量仍是本工区兜底——新工区应声明供体栅格（清单
// horizon.<target>*），否则 GT 会落在 project_area 的原点上。

struct OnnxAreaGrid
{
  // (xmin, dx, 0, ymax, 0, -dy) — 原点是左上角像元的外角。
  double gt[6] = { 0.0, 12793.0 / 640.0, 0.0, 16406.0, 0.0, -16406.0 / 410.0 };
  QString projection; // 声明的 D61 栅格自身 SRS（可读时优先于参数/常量）
  QString sourcePath; // 提供几何的 D61 栅格文件（DERIVED 父版本溯源用）
  bool fromDecl = false;
};

// 从图层清单取 horizon.D61* 栅格声明的网格几何；声明缺失或文件不可读时
// 保持常量（常量本来就是同一套 D61 geotransform）。
inline OnnxAreaGrid onnxAreaGrid( const QgisLayerService *layers )
{
  OnnxAreaGrid grid;
  if ( !layers )
    return grid;
  const QString prefix = QStringLiteral( "horizon.%1" )
                             .arg( AreaRules::active().targetHorizon );
  QString source;
  for ( const LayerDeclaration &d : layers->declared() )
  {
    if ( !d.layerId.startsWith( prefix ) ||
         d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
      continue;
    source = d.source;
    break;
  }
  if ( source.isEmpty() )
    return grid;
  const QString path = source.section( QLatin1Char( '|' ), 0, 0 );
  if ( path.isEmpty() || !QFile::exists( path ) )
    return grid;
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
    return grid;
  double gt[6] = { 0, 0, 0, 0, 0, 0 };
  if ( GDALGetGeoTransform( ds, gt ) == CE_None )
  {
    std::memcpy( grid.gt, gt, sizeof( gt ) );
    grid.fromDecl = true;
    grid.sourcePath = path;
  }
  const char *proj = GDALGetProjectionRef( ds );
  if ( proj && *proj )
    grid.projection = QString::fromUtf8( proj );
  GDALClose( ds );
  return grid;
}

// 挤成二维（去掉全部长度-1 维）后必须正好是工区网格 411×641，否则不写栅格、
// 不登记图层——plan 钉死的文案，附实际行列数（标量/点结果同样拒绝）。
inline bool resolveOnnxGrid( const QVector<float> &values, const QVector<int64_t> &outShape,
                             int &rows, int &cols, QString *error )
{
  const qsizetype n = values.size();
  if ( n <= 0 )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "ONNX output is empty" ) );
    return false;
  }
  QVector<qint64> dims;
  for ( qint64 d : outShape )
  {
    if ( d != 1 )
      dims.append( d );
  }
  // 标量（shape [] 或全 1）按 1×1 报告实际行列数。
  QStringList parts;
  for ( qint64 d : dims )
    parts << QString::number( d );
  const QString actual = parts.isEmpty() && n == 1 ? QStringLiteral( "1×1" )
                                                   : parts.join( QStringLiteral( "×" ) );
  if ( dims.size() == 2 && dims[0] > 0 && dims[1] > 0 &&
       dims[0] * dims[1] == static_cast<qint64>( n ) )
  {
    rows = static_cast<int>( dims[0] );
    cols = static_cast<int>( dims[1] );
  }
  else
  {
    rows = ( n == 1 ) ? 1 : 0;
    cols = ( n == 1 ) ? 1 : 0;
  }
  const AreaRules::OnnxGrid want = AreaRules::active().onnxGrid;
  if ( rows != want.rows || cols != want.cols )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "结果不是 %1×%2，没有写入栅格（实际 %3）" )
                         .arg( want.rows )
                         .arg( want.cols )
                         .arg( actual.isEmpty() ? QString::number( n ) : actual ) );
    return false;
  }
  return true;
}

// 把通过门禁的 411×641 结果写成 Float32 GeoTIFF，geotransform/SRS 用
// OnnxAreaGrid（D61 声明优先，否则 §3 常量 + 局部测网 CRS）。
inline QString writeOnnxRaster( const QString &path, const QVector<float> &values, int rows, int cols,
                        const OnnxAreaGrid &grid, const QVariantMap &params,
                        const QString &model, const QVector<int64_t> &outShape,
                        QString *error )
{
  GDALAllRegister();
  GDALDriverH drv = GDALGetDriverByName( "GTiff" );
  if ( !drv )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "GTiff driver is not available" ) );
    return QString();
  }
  if ( QFile::exists( path ) )
    QFile::remove( path );
  GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
  if ( !ds )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "cannot create prediction raster %1" ).arg( path ) );
    return QString();
  }
  double gt[6];
  std::memcpy( gt, grid.gt, sizeof( gt ) );
  GDALSetGeoTransform( ds, gt );
  // SRS 优先级：显式参数 > D61 栅格自身投影 > 局部测网 CRS（与 horizonbinner
  // 一致；绝不把局部米写成经纬度）。
  const QString auth = params.value( QStringLiteral( "crs" ) ).toString();
  if ( !auth.isEmpty() )
  {
    const QgsCoordinateReferenceSystem crs( auth );
    if ( crs.isValid() )
    {
      const QByteArray wkt = crs.toWkt( Qgis::CrsWktVariant::Wkt1Gdal ).toUtf8();
      GDALSetProjection( ds, wkt.constData() );
    }
  }
  else if ( !grid.projection.isEmpty() )
  {
    GDALSetProjection( ds, grid.projection.toUtf8().constData() );
  }
  else
  {
    OGRSpatialReference srs;
    if ( srs.SetFromUserInput( DataCatalog::localGridCrsWkt().toUtf8().constData() ) == OGRERR_NONE )
    {
      char *wkt = nullptr;
      if ( srs.exportToWkt( &wkt ) == OGRERR_NONE && wkt )
        GDALSetProjection( ds, wkt );
      CPLFree( wkt );
    }
  }
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, -9999.0 );
  if ( GDALRasterIO( band, GF_Write, 0, 0, cols, rows, const_cast<float *>( values.constData() ),
                     cols, rows, GDT_Float32, 0, 0 ) != CE_None )
  {
    GDALClose( ds );
    QFile::remove( path );
    paleo::workflow_detail::setError( error, QObject::tr( "failed to write prediction raster %1" ).arg( path ) );
    return QString();
  }

  QJsonObject prov;
  prov.insert( QStringLiteral( "model" ), model );
  prov.insert( QStringLiteral( "rows" ), rows );
  prov.insert( QStringLiteral( "cols" ), cols );
  prov.insert( QStringLiteral( "geotransform_source" ),
               grid.fromDecl ? QStringLiteral( "horizon.%1" ).arg(
                                   AreaRules::active().targetHorizon )
                             : QStringLiteral( "project_area" ) );
  QJsonArray gtJson;
  for ( int i = 0; i < 6; ++i )
    gtJson.append( gt[i] );
  prov.insert( QStringLiteral( "geotransform" ), gtJson );
  QJsonArray shapeJson;
  for ( qint64 d : outShape )
    shapeJson.append( static_cast<double>( d ) );
  prov.insert( QStringLiteral( "output_shape" ), shapeJson );
  const QByteArray json = QJsonDocument( prov ).toJson( QJsonDocument::Compact );
  GDALSetMetadataItem( ds, "PALEO_MODEL", model.toUtf8().constData(), nullptr );
  GDALSetMetadataItem( ds, "PALEO_PROVENANCE", json.constData(), nullptr );
  GDALClose( ds );
  return path;
}
#endif
} // namespace paleo::workflow_detail
