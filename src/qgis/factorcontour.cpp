// 层：QGIS 封装
#include "factorcontour.h"
#include "../services/paleotaskservice.h"
#include "qgiserrors_internal.h"

#include "../algorithms/singlefactor/contourlevels.h"
#include "../algorithms/singlefactor/fieldcontours.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>

#include <cmath>
#include <limits>
#include <vector>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

// 层：QGIS 封装

namespace
{
using paleo::qgis_detail::setError;

// 等值线 GPKG 落盘走「同目录临时文件 + 成功后原子替换」（#588 B.13）：
// 直接 remove 旧文件再 GDALCreate，一旦创建失败（磁盘满/权限/驱动缺失）
// 旧产物已被删除，用户拿到错误却两边都没有输出。
QString stagingPathFor( const QString &outputGpkg )
{
  return outputGpkg + QStringLiteral( ".tmp" );
}

// 写成功后把临时文件换成正式产物；失败（含临时文件缺失）返回 false，
// 此时正式产物保持调用前状态（不存在或仍是上一版）。
// #74：不能先 QFile::remove(正式) 再 rename——remove 成功而 rename 失败的
// 窗口内新旧两失。旧产物先改名 .bak，staging rename 成功后再删 .bak；
// rename 失败时把 .bak 改回原名并保留 staging 供排查。
bool commitStagingGpkg( const QString &stagingGpkg, const QString &outputGpkg,
                        QString *error )
{
  if ( !QFile::exists( stagingGpkg ) )
  {
    setError( error, QStringLiteral( "contour run reported success but '%1' is missing" )
                          .arg( stagingGpkg ) );
    return false;
  }
  const QString backup = outputGpkg + QStringLiteral( ".bak" );
  QFile::remove( backup );
  const bool hadOutput = QFile::exists( outputGpkg );
  if ( hadOutput && !QFile::rename( outputGpkg, backup ) )
  {
    setError( error, QStringLiteral( "cannot move existing contour output aside: %1" )
                          .arg( outputGpkg ) );
    return false;
  }
  if ( !QFile::rename( stagingGpkg, outputGpkg ) )
  {
    setError( error, QStringLiteral( "cannot move contour output into place: %1" )
                          .arg( outputGpkg ) );
    if ( hadOutput )
      QFile::rename( backup, outputGpkg ); // 回滚：旧产物还原
    return false;
  }
  QFile::remove( backup );
  return true;
}

} // namespace

namespace FactorContourService
{

namespace
{
bool generateWithOption(const QString &rasterPath, const QString &outputGpkg,
                        const QByteArray &levelOption, QString *error,
                        double capInterval = 0.0,
                        const std::function<bool()> &cancelled = {},
                        const std::function<void(double)> &progress = {}) {
  if ( rasterPath.isEmpty() || !QFile::exists( rasterPath ) )
  {
    setError( error, QStringLiteral( "contour input raster not found: %1" ).arg( rasterPath ) );
    return false;
  }
  if ( outputGpkg.isEmpty() )
  {
    setError( error, QStringLiteral( "contour output path is empty" ) );
    return false;
  }

  GDALAllRegister();

  GDALDatasetH src = GDALOpen( rasterPath.toUtf8().constData(), GA_ReadOnly );
  if ( !src )
  {
    setError( error, QStringLiteral( "cannot open raster '%1' for contouring" ).arg( rasterPath ) );
    return false;
  }
  GDALRasterBandH band = GDALGetRasterBand( src, 1 );
  if ( !band )
  {
    setError( error, QStringLiteral( "raster '%1' has no band 1" ).arg( rasterPath ) );
    GDALClose( src );
    return false;
  }
  if ( capInterval > 0.0 )
  {
    // #149：LEVEL_INTERVAL 路径同样先估级别数，间距过小直接拒绝（GDAL 会逐级追线卡死）。
    // 返回值在不同 GDAL 版本里是 void/CPLErr——不依赖它，用 NaN 哨兵判定是否算出。
    double minMax[2] = { std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::quiet_NaN() };
    GDALComputeRasterMinMax( band, FALSE, minMax );
    if ( std::isfinite( minMax[0] ) && std::isfinite( minMax[1] ) )
    {
      const double estimated =
          paleo::singlefactor::estimateContourLevelCount( minMax[1] - minMax[0], capInterval );
      if ( !( estimated <= static_cast<double>( paleo::singlefactor::kMaxContourLevels ) ) )
      {
        setError( error, QStringLiteral( "等值线间距过小：间距 %1 在值域 %2~%3 上将生成约 %4 条级别，"
                                         "超过上限 %5 条；请增大间距" )
                             .arg( capInterval, 0, 'g', 6 )
                             .arg( minMax[0], 0, 'g', 8 )
                             .arg( minMax[1], 0, 'g', 8 )
                             .arg( estimated, 0, 'g', 6 )
                             .arg( static_cast<qulonglong>( paleo::singlefactor::kMaxContourLevels ) ) );
        GDALClose( src );
        return false;
      }
    }
  }

  // GPKG 输出（OGR 面）：先写临时文件再原子替换，失败不影响旧产物。
  const QString stagingGpkg = stagingPathFor( outputGpkg );
  QFile::remove( stagingGpkg );
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
  {
    setError( error, QStringLiteral( "GDAL GPKG driver unavailable" ) );
    GDALClose( src );
    return false;
  }
  GDALDatasetH ds = GDALCreate( drv, stagingGpkg.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
  {
    setError( error, QStringLiteral( "cannot create contour output '%1'" ).arg( outputGpkg ) );
    GDALClose( src );
    return false;
  }

  OGRSpatialReferenceH srs = const_cast<OGRSpatialReferenceH>( GDALGetSpatialRef( src ) );
  OGRSpatialReferenceH preserved = nullptr;
  if(const char *wkt=GDALGetMetadataItem(src,"PALEO_CRS_WKT",nullptr)) {
    preserved=OSRNewSpatialReference(wkt);if(preserved)srs=preserved;
  }
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "contours", srs, wkbLineString, nullptr );
  if(preserved)OSRDestroySpatialReference(preserved);
  if ( !layer )
  {
    setError( error, QStringLiteral( "cannot create layer 'contours' in '%1'" ).arg( outputGpkg ) );
    GDALClose( ds );
    QFile::remove( stagingGpkg );
    GDALClose( src );
    return false;
  }
  OGRFieldDefnH idField = OGR_Fld_Create( "ID", OFTInteger64 );
  OGR_L_CreateField( layer, idField, true );
  OGR_Fld_Destroy( idField );
  OGRFieldDefnH elevField = OGR_Fld_Create( "ELEV", OFTReal );
  OGR_L_CreateField( layer, elevField, true );
  OGR_Fld_Destroy( elevField );

  // 与 gdal:contour 工具同参（LEVEL_INTERVAL/ID_FIELD/ELEV_FIELD 是
  // GDALContourGenerateEx 认的 option 名——utility 与 C API 共用底层）。
  const char *options[] = { levelOption.constData(), "ID_FIELD=0", "ELEV_FIELD=1", nullptr };

  struct Feedback {
    std::function<bool()> cancel;
    std::function<void(double)> report;
  } feedback{cancelled, progress};
  const CPLErr rc = GDALContourGenerateEx(
      band, layer, options,
      [](double fraction, const char *, void *data) -> int {
        const auto &f = *static_cast<Feedback *>(data);
        if (f.cancel && f.cancel())
          return FALSE;
        if (f.report)
          f.report(fraction * 100);
        return TRUE;
      },
      &feedback);

  GDALClose( ds );
  GDALClose( src );

  if ( rc != CE_None )
  {
    setError( error, QStringLiteral( "GDALContourGenerateEx failed (rc=%1)" ).arg( int( rc ) ) );
    QFile::remove( stagingGpkg );
    return false;
  }
  return commitStagingGpkg( stagingGpkg, outputGpkg, error );
}
} // namespace

bool generateContours(const QString &rasterPath, const QString &outputGpkg,
                      double interval, QString *error,
                      const std::function<bool()> &cancelled,
                      const std::function<void(double)> &progress) {
  if ( !( interval > 0.0 ) )
  {
    setError( error, QStringLiteral( "contour interval must be positive (got %1)" ).arg( interval ) );
    return false;
  }
  const QByteArray levelOption = QByteArray( "LEVEL_INTERVAL=" ) + QByteArray::number( interval );
  return generateWithOption(rasterPath, outputGpkg, levelOption, error,
                            interval, cancelled, progress);
}

bool generateFixedContours( const QString &rasterPath, const QString &outputGpkg,
                            const QVector<double> &levels, QString *error,
                            const std::function<bool()> &cancelled )
{
  if ( levels.isEmpty() )
  {
    setError( error, QStringLiteral( "contour levels are empty" ) );
    return false;
  }
  QStringList text;
  for ( double level : levels )
  {
    if ( !std::isfinite( level ) )
    {
      setError( error, QStringLiteral( "contour level must be finite" ) );
      return false;
    }
    text << QString::number( level, 'g', 17 );
  }
  const QByteArray levelOption = QByteArray( "FIXED_LEVELS=" ) + text.join( QLatin1Char( ',' ) ).toUtf8();
  return generateWithOption( rasterPath, outputGpkg, levelOption, error, 0.0, cancelled );
}

namespace
{
// Float64 结构栅格 → 升序 y 行主序网格（nodata → NaN）。尺寸与 sidecar
// 轴一致才返回 true。
bool readStructuralRaster( const QString &rasterPath, std::vector<double> *grid,
                           int *nxOut, int *nyOut, OGRSpatialReferenceH *srsOut,
                           QString *error )
{
  GDALDatasetH src = GDALOpen( rasterPath.toUtf8().constData(), GA_ReadOnly );
  if ( !src )
  {
    setError( error, QStringLiteral( "cannot open raster '%1' for contouring" ).arg( rasterPath ) );
    return false;
  }
  const int nx = GDALGetRasterXSize( src );
  const int ny = GDALGetRasterYSize( src );
  GDALRasterBandH band = GDALGetRasterBand( src, 1 );
  if ( !band || nx < 2 || ny < 2 )
  {
    setError( error, QStringLiteral( "raster '%1' has no band 1" ).arg( rasterPath ) );
    GDALClose( src );
    return false;
  }
  int hasNodata = 0;
  const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
  // L-09 同口径像元预算闸（algorithms/faciespolygonize.cpp:1424，1 亿格）：
  // raw + grid 两份 double 全栅格常驻（约 16 字节/像元），无闸的超大输入是
  // bad_alloc/OOM 而不是如实报错。
  if ( qint64( nx ) * ny > qint64( 100'000'000 ) )
  {
    setError( error, QStringLiteral( "raster '%1' exceeds the 100M-cell budget (%2×%3)" )
                         .arg( rasterPath ).arg( nx ).arg( ny ) );
    GDALClose( src );
    return false;
  }
  std::vector<double> raw( static_cast<std::size_t>( nx ) * static_cast<std::size_t>( ny ) );
  if ( GDALRasterIO( band, GF_Read, 0, 0, nx, ny, raw.data(), nx, ny, GDT_Float64, 0,
                     0 ) != CE_None )
  {
    setError( error, QStringLiteral( "cannot read band 1 of '%1'" ).arg( rasterPath ) );
    GDALClose( src );
    return false;
  }
  grid->assign( raw.size(), std::numeric_limits<double>::quiet_NaN() );
  for ( int r = 0; r < ny; ++r )
  {
    // 栅格行 0 = 最大 y → 网格行 = 升序 y 需翻转。
    const std::size_t dst = static_cast<std::size_t>( ny - 1 - r ) * static_cast<std::size_t>( nx );
    const std::size_t srcOff = static_cast<std::size_t>( r ) * static_cast<std::size_t>( nx );
    for ( int c = 0; c < nx; ++c )
    {
      const double v = raw[srcOff + static_cast<std::size_t>( c )];
      if ( hasNodata && v == nodata )
        continue;
      ( *grid )[dst + static_cast<std::size_t>( c )] = v;
    }
  }
  OGRSpatialReferenceH srs = const_cast<OGRSpatialReferenceH>( GDALGetSpatialRef( src ) );
  OGRSpatialReferenceH preserved = nullptr;
  if ( const char *wkt = GDALGetMetadataItem( src, "PALEO_CRS_WKT", nullptr ) )
  {
    preserved = OSRNewSpatialReference( wkt );
    if ( preserved )
      srs = preserved;
  }
  *srsOut = srs ? OSRClone( srs ) : nullptr;
  if ( preserved )
    OSRDestroySpatialReference( preserved );
  *nxOut = nx;
  *nyOut = ny;
  GDALClose( src );
  return true;
}

void createFieldDef( OGRLayerH layer, const char *name, OGRFieldType type )
{
  OGRFieldDefnH field = OGR_Fld_Create( name, type );
  OGR_L_CreateField( layer, field, true );
  OGR_Fld_Destroy( field );
}

// 北向上、无旋转的分析栅格 → 像元中心轴。grid 行序已翻成 y 升序。
bool readAnalysisField( const QString &rasterPath, std::vector<double> *grid,
                        std::vector<double> *xs, std::vector<double> *ys,
                        std::vector<std::uint8_t> *validMask, int *nxOut, int *nyOut,
                        OGRSpatialReferenceH *srsOut, QString *error )
{
  GDALDatasetH src = GDALOpen( rasterPath.toUtf8().constData(), GA_ReadOnly );
  if ( !src )
  {
    setError( error, QStringLiteral( "cannot open raster '%1' for contouring" ).arg( rasterPath ) );
    return false;
  }
  double gt[6] = {};
  if ( GDALGetGeoTransform( src, gt ) != CE_None || std::abs( gt[2] ) > 1e-9 ||
       std::abs( gt[4] ) > 1e-9 || !( gt[1] > 0.0 ) || !( gt[5] < 0.0 ) )
  {
    setError( error, QStringLiteral( "分析场不是北向上的规则网格，不能按制图规范提线：%1" ).arg( rasterPath ) );
    GDALClose( src );
    return false;
  }
  GDALClose( src );
  if ( !readStructuralRaster( rasterPath, grid, nxOut, nyOut, srsOut, error ) )
    return false;
  const int nx = *nxOut;
  const int ny = *nyOut;
  // #834 旁注：可分轴 xs[c]/ys[k] 只能表示北向上无旋转网格。上面已拒绝
  // gt[2]/gt[4]≠0；此处不可套完整仿射——那会让 xs 仍与行无关，看起来像"修了"
  // 实际仍错。旋转测网的静默错值修在 velocitymodel/mappingworkflow（#834）。
  xs->resize( static_cast<std::size_t>( nx ) );
  ys->resize( static_cast<std::size_t>( ny ) );
  for ( int c = 0; c < nx; ++c )
    ( *xs )[static_cast<std::size_t>( c )] = gt[0] + ( c + 0.5 ) * gt[1];
  for ( int k = 0; k < ny; ++k )
  {
    const int rasterRow = ny - 1 - k;
    ( *ys )[static_cast<std::size_t>( k )] = gt[3] + ( rasterRow + 0.5 ) * gt[5];
  }
  validMask->assign( grid->size(), 0 );
  for ( std::size_t i = 0; i < grid->size(); ++i )
    ( *validMask )[i] = std::isfinite( ( *grid )[i] ) ? 1 : 0;
  return true;
}

bool planContourLevels( const paleo::singlefactor::FieldContourSurface &surface,
                        const QVector<double> &levels, double interval,
                        std::vector<double> *useLevels, QString *error )
{
  if ( !levels.isEmpty() )
  {
    for ( const double level : levels )
    {
      if ( !std::isfinite( level ) )
      {
        setError( error, QStringLiteral( "等值级别必须为有限数值" ) );
        return false;
      }
      useLevels->push_back( level );
    }
    return true;
  }
  paleo::singlefactor::ContourLevelPlan plan;
  const bool ok = interval > 0.0
                      ? paleo::singlefactor::intervalContourLevels(
                            surface.grid, surface.validMask, interval, &plan )
                      : paleo::singlefactor::autoContourLevels(
                            surface.grid, surface.validMask, &plan );
  if ( !ok && plan.tooManyLevels )
  {
    setError( error, QStringLiteral( "等值线间距过小：间距 %1 在值域 %2~%3 上将生成约 %4 条级别，"
                                     "超过上限 %5 条；请增大间距" )
                         .arg( interval, 0, 'g', 6 )
                         .arg( plan.valueMin, 0, 'g', 8 )
                         .arg( plan.valueMax, 0, 'g', 8 )
                         .arg( plan.estimatedLevels, 0, 'g', 6 )
                         .arg( static_cast<qulonglong>( paleo::singlefactor::kMaxContourLevels ) ) );
    return false;
  }
  if ( !ok )
  {
    setError( error, QStringLiteral( "趋势面没有可提线的有效像元" ) );
    return false;
  }
  *useLevels = plan.levels;
  if ( useLevels->empty() )
  {
    setError( error, QStringLiteral( "等值级别为空" ) );
    return false;
  }
  return true;
}

bool writeFieldContourGpkg( const QString &outputGpkg, OGRSpatialReferenceH srs,
                            const paleo::singlefactor::FieldContourSurface &surface,
                            const paleo::singlefactor::FieldContourResult &result,
                            QString *error )
{
  // 同 generateWithOption：临时文件 + 原子替换，失败不毁旧产物。
  const QString stagingGpkg = stagingPathFor( outputGpkg );
  QFile::remove( stagingGpkg );
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
  {
    setError( error, QStringLiteral( "GDAL GPKG driver unavailable" ) );
    OSRDestroySpatialReference( srs );
    return false;
  }
  GDALDatasetH ds = GDALCreate( drv, stagingGpkg.toUtf8().constData(), 0, 0, 0,
                                GDT_Unknown, nullptr );
  if ( !ds )
  {
    setError( error, QStringLiteral( "cannot create contour output '%1'" ).arg( outputGpkg ) );
    OSRDestroySpatialReference( srs );
    return false;
  }
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "contours", srs, wkbLineString, nullptr );
  if ( srs )
    OSRDestroySpatialReference( srs );
  if ( !layer )
  {
    setError( error, QStringLiteral( "cannot create layer 'contours' in '%1'" ).arg( outputGpkg ) );
    GDALClose( ds );
    QFile::remove( stagingGpkg );
    return false;
  }
  createFieldDef( layer, "level", OFTReal );
  createFieldDef( layer, "closed", OFTInteger );
  createFieldDef( layer, "closure_type", OFTString );
  createFieldDef( layer, "value_source", OFTString );
  createFieldDef( layer, "geometry_policy", OFTString );
  createFieldDef( layer, "buffer_policy", OFTString );
  createFieldDef( layer, "buffer_half_width", OFTReal );
  createFieldDef( layer, "detour_applied", OFTInteger );

  const QByteArray valueSource = QString::fromStdString( surface.partition.geometryPolicy ).toUtf8();
  const QByteArray bufferPolicy = QString::fromStdString( surface.partition.bufferPolicy ).toUtf8();
  const double bufferHalfWidth = std::max( surface.barrierBufferDistance,
                                           surface.contourStopBufferDistance );

  bool wrote = true;
  QString writeErr;
  for ( const paleo::singlefactor::ContourLevelLines &entry : result.contours )
  {
    for ( const auto &line : entry.lines )
    {
      if ( line.size() < 2 )
        continue;
      OGRGeometryH geometry = OGR_G_CreateGeometry( wkbLineString );
      for ( const auto &pt : line )
        OGR_G_SetPoint_2D( geometry, OGR_G_GetPointCount( geometry ), pt.x, pt.y );
      const bool closed = line.size() > 2 && line.front().x == line.back().x &&
                          line.front().y == line.back().y;
      OGRFeatureH feature = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
      OGR_F_SetGeometryDirectly( feature, geometry );
      OGR_F_SetFieldDouble( feature, OGR_F_GetFieldIndex( feature, "level" ), entry.level );
      OGR_F_SetFieldInteger( feature, OGR_F_GetFieldIndex( feature, "closed" ),
                             closed ? 1 : 0 );
      OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "closure_type" ),
                            closed ? "interior" : "open" );
      OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "value_source" ),
                            valueSource.constData() );
      OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "geometry_policy" ),
                            valueSource.constData() );
      OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "buffer_policy" ),
                            bufferPolicy.constData() );
      OGR_F_SetFieldDouble( feature, OGR_F_GetFieldIndex( feature, "buffer_half_width" ),
                            bufferHalfWidth );
      OGR_F_SetFieldInteger( feature, OGR_F_GetFieldIndex( feature, "detour_applied" ),
                             result.detourApplied ? 1 : 0 );
      if ( OGR_L_CreateFeature( layer, feature ) != OGRERR_NONE )
      {
        wrote = false;
        writeErr = QStringLiteral( "等值线要素写入失败" );
      }
      OGR_F_Destroy( feature );
      if ( !wrote )
        break;
    }
    if ( !wrote )
      break;
  }
  GDALClose( ds );
  if ( !wrote )
  {
    setError( error, writeErr );
    QFile::remove( stagingGpkg );
    return false;
  }
  return commitStagingGpkg( stagingGpkg, outputGpkg, error );
}
} // namespace

bool generateStructuralContours( const QString &rasterPath,
                                 const QString &structuralJsonPath,
                                 const QString &outputGpkg,
                                 const QVector<double> &levels,
                                 double interval,
                                 QVector<double> *levelsUsed,
                                 QString *error,
                                 const std::function<bool()> &cancelled,
                                 bool smooth )
{
  if ( rasterPath.isEmpty() || !QFile::exists( rasterPath ) )
  {
    setError( error, QStringLiteral( "contour input raster not found: %1" ).arg( rasterPath ) );
    return false;
  }
  if ( structuralJsonPath.isEmpty() || !QFile::exists( structuralJsonPath ) )
  {
    setError( error, QStringLiteral( "结构面侧卡不存在：%1" ).arg( structuralJsonPath ) );
    return false;
  }
  if ( outputGpkg.isEmpty() )
  {
    setError( error, QStringLiteral( "contour output path is empty" ) );
    return false;
  }
  // D8：阶段边界轮询——栅格读取（GDALRasterIO 单次调用）无法打断，读取完成
  // 后立即给取消一个退出点，不再等到整条提线链结束才丢弃产物。
  if ( cancelled && cancelled() )
  {
    setError( error, QStringLiteral( "已取消" ) );
    return false;
  }

  GDALAllRegister();
  std::vector<double> grid;
  int nx = 0, ny = 0;
  OGRSpatialReferenceH srs = nullptr;
  if ( !readStructuralRaster( rasterPath, &grid, &nx, &ny, &srs, error ) )
    return false;
  if ( cancelled && cancelled() )
  {
    setError( error, QStringLiteral( "已取消" ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }

  QFile file( structuralJsonPath );
  if ( !file.open( QIODevice::ReadOnly ) )
  {
    setError( error, QStringLiteral( "无法读取 %1" ).arg( structuralJsonPath ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson( file.readAll(), &parseError );
  if ( parseError.error != QJsonParseError::NoError || !doc.isObject() )
  {
    setError( error, QStringLiteral( "不是 JSON 对象：%1" ).arg( structuralJsonPath ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }

  paleo::singlefactor::FieldContourSurface surface;
  QString loadErr;
  if ( !paleo::singlefactor::loadFieldContourSurface( doc.object().toVariantMap(), &surface,
                                                      &loadErr ) )
  {
    setError( error, loadErr );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }
  if ( static_cast<int>( surface.xs.size() ) != nx ||
       static_cast<int>( surface.ys.size() ) != ny )
  {
    setError( error, QStringLiteral( "结构面侧卡与栅格尺寸不一致：%1" ).arg( structuralJsonPath ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }
  surface.grid = std::move( grid );

  // 级别：显式列表 > 用户间距 > 自动（上游 ContourExtractDialog 规则）。
  std::vector<double> useLevels;
  if ( !levels.isEmpty() )
  {
    for ( const double level : levels )
    {
      if ( !std::isfinite( level ) )
      {
        setError( error, QStringLiteral( "等值级别必须为有限数值" ) );
        OSRDestroySpatialReference( srs );
        return false;
      }
      useLevels.push_back( level );
    }
  }
  else
  {
    paleo::singlefactor::ContourLevelPlan plan;
    const bool ok = interval > 0.0
                        ? paleo::singlefactor::intervalContourLevels(
                              surface.grid, surface.validMask, interval, &plan )
                        : paleo::singlefactor::autoContourLevels(
                              surface.grid, surface.validMask, &plan );
    if ( !ok && plan.tooManyLevels )
    {
      setError( error, QStringLiteral( "等值线间距过小：间距 %1 在值域 %2~%3 上将生成约 %4 条级别，"
                                       "超过上限 %5 条；请增大间距" )
                           .arg( interval, 0, 'g', 6 )
                           .arg( plan.valueMin, 0, 'g', 8 )
                           .arg( plan.valueMax, 0, 'g', 8 )
                           .arg( plan.estimatedLevels, 0, 'g', 6 )
                           .arg( static_cast<qulonglong>( paleo::singlefactor::kMaxContourLevels ) ) );
      OSRDestroySpatialReference( srs );
      return false;
    }
    if ( !ok )
    {
      setError( error, QStringLiteral( "趋势面没有可提线的有效像元" ) );
      OSRDestroySpatialReference( srs );
      return false;
    }
    useLevels = plan.levels;
  }
  if ( useLevels.empty() )
  {
    setError( error, QStringLiteral( "等值级别为空" ) );
    OSRDestroySpatialReference( srs );
    return false;
  }
  // D8：提线（extractFieldContours 内部无检查点）前最后退出点。
  if ( cancelled && cancelled() )
  {
    setError( error, QStringLiteral( "已取消" ) );
    OSRDestroySpatialReference( srs );
    return false;
  }
  if ( levelsUsed )
  {
    levelsUsed->clear();
    for ( const double level : useLevels )
      levelsUsed->push_back( level );
  }

  const paleo::singlefactor::FieldContourResult result =
      paleo::singlefactor::extractFieldContours( surface, useLevels, smooth );
  if ( result.status != paleo::singlefactor::Status::Ok )
  {
    setError( error, QString::fromStdString( result.message ) );
    OSRDestroySpatialReference( srs );
    return false;
  }
  return writeFieldContourGpkg( outputGpkg, srs, surface, result, error );
}

bool generateAnalysisFieldContours( const QString &rasterPath, const QString &outputGpkg,
                                    const QVector<double> &levels, double interval,
                                    QVector<double> *levelsUsed, QString *error,
                                    const std::function<bool()> &cancelled, bool smooth )
{
  if ( rasterPath.isEmpty() || !QFile::exists( rasterPath ) )
  {
    setError( error, QStringLiteral( "contour input raster not found: %1" ).arg( rasterPath ) );
    return false;
  }
  if ( outputGpkg.isEmpty() )
  {
    setError( error, QStringLiteral( "contour output path is empty" ) );
    return false;
  }
  if ( cancelled && cancelled() )
  {
    setError( error, QStringLiteral( "已取消" ) );
    return false;
  }

  std::vector<double> grid, xs, ys;
  std::vector<std::uint8_t> validMask;
  int nx = 0, ny = 0;
  OGRSpatialReferenceH srs = nullptr;
  if ( !readAnalysisField( rasterPath, &grid, &xs, &ys, &validMask, &nx, &ny, &srs, error ) )
    return false;
  if ( cancelled && cancelled() )
  {
    setError( error, QStringLiteral( "已取消" ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }

  paleo::singlefactor::FieldContourSurface surface;
  surface.xs = std::move( xs );
  surface.ys = std::move( ys );
  surface.grid = std::move( grid );
  surface.validMask = std::move( validMask );
  surface.partition.geometryPolicy = "field_only";

  std::vector<double> useLevels;
  QString levelErr;
  if ( !planContourLevels( surface, levels, interval, &useLevels, &levelErr ) )
  {
    setError( error, levelErr );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }
  if ( cancelled && cancelled() )
  {
    setError( error, QStringLiteral( "已取消" ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }
  if ( levelsUsed )
  {
    levelsUsed->clear();
    for ( const double level : useLevels )
      levelsUsed->push_back( level );
  }
  const paleo::singlefactor::FieldContourResult result =
      paleo::singlefactor::extractFieldContours( surface, useLevels, smooth );
  if ( result.status != paleo::singlefactor::Status::Ok )
  {
    setError( error, QString::fromStdString( result.message ) );
    if ( srs )
      OSRDestroySpatialReference( srs );
    return false;
  }
  return writeFieldContourGpkg( outputGpkg, srs, surface, result, error );
}

void requestContours(const QString &rasterPath, const QString &outputGpkg,
                     double interval, PaleoTaskService *tasks,
                     QObject *receiver,
                     std::function<void(bool, const QString &)> ready,
                     std::shared_ptr<void> lifetime) {
  if (!tasks) {
    QString error;
    const bool ok = generateContours(rasterPath, outputGpkg, interval, &error);
    ready(ok, error);
    return;
  }
  auto *task = tasks->start(
      QObject::tr("提取预览等值线"),
      [rasterPath, outputGpkg, interval, lifetime](PaleoTask *running) {
        QString error;
        const bool ok = generateContours(
            rasterPath, outputGpkg, interval, &error,
            [running] { return running->cancelRequested(); },
            [running](double p) { running->reportStage("build", qRound(p)); });
        // 不删正式输出：等值线走「临时文件 + 成功后原子替换」，失败/取消时
        // 正式产物保持调用前状态（不存在或仍是上一版）。这里再 remove 会把
        // 上一版好产物一起删掉，用户拿到错误却两边都没有输出（#588 B.13）。
        return ok ? QString() : error;
      },
      {}, true);
  QObject::connect(receiver, &QObject::destroyed, task,
                   &PaleoTask::requestCancel);
  QObject::connect(
      task, &PaleoTask::finished, receiver, [task, ready, lifetime] {
        if (task->state() == PaleoTask::State::Cancelled)
          return;
        ready(task->state() == PaleoTask::State::Succeeded, task->errorText());
      });
}
} // namespace FactorContourService
