// 层：数据
// PaleoDistanceTransformAlgorithm（paleo:paleo_distance_transform）——
// welldist 单因素的距离变换引擎，id/参数面按 SingleFactorContracts 冻结契约
//（src/services/singlefactordef.h：INPUT=井点图层，OUTPUT=栅格，CELL_SIZE=正数，
// 绕障=约束线）。实现见 paleoalgorithms.h 类注。
#include "paleoalgorithms.h"

#include <qgsprocessingparameters.h>
#include <qgsprocessingutils.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsexception.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_error.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers> // std::numbers::sqrt2——M_SQRT2 在 MSVC 下不定义
#include <queue>
#include <utility>
#include <vector>

namespace
{

constexpr float PALEO_DT_NODATA = -9999.0f;

// Single-band Float32 GeoTIFF（与 welldist.cpp 同一文件内复制习惯）。
GDALDatasetH createFloatRaster( const QString &outPath, int nCols, int nRows,
                                const double geoTransform[6], double nodata )
{
  GDALAllRegister();
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
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, nodata );
  return ds;
}

} // namespace

QString PaleoDistanceTransformAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Distance to the nearest well as a single-factor surface. Without CONSTRAINTS "
    "(or with none typed 'break_line') each cell holds the exact Euclidean distance "
    "to the closest INPUT point (map units). When CONSTRAINTS contain 'break_line' "
    "features, those lines are hard barriers and the cell value becomes the "
    "barrier-avoiding path distance: an 8-connected Dijkstra over the output grid "
    "(edge costs 1 and sqrt(2) cells, well cells seeded at 0). Barrier cells and "
    "cells with no path to any well become nodata. Grid covers the input extent "
    "grown by 10%% per side using CELL_SIZE cells." );
}

void PaleoDistanceTransformAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "INPUT" ), QStringLiteral( "Wells (point layer)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorPoint ) ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "Barrier lines (break_line, optional)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorLine ),
      QVariant(), true ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "Cell size (map units)" ),
      Qgis::ProcessingNumberParameterType::Double, 1.0 ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Distance raster" ) ) );
}

QVariantMap PaleoDistanceTransformAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                               QgsProcessingContext &context,
                                                               QgsProcessingFeedback *feedback )
{
  std::unique_ptr<QgsProcessingFeatureSource> source(
      parameterAsSource( parameters, QStringLiteral( "INPUT" ), context ) );
  if ( !source )
    throw QgsProcessingException( invalidSourceError( parameters, QStringLiteral( "INPUT" ) ) );

  const double cellSize = parameterAsDouble( parameters, QStringLiteral( "CELL_SIZE" ), context );
  if ( cellSize <= 0.0 || !std::isfinite( cellSize ) )
    throw QgsProcessingException( QStringLiteral( "CELL_SIZE must be > 0" ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // ---- gather well points ---------------------------------------------------
  std::vector<QgsPointXY> wells;
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
      wells.push_back( g.isMultipart() ? g.asMultiPoint().value( 0 ) : g.asPoint() );
    }
  }
  if ( wells.empty() )
    throw QgsProcessingException( QStringLiteral( "INPUT contains no usable point features" ) );

  // ---- break_line barriers（typed 语义与 ConstraintIDW 一致：仅 break_line
  //      阻断；direction_line/旧 shape 词对距离面不参与）---------------------
  std::vector<QgsPolylineXY> barrierPolylines;
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
      }
      const int typeIdx = constraints->fields().lookupField( QStringLiteral( "type" ) );
      QgsFeatureIterator cit = constraints->getFeatures( QgsFeatureRequest() );
      QgsFeature cf;
      while ( cit.nextFeature( cf ) )
      {
        if ( !cf.hasGeometry() || cf.geometry().isEmpty() )
          continue;
        const QString ctype = typeIdx >= 0 ? cf.attribute( typeIdx ).toString().trimmed()
                                           : QString();
        if ( ctype != QLatin1String( "break_line" ) )
          continue;
        QgsGeometry g = cf.geometry();
        if ( xform )
        {
          const Qgis::GeometryOperationResult tr = g.transform( *xform );
          if ( tr != Qgis::GeometryOperationResult::Success )
            throw QgsProcessingException(
                QStringLiteral( "Constraint geometry failed to transform into the well CRS" ) );
        }
        const QgsMultiPolylineXY mpl = g.isMultipart()
                                           ? g.asMultiPolyline()
                                           : QgsMultiPolylineXY{ g.asPolyline() };
        for ( const QgsPolylineXY &pl : mpl )
          if ( pl.size() >= 2 )
            barrierPolylines.push_back( pl );
      }
    }
  }
  const bool hasBarriers = !barrierPolylines.empty();

  // ---- output grid：10% margin，退化轴补一格（与 ConstraintIDW 同约定）----
  const QgsRectangle raw = source->sourceExtent();
  const double xPad = raw.width() > 0.0 ? raw.width() * 0.1 : cellSize;
  const double yPad = raw.height() > 0.0 ? raw.height() * 0.1 : cellSize;
  QgsRectangle extent( raw.xMinimum() - xPad, raw.yMinimum() - yPad,
                       raw.xMaximum() + xPad, raw.yMaximum() + yPad );
  const int nCols = std::max( 1, static_cast<int>( std::ceil( extent.width() / cellSize ) ) );
  const int nRows = std::max( 1, static_cast<int>( std::ceil( extent.height() / cellSize ) ) );
  auto cellOf = [&]( double x, double y, int &c, int &r ) {
    c = static_cast<int>( std::floor( ( x - extent.xMinimum() ) / cellSize ) );
    r = static_cast<int>( std::floor( ( extent.yMaximum() - y ) / cellSize ) );
    return c >= 0 && c < nCols && r >= 0 && r < nRows;
  };

  const double gt[6] = { extent.xMinimum(), cellSize, 0.0,
                         extent.yMaximum(), 0.0, -cellSize };
  GDALDatasetH outDs = createFloatRaster( outPath, nCols, nRows, gt, PALEO_DT_NODATA );
  if ( !outDs )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );
  GDALSetMetadataItem( outDs, "PALEO_BREAK_LINES",
                       QString::number( barrierPolylines.size() ).toUtf8().constData(), nullptr );
  GDALSetMetadataItem( outDs, "PALEO_BARRIER_AWARE", hasBarriers ? "1" : "0", nullptr );
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  const qsizetype cellCount = static_cast<qsizetype>( nCols ) * nRows;
  if ( !hasBarriers )
  {
    // ---- exact Euclidean（与 paleo:paleo_welldist 同口径）-------------------
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
        double best2 = std::numeric_limits<double>::infinity();
        for ( const QgsPointXY &w : wells )
        {
          const double dx = w.x() - x, dy = w.y() - y;
          const double d2 = dx * dx + dy * dy;
          if ( d2 < best2 )
            best2 = d2;
        }
        rowBuf[c] = static_cast<float>( std::sqrt( best2 ) );
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

  // ---- barrier rasterization（半格超采样步进，同 ConstraintIDW 口径）--------
  std::vector<uint8_t> blocked( static_cast<size_t>( cellCount ), 0 );
  for ( const QgsPolylineXY &pl : barrierPolylines )
    for ( int i = 1; i < pl.size(); ++i )
    {
      const QgsPointXY a = pl[i - 1], b = pl[i];
      const double len = a.distance( b );
      const int steps = std::max( 1, static_cast<int>( std::ceil( len / ( cellSize * 0.5 ) ) ) );
      for ( int s = 0; s <= steps; ++s )
      {
        int c = 0, r = 0;
        if ( cellOf( a.x() + ( b.x() - a.x() ) * s / steps,
                     a.y() + ( b.y() - a.y() ) * s / steps, c, r ) )
          blocked[static_cast<qsizetype>( r ) * nCols + c] = 1;
      }
    }

  // ---- multi-source Dijkstra（8 邻接，边权 1 / √2 格）-----------------------
  std::vector<double> dist( static_cast<size_t>( cellCount ),
                            std::numeric_limits<double>::infinity() );
  using Item = std::pair<double, qsizetype>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  for ( const QgsPointXY &w : wells )
  {
    int c = 0, r = 0;
    if ( !cellOf( w.x(), w.y(), c, r ) )
      continue; // margin math keeps wells inside; guard anyway
    const qsizetype idx = static_cast<qsizetype>( r ) * nCols + c;
    if ( blocked[static_cast<size_t>( idx )] )
      continue; // well pinned on a barrier: unreachable seed
    if ( dist[static_cast<size_t>( idx )] > 0.0 )
    {
      dist[static_cast<size_t>( idx )] = 0.0;
      pq.push( { 0.0, idx } );
    }
  }
  const double ortho = cellSize, diag = cellSize * std::numbers::sqrt2;
  while ( !pq.empty() )
  {
    const Item cur = pq.top();
    pq.pop();
    if ( cur.first > dist[static_cast<size_t>( cur.second )] )
      continue; // stale entry
    const int cr = static_cast<int>( cur.second / nCols );
    const int cc = static_cast<int>( cur.second % nCols );
    for ( int dr = -1; dr <= 1; ++dr )
      for ( int dc = -1; dc <= 1; ++dc )
      {
        if ( dr == 0 && dc == 0 )
          continue;
        const int nr = cr + dr, nc = cc + dc;
        if ( nr < 0 || nr >= nRows || nc < 0 || nc >= nCols )
          continue;
        const qsizetype n = static_cast<qsizetype>( nr ) * nCols + nc;
        if ( blocked[static_cast<size_t>( n )] )
          continue;
        const double nd = cur.first + ( ( dr != 0 && dc != 0 ) ? diag : ortho );
        if ( nd < dist[static_cast<size_t>( n )] )
        {
          dist[static_cast<size_t>( n )] = nd;
          pq.push( { nd, n } );
        }
      }
  }

  QVector<float> rowBuf( nCols );
  for ( int r = 0; r < nRows; ++r )
  {
    if ( feedback && feedback->isCanceled() )
    {
      GDALClose( outDs );
      throw QgsProcessingException( QStringLiteral( "Canceled" ) );
    }
    for ( int c = 0; c < nCols; ++c )
    {
      const qsizetype idx = static_cast<qsizetype>( r ) * nCols + c;
      rowBuf[c] = ( blocked[static_cast<size_t>( idx )] ||
                    !std::isfinite( dist[static_cast<size_t>( idx )] ) )
                      ? PALEO_DT_NODATA
                      : static_cast<float>( dist[static_cast<size_t>( idx )] );
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
