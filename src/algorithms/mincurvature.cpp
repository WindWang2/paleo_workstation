// 层：数据
#include "constraintbarriers.h"
#include "paleoalgorithms.h"
#include "rasterout.h"

#include <qgsprocessingoutputs.h>
#include "gridsolver.h"

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
    "membrane/harmonic behavior. Full-block break lines in CONSTRAINTS act "
    "as hard barriers: barrier cells become nodata and the relaxation treats "
    "them as no-flux internal boundaries, so each fault block interpolates "
    "only from its own side. Unified constraint semantics apply (params_json "
    "first, then the 'type' column): disabled rows, non-blocking blockMode such "
    "as display_only, invalid params and invalid geometry are skipped; every "
    "decision is returned in CONSTRAINT_RECEIPTS. Output carries QC metadata: "
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
  addOutput( new QgsProcessingOutputString(
      QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "Per-constraint consumption receipts (JSON array)" ) ) );
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
  paleo::algorithms::UnifiedBarrierRead barrierRead; // 完全阻断部件 + 引擎回执
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
        // BIZ-10（方向58）：显式前置 transform 有效性——无大地基准的工程 CRS
        //（LOCAL_GRID_WKT ENGCRS）与大地/投影 CRS 之间 PROJ 造不出坐标操作
        //（projinfo：Candidate operations found: 0）。如实报因，不依赖 QGIS
        // 对无效 transform 的内部处理（直通或抛 QgsCsException）。
        if ( !xform->isValid() )
          throw QgsProcessingException(
              QStringLiteral( "Cannot transform constraints into the %1 CRS: no coordinate operation "
                              "between %2 and %3 (engineering CRS without geodetic datum?)" )
                  .arg( QStringLiteral( "input" ), from.userFriendlyIdentifier(),
                        to.userFriendlyIdentifier() ) );
      }
      // P2：统一语义——只有完全阻断打断线成为无通量内边界；停用、display_only/
      // none、非法参数、无效几何不用并留回执（不再只看 type 列）。
      barrierRead = paleo::algorithms::readUnifiedHardBarriers(
          constraints.get(), QStringLiteral( "min_curvature" ), [&xform]( QgsGeometry &g ) {
            if ( !xform )
              return;
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
              throw QgsProcessingException( QStringLiteral(
                  "Constraint geometry failed to transform into the input CRS" ) );
          } );
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
                         PALEO_NODATA, PaleoRasterOut::canonicalWktFromParameters( parameters ) );
  if ( !outDs )
    throw QgsProcessingException(
        QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );

  // ---- break_line barriers: half-cell supersampled rasterization -------------
  // （与 ConstraintIDW 的栅格化同口径；隔离语义交给核的无通量内边界。）
  // P3：逐部件记是否落格；未落格的阻断线回执改为 outside_compute_domain。
  std::vector<std::uint8_t> barrierMask;
  int touchedParts = 0;
  if ( !barrierRead.parts.isEmpty() )
  {
    touchedParts = paleo::algorithms::rasterizeBarrierParts(
        barrierRead, extent.xMinimum(), extent.yMaximum(), cellSize, dims.cols, dims.rows, barrierMask,
        [feedback] { return feedback && feedback->isCanceled(); } );
    if ( touchedParts < 0 )
    {
      GDALClose( outDs );
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    }
    if ( touchedParts == 0 )
      barrierMask.clear();
  }
  const QVariantList &constraintReceipts = barrierRead.receipts;

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
  meta( "PALEO_BREAK_LINES", QString::number( touchedParts ) );
  meta( "PALEO_DIST_TO_DATA_MAX", QString::number( distMax ) );
  meta( "PALEO_DIST_TO_DATA_MEAN",
        distCount > 0 ? QString::number( distSum / distCount ) : QString() );
  GDALClose( outDs );

  QVariantMap out;
  out.insert( QStringLiteral( "OUTPUT" ), outPath );
  out.insert( QStringLiteral( "CONVERGED" ), stats.converged );
  out.insert( QStringLiteral( "SWEEPS" ), stats.sweeps );
  out.insert( QStringLiteral( "CONSTRAINT_RECEIPTS" ), paleo::algorithms::constraintReceiptsJson( constraintReceipts ) );
  return out;
}
