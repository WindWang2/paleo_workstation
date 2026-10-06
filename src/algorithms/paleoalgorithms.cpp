// 层：数据
#include "paleoalgorithms.h"
#include "rasterout.h"
#include "singlefactor/localdirectionalgorithm.h"
#include "singlefactor/structuralalgorithm.h"
#include "domain/singlefactorrequest.h"

#include <qgsprocessingparameters.h>
#include <qgsprocessingutils.h> // QgsProcessingFeatureSource
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsexception.h>
#include <qgsrasterlayer.h>
#include <qgswkbtypes.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_error.h>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers> // std::numbers::pi——M_PI 在 MSVC <cmath> 下不定义
#include <vector>

// ---------------------------------------------------------------------------
// Shared helpers (file-local)
// ---------------------------------------------------------------------------

namespace
{

constexpr float PALEO_NODATA = -9999.0f;

// Open a GDAL dataset from a QgsRasterLayer, throwing a Processing error on failure.
GDALDatasetH openRaster( const QgsMapLayer *layer, GDALAccess access = GA_ReadOnly )
{
  if ( !layer )
    return nullptr;
  const QString src = layer->source();
  GDALDatasetH ds = GDALOpen( src.toUtf8().constData(), access );
  return ds;
}


// Grid description pulled from an open GDAL dataset.
struct GridInfo
{
  int cols = 0;
  int rows = 0;
  double gt[6] = {0, 0, 0, 0, 0, 0};
};

GridInfo gridOf( GDALDatasetH ds )
{
  GridInfo g;
  g.cols = GDALGetRasterXSize( ds );
  g.rows = GDALGetRasterYSize( ds );
  GDALGetGeoTransform( ds, g.gt );
  return g;
}

bool sameGrid( const GridInfo &a, const GridInfo &b )
{
  if ( a.cols != b.cols || a.rows != b.rows )
    return false;
  // Tolerance: a small fraction of the cell size absorbs FP noise in GeoTransforms.
  const double tol = std::max( { std::fabs( a.gt[1] ), std::fabs( a.gt[5] ), 1.0 } ) * 1e-6;
  for ( int i = 0; i < 6; ++i )
  {
    if ( std::fabs( a.gt[i] - b.gt[i] ) > tol )
      return false;
  }
  return true;
}

// Is v a "no data" cell for this band? Honors the band nodata flag plus NaN.
inline bool isNoData( float v, bool hasNodata, double nodata )
{
  // #165：Float32 读入的像元须与 float 化的 nodata 比较——1e30/-99999.9 等
  // 非 float 可表示的 nodata 落盘后是 (float)nodata，与 double 永不相等。
  return std::isnan( v ) || ( hasNodata && v == static_cast<float>( nodata ) );
}

void bandNodata( GDALRasterBandH band, bool &hasNodata, double &nodata )
{
  int flag = 0;
  nodata = GDALGetRasterNoDataValue( band, &flag );
  hasNodata = flag != 0;
}

// 单波段 GeoTIFF。GeoTransform 与 CRS 只在这里写，float / byte 共用。
// canonicalCrsWkt 非空且与 crs 同坐标系时按原串写出（EDATUM 保真，见
// rasterout.h ARCH-05 注）。
GDALDatasetH createGTiff( const QString &outPath, int nCols, int nRows, GDALDataType type,
                          const double geoTransform[6], const QgsCoordinateReferenceSystem &crs,
                          const QString &canonicalCrsWkt )
{
  GDALAllRegister(); // idempotent — safe under an already-initialized QGIS runtime
  GDALDriverH drv = GDALGetDriverByName( "GTiff" );
  if ( !drv )
    return nullptr;
  GDALDatasetH ds = GDALCreate( drv, outPath.toUtf8().constData(), nCols, nRows, 1, type, nullptr );
  if ( !ds )
    return nullptr;
  if ( GDALSetGeoTransform( ds, const_cast<double *>( geoTransform ) ) != CE_None )
  {
    GDALClose( ds );
    return nullptr;
  }
  if ( crs.isValid() )
  {
    // QGIS's engineering CRS exporter can omit EDATUM; GeoTIFF then loses
    // the local datum and no longer compares equal to the project grid.
    // The canonical WKT now arrives as a parameter (ARCH-05) — algorithms
    // no longer consult the catalog for it.
    QString wkt;
    if ( !canonicalCrsWkt.isEmpty() )
    {
      const auto canonical = QgsCoordinateReferenceSystem::fromWkt( canonicalCrsWkt );
      if ( crs == canonical || crs.toWkt() == canonical.toWkt() )
        wkt = canonicalCrsWkt;
    }
    if ( wkt.isEmpty() )
      wkt = crs.toWkt( Qgis::CrsWktVariant::PreferredGdal );
    const QByteArray wktUtf8 = wkt.toUtf8();
    GDALSetProjection( ds, wktUtf8.constData() );
    GDALSetMetadataItem( ds, "PALEO_CRS_WKT", wktUtf8.constData(), nullptr );
  }
  else if ( !canonicalCrsWkt.isEmpty() )
  {
    // BIZ-11（方向58）：输入无 CRS（无效 crs）但调用方给了规范局部网格 WKT
    //（workflow 恒注入 LOCAL_GRID_WKT）——工程内一切栅格都在该网格上，按规范
    // 串写出，不再产出无投影 GeoTIFF。无规范覆盖的直调（测试/外部脚本）
    // 仍保持旧行为（不编造 CRS），见 rasterout.h 残余面说明。
    const QByteArray wktUtf8 = canonicalCrsWkt.toUtf8();
    GDALSetProjection( ds, wktUtf8.constData() );
    GDALSetMetadataItem( ds, "PALEO_CRS_WKT", wktUtf8.constData(), nullptr );
  }
  return ds;
}

} // namespace

// 唯一实现（声明与契约见 rasterout.h）。
GDALDatasetH PaleoRasterOut::createFloatRaster( const QString &outPath, int nCols, int nRows,
                                const double geoTransform[6],
                                const QgsCoordinateReferenceSystem &crs,
                                double nodata,
                                const QString &canonicalCrsWkt )
{
  GDALDatasetH ds = createGTiff( outPath, nCols, nRows, GDT_Float32, geoTransform, crs, canonicalCrsWkt );
  if ( !ds )
    return nullptr;
  GDALSetRasterNoDataValue( GDALGetRasterBand( ds, 1 ), nodata );
  return ds;
}

GDALDatasetH PaleoRasterOut::createByteRaster( const QString &outPath, int nCols, int nRows,
                                               const double geoTransform[6],
                                               const QgsCoordinateReferenceSystem &crs,
                                               const QString &canonicalCrsWkt )
{
  return createGTiff( outPath, nCols, nRows, GDT_Byte, geoTransform, crs, canonicalCrsWkt );
}

GDALDatasetH PaleoRasterOut::createDoubleRaster( const QString &outPath, int nCols, int nRows,
                                                 const double geoTransform[6],
                                                 const QgsCoordinateReferenceSystem &crs,
                                                 double nodata,
                                                 const QString &canonicalCrsWkt )
{
  GDALDatasetH ds = createGTiff( outPath, nCols, nRows, GDT_Float64, geoTransform, crs, canonicalCrsWkt );
  if ( !ds )
    return nullptr;
  GDALSetRasterNoDataValue( GDALGetRasterBand( ds, 1 ), nodata );
  return ds;
}

// ---------------------------------------------------------------------------
// PaleoAlgoGuards — CELL_SIZE × extent 的公共网格守卫（三引擎同口径）
// ---------------------------------------------------------------------------

namespace PaleoAlgoGuards
{

GridDims gridDimsForExtent( const QgsRectangle &extent, double cellSize )
{
  // 同 horizonbinner 的上限：1 亿像元（≈400MB float 带 + 守卫路径整网格
  // 标注数组）。维度各自另守 INT_MAX，防 double→int 截断 UB。
  constexpr double kMaxDim = 2147483647.0;
  constexpr qint64 kMaxCellCount = 100'000'000;
  const double colsD = std::ceil( extent.width() / cellSize );
  const double rowsD = std::ceil( extent.height() / cellSize );
  if ( !( colsD >= 1.0 ) || !( rowsD >= 1.0 ) || colsD > kMaxDim || rowsD > kMaxDim )
    throw QgsProcessingException( QStringLiteral(
        "CELL_SIZE (%1) is inconsistent with the input extent (%2 x %3)" )
        .arg( cellSize )
        .arg( extent.width() )
        .arg( extent.height() ) );
  const qint64 cells = static_cast<qint64>( colsD ) * static_cast<qint64>( rowsD );
  if ( cells > kMaxCellCount )
    throw QgsProcessingException( QStringLiteral(
        "Requested output grid %1x%2 (%3 cells) exceeds the %4-cell budget — "
        "increase CELL_SIZE or reduce the input extent" )
        .arg( static_cast<qint64>( colsD ) )
        .arg( static_cast<qint64>( rowsD ) )
        .arg( cells )
        .arg( kMaxCellCount ) );
  return { static_cast<int>( colsD ), static_cast<int>( rowsD ) };
}

QString geographicCrsWarning( const QString &crsIdentifier )
{
  return QStringLiteral(
      "Input CRS '%1' is geographic: CELL_SIZE and output distances are in "
      "degrees, not meters. Reproject the input layer to a projected CRS for "
      "metric distances." ).arg( crsIdentifier );
}

} // namespace PaleoAlgoGuards

// ---------------------------------------------------------------------------
// ConstraintIDWAlgorithm
// ---------------------------------------------------------------------------

QString ConstraintIDWAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Inverse-distance-weighted (power = 2) interpolation of a numeric z-field on a "
    "point layer. The output grid covers the input extent grown by 10%% on every side, "
    "using CELL_SIZE cells. CONSTRAINTS line features are classified by their 'type' "
    "attribute (the ConstraintStore vocabulary): 'break_line' features are hard "
    "barriers — they contribute to the region-of-interest clip AND block interpolation "
    "(cells only see samples reachable without crossing a barrier; barrier cells "
    "themselves become nodata; reachability is evaluated on the output grid, so "
    "detours around barrier ends are honored at cell resolution). 'direction_line' "
    "features supply a length-weighted mean direction field: with ANISO_RATIO r, "
    "distances are stretched to d2 = u^2/r^2 + v^2*r^2 (u along, v across the field), "
    "elongating the interpolated surface along the supply direction. Features with "
    "any other or missing type keep the legacy behavior: they only clip output to "
    "the convex hull of the constraint geometry. FACIES_CODE is recorded as raster "
    "metadata (PALEO_FACIES_CODE), barrier/direction counts as PALEO_BREAK_LINES / "
    "PALEO_DIRECTION_LINES, and the resolved field as PALEO_ANISO_RATIO / "
    "PALEO_ANISO_ANGLE_DEG." );
}

void ConstraintIDWAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "INPUT" ), QStringLiteral( "Input point layer" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorPoint ) ) );
  addParameter( new QgsProcessingParameterField(
      QStringLiteral( "FIELD" ), QStringLiteral( "Z value field" ), QVariant(),
      QStringLiteral( "INPUT" ), Qgis::ProcessingFieldParameterDataType::Numeric ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "Constraint lines (optional)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorLine ),
      QVariant(), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "FACIES_CODE" ), QStringLiteral( "Facies code (metadata)" ),
      Qgis::ProcessingNumberParameterType::Integer, QVariant(), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "Cell size (map units)" ),
      Qgis::ProcessingNumberParameterType::Double, 1.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "ANISO_RATIO" ),
      QStringLiteral( "Anisotropy ratio along direction lines (>= 1, only used when "
                      "direction_line constraints exist)" ),
      Qgis::ProcessingNumberParameterType::Double, 2.0, true, 0.0 ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Interpolated raster" ) ) );
}

QVariantMap ConstraintIDWAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                    QgsProcessingContext &context,
                                                    QgsProcessingFeedback *feedback )
{
  std::unique_ptr<QgsProcessingFeatureSource> source(
      parameterAsSource( parameters, QStringLiteral( "INPUT" ), context ) );
  if ( !source )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "INPUT" ) ) );

  const QString fieldName = parameterAsString( parameters, QStringLiteral( "FIELD" ), context );
  const int fieldIdx = source->fields().lookupField( fieldName );
  if ( fieldIdx < 0 )
    throw QgsProcessingException(
        QStringLiteral( "Z field '%1' not found on INPUT layer" ).arg( fieldName ) );

  const double cellSize = parameterAsDouble( parameters, QStringLiteral( "CELL_SIZE" ), context );
  if ( cellSize <= 0.0 || !std::isfinite( cellSize ) )
    throw QgsProcessingException( QStringLiteral( "CELL_SIZE must be > 0" ) );
  if ( source->sourceCrs().isGeographic() && feedback )
    feedback->pushWarning( PaleoAlgoGuards::geographicCrsWarning(
        source->sourceCrs().userFriendlyIdentifier() ) );
  // ANISO_RATIO only takes effect when direction_line constraints exist; values
  // below 1.0 are clamped to 1.0 (isotropic) — a sub-unit ratio is the same
  // field with the direction rotated by 90°, which direction_line already
  // expresses directly.
  double anisoRatio = parameterAsDouble( parameters, QStringLiteral( "ANISO_RATIO" ), context );
  if ( !std::isfinite( anisoRatio ) || anisoRatio < 1.0 )
    anisoRatio = 1.0;
  // 上界：超过 ~1000:1 后各向异性距离退化（离轴权重塌成 0、轴向权重溢出
  // 成 inf → NaN 格）——按非法参数拒绝，不静默产出全 nodata/NaN 面。
  constexpr double kAnisoRatioMax = 1000.0;
  if ( anisoRatio > kAnisoRatioMax )
    throw QgsProcessingException( QStringLiteral(
        "ANISO_RATIO must be <= %1 (got %2)" ).arg( kAnisoRatioMax ).arg( anisoRatio ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // ---- gather sample points -------------------------------------------------
  struct Sample
  {
    double x, y, z;
  };
  std::vector<Sample> samples;
  {
    QgsFeatureIterator it = source->getFeatures( QgsFeatureRequest() );
    QgsFeature f;
    while ( it.nextFeature( f ) )
    {
      if ( feedback && feedback->isCanceled() )
        throw QgsProcessingException( QStringLiteral( "Canceled" ) );
      if ( !f.hasGeometry() || f.geometry().isEmpty() )
        continue;
      const QgsGeometry g = f.geometry();
      const QgsPointXY p = g.isMultipart() ? g.asMultiPoint().value( 0 ) : g.asPoint();
      const QVariant zv = f.attribute( fieldIdx );
      bool ok = false;
      const double z = zv.toDouble( &ok );
      if ( !ok || !std::isfinite( z ) )
        continue; // skip non-numeric and NaN/Inf rather than poison the grid
      samples.push_back( { p.x(), p.y(), z } );
    }
  }
  if ( samples.empty() )
    throw QgsProcessingException( QStringLiteral( "INPUT contains no usable point features" ) );

  // ---- constraints: typed semantics (C1) ------------------------------------
  // The ConstraintStore 'type' column classifies each constraint line:
  //   'break_line'     hard barrier — contributes to the ROI clip AND blocks
  //                    interpolation (see the connectivity pass below);
  //   'direction_line' anisotropy direction field — no clip, no barrier;
  //   other/missing    legacy §11 behavior — convex-hull ROI clip only.
  // Unknown-type layers therefore keep bit-identical behavior with the
  // pre-C1 algorithm.
  QVector<QgsGeometry> breakGeoms, directionGeoms, hullGeoms;
  {
    std::unique_ptr<QgsProcessingFeatureSource> constraints(
        parameterAsSource( parameters, QStringLiteral( "CONSTRAINTS" ), context ) );
    if ( constraints )
    {
      const QgsCoordinateReferenceSystem from = constraints->sourceCrs();
      const QgsCoordinateReferenceSystem to = source->sourceCrs();
      std::unique_ptr<QgsCoordinateTransform> xform;
      if ( from.isValid() && to.isValid() && from != to )
      {
        try
        {
          xform = std::make_unique<QgsCoordinateTransform>( from, to, context.transformContext() );
        }
        catch ( const QgsCsException &e )
        {
          throw QgsProcessingException(
              QStringLiteral( "Cannot transform constraints into the well CRS: %1" ).arg( e.what() ) );
        }
        // BIZ-10（方向58）：显式前置 transform 有效性——无大地基准的工程 CRS
        //（LOCAL_GRID_WKT ENGCRS）与大地/投影 CRS 之间 PROJ 造不出坐标操作
        //（projinfo：Candidate operations found: 0）。如实报因，不依赖 QGIS
        // 对无效 transform 的内部处理（直通或抛 QgsCsException）。
        if ( !xform->isValid() )
          throw QgsProcessingException(
              QStringLiteral( "Cannot transform constraints into the %1 CRS: no coordinate operation "
                              "between %2 and %3 (engineering CRS without geodetic datum?)" )
                  .arg( QStringLiteral( "well" ), from.userFriendlyIdentifier(),
                        to.userFriendlyIdentifier() ) );
      }
      const int typeIdx = constraints->fields().lookupField( QStringLiteral( "type" ) );
      QgsFeatureIterator cit = constraints->getFeatures( QgsFeatureRequest() );
      QgsFeature cf;
      while ( cit.nextFeature( cf ) )
      {
        if ( !cf.hasGeometry() || cf.geometry().isEmpty() )
          continue;
        QgsGeometry g = cf.geometry();
        if ( xform )
        {
          Qgis::GeometryOperationResult tr = Qgis::GeometryOperationResult::Success;
          try
          {
            tr = g.transform( *xform );
          }
          catch ( const QgsCsException & )
          {
            tr = Qgis::GeometryOperationResult::NothingHappened; // BIZ-10：逐要素变换异常归入同一报因
          }
          if ( tr != Qgis::GeometryOperationResult::Success )
            throw QgsProcessingException(
                QStringLiteral( "Constraint geometry failed to transform into the well CRS" ) );
        }
        const QString ctype = typeIdx >= 0 ? cf.attribute( typeIdx ).toString().trimmed() : QString();
        switch ( paleo::singlefactor::legacyConstraintRole( ctype ) )
        {
          case paleo::singlefactor::LegacyConstraintRole::HardBarrier:
            breakGeoms.append( g );
            break;
          case paleo::singlefactor::LegacyConstraintRole::DirectionGuide:
            directionGeoms.append( g );
            break;
          case paleo::singlefactor::LegacyConstraintRole::NotInLegacyEngine:
            // interpretive_boundary / contour_stop / cartographic_detour 不参与旧 IDW。
            break;
          case paleo::singlefactor::LegacyConstraintRole::HullClip:
            hullGeoms.append( g );
            break;
        }
      }
      // break_lines also bound the region of influence (a barrier is a boundary).
      hullGeoms += breakGeoms;
    }
  }
  QgsGeometry hull;
  if ( hullGeoms.size() == 1 )
    hull = hullGeoms.at( 0 ).convexHull();
  else if ( hullGeoms.size() > 1 )
    hull = QgsGeometry::unaryUnion( hullGeoms ).convexHull();
  const bool clipToHull = !hull.isNull() && !hull.isEmpty() &&
                          QgsWkbTypes::geometryType( hull.wkbType() ) == Qgis::GeometryType::Polygon;

  // ---- output grid ----------------------------------------------------------
  // 10% margin on every side. A zero-width axis (collinear or single wells)
  // still gets one cell of padding so the wells land inside the grid instead
  // of collapsing the whole extent.
  const QgsRectangle raw = source->sourceExtent();
  const double xPad = raw.width() > 0.0 ? raw.width() * 0.1 : cellSize;
  const double yPad = raw.height() > 0.0 ? raw.height() * 0.1 : cellSize;
  QgsRectangle extent( raw.xMinimum() - xPad, raw.yMinimum() - yPad,
                       raw.xMaximum() + xPad, raw.yMaximum() + yPad );
  const PaleoAlgoGuards::GridDims dims =
      PaleoAlgoGuards::gridDimsForExtent( extent, cellSize );
  const int nCols = dims.cols;
  const int nRows = dims.rows;

  const double gt[6] = { extent.xMinimum(), cellSize, 0.0,
                         extent.yMaximum(), 0.0, -cellSize };
  GDALDatasetH outDs = PaleoRasterOut::createFloatRaster( outPath, nCols, nRows, gt,
                                          source->sourceCrs(), PALEO_NODATA,
                                          PaleoRasterOut::canonicalWktFromParameters( parameters ) );
  if ( !outDs )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );

  // Record facies code as dataset metadata when the caller supplied one.
  const QVariant faciesRaw = parameters.value( QStringLiteral( "FACIES_CODE" ) );
  if ( faciesRaw.isValid() && !faciesRaw.isNull() )
  {
    const int faciesCode = parameterAsInt( parameters, QStringLiteral( "FACIES_CODE" ), context );
    const QByteArray v = QString::number( faciesCode ).toUtf8();
    GDALSetMetadataItem( outDs, "PALEO_FACIES_CODE", v.constData(), nullptr );
  }

  // ---- direction_line field: length-weighted mean direction (C1) -----------
  // Each segment votes with its length at doubled angle (a line has no
  // forward/backward distinction), giving the mean axis θ mod π.
  bool hasAniso = false;
  double cosTheta = 1.0, sinTheta = 0.0;
  if ( !directionGeoms.isEmpty() && anisoRatio > 1.0 )
  {
    double sumCos = 0.0, sumSin = 0.0, totalLen = 0.0;
    for ( const QgsGeometry &g : directionGeoms )
    {
      const QgsMultiPolylineXY mpl = g.isMultipart()
                                         ? g.asMultiPolyline()
                                         : QgsMultiPolylineXY{ g.asPolyline() };
      for ( const QgsPolylineXY &pl : mpl )
        for ( int i = 1; i < pl.size(); ++i )
        {
          const double dx = pl[i].x() - pl[i - 1].x();
          const double dy = pl[i].y() - pl[i - 1].y();
          const double len = std::hypot( dx, dy );
          if ( len <= 0.0 )
            continue;
          const double a2 = 2.0 * std::atan2( dy, dx );
          sumCos += std::cos( a2 ) * len;
          sumSin += std::sin( a2 ) * len;
          totalLen += len;
        }
    }
    if ( totalLen > 0.0 )
    {
      const double theta = 0.5 * std::atan2( sumSin, sumCos );
      cosTheta = std::cos( theta );
      sinTheta = std::sin( theta );
      hasAniso = true;
    }
  }
  GDALSetMetadataItem( outDs, "PALEO_BREAK_LINES",
                       QString::number( breakGeoms.size() ).toUtf8().constData(), nullptr );
  GDALSetMetadataItem( outDs, "PALEO_DIRECTION_LINES",
                       QString::number( directionGeoms.size() ).toUtf8().constData(), nullptr );
  if ( hasAniso )
  {
    GDALSetMetadataItem( outDs, "PALEO_ANISO_RATIO",
                         QString::number( anisoRatio ).toUtf8().constData(), nullptr );
    GDALSetMetadataItem( outDs, "PALEO_ANISO_ANGLE_DEG",
                         QString::number( std::atan2( sinTheta, cosTheta ) * 180.0 /
                                          std::numbers::pi )
                             .toUtf8()
                             .constData(),
                         nullptr );
  }

  // ---- break_line barriers: grid reachability (C1) --------------------------
  // Barriers are rasterized onto the output grid (half-cell supersampled walk
  // keeps the wall closed against diagonal leaks); non-barrier cells are
  // labeled by 4-connectivity BFS. A cell only interpolates from samples in
  // its own component — detours around barrier ends are therefore honored at
  // cell resolution. Within one component the plain Euclidean distance is
  // used (a straight line may still cross a concave barrier; documented
  // approximation, weight semantics only).
  const bool hasBarriers = !breakGeoms.isEmpty();
  std::vector<int> comp;        // per cell: >=0 component id, -2 barrier
  std::vector<int> sampleComp;  // per sample: component id, -1 unreachable
  auto cellOf = [&]( double x, double y, int &c, int &r ) {
    c = static_cast<int>( std::floor( ( x - extent.xMinimum() ) / cellSize ) );
    r = static_cast<int>( std::floor( ( extent.yMaximum() - y ) / cellSize ) );
    return c >= 0 && c < nCols && r >= 0 && r < nRows;
  };
  if ( hasBarriers )
  {
    const qsizetype cellCount = static_cast<qsizetype>( nCols ) * nRows;
    comp.assign( static_cast<size_t>( cellCount ), -1 );
    for ( const QgsGeometry &g : breakGeoms )
    {
      const QgsMultiPolylineXY mpl = g.isMultipart()
                                         ? g.asMultiPolyline()
                                         : QgsMultiPolylineXY{ g.asPolyline() };
      for ( const QgsPolylineXY &pl : mpl )
        for ( int i = 1; i < pl.size(); ++i )
        {
          if ( feedback && feedback->isCanceled() )
          {
            GDALClose( outDs );
            throw QgsProcessingException( QStringLiteral( "Canceled" ) );
          }
          const QgsPointXY &a = pl[i - 1];
          const QgsPointXY &b = pl[i];
          const double len = a.distance( b );
          const int steps = std::max( 1, static_cast<int>( std::ceil( len / ( cellSize * 0.5 ) ) ) );
          for ( int s = 0; s <= steps; ++s )
          {
            int c = 0, r = 0;
            if ( cellOf( a.x() + ( b.x() - a.x() ) * s / steps,
                         a.y() + ( b.y() - a.y() ) * s / steps, c, r ) )
              comp[static_cast<qsizetype>( r ) * nCols + c] = -2;
          }
        }
    }
    int nextComp = 0;
    std::vector<qsizetype> queue;
    queue.reserve( static_cast<size_t>( cellCount ) );
    qsizetype bfsVisited = 0; // 节流：每 1024 格查一次取消，避免虚调用过密
    for ( qsizetype start = 0; start < cellCount; ++start )
    {
      if ( comp[static_cast<size_t>( start )] != -1 )
        continue;
      comp[static_cast<size_t>( start )] = nextComp;
      queue.clear();
      queue.push_back( start );
      while ( !queue.empty() )
      {
        if ( feedback && ( ++bfsVisited & 1023 ) == 0 )
        {
          if ( feedback->isCanceled() )
          {
            GDALClose( outDs );
            throw QgsProcessingException( QStringLiteral( "Canceled" ) );
          }
          feedback->setProgress( 100.0 * static_cast<double>( bfsVisited ) /
                                 static_cast<double>( cellCount ) );
        }
        const qsizetype cur = queue.back();
        queue.pop_back();
        const int cr = static_cast<int>( cur / nCols ), cc = static_cast<int>( cur % nCols );
        const int nr[4] = { cr - 1, cr + 1, cr, cr };
        const int nc[4] = { cc, cc, cc - 1, cc + 1 };
        for ( int k = 0; k < 4; ++k )
        {
          if ( nr[k] < 0 || nr[k] >= nRows || nc[k] < 0 || nc[k] >= nCols )
            continue;
          const qsizetype n = static_cast<qsizetype>( nr[k] ) * nCols + nc[k];
          if ( comp[static_cast<size_t>( n )] == -1 )
          {
            comp[static_cast<size_t>( n )] = nextComp;
            queue.push_back( n );
          }
        }
      }
      ++nextComp;
    }
    // Snap each sample to its component; a sample on a barrier cell takes a
    // 4-neighbor component (a well pinched onto the line still belongs to the
    // side it can be reached from); fully walled-in samples are unreachable.
    sampleComp.resize( samples.size(), -1 );
    for ( size_t si = 0; si < samples.size(); ++si )
    {
      int c = 0, r = 0;
      if ( !cellOf( samples[si].x, samples[si].y, c, r ) )
        continue; // outside the grid (margin math guarantees this does not happen)
      const auto cellComp = [&]( int cc, int rr ) {
        return ( cc < 0 || cc >= nCols || rr < 0 || rr >= nRows )
                   ? -3
                   : comp[static_cast<qsizetype>( rr ) * nCols + cc];
      };
      const int own = cellComp( c, r );
      if ( own >= 0 )
      {
        sampleComp[si] = own;
        continue;
      }
      const int around[4] = { cellComp( c - 1, r ), cellComp( c + 1, r ),
                              cellComp( c, r - 1 ), cellComp( c, r + 1 ) };
      for ( int k = 0; k < 4; ++k )
        if ( around[k] >= 0 )
        {
          sampleComp[si] = around[k];
          break;
        }
    }
  }

  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  // ---- plain IDW (power = 2) honoring barriers + anisotropy ------------------
  // O(rows * cols * points); adequate for P1-scale inputs. A spatial index /
  // neighbor cutoff is a documented optimization path for large point sets.
  const double invAniso2 = 1.0 / ( anisoRatio * anisoRatio );
  const double aniso2 = anisoRatio * anisoRatio;
  QVector<float> rowBuf( nCols );
  for ( int r = 0; r < nRows; ++r )
  {
    if ( feedback && feedback->isCanceled() )
    {
      GDALClose( outDs );
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    }
    const double y = extent.yMaximum() - ( r + 0.5 ) * cellSize;
    for ( int c = 0; c < nCols; ++c )
    {
      const double x = extent.xMinimum() + ( c + 0.5 ) * cellSize;
      const qsizetype cellIdx = static_cast<qsizetype>( r ) * nCols + c;
      if ( hasBarriers && comp[static_cast<size_t>( cellIdx )] < 0 )
      {
        rowBuf[c] = PALEO_NODATA; // barrier cell (or walled-in) — no interpolation
        continue;
      }
      if ( clipToHull && !hull.contains( x, y ) )
      {
        rowBuf[c] = PALEO_NODATA;
        continue;
      }
      double weightSum = 0.0, valueSum = 0.0;
      bool exact = false;
      double exactValue = 0.0;
      for ( size_t si = 0; si < samples.size(); ++si )
      {
        if ( hasBarriers && sampleComp[si] != comp[static_cast<size_t>( cellIdx )] )
          continue; // separated by a break_line barrier
        const Sample &s = samples[si];
        const double dx = s.x - x, dy = s.y - y;
        double d2 = dx * dx + dy * dy;
        if ( hasAniso )
        {
          // Stretch across / squeeze along the direction field axis:
          // u = along, v = across → d2 = u²/r² + v²·r² (r=1 isotropic).
          const double u = dx * cosTheta + dy * sinTheta;
          const double v = -dx * sinTheta + dy * cosTheta;
          d2 = u * u * invAniso2 + v * v * aniso2;
        }
        if ( d2 < 1e-12 )
        {
          exact = true;
          exactValue = s.z;
          break;
        }
        const double w = 1.0 / d2; // power = 2
        weightSum += w;
        valueSum += w * s.z;
      }
      // 非有限值（各向异性极端化/数值溢出产出的 inf/NaN）一律落 nodata——
      // 不把 NaN 写进栅格毒化下游（NaN 不是 nodata）。float 化后再查一次：
      // |z|>FLT_MAX 的有限 double 也会溢出成 ±inf。
      double cell = std::numeric_limits<double>::quiet_NaN();
      if ( exact )
        cell = exactValue;
      else if ( weightSum > 0.0 )
        cell = valueSum / weightSum;
      const float out = static_cast<float>( cell );
      rowBuf[c] = std::isfinite( out ) ? out : PALEO_NODATA;
    }
    if ( GDALRasterIO( outBand, GF_Write, 0, r, nCols, 1, rowBuf.data(),
                       nCols, 1, GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( outDs );
      throw QgsProcessingException( QStringLiteral( "GDAL write failed at row %1" ).arg( r ) );
    }
    if ( feedback )
      feedback->setProgress( 100.0 * static_cast<double>( r + 1 ) / nRows );
  }

  GDALClose( outDs );

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  return out;
}

// ---------------------------------------------------------------------------
// FaciesFusionAlgorithm
// ---------------------------------------------------------------------------

QString FaciesFusionAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Fuse N single-facies rasters (band 1) into one coded facies raster. INPUTS "
    "order is the priority order: for each cell the first input with a valid "
    "nonzero value wins; cells with no valid input become nodata. All inputs must "
    "share the grid (dimensions and geotransform) of the first input." );
}

void FaciesFusionAlgorithm::initAlgorithm( const QVariantMap & )
{
  auto *inputs = new QgsProcessingParameterMultipleLayers(
      QStringLiteral( "INPUTS" ), QStringLiteral( "Input facies rasters (priority order)" ),
      Qgis::ProcessingSourceType::Raster );
  inputs->setMinimumNumberInputs( 1 );
  addParameter( inputs );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Fused facies raster" ) ) );
}

QVariantMap FaciesFusionAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                   QgsProcessingContext &context,
                                                   QgsProcessingFeedback *feedback )
{
  const QList<QgsMapLayer *> layers = parameterAsLayerList( parameters, QStringLiteral( "INPUTS" ), context );
  if ( layers.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "INPUTS requires at least one raster layer" ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // Open all inputs via the GDAL C API (band 1 of each).
  struct Input
  {
    GDALDatasetH ds = nullptr;
    GDALRasterBandH band = nullptr;
    bool hasNodata = false;
    double nodata = 0;
    GridInfo grid;
    QVector<float> rowBuf;
  };
  std::vector<std::unique_ptr<Input>> inputs;
  inputs.reserve( static_cast<size_t>( layers.size() ) );
  auto closeAll = [&inputs] {
    for ( auto &in : inputs )
      if ( in->ds )
        GDALClose( in->ds );
  };

  GridInfo ref;
  for ( int i = 0; i < layers.size(); ++i )
  {
    QgsRasterLayer *rl = qobject_cast<QgsRasterLayer *>( layers.at( i ) );
    if ( !rl )
    {
      closeAll();
      throw QgsProcessingException(
          QStringLiteral( "INPUTS layer %1 is not a raster layer" ).arg( i + 1 ) );
    }
    auto in = std::make_unique<Input>();
    in->ds = openRaster( rl );
    if ( !in->ds )
    {
      closeAll();
      throw QgsProcessingException(
          QStringLiteral( "GDAL cannot open input %1 (%2)" ).arg( i + 1 ).arg( rl->source() ) );
    }
    if ( GDALGetRasterCount( in->ds ) < 1 )
    {
      closeAll();
      throw QgsProcessingException(
          QStringLiteral( "Input %1 has no raster band" ).arg( i + 1 ) );
    }
    in->band = GDALGetRasterBand( in->ds, 1 );
    bandNodata( in->band, in->hasNodata, in->nodata );
    in->grid = gridOf( in->ds );
    if ( i == 0 )
      ref = in->grid;
    else if ( !sameGrid( ref, in->grid ) )
    {
      closeAll();
      throw QgsProcessingException(
          QStringLiteral( "Input %1 grid does not match the first input "
                          "(same extent and cell size required)" ).arg( i + 1 ) );
    }
    in->rowBuf.resize( ref.cols );
    inputs.push_back( std::move( in ) );
  }

  // Output grid = first input's grid. Nodata: inherit from first input when
  // defined, else use the paleo default.
  const double outNodata = inputs.front()->hasNodata ? inputs.front()->nodata
                                                     : static_cast<double>( PALEO_NODATA );
  GDALDatasetH outDs = PaleoRasterOut::createFloatRaster( outPath, ref.cols, ref.rows, ref.gt,
                                          layers.at( 0 )->crs(), outNodata,
                                          PaleoRasterOut::canonicalWktFromParameters( parameters ) );
  if ( !outDs )
  {
    closeAll();
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );
  }
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  QVector<float> outRow( ref.cols );
  const int nInputs = static_cast<int>( inputs.size() );
  for ( int r = 0; r < ref.rows; ++r )
  {
    if ( feedback && feedback->isCanceled() )
    {
      GDALClose( outDs );
      closeAll();
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    }
    for ( auto &in : inputs )
    {
      if ( GDALRasterIO( in->band, GF_Read, 0, r, ref.cols, 1, in->rowBuf.data(),
                         ref.cols, 1, GDT_Float32, 0, 0 ) != CE_None )
      {
        GDALClose( outDs );
        closeAll();
        throw QgsProcessingException( QStringLiteral( "GDAL read failed at row %1" ).arg( r ) );
      }
    }
    for ( int c = 0; c < ref.cols; ++c )
    {
      float v = static_cast<float>( outNodata );
      for ( int i = 0; i < nInputs; ++i ) // priority = parameter order
      {
        const float iv = inputs[i]->rowBuf[c];
        if ( !isNoData( iv, inputs[i]->hasNodata, inputs[i]->nodata ) && iv != 0.0f )
        {
          v = iv;
          break;
        }
      }
      outRow[c] = v;
    }
    if ( GDALRasterIO( outBand, GF_Write, 0, r, ref.cols, 1, outRow.data(),
                       ref.cols, 1, GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( outDs );
      closeAll();
      throw QgsProcessingException( QStringLiteral( "GDAL write failed at row %1" ).arg( r ) );
    }
    if ( feedback )
      feedback->setProgress( 100.0 * static_cast<double>( r + 1 ) / ref.rows );
  }

  closeAll();
  GDALClose( outDs );

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  return out;
}

// ---------------------------------------------------------------------------
// GeologicalSmoothingAlgorithm
// ---------------------------------------------------------------------------

QString GeologicalSmoothingAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Majority (mode) filter over a 3x3 window for coded rasters, applied PASSES "
    "times. Coded values are kept exact (no interpolation); nodata cells stay "
    "nodata and are excluded from neighbor counts; ties keep the cell's current "
    "value (ties between other codes resolve to the smaller code)." );
}

void GeologicalSmoothingAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterRasterLayer(
      QStringLiteral( "INPUT" ), QStringLiteral( "Input coded raster" ) ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "PASSES" ), QStringLiteral( "Filter passes" ),
      Qgis::ProcessingNumberParameterType::Integer, 1, false, 0.0 ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Smoothed raster" ) ) );
}

QVariantMap GeologicalSmoothingAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                            QgsProcessingContext &context,
                                                            QgsProcessingFeedback *feedback )
{
  QgsRasterLayer *rl = parameterAsRasterLayer( parameters, QStringLiteral( "INPUT" ), context );
  if ( !rl )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "INPUT" ) ) );

  const int passes = parameterAsInt( parameters, QStringLiteral( "PASSES" ), context );
  if ( passes < 0 )
    throw QgsProcessingException( QStringLiteral( "PASSES must be >= 0" ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  GDALDatasetH inDs = openRaster( rl );
  if ( !inDs )
    throw QgsProcessingException( QStringLiteral( "GDAL cannot open %1" ).arg( rl->source() ) );
  if ( GDALGetRasterCount( inDs ) < 1 )
  {
    GDALClose( inDs );
    throw QgsProcessingException( QStringLiteral( "INPUT raster has no band" ) );
  }
  GDALRasterBandH inBand = GDALGetRasterBand( inDs, 1 );
  const GridInfo grid = gridOf( inDs );
  bool hasNodata = false;
  double nodata = 0;
  bandNodata( inBand, hasNodata, nodata );
  const double outNodata = hasNodata ? nodata : static_cast<double>( PALEO_NODATA );

  const qsizetype cellCount = static_cast<qsizetype>( grid.cols ) * grid.rows;
  QVector<float> cur( cellCount ), nxt( cellCount );
  if ( GDALRasterIO( inBand, GF_Read, 0, 0, grid.cols, grid.rows, cur.data(),
                     grid.cols, grid.rows, GDT_Float32, 0, 0 ) != CE_None )
  {
    GDALClose( inDs );
    throw QgsProcessingException( QStringLiteral( "GDAL read of INPUT failed" ) );
  }
  GDALClose( inDs );

  // One pass = simultaneous mode filter (double buffer keeps passes independent).
  auto onePass = [&]( const QVector<float> &src, QVector<float> &dst ) {
    for ( int r = 0; r < grid.rows; ++r )
    {
      if ( feedback && feedback->isCanceled() )
        return false;
      for ( int c = 0; c < grid.cols; ++c )
      {
        const qsizetype idx = static_cast<qsizetype>( r ) * grid.cols + c;
        const float center = src[idx];
        if ( isNoData( center, hasNodata, nodata ) )
        {
          dst[idx] = center; // nodata preserved exactly
          continue;
        }
        // Mode over the 3x3 window including center, excluding nodata.
        // <=9 cells → tiny linear count; deterministic tie-break below.
        float values[9];
        int counts[9];
        int nDistinct = 0;
        for ( int dr = -1; dr <= 1; ++dr )
        {
          const int rr = r + dr;
          if ( rr < 0 || rr >= grid.rows )
            continue;
          for ( int dc = -1; dc <= 1; ++dc )
          {
            const int cc = c + dc;
            if ( cc < 0 || cc >= grid.cols )
              continue;
            const float v = src[static_cast<qsizetype>( rr ) * grid.cols + cc];
            if ( isNoData( v, hasNodata, nodata ) )
              continue;
            int k = 0;
            for ( ; k < nDistinct; ++k )
              if ( values[k] == v ) // exact compare — coded values are integers in Float32
                break;
            if ( k == nDistinct )
            {
              values[nDistinct] = v;
              counts[nDistinct] = 1;
              ++nDistinct;
            }
            else
              ++counts[k];
          }
        }
        // Winner = highest count; ties keep the center code; ties among other
        // codes resolve to the numerically smaller code for determinism.
        int best = -1;
        int centerIdx = -1;
        for ( int k = 0; k < nDistinct; ++k )
          if ( values[k] == center )
            centerIdx = k;
        for ( int k = 0; k < nDistinct; ++k )
        {
          if ( best < 0 || counts[k] > counts[best] ||
               ( counts[k] == counts[best] && values[k] < values[best] ) )
            best = k;
        }
        float result = values[best];
        if ( centerIdx >= 0 && counts[centerIdx] == counts[best] )
          result = center; // incumbent wins ties
        dst[idx] = result;
      }
    }
    return true;
  };

  for ( int p = 0; p < passes; ++p )
  {
    if ( !onePass( cur, nxt ) )
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    cur = nxt;
    if ( feedback )
      feedback->setProgress( 80.0 * static_cast<double>( p + 1 ) / passes );
  }

  GDALDatasetH outDs = PaleoRasterOut::createFloatRaster( outPath, grid.cols, grid.rows, grid.gt,
                                          rl->crs(), outNodata,
                                          PaleoRasterOut::canonicalWktFromParameters( parameters ) );
  if ( !outDs )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );
  if ( GDALRasterIO( outBand, GF_Write, 0, 0, grid.cols, grid.rows, cur.data(),
                     grid.cols, grid.rows, GDT_Float32, 0, 0 ) != CE_None )
  {
    GDALClose( outDs );
    throw QgsProcessingException( QStringLiteral( "GDAL write of OUTPUT failed" ) );
  }
  GDALClose( outDs );
  if ( feedback )
    feedback->setProgress( 100 );

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  return out;
}

// ---------------------------------------------------------------------------
// IsopachAlgorithm
// ---------------------------------------------------------------------------

QString IsopachAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Cell-wise thickness: OUTPUT = INPUT_TOP - INPUT_BASE over two structural "
    "surface rasters sharing the same grid (dimensions + geotransform). Cells "
    "where either input is nodata become nodata. With NEGATIVE_TO_NODATA the "
    "inverted-thickness cells (base above top) are masked to nodata instead of "
    "emitting negative thickness." );
}

void IsopachAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterRasterLayer(
      QStringLiteral( "INPUT_TOP" ), QStringLiteral( "Top structural surface" ) ) );
  addParameter( new QgsProcessingParameterRasterLayer(
      QStringLiteral( "INPUT_BASE" ), QStringLiteral( "Base structural surface" ) ) );
  addParameter( new QgsProcessingParameterBoolean(
      QStringLiteral( "NEGATIVE_TO_NODATA" ),
      QStringLiteral( "Mask inverted thickness (base above top) to nodata" ), false ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Thickness (isopach) raster" ) ) );
}

QVariantMap IsopachAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                QgsProcessingContext &context,
                                                QgsProcessingFeedback *feedback )
{
  QgsRasterLayer *topLayer = parameterAsRasterLayer( parameters, QStringLiteral( "INPUT_TOP" ), context );
  QgsRasterLayer *baseLayer = parameterAsRasterLayer( parameters, QStringLiteral( "INPUT_BASE" ), context );
  if ( !topLayer )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "INPUT_TOP" ) ) );
  if ( !baseLayer )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "INPUT_BASE" ) ) );

  const bool negToNodata = parameterAsBool( parameters, QStringLiteral( "NEGATIVE_TO_NODATA" ), context );
  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  GDALDatasetH topDs = openRaster( topLayer );
  GDALDatasetH baseDs = openRaster( baseLayer );
  auto closeInputs = [&] {
    if ( topDs ) GDALClose( topDs );
    if ( baseDs ) GDALClose( baseDs );
  };
  if ( !topDs || !baseDs )
  {
    closeInputs();
    throw QgsProcessingException( QStringLiteral( "GDAL cannot open one of the input rasters" ) );
  }
  if ( GDALGetRasterCount( topDs ) < 1 || GDALGetRasterCount( baseDs ) < 1 )
  {
    closeInputs();
    throw QgsProcessingException( QStringLiteral( "Input raster has no band" ) );
  }

  GDALRasterBandH topBand = GDALGetRasterBand( topDs, 1 );
  GDALRasterBandH baseBand = GDALGetRasterBand( baseDs, 1 );
  const GridInfo grid = gridOf( topDs );
  if ( !sameGrid( grid, gridOf( baseDs ) ) )
  {
    closeInputs();
    throw QgsProcessingException(
        QStringLiteral( "INPUT_BASE grid does not match INPUT_TOP "
                        "(same extent and cell size required)" ) );
  }
  bool topNodataOk = false, baseNodataOk = false;
  double topNodata = 0, baseNodata = 0;
  bandNodata( topBand, topNodataOk, topNodata );
  bandNodata( baseBand, baseNodataOk, baseNodata );

  const double outNodata = topNodataOk ? topNodata : static_cast<double>( PALEO_NODATA );
  GDALDatasetH outDs = PaleoRasterOut::createFloatRaster( outPath, grid.cols, grid.rows, grid.gt,
                                          topLayer->crs(), outNodata,
                                          PaleoRasterOut::canonicalWktFromParameters( parameters ) );
  if ( !outDs )
  {
    closeInputs();
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );
  }
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  QVector<float> topRow( grid.cols ), baseRow( grid.cols ), outRow( grid.cols );
  for ( int r = 0; r < grid.rows; ++r )
  {
    if ( feedback && feedback->isCanceled() )
    {
      GDALClose( outDs );
      closeInputs();
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    }
    if ( GDALRasterIO( topBand, GF_Read, 0, r, grid.cols, 1, topRow.data(),
                       grid.cols, 1, GDT_Float32, 0, 0 ) != CE_None ||
         GDALRasterIO( baseBand, GF_Read, 0, r, grid.cols, 1, baseRow.data(),
                       grid.cols, 1, GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( outDs );
      closeInputs();
      throw QgsProcessingException( QStringLiteral( "GDAL read failed at row %1" ).arg( r ) );
    }
    for ( int c = 0; c < grid.cols; ++c )
    {
      const float t = topRow[c], b = baseRow[c];
      if ( isNoData( t, topNodataOk, topNodata ) || isNoData( b, baseNodataOk, baseNodata ) )
      {
        outRow[c] = static_cast<float>( outNodata );
        continue;
      }
      const float thickness = t - b;
      outRow[c] = ( negToNodata && thickness < 0.0f )
                      ? static_cast<float>( outNodata )
                      : thickness;
    }
    if ( GDALRasterIO( outBand, GF_Write, 0, r, grid.cols, 1, outRow.data(),
                       grid.cols, 1, GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( outDs );
      closeInputs();
      throw QgsProcessingException( QStringLiteral( "GDAL write failed at row %1" ).arg( r ) );
    }
    if ( feedback )
      feedback->setProgress( 100.0 * static_cast<double>( r + 1 ) / grid.rows );
  }

  closeInputs();
  GDALClose( outDs );

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  return out;
}

// ---------------------------------------------------------------------------
// PaleoProvider
// ---------------------------------------------------------------------------

void PaleoProvider::loadAlgorithms()
{
  addAlgorithm( new PaleoWellDistanceAlgorithm() ); // welldist 核（welldist.cpp）
  addAlgorithm( new PaleoDistanceTransformAlgorithm() ); // welldist 绕障引擎（distancetransform.cpp，C5）
  addAlgorithm( new ConstraintIDWAlgorithm() );
  addAlgorithm( new LocalDirectionIdwAlgorithm() );
  addAlgorithm( new StructuralIdwAlgorithm() ); // 上游默认法 structural_idw（区域方向核）
  addAlgorithm( new SurferIdwAlgorithm() );
  addAlgorithm( new CartographicWorkAlgorithm() );
  addAlgorithm( new FaciesFusionAlgorithm() );
  addAlgorithm( new GeologicalSmoothingAlgorithm() );
  addAlgorithm( new IsopachAlgorithm() );
  addAlgorithm( new MinimumCurvatureAlgorithm() ); // 连续曲率张力样条网格化（mincurvature.cpp）
  addAlgorithm( new FaciesPolygonizeAlgorithm() );
}
