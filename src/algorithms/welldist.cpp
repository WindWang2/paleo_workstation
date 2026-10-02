// 层：数据
// PaleoWellDistanceAlgorithm（paleo:paleo_welldist）——实现见 paleoalgorithms.h
// 类注。栅格写口走共享的 PaleoRasterOut::createFloatRaster（rasterout.h，
// 恒写 CRS/GeoTransform）。
#include "paleoalgorithms.h"
#include "rasterout.h"

#include <qgsprocessingparameters.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsexception.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <cpl_error.h>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace
{

constexpr float PALEO_WELLDIST_NODATA = -9999.0f;

} // namespace

QString PaleoWellDistanceAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Distance to the nearest well: each output cell holds the exact Euclidean "
    "distance from the cell center to the closest point feature of INPUT, in "
    "INPUT's map units. The grid covers the input extent grown by 10%% on every "
    "side using CELL_SIZE cells. Distances are computed directly against the "
    "point coordinates (no cell snapping), so a well's zero-distance halo is "
    "bounded by half a cell diagonal. Wells are typically the catalog's wells "
    "layer; the single-factor registry maps this algorithm to the 'welldist' "
    "factor (距井距离)." );
}

void PaleoWellDistanceAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "INPUT" ), QStringLiteral( "Wells (point layer)" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorPoint ) ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "Cell size (map units)" ),
      Qgis::ProcessingNumberParameterType::Double, 1.0 ) );
  addParameter( new QgsProcessingParameterRasterDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Distance raster" ) ) );
}

QVariantMap PaleoWellDistanceAlgorithm::processAlgorithm( const QVariantMap &parameters,
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
  if ( source->sourceCrs().isGeographic() && feedback )
    feedback->pushWarning( PaleoAlgoGuards::geographicCrsWarning(
        source->sourceCrs().userFriendlyIdentifier() ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // ---- gather well points（几何即身份：无属性参与）------------------------
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

  // ---- output grid：10% margin，退化轴补一格（与 ConstraintIDW 同约定）----
  const QgsRectangle raw = source->sourceExtent();
  const double xPad = raw.width() > 0.0 ? raw.width() * 0.1 : cellSize;
  const double yPad = raw.height() > 0.0 ? raw.height() * 0.1 : cellSize;
  const QgsRectangle extent( raw.xMinimum() - xPad, raw.yMinimum() - yPad,
                             raw.xMaximum() + xPad, raw.yMaximum() + yPad );
  const PaleoAlgoGuards::GridDims dims =
      PaleoAlgoGuards::gridDimsForExtent( extent, cellSize );
  const int nCols = dims.cols;
  const int nRows = dims.rows;

  const double gt[6] = { extent.xMinimum(), cellSize, 0.0,
                         extent.yMaximum(), 0.0, -cellSize };
  GDALDatasetH outDs = PaleoRasterOut::createFloatRaster( outPath, nCols, nRows, gt, source->sourceCrs(),
                                                          PALEO_WELLDIST_NODATA );
  if ( !outDs )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  // ---- exact nearest-well distance per cell center ------------------------
  // O(rows*cols*points) 精确直算：距离无近似（非 cell-snake 栅格化距离），
  // 井点规模为工区级（≤几百口）时毫秒~秒级。空间索引/两遍 EDT 是大点集的
  // 文档化优化路径，不改输出语义。
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
