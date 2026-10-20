// 层：功能
#include "algorithmadapters.h"
#include "algorithmpublish.h"

#include "../algorithms/rasterout.h"
#include "../catalog/datacatalog.h"

#include <gdal.h>

#include <cmath>
#include <cstring>

namespace paleo
{
namespace
{

bool writeFloatRaster( const QString &path, const std::vector<double> &values, int cols, int rows,
                       const double geoTransform[6], const QgsCoordinateReferenceSystem &crs,
                       const QString &canonicalCrsWkt )
{
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( path, cols, rows, geoTransform, crs, -9999.0,
                                                       canonicalCrsWkt );
  if ( !ds )
    return false;
  std::vector<float> row( static_cast<std::size_t>( cols ) );
  bool ok = true;
  for ( int r = 0; r < rows && ok; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double value = values[static_cast<std::size_t>( r ) * cols + c];
      row[static_cast<std::size_t>( c )] =
          std::isfinite( value ) ? static_cast<float>( value ) : -9999.0f;
    }
    if ( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Write, 0, r, cols, 1, row.data(), cols, 1,
                       GDT_Float32, 0, 0 ) != CE_None )
      ok = false;
  }
  GDALClose( ds );
  return ok;
}

} // namespace

ProcessingAlgorithmAdapter::ProcessingAlgorithmAdapter( QgisProcessingService *proc )
  : m_proc( proc )
{
}

AlgorithmRunResult ProcessingAlgorithmAdapter::run(
    const AlgorithmSpec &spec, const QVariantMap &paramValues, QString *error,
    const QgisProcessingService::ProcessingHooks &hooks )
{
  AlgorithmRunResult r;
  if ( spec.id.isEmpty() )
  {
    r.ok = false;
    r.error = QStringLiteral( "ProcessingAlgorithmAdapter：AlgorithmSpec.id 为空" );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( spec.engine != QStringLiteral( "processing" ) )
  {
    r.ok = false;
    r.error = QStringLiteral( "ProcessingAlgorithmAdapter：engine 不是 processing（%1）" )
                  .arg( spec.engine );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( !m_proc )
  {
    r.ok = false;
    r.error = QStringLiteral( "ProcessingAlgorithmAdapter：未绑定 QgisProcessingService" );
    if ( error )
      *error = r.error;
    return r;
  }

  const QVariantMap results = m_proc->run( spec.id, paramValues, error, hooks );
  if ( results.isEmpty() )
  {
    r.ok = false;
    r.error = ( error && !error->isEmpty() )
                  ? *error
                  : QStringLiteral( "Processing 未返回结果（失败或取消）" );
    return r;
  }

  r = runResultFromProcessingOutput( results );
  if ( !r.ok )
  {
    r.error = QStringLiteral( "Processing 结果缺少 OUTPUT 路径" );
    if ( error )
      *error = r.error;
    return r;
  }

  for ( const AlgoPort &p : spec.outputs )
  {
    if ( p.kind == QLatin1String( "string" ) && results.contains( p.id ) )
      r.diagnostics.insert( p.id, results.value( p.id ) );
  }
  for ( auto it = results.constBegin(); it != results.constEnd(); ++it )
  {
    if ( !r.diagnostics.contains( it.key() ) )
      r.diagnostics.insert( it.key(), it.value() );
  }
  return r;
}

AlgorithmRunResult ProcessingAlgorithmAdapter::runById(
    const QString &algorithmId, const QVariantMap &paramValues, QString *error,
    const QgisProcessingService::ProcessingHooks &hooks )
{
  const AlgorithmSpec spec = AlgorithmSpecRegistry::spec( algorithmId );
  if ( spec.id.isEmpty() )
  {
    AlgorithmSpec fallback;
    fallback.id = algorithmId;
    fallback.engine = QStringLiteral( "processing" );
    fallback.complete = false;
    AlgoPort o;
    o.id = QStringLiteral( "OUTPUT" );
    o.kind = QStringLiteral( "raster" );
    fallback.outputs = { o };
    return run( fallback, paramValues, error, hooks );
  }
  return run( spec, paramValues, error, hooks );
}

AlgorithmRunResult NativeCppAdapter::run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                                          QString *error )
{
  Q_UNUSED( paramValues );
  AlgorithmRunResult r;
  r.ok = false;
  if ( spec.id == QLatin1String( "geostat:ordinary_kriging" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：geostat:ordinary_kriging 请使用类型化 "
        "runOrdinaryKriging(OrdinaryKrigingInputs)" );
  }
  else
  {
    r.error = QStringLiteral( "NativeCppAdapter 尚未实现：%1（TODO）" ).arg( spec.id );
  }
  if ( error )
    *error = r.error;
  return r;
}

AlgorithmRunResult NativeCppAdapter::runOrdinaryKriging( const OrdinaryKrigingInputs &in,
                                                         QString *error )
{
  AlgorithmRunResult r;
  if ( in.estimatePath.isEmpty() || !in.grid.isValid() )
  {
    r.ok = false;
    r.error = QStringLiteral( "ordinary_kriging：estimatePath 或 GridSpec 无效" );
    if ( error )
      *error = r.error;
    return r;
  }

  const paleo::geostat::KrigingResult kr =
      paleo::geostat::ordinaryKriging( in.samples, in.grid, in.model, in.params, in.control );
  if ( kr.status == paleo::geostat::Status::Cancelled )
  {
    r.ok = false;
    r.error = QStringLiteral( "已取消" );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( kr.status != paleo::geostat::Status::Ok )
  {
    r.ok = false;
    r.error = QStringLiteral( "克里金失败：%1" ).arg( QString::fromStdString( kr.message ) );
    if ( error )
      *error = r.error;
    return r;
  }

  const QString wkt = in.canonicalCrsWkt.isEmpty() ? DataCatalog::localGridCrsWkt()
                                                   : in.canonicalCrsWkt;
  if ( !writeFloatRaster( in.estimatePath, kr.estimate, in.grid.cols, in.grid.rows,
                          in.geoTransform.data(), in.crs, wkt ) )
  {
    r.ok = false;
    r.error = QStringLiteral( "克里金估值栅格写盘失败" );
    if ( error )
      *error = r.error;
    return r;
  }
  AlgorithmOutputArtifact estimate;
  estimate.portId = QStringLiteral( "OUTPUT" );
  estimate.absolutePath = in.estimatePath;
  estimate.suggestedAssetType = QStringLiteral( "single_factor_raster" );
  r.artifacts.append( estimate );

  if ( !in.variancePath.isEmpty() )
  {
    if ( !writeFloatRaster( in.variancePath, kr.variance, in.grid.cols, in.grid.rows,
                            in.geoTransform.data(), in.crs, wkt ) )
    {
      r.ok = false;
      r.error = QStringLiteral( "克里金方差栅格写盘失败" );
      if ( error )
        *error = r.error;
      return r;
    }
    AlgorithmOutputArtifact variance;
    variance.portId = QStringLiteral( "VARIANCE" );
    variance.absolutePath = in.variancePath;
    variance.suggestedAssetType = QStringLiteral( "single_factor_variance" );
    r.artifacts.append( variance );
  }

  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "finite_cells" ), kr.finiteCells );
  r.diagnostics.insert( QStringLiteral( "nodata_cells" ), kr.nodataCells );
  r.diagnostics.insert( QStringLiteral( "solver_failures" ), kr.solverFailures );
  r.diagnostics.insert( QStringLiteral( "merged_duplicates" ), kr.mergedDuplicates );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "geostat:ordinary_kriging" ) );
  return r;
}

} // namespace paleo
