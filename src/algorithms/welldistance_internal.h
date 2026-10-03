// 层：数据
#pragma once

#include <qgsprocessingutils.h>
#include <qgsprocessingfeedback.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsexception.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <QVector>
#include <gdal.h>
#include <cmath>
#include <limits>
#include <vector>

namespace paleo::well_distance_detail {

inline std::vector<QgsPointXY> gatherWellPoints(QgsProcessingFeatureSource *source,
                                               QgsProcessingFeedback *feedback)
{
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

  return wells;
}

// Exact cell-center calculation shared by welldist and the no-barrier branch
// of distance_transform. On failure this closes outDs before throwing, as the
// original loops did; the caller closes it after a successful write.
inline void writeEuclideanDistances(GDALDatasetH outDs, int nCols, int nRows,
                                     const QgsRectangle &extent, double cellSize,
                                     const std::vector<QgsPointXY> &wells,
                                     QgsProcessingFeedback *feedback)
{
  GDALRasterBandH outBand = GDALGetRasterBand(outDs, 1);
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

}

} // namespace paleo::well_distance_detail
