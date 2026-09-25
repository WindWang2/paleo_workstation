#include "paleoalgorithms.h"

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

// Create a single-band Float32 GeoTIFF at outPath covering extent with cellSize.
// Returns nullptr on failure. Caller must GDALClose().
GDALDatasetH createFloatRaster( const QString &outPath, int nCols, int nRows,
                                const double geoTransform[6],
                                const QgsCoordinateReferenceSystem &crs,
                                double nodata )
{
  GDALAllRegister(); // idempotent — safe under an already-initialized QGIS runtime
  GDALDriverH drv = GDALGetDriverByName( "GTiff" );
  if ( !drv )
    return nullptr;
  GDALDatasetH ds = GDALCreate( drv, outPath.toUtf8().constData(), nCols, nRows, 1,
                                GDT_Float32, nullptr );
  if ( !ds )
    return nullptr;
  if ( GDALSetGeoTransform( ds, const_cast<double *>( geoTransform ) ) != CE_None )
  {
    GDALClose( ds );
    return nullptr;
  }
  if ( crs.isValid() )
  {
    const QByteArray wkt = crs.toWkt( Qgis::CrsWktVariant::Wkt1Gdal ).toUtf8();
    GDALSetProjection( ds, wkt.constData() );
  }
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, nodata );
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
  return std::isnan( v ) || ( hasNodata && static_cast<double>( v ) == nodata );
}

void bandNodata( GDALRasterBandH band, bool &hasNodata, double &nodata )
{
  int flag = 0;
  nodata = GDALGetRasterNoDataValue( band, &flag );
  hasNodata = flag != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// ConstraintIDWAlgorithm
// ---------------------------------------------------------------------------

QString ConstraintIDWAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Inverse-distance-weighted (power = 2) interpolation of a numeric z-field on a "
    "point layer. The output grid covers the input extent grown by 10%% on every side, "
    "using CELL_SIZE cells. If CONSTRAINTS line features are supplied, output cells "
    "outside the convex hull of the constraint geometry are set to nodata "
    "(constraints act as a region-of-influence clip). Barrier-aware distance around "
    "constraint lines is reserved for a future release. FACIES_CODE is recorded as "
    "raster metadata (PALEO_FACIES_CODE)." );
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

  // ---- constraint hull (region-of-influence clip) ---------------------------
  // P1 simplification: constraints clip output to the convex hull of the
  // constraint line geometry. True barrier-aware IDW (distance measured around
  // constraint lines) is RESERVED — the parameter is part of the stable
  // interface and is fully consumed here, but only the convex-region clip is
  // applied. If the hull is degenerate/unavailable the output passes through
  // unclipped.
  QgsGeometry hull;
  {
    std::unique_ptr<QgsProcessingFeatureSource> constraints(
        parameterAsSource( parameters, QStringLiteral( "CONSTRAINTS" ), context ) );
    if ( constraints )
    {
      QVector<QgsGeometry> geoms;
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
      }
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
            throw QgsProcessingException(
                QStringLiteral( "Constraint geometry failed to transform into the well CRS" ) );
        }
        geoms.append( g );
      }
      if ( geoms.size() == 1 )
        hull = geoms.at( 0 ).convexHull();
      else if ( geoms.size() > 1 )
        hull = QgsGeometry::unaryUnion( geoms ).convexHull();
    }
  }
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
  const int nCols = std::max( 1, static_cast<int>( std::ceil( extent.width() / cellSize ) ) );
  const int nRows = std::max( 1, static_cast<int>( std::ceil( extent.height() / cellSize ) ) );

  const double gt[6] = { extent.xMinimum(), cellSize, 0.0,
                         extent.yMaximum(), 0.0, -cellSize };
  GDALDatasetH outDs = createFloatRaster( outPath, nCols, nRows, gt,
                                          source->sourceCrs(), PALEO_NODATA );
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

  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  // ---- plain IDW (power = 2) -------------------------------------------------
  // O(rows * cols * points); adequate for P1-scale inputs. A spatial index /
  // neighbor cutoff is a documented optimization path for large point sets.
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
      if ( clipToHull && !hull.contains( x, y ) )
      {
        rowBuf[c] = PALEO_NODATA;
        continue;
      }
      double weightSum = 0.0, valueSum = 0.0;
      bool exact = false;
      double exactValue = 0.0;
      for ( const Sample &s : samples )
      {
        const double dx = s.x - x, dy = s.y - y;
        const double d2 = dx * dx + dy * dy;
        if ( d2 == 0.0 )
        {
          exact = true;
          exactValue = s.z;
          break;
        }
        const double w = 1.0 / d2; // power = 2
        weightSum += w;
        valueSum += w * s.z;
      }
      rowBuf[c] = exact ? static_cast<float>( exactValue )
                        : static_cast<float>( valueSum / weightSum );
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
  GDALDatasetH outDs = createFloatRaster( outPath, ref.cols, ref.rows, ref.gt,
                                          layers.at( 0 )->crs(), outNodata );
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

  GDALDatasetH outDs = createFloatRaster( outPath, grid.cols, grid.rows, grid.gt,
                                          rl->crs(), outNodata );
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
  GDALDatasetH outDs = createFloatRaster( outPath, grid.cols, grid.rows, grid.gt,
                                          topLayer->crs(), outNodata );
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
  addAlgorithm( new ConstraintIDWAlgorithm() );
  addAlgorithm( new FaciesFusionAlgorithm() );
  addAlgorithm( new GeologicalSmoothingAlgorithm() );
  addAlgorithm( new IsopachAlgorithm() );
  addAlgorithm( new FaciesPolygonizeAlgorithm() );
}
