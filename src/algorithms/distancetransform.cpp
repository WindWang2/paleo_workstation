// 层：数据
// PaleoDistanceTransformAlgorithm（paleo:paleo_distance_transform）——
// welldist 单因素的距离变换引擎，id/参数面按 SingleFactorContracts 冻结契约
//（src/services/singlefactordef.h：INPUT=井点图层，OUTPUT=栅格，CELL_SIZE=正数，
// 绕障=约束线）。实现见 paleoalgorithms.h 类注。
#include "constraintbarriers.h"
#include "paleoalgorithms.h"
#include "rasterout.h"
#include "welldistance_internal.h"

#include <qgsprocessingoutputs.h>
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

} // namespace

QString PaleoDistanceTransformAlgorithm::shortHelpString() const
{
  return QStringLiteral(
    "Distance to the nearest well as a single-factor surface. Without CONSTRAINTS "
    "(or with no full-block break line) each cell holds the exact Euclidean distance "
    "to the closest INPUT point (map units). When CONSTRAINTS contain full-block break "
    "lines (unified constraint semantics: params_json first, then the 'type' column; "
    "disabled rows, non-blocking blockMode such as display_only, invalid params and "
    "invalid geometry are skipped and listed in CONSTRAINT_RECEIPTS), those lines are "
    "hard barriers and the cell value becomes the "
    "barrier-avoiding path distance: a 16-connected Dijkstra over the output grid "
    "(edge costs 1, sqrt(2) and sqrt(5) cells, well cells seeded at 0; diagonal and "
    "knight moves are disallowed when any cell of the box they cut across is a "
    "barrier, so walls rasterized to corner-touching cells still seal). Barrier "
    "cells and cells with no path to any well become nodata. Grid covers the input "
    "extent grown by 10%% per side using CELL_SIZE cells." );
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
  addOutput( new QgsProcessingOutputString(
      QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "Per-constraint consumption receipts (JSON array)" ) ) );
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
  if ( source->sourceCrs().isGeographic() && feedback )
    feedback->pushWarning( PaleoAlgoGuards::geographicCrsWarning(
        source->sourceCrs().userFriendlyIdentifier() ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT raster destination" ) );

  // ---- gather well points ---------------------------------------------------
  const auto wells = paleo::well_distance_detail::gatherWellPoints(source.get(), feedback);

  // ---- break_line barriers（typed 语义与 ConstraintIDW 一致：仅 break_line
  //      阻断；direction_line/旧 shape 词对距离面不参与）---------------------
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
      // P2：统一语义（domain/constraintsemantics）——只有完全阻断打断线隔断距离；
      // 停用、display_only/none、非法参数、无效几何不用并留回执。
      barrierRead = paleo::algorithms::readUnifiedHardBarriers(
          constraints.get(), QStringLiteral( "distance_transform" ), [&xform]( QgsGeometry &g ) {
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
              throw QgsProcessingException(
                  QStringLiteral( "Constraint geometry failed to transform into the well CRS" ) );
          } );
    }
  }

  // ---- output grid：10% margin，退化轴补一格（与 ConstraintIDW 同约定）----
  const QgsRectangle raw = source->sourceExtent();
  const double xPad = raw.width() > 0.0 ? raw.width() * 0.1 : cellSize;
  const double yPad = raw.height() > 0.0 ? raw.height() * 0.1 : cellSize;
  QgsRectangle extent( raw.xMinimum() - xPad, raw.yMinimum() - yPad,
                       raw.xMaximum() + xPad, raw.yMaximum() + yPad );
  const PaleoAlgoGuards::GridDims dims =
      PaleoAlgoGuards::gridDimsForExtent( extent, cellSize );
  const int nCols = dims.cols;
  const int nRows = dims.rows;
  auto cellOf = [&]( double x, double y, int &c, int &r ) {
    c = static_cast<int>( std::floor( ( x - extent.xMinimum() ) / cellSize ) );
    r = static_cast<int>( std::floor( ( extent.yMaximum() - y ) / cellSize ) );
    return c >= 0 && c < nCols && r >= 0 && r < nRows;
  };

  const double gt[6] = { extent.xMinimum(), cellSize, 0.0,
                         extent.yMaximum(), 0.0, -cellSize };
  GDALDatasetH outDs = PaleoRasterOut::createFloatRaster( outPath, nCols, nRows, gt, source->sourceCrs(),
                                                          PALEO_DT_NODATA,
                                                          PaleoRasterOut::canonicalWktFromParameters( parameters ) );
  if ( !outDs )
    throw QgsProcessingException( QStringLiteral( "Cannot create output raster %1" ).arg( outPath ) );
  const qsizetype cellCount = static_cast<qsizetype>( nCols ) * nRows;
  // ---- barrier rasterization（半格超采样步进，同 ConstraintIDW 口径）--------
  // P3：逐部件记是否落格；一个格都没落上的阻断线不隔断、回执如实改为
  // outside_compute_domain，且不因它把精确欧氏距离换成 8 邻接近似。
  std::vector<uint8_t> blocked;
  const int touchedParts = paleo::algorithms::rasterizeBarrierParts(
      barrierRead, extent.xMinimum(), extent.yMaximum(), cellSize, nCols, nRows, blocked,
      [feedback] { return feedback && feedback->isCanceled(); } );
  if ( touchedParts < 0 )
  {
    GDALClose( outDs );
    throw QgsProcessingException( QStringLiteral( "Canceled" ) );
  }
  const bool hasBarriers = touchedParts > 0;
  const QVariantList &constraintReceipts = barrierRead.receipts;
  GDALSetMetadataItem( outDs, "PALEO_BREAK_LINES", QString::number( touchedParts ).toUtf8().constData(), nullptr );
  GDALSetMetadataItem( outDs, "PALEO_BARRIER_AWARE", hasBarriers ? "1" : "0", nullptr );
  GDALRasterBandH outBand = GDALGetRasterBand( outDs, 1 );

  if ( !hasBarriers )
  {
    // ---- exact Euclidean（与 paleo:paleo_welldist 同口径）-------------------
    paleo::well_distance_detail::writeEuclideanDistances(
        outDs, nCols, nRows, extent, cellSize, wells, feedback);
    GDALClose( outDs );
    QVariantMap out;
    out.insert( QStringLiteral( "OUTPUT" ), outPath );
    out.insert( QStringLiteral( "CONSTRAINT_RECEIPTS" ), paleo::algorithms::constraintReceiptsJson( constraintReceipts ) );
    return out;
  }

  // ---- multi-source Dijkstra（16 邻接，边权 1 / √2 / √5 格；对角步穿角检查
  //      与骑马步包围盒检查见下方松弛循环）-----------------------------------
  // issue #721：一有屏障整幅退化成 8 邻域 chamfer（对欧氏距离系统性高估最多
  // ~8%）。补 8 个骑马步（±2,±1）/（±1,±2）（代价 √5 格 = 该位移的精确长度）
  // 后最大相对误差降到 ~3%，且井格 0 / 正交邻格 1 / 对角 √2 这些契约值不变
  // （tst_algorithm_harness 冻结口径）。
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
  const double knight = cellSize * std::sqrt( 5.0 );
  // 16 个邻接位移（4 正交 + 4 对角 + 8 骑马步）与对应边权。
  const int neighborDc[16] = { 1, -1, 0, 0, 1, 1, -1, -1, 2, 2, -2, -2, 1, 1, -1, -1 };
  const int neighborDr[16] = { 0, 0, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 2, 2, -2, -2 };
  const double neighborWeight[16] = { ortho, ortho, ortho, ortho, diag, diag, diag, diag,
                                      knight, knight, knight, knight, knight, knight, knight,
                                      knight };
  const auto barrierAt = [&]( int r, int c ) {
    return blocked[static_cast<size_t>( r ) * nCols + c] != 0;
  };
  qsizetype settled = 0; // 每 4096 次松弛查一次取消 + 报进度（虚调用节流）
  while ( !pq.empty() )
  {
    const Item cur = pq.top();
    pq.pop();
    if ( cur.first > dist[static_cast<size_t>( cur.second )] )
      continue; // stale entry
    if ( feedback && ( ++settled & 4095 ) == 0 )
    {
      if ( feedback->isCanceled() )
      {
        GDALClose( outDs );
        throw QgsProcessingException( QStringLiteral( "Canceled" ) );
      }
      feedback->setProgress( 100.0 * static_cast<double>( settled ) /
                             static_cast<double>( cellCount ) );
    }
    const int cr = static_cast<int>( cur.second / nCols );
    const int cc = static_cast<int>( cur.second % nCols );
    for ( int k = 0; k < 16; ++k )
    {
      const int nr = cr + neighborDr[k], nc = cc + neighborDc[k];
      if ( nr < 0 || nr >= nRows || nc < 0 || nc >= nCols )
        continue;
      const qsizetype n = static_cast<qsizetype>( nr ) * nCols + nc;
      if ( barrierAt( nr, nc ) )
        continue;
      if ( neighborDr[k] != 0 && neighborDc[k] != 0 )
      {
        // 对角/骑马步都可能从两个角接障碍格之间斜穿：
        // · 对角步（±1,±1）：两侧正交格任一被挡即禁行（issue #292 斜墙角点密封）。
        // · 骑马步（±2,±1）/（±1,±2）：线段穿过源格与目标格之间的包围盒，
        //   保守要求该包围盒内（除两端）全部自由——包括它掠过的格角，防
        //   角接屏障被零宽缝隙穿过。
        if ( std::abs( neighborDr[k] ) == 1 && std::abs( neighborDc[k] ) == 1 )
        {
          if ( barrierAt( cr + neighborDr[k], cc ) || barrierAt( cr, cc + neighborDc[k] ) )
            continue;
        }
        else
        {
          const int rLo = std::min( cr, nr ), rHi = std::max( cr, nr );
          const int cLo = std::min( cc, nc ), cHi = std::max( cc, nc );
          bool gated = false;
          for ( int rr = rLo; rr <= rHi && !gated; ++rr )
            for ( int cc2 = cLo; cc2 <= cHi; ++cc2 )
            {
              if ( ( rr == cr && cc2 == cc ) || ( rr == nr && cc2 == nc ) )
                continue; // 两端点格已单独判定
              if ( barrierAt( rr, cc2 ) )
              {
                gated = true;
                break;
              }
            }
          if ( gated )
            continue;
        }
      }
      const double nd = cur.first + neighborWeight[k];
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
  out.insert( QStringLiteral( "CONSTRAINT_RECEIPTS" ), paleo::algorithms::constraintReceiptsJson( constraintReceipts ) );
  return out;
}
