// 层：功能
#include "algorithmadapters.h"
#include "algorithmpublish.h"

#include "../services/nativerunners.h"

#include "../algorithms/rasterout.h"
#include "../catalog/datacatalog.h"

#include <gdal.h>

#include <cmath>
#include <cstring>
#include <limits>

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

QString crsWktOrLocal( const QString &canonicalCrsWkt )
{
  return canonicalCrsWkt.isEmpty() ? DataCatalog::localGridCrsWkt() : canonicalCrsWkt;
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
  const QString id = spec.id;
  if ( id == QLatin1String( "geostat:ordinary_kriging" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：geostat:ordinary_kriging 请使用类型化 "
        "runOrdinaryKriging(OrdinaryKrigingInputs)" );
  }
  else if ( id == QLatin1String( "geostat:ordinary_cokriging" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：geostat:ordinary_cokriging 请使用类型化 "
        "runOrdinaryCoKriging(OrdinaryCoKrigingInputs)" );
  }
  else if ( id == QLatin1String( "geostat:sgs" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：geostat:sgs 请使用类型化 runSgs(SgsInputs)" );
  }
  else if ( id == QLatin1String( "hztrack:track_section" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：hztrack:track_section 请使用类型化 "
        "runTrackSection(TrackSectionInputs)" );
  }
  else if ( id == QLatin1String( "inversion:bandlimited" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：inversion:bandlimited 请使用类型化 "
        "runBandlimited(BandlimitedInputs)" );
  }
  else if ( id == QLatin1String( "faciesmapping:synthesize" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：faciesmapping:synthesize 请使用类型化 "
        "runSynthesizeFacies(SynthesizeFaciesInputs)" );
  }
  else if ( id == QLatin1String( "inversion:sparse_spike" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：inversion:sparse_spike 请使用类型化 "
        "runSparseSpike(SparseSpikeInputs)" );
  }
  else if ( id == QLatin1String( "wellsiting:diagnose_coverage" ) )
  {
    r.error = QStringLiteral(
        "NativeCppAdapter：wellsiting:diagnose_coverage 请使用类型化 "
        "runDiagnoseCoverage(DiagnoseCoverageInputs)" );
  }
  else
  {
    r.error = QStringLiteral( "NativeCppAdapter 尚未实现：%1（TODO）" ).arg( id );
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

  const QString wkt = crsWktOrLocal( in.canonicalCrsWkt );
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

AlgorithmRunResult NativeCppAdapter::runOrdinaryCoKriging( const OrdinaryCoKrigingInputs &in,
                                                           QString *error )
{
  AlgorithmRunResult r;
  if ( in.estimatePath.isEmpty() || !in.grid.isValid() )
  {
    r.ok = false;
    r.error = QStringLiteral( "ordinary_cokriging：estimatePath 或 GridSpec 无效" );
    if ( error )
      *error = r.error;
    return r;
  }

  const paleo::geostat::CoKrigingResult cr = paleo::geostat::ordinaryCoKriging(
      in.primary, in.secondary, in.grid, in.model, in.params, in.control );
  if ( cr.status == paleo::geostat::Status::Cancelled )
  {
    r.ok = false;
    r.error = QStringLiteral( "已取消" );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( cr.status != paleo::geostat::Status::Ok )
  {
    r.ok = false;
    r.error = QStringLiteral( "协克里金失败：%1" ).arg( QString::fromStdString( cr.message ) );
    if ( error )
      *error = r.error;
    return r;
  }

  const QString wkt = crsWktOrLocal( in.canonicalCrsWkt );
  if ( !writeFloatRaster( in.estimatePath, cr.estimates, in.grid.cols, in.grid.rows,
                          in.geoTransform.data(), in.crs, wkt ) )
  {
    r.ok = false;
    r.error = QStringLiteral( "协克里金估值栅格写盘失败" );
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
    if ( !writeFloatRaster( in.variancePath, cr.variances, in.grid.cols, in.grid.rows,
                            in.geoTransform.data(), in.crs, wkt ) )
    {
      r.ok = false;
      r.error = QStringLiteral( "协克里金方差栅格写盘失败" );
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
  r.diagnostics.insert( QStringLiteral( "finite_cells" ), cr.finiteCells );
  r.diagnostics.insert( QStringLiteral( "nodata_cells" ), cr.nodataCells );
  r.diagnostics.insert( QStringLiteral( "solver_failures" ), cr.solverFailures );
  r.diagnostics.insert( QStringLiteral( "merged_primary_duplicates" ), cr.mergedPrimaryDuplicates );
  r.diagnostics.insert( QStringLiteral( "merged_secondary_duplicates" ),
                        cr.mergedSecondaryDuplicates );
  r.diagnostics.insert( QStringLiteral( "secondary_samples" ),
                        static_cast<int>( in.secondary.size() ) );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "geostat:ordinary_cokriging" ) );
  return r;
}

AlgorithmRunResult NativeCppAdapter::runSgs( const SgsInputs &in, QString *error )
{
  AlgorithmRunResult r;
  if ( in.meanPath.isEmpty() || !in.grid.isValid() )
  {
    r.ok = false;
    r.error = QStringLiteral( "sgs：meanPath 或 GridSpec 无效" );
    if ( error )
      *error = r.error;
    return r;
  }

  const paleo::geostat::SgsResult sr =
      paleo::geostat::sgs( in.samples, in.grid, in.model, in.params, in.control );
  if ( sr.status == paleo::geostat::Status::Cancelled )
  {
    r.ok = false;
    r.error = QStringLiteral( "已取消" );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( sr.status != paleo::geostat::Status::Ok )
  {
    r.ok = false;
    r.error = QStringLiteral( "SGS 失败：%1" ).arg( QString::fromStdString( sr.message ) );
    if ( error )
      *error = r.error;
    return r;
  }

  const std::size_t cells =
      static_cast<std::size_t>( in.grid.rows ) * static_cast<std::size_t>( in.grid.cols );
  std::vector<double> mean( cells, std::numeric_limits<double>::quiet_NaN() );
  std::vector<double> spread( cells, std::numeric_limits<double>::quiet_NaN() );
  for ( std::size_t cell = 0; cell < cells; ++cell )
  {
    double sum = 0;
    double sumSq = 0;
    int n = 0;
    for ( const std::vector<double> &realization : sr.realizations )
    {
      const double value = realization[cell];
      if ( std::isfinite( value ) )
      {
        sum += value;
        sumSq += value * value;
        ++n;
      }
    }
    if ( n > 0 )
    {
      const double m = sum / n;
      mean[cell] = m;
      spread[cell] = n > 1 ? std::sqrt( std::max( 0.0, sumSq / n - m * m ) ) : 0.0;
    }
  }

  const QString wkt = crsWktOrLocal( in.canonicalCrsWkt );
  if ( !writeFloatRaster( in.meanPath, mean, in.grid.cols, in.grid.rows, in.geoTransform.data(),
                          in.crs, wkt ) )
  {
    r.ok = false;
    r.error = QStringLiteral( "SGS 均值栅格写盘失败" );
    if ( error )
      *error = r.error;
    return r;
  }
  AlgorithmOutputArtifact meanArt;
  meanArt.portId = QStringLiteral( "OUTPUT" );
  meanArt.absolutePath = in.meanPath;
  meanArt.suggestedAssetType = QStringLiteral( "single_factor_raster" );
  r.artifacts.append( meanArt );

  if ( !in.stdPath.isEmpty() )
  {
    if ( !writeFloatRaster( in.stdPath, spread, in.grid.cols, in.grid.rows, in.geoTransform.data(),
                            in.crs, wkt ) )
    {
      r.ok = false;
      r.error = QStringLiteral( "SGS 标准差栅格写盘失败" );
      if ( error )
        *error = r.error;
      return r;
    }
    AlgorithmOutputArtifact stdArt;
    stdArt.portId = QStringLiteral( "STD" );
    stdArt.absolutePath = in.stdPath;
    stdArt.suggestedAssetType = QStringLiteral( "single_factor_variance" );
    r.artifacts.append( stdArt );
  }

  QStringList memberPaths;
  if ( in.persistRealizations && !in.memberPathPrefix.isEmpty() )
  {
    for ( std::size_t k = 0; k < sr.realizations.size(); ++k )
    {
      const QString memberPath =
          in.memberPathPrefix
          + QStringLiteral( "%1.tif" ).arg( k, 3, 10, QLatin1Char( '0' ) );
      if ( !writeFloatRaster( memberPath, sr.realizations[k], in.grid.cols, in.grid.rows,
                              in.geoTransform.data(), in.crs, wkt ) )
      {
        r.ok = false;
        r.error = QStringLiteral( "SGS 成员栅格写盘失败（成员 %1）" ).arg( k );
        if ( error )
          *error = r.error;
        return r;
      }
      memberPaths << memberPath;
      AlgorithmOutputArtifact mem;
      mem.portId = QStringLiteral( "MEMBER_%1" ).arg( k );
      mem.absolutePath = memberPath;
      mem.suggestedAssetType = QStringLiteral( "realization_member" );
      r.artifacts.append( mem );
    }
  }

  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "finite_cells" ), sr.finiteCells );
  r.diagnostics.insert( QStringLiteral( "nodata_cells" ), sr.nodataCells );
  r.diagnostics.insert( QStringLiteral( "solver_failures" ), sr.solverFailures );
  r.diagnostics.insert( QStringLiteral( "realizations" ), in.params.nRealizations );
  r.diagnostics.insert( QStringLiteral( "seed" ), static_cast<qulonglong>( in.params.seed ) );
  r.diagnostics.insert( QStringLiteral( "sample_mean" ), sr.sampleMean );
  r.diagnostics.insert( QStringLiteral( "sample_std" ), sr.sampleStd );
  r.diagnostics.insert( QStringLiteral( "member_paths" ), memberPaths );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "geostat:sgs" ) );
  return r;
}

AlgorithmRunResult NativeCppAdapter::runTrackSection( const TrackSectionInputs &in,
                                                      paleo::hztrack::TrackResult *out,
                                                      QString *error )
{
  return paleo::native_runners::runTrackSection( in.section, in.nTraces, in.nSamples, in.seed,
                                                 in.options, in.coherence, in.cancelled, out,
                                                 error );
}

AlgorithmRunResult NativeCppAdapter::runBandlimited( const BandlimitedInputs &in,
                                                     paleo::inversion::BandlimitedResult *out,
                                                     QString *error )
{
  AlgorithmRunResult r;
  if ( !in.trace || in.n <= 0 )
  {
    r.ok = false;
    r.error = QStringLiteral( "inversion:bandlimited：trace 无效" );
    if ( error )
      *error = r.error;
    return r;
  }
  paleo::inversion::BandlimitedResult local;
  paleo::inversion::BandlimitedResult &br = out ? *out : local;
  br = paleo::inversion::bandlimitedInversion( in.trace, in.n, in.sampleIntervalMs, in.wavelet,
                                               in.lowFreq, in.options );
  if ( !br.ok )
  {
    r.ok = false;
    r.error = QStringLiteral( "带限反演失败：%1" ).arg( QString::fromStdString( br.reason ) );
    if ( error )
      *error = r.error;
    return r;
  }
  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "low_freq_merged" ), br.lowFreqMerged );
  r.diagnostics.insert( QStringLiteral( "low_freq_variance_fraction" ), br.lowFreqVarianceFraction );
  r.diagnostics.insert( QStringLiteral( "amplitude_calibrated" ), br.amplitudeCalibrated );
  r.diagnostics.insert( QStringLiteral( "clamped_fraction" ), br.clampedFraction );
  r.diagnostics.insert( QStringLiteral( "impedance_samples" ),
                        static_cast<int>( br.impedance.size() ) );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "inversion:bandlimited" ) );
  return r;
}

AlgorithmRunResult NativeCppAdapter::runSynthesizeFacies( const SynthesizeFaciesInputs &in,
                                                          paleo::faciesmapping::SynthesisResult *out,
                                                          QString *error )
{
  AlgorithmRunResult r;
  if ( !in.regions || !in.sources )
  {
    r.ok = false;
    r.error = QStringLiteral( "faciesmapping:synthesize：regions/sources 指针为空" );
    if ( error )
      *error = r.error;
    return r;
  }
  paleo::faciesmapping::SynthesisResult local;
  paleo::faciesmapping::SynthesisResult &sr = out ? *out : local;
  sr = paleo::faciesmapping::synthesizeFaciesRegions( *in.regions, *in.sources, in.options,
                                                      in.control );
  if ( sr.status == paleo::faciesmapping::Status::Cancelled )
  {
    r.ok = false;
    r.error = QStringLiteral( "已取消" );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( sr.status != paleo::faciesmapping::Status::Ok )
  {
    r.ok = false;
    r.error = QStringLiteral( "证据合成失败：%1" ).arg( QString::fromStdString( sr.message ) );
    if ( error )
      *error = r.error;
    return r;
  }
  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "region_count" ), static_cast<int>( sr.regions.size() ) );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "faciesmapping:synthesize" ) );
  for ( auto it = sr.diagnostics.constBegin(); it != sr.diagnostics.constEnd(); ++it )
    r.diagnostics.insert( it.key(), it.value() );
  return r;
}


AlgorithmRunResult NativeCppAdapter::runSparseSpike( const SparseSpikeInputs &in,
                                                     paleo::inversion::SparseSpikeResult *out,
                                                     QString *error )
{
  AlgorithmRunResult r;
  if ( !in.trace || in.n <= 0 || !in.wavelet )
  {
    r.ok = false;
    r.error = QStringLiteral( "inversion:sparse_spike：trace/wavelet 无效" );
    if ( error )
      *error = r.error;
    return r;
  }
  paleo::inversion::SparseSpikeResult local;
  paleo::inversion::SparseSpikeResult &sr = out ? *out : local;
  sr = paleo::inversion::sparseSpikeInversion( in.trace, in.n, in.sampleIntervalMs, *in.wavelet,
                                               in.lowFreq, in.options );
  if ( !sr.ok )
  {
    r.ok = false;
    r.error = QStringLiteral( "稀疏脉冲反演失败：%1" ).arg( QString::fromStdString( sr.reason ) );
    if ( error )
      *error = r.error;
    return r;
  }
  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "residual_energy_ratio" ), sr.residualEnergyRatio );
  r.diagnostics.insert( QStringLiteral( "iterations" ), sr.iterations );
  r.diagnostics.insert( QStringLiteral( "converged" ), sr.converged );
  r.diagnostics.insert( QStringLiteral( "lambda_used" ), sr.lambdaUsed );
  r.diagnostics.insert( QStringLiteral( "amplitude_calibrated" ), sr.amplitudeCalibrated );
  r.diagnostics.insert( QStringLiteral( "clamped_fraction" ), sr.clampedFraction );
  r.diagnostics.insert( QStringLiteral( "impedance_samples" ),
                        static_cast<int>( sr.impedance.size() ) );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "inversion:sparse_spike" ) );
  return r;
}

AlgorithmRunResult NativeCppAdapter::runDiagnoseCoverage(
    const DiagnoseCoverageInputs &in, paleo::wellsiting::CoverageReport *outReport,
    paleo::wellsiting::SitingField *outField, QString *error )
{
  AlgorithmRunResult r;
  if ( !in.domain || !in.wells )
  {
    r.ok = false;
    r.error = QStringLiteral( "wellsiting:diagnose_coverage：domain/wells 指针为空" );
    if ( error )
      *error = r.error;
    return r;
  }
  paleo::wellsiting::SitingField fieldLocal;
  paleo::wellsiting::SitingField &field = outField ? *outField : fieldLocal;
  field = paleo::wellsiting::sampleField( *in.domain, *in.wells, in.options );
  paleo::wellsiting::CoverageReport reportLocal;
  paleo::wellsiting::CoverageReport &report = outReport ? *outReport : reportLocal;
  report = paleo::wellsiting::describeField( field );
  if ( field.status != paleo::wellsiting::Status::Ok )
  {
    r.ok = false;
    r.error = QString::fromStdString( field.message.empty() ? report.message : field.message );
    if ( error )
      *error = r.error;
    r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                          QStringLiteral( "wellsiting:diagnose_coverage" ) );
    return r;
  }
  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "hole_count" ), report.holeCount );
  r.diagnostics.insert( QStringLiteral( "hole_area_total" ), report.holeAreaTotal );
  r.diagnostics.insert( QStringLiteral( "coverage_ratio" ), report.coverageRatio );
  r.diagnostics.insert( QStringLiteral( "domain_area" ), field.domainArea );
  r.diagnostics.insert( QStringLiteral( "domain_mean_distance" ),
                        std::isfinite( report.domainMeanDistance )
                            ? QVariant( report.domainMeanDistance )
                            : QVariant() );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "wellsiting:diagnose_coverage" ) );
  return r;
}

} // namespace paleo
