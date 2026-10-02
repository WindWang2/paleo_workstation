// 层：数据
#include "paleoalgorithms.h"
#include "rasterout.h"
#include "gridsolver.h"
#include "../catalog/datacatalog.h"

#include <qgsprocessingparameters.h>
#include <qgsprocessingutils.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsexception.h>
#include <qgsrasterlayer.h>
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

#include <cmath>
#include <memory>
#include <vector>

// GeoTIFF 写口走共享的 PaleoRasterOut::createFloatRaster（rasterout.h）。

namespace
{

constexpr float PALEO_NODATA = -9999.0f;

} // namespace

// ---------------------------------------------------------------------------
// MinimumCurvatureAlgorithm — paleo_min_curvature
// ---------------------------------------------------------------------------

QString MinimumCurvatureAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Continuous-curvature splines in tension (Smith & Wessel 1990; the GMT "
    "'surface' family). Solves (1-T)*grad4(z) - T*gamma*grad2(z) = 0 by "
    "Gauss-Seidel iteration with over-relaxation; data points constrain the "
    "sheet at their own locations (bilinear misfit back-projected to the "
    "nearest node, Briggs/GMT semantics). T=0 is pure minimum curvature "
    "(exact for planes and quadratics); larger T suppresses overshoot toward "
    "membrane/harmonic behavior. CONSTRAINTS lines with type 'break_line' act "
    "as hard barriers: barrier cells become nodata and the relaxation treats "
    "them as no-flux internal boundaries, so each fault block interpolates "
    "only from its own side (consumes the C1 typed vocabulary; other types "
    "are ignored by this algorithm). Output carries QC metadata: "
    "PALEO_TENSION/PALEO_SWEEPS/PALEO_CONVERGED/PALEO_FINAL_DELTA plus "
    "binning counters and distance-to-data quality statistics "
    "(PALEO_DIST_TO_DATA_MAX/MEAN, in map units)." );
}

void MinimumCurvatureAlgorithm::initAlgorithm( const QVariantMap & )
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
      QStringLiteral( "TENSION" ),
      QStringLiteral( "Tension T in [0,1): 0 = minimum curvature, larger suppresses "
                      "overshoot" ),
      Qgis::ProcessingNumberParameterType::Double, 0.25, true, 0.0, 0.999 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MAX_SWEEPS" ), QStringLiteral( "Iteration sweep limit" ),
      Qgis::ProcessingNumberParameterType::Integer, 500, true, 1 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "Cell size (map units)" ),
      Qgis::ProcessingNumberParameterType::Double, 1.0, false, 1e-9 ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Gridded surface raster" ) ) );
}

QVariantMap MinimumCurvatureAlgorithm::processAlgorithm( const QVariantMap &parameters,
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
  if ( !( cellSize > 0.0 ) || !std::isfinite( cellSize ) )
    throw QgsProcessingException( QStringLiteral( "CELL_SIZE must be > 0" ) );
  if ( source->sourceCrs().isGeographic() && feedback )
    feedback->pushWarning( PaleoAlgoGuards::geographicCrsWarning(
        source->sourceCrs().userFriendlyIdentifier() ) );

  const double tension =
      parameterAsDouble( parameters, QStringLiteral( "TENSION" ), context );
  const int maxSweeps = parameterAsInt( parameters, QStringLiteral( "MAX_SWEEPS" ), context );
  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // ---- gather sample points --------------------------------------------------
  std::vector<paleo::gridsolver::ScatterPoint> samples;
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

  // ---- constraints: only break_line acts as a barrier (typed C1 vocabulary) --
  QVector<QgsGeometry> breakGeoms;
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
          throw QgsProcessingException( QStringLiteral(
              "Cannot transform constraints into the input CRS: %1" ).arg( e.what() ) );
        }
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
          const Qgis::GeometryOperationResult tr = g.transform( *xform );
          if ( tr != Qgis::GeometryOperationResult::Success )
            throw QgsProcessingException( QStringLiteral(
                "Constraint geometry failed to transform into the input CRS" ) );
        }
        const QString ctype =
            typeIdx >= 0 ? cf.attribute( typeIdx ).toString().trimmed() : QString();
        if ( ctype == QLatin1String( "break_line" ) )
          breakGeoms.append( g ); // direction_line/legacy hull 对网格化无语义，忽略
      }
    }
  }

  // ---- output grid: 10% margin（与 ConstraintIDW 同约定）+ 公共守卫 ----------
  const QgsRectangle raw = source->sourceExtent();
  const double xPad = raw.width() > 0.0 ? raw.width() * 0.1 : cellSize;
  const double yPad = raw.height() > 0.0 ? raw.height() * 0.1 : cellSize;
  const QgsRectangle extent( raw.xMinimum() - xPad, raw.yMinimum() - yPad,
                             raw.xMaximum() + xPad, raw.yMaximum() + yPad );
  const PaleoAlgoGuards::GridDims dims =
      PaleoAlgoGuards::gridDimsForExtent( extent, cellSize ); // Issue #33 算法侧同款
  if ( dims.cols < 4 || dims.rows < 4 )
    throw QgsProcessingException( QStringLiteral(
        "Output grid is smaller than the 4x4 minimum the curvature stencil needs" ) );

  paleo::gridsolver::GridGeometry geom;
  geom.cols = dims.cols;
  geom.rows = dims.rows;
  geom.originX = extent.xMinimum();
  geom.originY = extent.yMaximum(); // 北向上：左上角
  geom.dx = cellSize;
  geom.dy = cellSize;

  const double gt[6] = { extent.xMinimum(), cellSize, 0.0, extent.yMaximum(), 0.0,
                         -cellSize };
  GDALDatasetH outDs =
      PaleoRasterOut::createFloatRaster( outPath, dims.cols, dims.rows, gt, source->sourceCrs(),
                         PALEO_NODATA );
  if ( !outDs )
    throw QgsProcessingException(
        QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );

  // ---- break_line barriers: half-cell supersampled rasterization -------------
  // （与 ConstraintIDW 的栅格化同口径；隔离语义交给核的无通量内边界。）
  std::vector<std::uint8_t> barrierMask;
  if ( !breakGeoms.isEmpty() )
  {
    barrierMask.assign( static_cast<std::size_t>( dims.rows ) * dims.cols, 0 );
    auto cellOf = [&]( double x, double y, int &c, int &r ) {
      c = static_cast<int>( std::floor( ( x - extent.xMinimum() ) / cellSize ) );
      r = static_cast<int>( std::floor( ( extent.yMaximum() - y ) / cellSize ) );
      return c >= 0 && c < dims.cols && r >= 0 && r < dims.rows;
    };
    for ( const QgsGeometry &g : breakGeoms )
    {
      const QgsMultiPolylineXY mpl =
          g.isMultipart() ? g.asMultiPolyline() : QgsMultiPolylineXY{ g.asPolyline() };
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
          const int steps =
              std::max( 1, static_cast<int>( std::ceil( len / ( cellSize * 0.5 ) ) ) );
          for ( int s = 0; s <= steps; ++s )
          {
            int c = 0, r = 0;
            if ( cellOf( a.x() + ( b.x() - a.x() ) * s / steps,
                         a.y() + ( b.y() - a.y() ) * s / steps, c, r ) )
              barrierMask[static_cast<std::size_t>( r ) * dims.cols + c] = 1;
          }
        }
    }
  }

  // ---- solve ------------------------------------------------------------------
  paleo::gridsolver::GriddingParams gp;
  gp.tension = tension;
  gp.maxSweeps = maxSweeps > 0 ? maxSweeps : 500;
  paleo::gridsolver::IterationControl control;
  control.cancelRequested = [feedback]()
  { return feedback && feedback->isCanceled(); };
  control.onSweep = [feedback]( int sweep, int maxS, double )
  {
    if ( feedback )
      feedback->setProgress( 100.0 * static_cast<double>( sweep ) /
                             static_cast<double>( maxS ) );
  };
  std::vector<float> z;
  paleo::gridsolver::GriddingStats stats;
  QString solveErr;
  const bool ok = paleo::gridsolver::solveMinimumCurvature(
      samples, geom, gp, barrierMask.empty() ? nullptr : barrierMask.data(), &z, &stats,
      &solveErr, control );
  if ( !ok )
  {
    GDALClose( outDs );
    throw QgsProcessingException( QStringLiteral( "Gridding failed: %1" ).arg( solveErr ) );
  }

  // ---- quality surface: distance to nearest data (map units) ------------------
  const std::vector<float> dist = paleo::gridsolver::distanceToData( samples, geom );
  double distMax = 0.0, distSum = 0.0;
  long long distCount = 0;
  for ( float d : dist )
  {
    if ( std::isnan( d ) )
      continue;
    distMax = std::max( distMax, static_cast<double>( d ) );
    distSum += d;
    ++distCount;
  }

  // ---- write rows (NaN → nodata) + QC metadata --------------------------------
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );
  std::vector<float> row( dims.cols );
  for ( int r = 0; r < dims.rows; ++r )
  {
    for ( int c = 0; c < dims.cols; ++c )
    {
      const float v = z[static_cast<std::size_t>( r ) * dims.cols + c];
      row[c] = std::isnan( v ) ? PALEO_NODATA : v;
    }
    if ( GDALRasterIO( outBand, GF_Write, 0, r, dims.cols, 1, row.data(), dims.cols, 1,
                       GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( outDs );
      throw QgsProcessingException(
          QStringLiteral( "GDAL write failed at row %1" ).arg( r ) );
    }
  }
  const auto meta = [&]( const char *key, const QString &value )
  { GDALSetMetadataItem( outDs, key, value.toUtf8().constData(), nullptr ); };
  meta( "PALEO_ALGORITHM", QStringLiteral( "min_curvature" ) );
  meta( "PALEO_TENSION", QString::number( tension ) );
  meta( "PALEO_SWEEPS", QString::number( stats.sweeps ) );
  meta( "PALEO_CONVERGED", stats.converged ? QStringLiteral( "1" ) : QStringLiteral( "0" ) );
  meta( "PALEO_FINAL_DELTA", QString::number( stats.finalDelta ) );
  meta( "PALEO_CONSTRAINED_NODES", QString::number( stats.constrainedNodes ) );
  meta( "PALEO_COLLISIONS", QString::number( stats.collisions ) );
  meta( "PALEO_REJECTED", QString::number( stats.rejected ) );
  meta( "PALEO_BREAK_LINES", QString::number( breakGeoms.size() ) );
  meta( "PALEO_DIST_TO_DATA_MAX", QString::number( distMax ) );
  meta( "PALEO_DIST_TO_DATA_MEAN",
        distCount > 0 ? QString::number( distSum / distCount ) : QString() );
  GDALClose( outDs );

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  out.insert( QStringLiteral( "CONVERGED" ), stats.converged );
  out.insert( QStringLiteral( "SWEEPS" ), stats.sweeps );
  return out;
}
