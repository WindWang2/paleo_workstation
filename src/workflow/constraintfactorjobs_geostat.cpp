// 层：功能
// 方向57：从 constraintfactorjobs.cpp 按函数族析出。方法定义逐字搬迁，
// 类契约仍在 workflows.h（公共 API 零改动）；族间共享辅助经
// constraintfactorjobs_internal.h。
#include "workflows.h"
#include "constraintworkflow_internal.h"
#include "workflows_internal.h"
#include "../algorithms/geostat/kriging.h"     // KrigingParams / KrigingResult（方向18）
#include "../algorithms/geostat/cokriging.h"   // CoKrigingModel / ordinaryCoKriging
#include "../algorithms/geostat/sgs.h"         // SgsParams / SgsResult（方向18）
#include "../algorithms/geostat/variogram.h"   // 变差函数模型（方向18）
#include "../algorithms/rasterout.h"          // PaleoRasterOut / createFloatRaster
#include "../algorithms/ensemblestats.h"     // StatsRequest（方向47 集合统计派生）
#include "../catalog/datacatalog.h"
#include "../algorithms/singlefactor/cartographicworkfile.h"
#include "../domain/arearules.h"
#include "../domain/singlefactorrequest.h"  // 制图工作场不进融合/分相
#include "../io/constraintstore.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/factorcontour.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/qgiseditingservice.h"    // 拓扑提交门（geometryCommitError）
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisstyleservice.h"      // applyFaciesBoundaryStyle
#include "../services/jobrunner.h"
#include "../services/singlefactordef.h"
#include "boundarysemantics.h"             // 相界地质语义类型词表
#include "derivedassets.h"
#include "mappingworkflow.h"
#include "realizationworkflow.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QVariantList>
#include <qgscoordinatereferencesystem.h>
#include <qgsfield.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectorfilewriter.h> // WS-C：参与井点 samples GPKG 写出
#include <qgsvectorlayer.h>
#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <variant>
#include "constraintfactorjobs_internal.h"

using namespace paleo::constraint_detail;

// ---- 方向18：克里金 / SGS 三段式（LocalDirectionJob 同构）----

namespace
{
// 克里金/SGS 的降级阈值：有效样本少于 8 口时变差函数欠定，自动降级
// 约束 IDW 并如实记 method_actual="idw"。降级是能力事实，不是性能裁剪
//（局部邻域克里金对大样本量无悬崖，见 docs/progress/geostat-methods.md）。
constexpr int kGeostatMinSamples = 8;
// 工作流层栅格预算：交互式成图上限 4M 格（核层另有 1e8 硬闸）。
constexpr qint64 kGeostatMaxWorkflowCells = 4'000'000;

paleo::geostat::VariogramModelType variogramTypeFor( const QString &name )
{
  if ( name == QLatin1String( "exponential" ) )
    return paleo::geostat::VariogramModelType::Exponential;
  if ( name == QLatin1String( "gaussian" ) )
    return paleo::geostat::VariogramModelType::Gaussian;
  return paleo::geostat::VariogramModelType::Spherical;
}

QString variogramTypeName( paleo::geostat::VariogramModelType type )
{
  switch ( type )
  {
    case paleo::geostat::VariogramModelType::Spherical:
      return QStringLiteral( "spherical" );
    case paleo::geostat::VariogramModelType::Exponential:
      return QStringLiteral( "exponential" );
    case paleo::geostat::VariogramModelType::Gaussian:
      return QStringLiteral( "gaussian" );
  }
  return QStringLiteral( "spherical" );
}

// NaN → nodata 的 float32 写出（mincurvature 同款 geoTransform/CRS 口径）。
bool writeFloatRaster( const QString &path, const std::vector<double> &values, int cols, int rows,
                       const double geoTransform[6], const QgsCoordinateReferenceSystem &crs,
                       const QString &canonicalCrsWkt = QString() )
{
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( path, cols, rows, geoTransform, crs, -9999.0, canonicalCrsWkt );
  if ( !ds )
    return false;
  std::vector<float> row( static_cast<std::size_t>( cols ) );
  bool ok = true;
  for ( int r = 0; r < rows && ok; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double value = values[static_cast<std::size_t>( r ) * cols + c];
      row[static_cast<std::size_t>( c )] = std::isfinite( value ) ? static_cast<float>( value ) : -9999.0f;
    }
    if ( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Write, 0, r, cols, 1,
                       row.data(), cols, 1, GDT_Float32, 0, 0 ) != CE_None )
      ok = false;
  }
  GDALClose( ds );
  return ok;
}

QVariantMap variogramExtra( const paleo::geostat::VariogramModel &model, double fitR2, double fitRmse )
{
  QVariantMap map;
  map.insert( QStringLiteral( "model_type" ), variogramTypeName( model.type ) );
  map.insert( QStringLiteral( "nugget" ), model.nugget );
  map.insert( QStringLiteral( "sill" ), model.sill );
  map.insert( QStringLiteral( "range" ), model.range );
  map.insert( QStringLiteral( "anisotropy_ratio" ), model.anisotropyRatio );
  map.insert( QStringLiteral( "azimuth" ), model.azimuthDeg );
  map.insert( QStringLiteral( "fit_r2" ), fitR2 );
  map.insert( QStringLiteral( "fit_rmse" ), fitRmse );
  return map;
}

bool writeJsonFile( const QString &path, const QVariantMap &payload )
{
  QFile file( path );
  if ( !file.open( QIODevice::WriteOnly ) )
    return false;
  file.write( QJsonDocument( QJsonObject::fromVariantMap( payload ) ).toJson( QJsonDocument::Compact ) );
  return true;
}
} // namespace

bool ConstraintWorkflow::prepareGeostatJob( const QString &horizon, const QString &factorId,
                                            const QString &method, const QVariantMap &inputParams,
                                            GeostatJob *job, QString *error )
{
  QVariantMap params = inputParams;
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少地质统计任务" ) );
    return false;
  }
  if ( method != QLatin1String( "kriging" ) && method != QLatin1String( "sgs" ) &&
       method != QLatin1String( "cokriging" ) )
  {
    paleo::workflow_detail::setError( error, tr( "未知地质统计方法：%1" ).arg( method ) );
    return false;
  }
  if (!prepareFactorInputs(horizon, factorId, params, error))
    return false;
  *job = GeostatJob();
  job->generation = ++m_publishGeneration;
  job->horizon = horizon;
  job->factorId = factorId;
  job->method = method;
  job->params = params;

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  bool known = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( factorId, &known );
  if ( !known )
  {
    paleo::workflow_detail::setError( error, tr( "未知单因素 id：%1" ).arg( factorId ) );
    return false;
  }
  job->field = params.value( QStringLiteral( "field" ), def.defaultParams.value( QStringLiteral( "field" ) ) ).toString();
  job->cellSize = params.value( QStringLiteral( "cellSize" ), def.defaultParams.value( QStringLiteral( "cellSize" ), 1.0 ) )
                      .toDouble();
  if ( job->field.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "插值字段为空" ) );
    return false;
  }
  if ( !( job->cellSize > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }
  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon ) : overridePoints;
  if ( pointsId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    paleo::workflow_detail::setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == pointsId )
    {
      job->wellUri = d.source;
      break;
    }
  }
  if ( job->wellUri.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "井点图层 %1 没有数据源" ).arg( pointsId ) );
    return false;
  }
  job->parentPaths << job->wellUri.section( QLatin1Char( '|' ), 0, 0 );
  if ( method == QLatin1String( "cokriging" ) )
  {
    const QString covariateLayerId = params.value( QStringLiteral( "covariateLayerId" ) ).toString();
    if ( covariateLayerId.trimmed().isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "协克里金方法需要有效的次级协变量图层 (covariateLayerId)" ) );
      return false;
    }
    QString instErr;
    QgsMapLayer *covLayer = layers->instantiate( covariateLayerId, &instErr );
    auto *covRaster = qobject_cast<QgsRasterLayer *>( covLayer );
    if ( !covRaster || !covRaster->isValid() )
    {
      paleo::workflow_detail::setError( error, tr( "协克里金方法需要有效的次级协变量图层 (covariateLayerId)" ) );
      return false;
    }
    job->parentPaths << covRaster->source().section( QLatin1Char( '|' ), 0, 0 );
  }
  job->prepared = true;
  return true;
}

bool ConstraintWorkflow::computeGeostatJob( GeostatJob *job, const std::function<bool()> &cancelled,
                                            const std::function<void( double )> &progress )
{
  if ( !job || !job->prepared )
  {
    if ( job )
      job->error = tr( "地质统计任务尚未准备" );
    return false;
  }
  QgisProcessingService *proc = m_proc.data();
  if ( !proc )
  {
    job->error = tr( "constraint workflow is not bound to services" );
    return false;
  }
  if ( cancelled && cancelled() )
  {
    job->error = tr( "已取消" );
    return false;
  }

  auto wells = std::make_unique<QgsVectorLayer>( job->wellUri, QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  if ( !wells || !wells->isValid() )
  {
    job->error = tr( "井点图层无法读取" );
    return false;
  }
  const int fieldIndex = wells->fields().lookupField( job->field );
  if ( fieldIndex < 0 )
  {
    job->error = tr( "井点图层没有字段 %1" ).arg( job->field );
    return false;
  }
  std::vector<paleo::geostat::Sample> samples;
  QgsFeature feature;
  QgsFeatureIterator it = wells->getFeatures( QgsFeatureRequest().setSubsetOfAttributes(
      QgsAttributeList{ fieldIndex } ) );
  while ( it.nextFeature( feature ) )
  {
    const QgsGeometry geometry = feature.geometry();
    if ( geometry.isNull() || geometry.isEmpty() )
      continue;
    const QgsPointXY point = geometry.isMultipart() ? geometry.asMultiPoint().value( 0 ) : geometry.asPoint();
    const QVariant value = feature.attribute( fieldIndex );
    const double z = value.toDouble();
    if ( !std::isfinite( point.x() ) || !std::isfinite( point.y() ) || !std::isfinite( z ) )
      continue;
    samples.push_back( paleo::geostat::Sample{ point.x(), point.y(), z } );
  }
  if ( samples.empty() )
  {
    job->error = tr( "井点图层没有可用样本（字段 %1）" ).arg( job->field );
    return false;
  }

  const QString tempDir = QDir( QDir::tempPath() )
                              .filePath( QStringLiteral( "paleo-gs-%1" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
  if ( !QDir().mkpath( tempDir ) )
  {
    job->error = tr( "无法创建临时计算目录" );
    return false;
  }
  const auto cleanupTemp = [tempDir]() { QDir( tempDir ).removeRecursively(); };
  job->outputPath = QDir( tempDir ).filePath( QStringLiteral( "factor.tif" ) );
  job->qcPath = QDir( tempDir ).filePath( QStringLiteral( "qc.json" ) );

  QVariantMap qcParameters = job->params;
  qcParameters.insert( QStringLiteral( "field" ), job->field );
  qcParameters.insert( QStringLiteral( "cell_size" ), job->cellSize );

  // ---- 降级：样本不足阈值 → 约束 IDW，method_actual 如实标注 ----
  if ( static_cast<int>( samples.size() ) < kGeostatMinSamples )
  {
    QVariantMap runParams;
    runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
    runParams.insert( QStringLiteral( "FIELD" ), job->field );
    runParams.insert( QStringLiteral( "CELL_SIZE" ), job->cellSize );
    runParams.insert( QStringLiteral( "OUTPUT" ), job->outputPath );
    QgisProcessingService::ProcessingHooks hooks;
    hooks.cancelled = cancelled;
    hooks.progress = progress;
    QString runErr;
    const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_constraint_idw" ), runParams, &runErr, hooks );
    if ( results.isEmpty() || !QFile::exists( job->outputPath ) )
    {
      cleanupTemp();
      job->outputPath.clear();
      job->error = runErr.isEmpty() ? tr( "降级 IDW 未返回输出" ) : runErr;
      return false;
    }
    job->degraded = true;
    QVariantMap qc;
    QVariantMap parameters = qcParameters;
    parameters.insert( QStringLiteral( "method_actual" ), QStringLiteral( "idw" ) );
    parameters.insert( QStringLiteral( "degraded_reason" ),
                       QStringLiteral( "samples %1 < %2" ).arg( samples.size() ).arg( kGeostatMinSamples ) );
    qc.insert( QStringLiteral( "schema" ), QStringLiteral( "paleo_geostat_qc_v1" ) );
    qc.insert( QStringLiteral( "method" ), job->method );
    qc.insert( QStringLiteral( "method_actual" ), QStringLiteral( "idw" ) );
    qc.insert( QStringLiteral( "samples" ), static_cast<int>( samples.size() ) );
    qc.insert( QStringLiteral( "parameters" ), parameters );
    if ( !writeJsonFile( job->qcPath, qc ) )
    {
      cleanupTemp();
      job->error = tr( "QC 写盘失败" );
      return false;
    }
    job->ok = true;
    return true;
  }

  // ---- 网格：样本包围盒 + 2 像元边距 ----
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for ( const paleo::geostat::Sample &sample : samples )
  {
    minX = std::min( minX, sample.x );
    minY = std::min( minY, sample.y );
    maxX = std::max( maxX, sample.x );
    maxY = std::max( maxY, sample.y );
  }
  const double margin = 2.0 * job->cellSize;
  const double spanX = std::max( maxX - minX, job->cellSize );
  const double spanY = std::max( maxY - minY, job->cellSize );
  paleo::geostat::GridSpec grid;
  grid.pixelWidth = job->cellSize;
  grid.pixelHeight = -job->cellSize;
  grid.cols = static_cast<int>( std::ceil( ( spanX + 2 * margin ) / job->cellSize ) );
  grid.rows = static_cast<int>( std::ceil( ( spanY + 2 * margin ) / job->cellSize ) );
  grid.originX = ( minX + maxX ) / 2.0 - grid.cols * job->cellSize / 2.0;
  grid.originY = ( minY + maxY ) / 2.0 + grid.rows * job->cellSize / 2.0;
  grid.crs = wells->sourceCrs().toWkt().toStdString();
  if ( static_cast<qint64>( grid.rows ) * grid.cols > kGeostatMaxWorkflowCells )
  {
    cleanupTemp();
    job->error = tr( "地质统计栅格 %1×%2 超过工作流预算 %3 格——像元过细或井域过大" )
                     .arg( grid.cols )
                     .arg( grid.rows )
                     .arg( static_cast<int>( kGeostatMaxWorkflowCells ) );
    return false;
  }

  // ---- 变差函数：显式参数或自动拟合（可选方向各向异性） ----
  const QString modelName = job->params.value( QStringLiteral( "variogramModel" ), QStringLiteral( "spherical" ) ).toString();
  const paleo::geostat::VariogramModelType modelType = variogramTypeFor( modelName );
  const double explicitNugget = job->params.value( QStringLiteral( "nugget" ), 0.0 ).toDouble();
  const double explicitSill = job->params.value( QStringLiteral( "sill" ), 0.0 ).toDouble();
  const double explicitRange = job->params.value( QStringLiteral( "range" ), 0.0 ).toDouble();
  const double azimuthParam = job->params.value( QStringLiteral( "azimuth" ), -1.0 ).toDouble();
  const int maxPoints = job->params.value( QStringLiteral( "maxPoints" ), 16 ).toInt();
  const bool hasAzimuth = azimuthParam >= 0.0 && azimuthParam <= 360.0;

  paleo::geostat::VariogramModel model;
  model.type = modelType;
  double fitR2 = 0;
  double fitRmse = 0;
  bool autoFit = true;
  if ( explicitRange > 0 && explicitSill > 0 )
  {
    model.nugget = std::max( 0.0, explicitNugget );
    model.sill = explicitSill;
    model.range = explicitRange;
    model.anisotropyRatio = std::max( 1.0, job->params.value( QStringLiteral( "anisotropyRatio" ), 1.0 ).toDouble() );
    if ( hasAzimuth )
      model.azimuthDeg = azimuthParam;
    autoFit = false;
  }
  else
  {
    const double area = spanX * spanY;
    const double lag = std::max( job->cellSize, std::sqrt( area / samples.size() ) );
    const int nLags = 12;
    if ( hasAzimuth )
    {
      paleo::geostat::VariogramDirection along;
      along.omnidirectional = false;
      along.azimuthDeg = azimuthParam;
      along.toleranceDeg = 22.5;
      paleo::geostat::VariogramDirection across = along;
      across.azimuthDeg = std::fmod( azimuthParam + 90.0, 360.0 );
      const paleo::geostat::VariogramFit fitAlong =
          paleo::geostat::fitVariogram( paleo::geostat::experimentalVariogram( samples, lag, nLags, along ), modelType );
      const paleo::geostat::VariogramFit fitAcross =
          paleo::geostat::fitVariogram( paleo::geostat::experimentalVariogram( samples, lag, nLags, across ), modelType );
      if ( fitAlong.status != paleo::geostat::Status::Ok || fitAcross.status != paleo::geostat::Status::Ok )
      {
        cleanupTemp();
        job->error = tr( "方向变差函数拟合失败：%1 / %2" )
                         .arg( QString::fromStdString( fitAlong.message ), QString::fromStdString( fitAcross.message ) );
        return false;
      }
      model = fitAlong.model;
      model.azimuthDeg = azimuthParam;
      model.anisotropyRatio = fitAcross.model.range > 0
                                   ? std::clamp( fitAlong.model.range / fitAcross.model.range, 1.0, 8.0 )
                                   : 1.0;
      fitR2 = fitAlong.r2;
      fitRmse = fitAlong.rmse;
    }
    else
    {
      const paleo::geostat::VariogramFit fit =
          paleo::geostat::fitVariogram( paleo::geostat::experimentalVariogram( samples, lag, nLags ), modelType );
      if ( fit.status != paleo::geostat::Status::Ok )
      {
        cleanupTemp();
        job->error = tr( "变差函数拟合失败：%1" ).arg( QString::fromStdString( fit.message ) );
        return false;
      }
      model = fit.model;
      fitR2 = fit.r2;
      fitRmse = fit.rmse;
    }
  }
  if ( !( model.range > 0 ) || model.nugget < 0 || model.sill < 0 )
  {
    cleanupTemp();
    job->error = tr( "变差函数模型参数无效" );
    return false;
  }
  if ( model.nugget + model.sill <= 0 )
  {
    // 恒定场：零基台 → 克氏方程组全零无解。如实拒绝，不产空栅格。
    cleanupTemp();
    job->error = tr( "字段 %1 在井点上是常量（零基台），克里金无空间结构可解" ).arg( job->field );
    return false;
  }

  paleo::geostat::Control control;
  control.cancelled = cancelled;
  control.progress = progress;
  const double geoTransform[6] = { grid.originX, grid.pixelWidth, 0.0, grid.originY, 0.0, grid.pixelHeight };
  const QgsCoordinateReferenceSystem crs = wells->sourceCrs();

  QVariantMap counts;
  QVariantMap sgsNode;
  if ( job->method == QLatin1String( "kriging" ) )
  {
    paleo::geostat::KrigingParams krigingParams;
    krigingParams.maxPoints = maxPoints;
    const paleo::geostat::KrigingResult result =
        paleo::geostat::ordinaryKriging( samples, grid, model, krigingParams, control );
    if ( result.status == paleo::geostat::Status::Cancelled )
    {
      cleanupTemp();
      job->error = tr( "已取消" );
      return false;
    }
    if ( result.status != paleo::geostat::Status::Ok )
    {
      cleanupTemp();
      job->error = tr( "克里金失败：%1" ).arg( QString::fromStdString( result.message ) );
      return false;
    }
    job->supportPath = QDir( tempDir ).filePath( QStringLiteral( "variance.tif" ) );
    if ( !writeFloatRaster( job->outputPath, result.estimate, grid.cols, grid.rows, geoTransform, crs,
                             DataCatalog::localGridCrsWkt() ) ||
         !writeFloatRaster( job->supportPath, result.variance, grid.cols, grid.rows, geoTransform, crs,
                            DataCatalog::localGridCrsWkt() ) )
    {
      cleanupTemp();
      job->error = tr( "克里金栅格写盘失败" );
      return false;
    }
    counts.insert( QStringLiteral( "finite" ), result.finiteCells );
    counts.insert( QStringLiteral( "nodata" ), result.nodataCells );
    counts.insert( QStringLiteral( "solver_failures" ), result.solverFailures );
  }
  else if ( job->method == QLatin1String( "cokriging" ) )
  {
    const QString covariateLayerId = job->params.value( QStringLiteral( "covariateLayerId" ) ).toString();
    QgisLayerService *layers = m_layers.data();
    QgsRasterLayer *covRaster = nullptr;
    std::unique_ptr<QgsRasterLayer> ownedRaster;
    if ( layers )
      covRaster = qobject_cast<QgsRasterLayer *>( layers->instantiate( covariateLayerId ) );
    if ( !covRaster && !job->parentPaths.isEmpty() )
    {
      ownedRaster = std::make_unique<QgsRasterLayer>( job->parentPaths.last(), QStringLiteral( "covariate" ) );
      if ( ownedRaster && ownedRaster->isValid() )
        covRaster = ownedRaster.get();
    }
    if ( !covRaster || !covRaster->isValid() )
    {
      cleanupTemp();
      job->error = tr( "协克里金方法需要有效的次级协变量图层 (covariateLayerId)" );
      return false;
    }

    std::vector<double> primaryZ;
    std::vector<double> secondaryZ;
    primaryZ.reserve( samples.size() );
    secondaryZ.reserve( samples.size() );
    for ( const paleo::geostat::Sample &s : samples )
    {
      bool ok = false;
      const double v2 = covRaster->dataProvider()->sample( QgsPointXY( s.x, s.y ), 1, &ok );
      if ( ok && std::isfinite( v2 ) )
      {
        primaryZ.push_back( s.value );
        secondaryZ.push_back( v2 );
      }
    }

    double rho = 0.0;
    if ( job->params.contains( QStringLiteral( "crossCorrelation" ) ) )
    {
      rho = job->params.value( QStringLiteral( "crossCorrelation" ) ).toDouble();
    }
    else if ( job->params.contains( QStringLiteral( "cross_correlation" ) ) )
    {
      rho = job->params.value( QStringLiteral( "cross_correlation" ) ).toDouble();
    }
    else if ( primaryZ.size() >= 3 )
    {
      const std::size_t n = primaryZ.size();
      double sum1 = 0, sum2 = 0;
      for ( std::size_t i = 0; i < n; ++i )
      {
        sum1 += primaryZ[i];
        sum2 += secondaryZ[i];
      }
      const double mean1 = sum1 / static_cast<double>( n );
      const double mean2 = sum2 / static_cast<double>( n );
      double cov = 0, var1 = 0, var2 = 0;
      for ( std::size_t i = 0; i < n; ++i )
      {
        const double d1 = primaryZ[i] - mean1;
        const double d2 = secondaryZ[i] - mean2;
        cov += d1 * d2;
        var1 += d1 * d1;
        var2 += d2 * d2;
      }
      if ( var1 > 1e-12 && var2 > 1e-12 )
        rho = cov / std::sqrt( var1 * var2 );
      else
        rho = 0.0;
    }
    rho = std::clamp( rho, -0.99, 0.99 );

    std::vector<paleo::geostat::Sample> secondarySamples;
    const int totalCells = grid.cols * grid.rows;
    const int maxSec = job->params.value( QStringLiteral( "covariateMaxSamples" ), 2048 ).toInt();
    const int step = std::max( 1, static_cast<int>( std::ceil( std::sqrt( static_cast<double>( totalCells ) / std::max( 64, maxSec ) ) ) ) );
    for ( int r = 0; r < grid.rows; r += step )
    {
      for ( int c = 0; c < grid.cols; c += step )
      {
        const double gx = grid.cellCenterX( c );
        const double gy = grid.cellCenterY( r );
        bool ok = false;
        const double gv = covRaster->dataProvider()->sample( QgsPointXY( gx, gy ), 1, &ok );
        if ( ok && std::isfinite( gv ) )
          secondarySamples.push_back( paleo::geostat::Sample{ gx, gy, gv } );
      }
    }
    if ( secondarySamples.empty() )
    {
      cleanupTemp();
      job->error = tr( "协变量栅格在计算网格范围内无有效数据" );
      return false;
    }

    paleo::geostat::CoKrigingModel coModel;
    coModel.primary = model;
    // #326：次级自变差必须与交叉项同单位。交叉项是 ρ·γ1（主变量单位，
    // cokriging.h crossSemivariance），所以 γ22 也要落在主变量基台上——
    // 即 scaled MM1 的两基台相等那种退化。原实现把 γ22 的拱高换成协变量
    // 井点原始方差，地震属性与孔隙度/厚度差几个数量级时，方程组里混了两
    // 种单位：Pearson ρ 不再是那个 ρ，次级块还会主导条件数。测得的协变量
    // 方差只进 QC 参数，不改模型。
    coModel.secondary = model;
    coModel.secondary.nugget = 0.0;
    if ( !secondaryZ.empty() )
    {
      double secMean = 0;
      for ( double val : secondaryZ )
        secMean += val;
      secMean /= secondaryZ.size();
      double secVar = 0;
      for ( double val : secondaryZ )
        secVar += ( val - secMean ) * ( val - secMean );
      secVar /= secondaryZ.size();
      job->params.insert( QStringLiteral( "covariate_sample_variance" ),
                          QString::number( secVar, 'g', 10 ) );
    }
    coModel.crossCorrelation = rho;

    paleo::geostat::KrigingParams krigingParams;
    krigingParams.maxPoints = maxPoints;
    const paleo::geostat::CoKrigingResult result =
        paleo::geostat::ordinaryCoKriging( samples, secondarySamples, grid, coModel, krigingParams,
                                           [&control]( double frac ) -> bool {
                                             if ( control.cancelled && control.cancelled() )
                                               return false;
                                             if ( control.progress )
                                               control.progress( frac );
                                             return true;
                                           } );
    if ( result.status == paleo::geostat::Status::Cancelled )
    {
      cleanupTemp();
      job->error = tr( "已取消" );
      return false;
    }
    if ( result.status != paleo::geostat::Status::Ok )
    {
      cleanupTemp();
      job->error = tr( "协克里金失败：%1" ).arg( QString::fromStdString( result.message ) );
      return false;
    }
    job->supportPath = QDir( tempDir ).filePath( QStringLiteral( "variance.tif" ) );
    if ( !writeFloatRaster( job->outputPath, result.estimates, grid.cols, grid.rows, geoTransform, crs,
                            DataCatalog::localGridCrsWkt() ) ||
         !writeFloatRaster( job->supportPath, result.variances, grid.cols, grid.rows, geoTransform, crs,
                            DataCatalog::localGridCrsWkt() ) )
    {
      cleanupTemp();
      job->error = tr( "协克里金栅格写盘失败" );
      return false;
    }
    counts.insert( QStringLiteral( "finite" ), result.finiteCells );
    counts.insert( QStringLiteral( "nodata" ), result.nodataCells );
    counts.insert( QStringLiteral( "solver_failures" ), result.solverFailures );
    counts.insert( QStringLiteral( "secondary_samples" ), static_cast<int>( secondarySamples.size() ) );
    qcParameters.insert( QStringLiteral( "cross_correlation" ), rho );
    qcParameters.insert( QStringLiteral( "covariate_layer_id" ), covariateLayerId );
  }
  else
  {
    paleo::geostat::SgsParams sgsParams;
    sgsParams.nRealizations = job->params.value( QStringLiteral( "realizations" ), 4 ).toInt();
    sgsParams.seed = static_cast<std::uint64_t>( job->params.value( QStringLiteral( "seed" ), 42 ).toLongLong() );
    sgsParams.maxPoints = maxPoints;
    const paleo::geostat::SgsResult result = paleo::geostat::sgs( samples, grid, model, sgsParams, control );
    if ( result.status == paleo::geostat::Status::Cancelled )
    {
      cleanupTemp();
      job->error = tr( "已取消" );
      return false;
    }
    if ( result.status != paleo::geostat::Status::Ok )
    {
      cleanupTemp();
      job->error = tr( "SGS 失败：%1" ).arg( QString::fromStdString( result.message ) );
      return false;
    }
    const std::size_t cells = static_cast<std::size_t>( grid.rows ) * grid.cols;
    std::vector<double> mean( cells, std::numeric_limits<double>::quiet_NaN() );
    std::vector<double> spread( cells, std::numeric_limits<double>::quiet_NaN() );
    for ( std::size_t cell = 0; cell < cells; ++cell )
    {
      double sum = 0;
      double sumSq = 0;
      int n = 0;
      for ( const std::vector<double> &realization : result.realizations )
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
    job->supportPath = QDir( tempDir ).filePath( QStringLiteral( "std.tif" ) );
    if ( !writeFloatRaster( job->outputPath, mean, grid.cols, grid.rows, geoTransform, crs,
                             DataCatalog::localGridCrsWkt() ) ||
         !writeFloatRaster( job->supportPath, spread, grid.cols, grid.rows, geoTransform, crs,
                            DataCatalog::localGridCrsWkt() ) )
    {
      cleanupTemp();
      job->error = tr( "SGS 栅格写盘失败" );
      return false;
    }
    // 方向 47：成员持久化——各 realization 写成员栅格，发布段收编为
    // realization_set 集合（契约见 catalog/realizationset.h）。
    // params.persistRealizations=false → 只产均值+标准差旁路的旧形态。
    if ( job->params.value( QStringLiteral( "persistRealizations" ), true ).toBool() )
    {
      for ( std::size_t k = 0; k < result.realizations.size(); ++k )
      {
        const QString memberPath = QDir( tempDir ).filePath(
            QStringLiteral( "member_r%1.tif" ).arg( k, 3, 10, QLatin1Char( '0' ) ) );
        if ( !writeFloatRaster( memberPath, result.realizations[k], grid.cols,
                                grid.rows, geoTransform, crs, DataCatalog::localGridCrsWkt() ) )
        {
          cleanupTemp();
          job->error = tr( "SGS 成员栅格写盘失败（成员 %1）" ).arg( k );
          return false;
        }
        job->memberPaths << memberPath;
      }
      job->memberSeed = sgsParams.seed;
    }
    counts.insert( QStringLiteral( "finite" ), result.finiteCells );
    counts.insert( QStringLiteral( "nodata" ), result.nodataCells );
    counts.insert( QStringLiteral( "solver_failures" ), result.solverFailures );
    sgsNode.insert( QStringLiteral( "realizations" ), sgsParams.nRealizations );
    sgsNode.insert( QStringLiteral( "seed" ), static_cast<qulonglong>( sgsParams.seed ) );
    sgsNode.insert( QStringLiteral( "sample_mean" ), result.sampleMean );
    sgsNode.insert( QStringLiteral( "sample_std" ), result.sampleStd );
  }

  QVariantMap qc;
  qc.insert( QStringLiteral( "schema" ), QStringLiteral( "paleo_geostat_qc_v1" ) );
  qc.insert( QStringLiteral( "method" ), job->method );
  qc.insert( QStringLiteral( "method_actual" ), job->method );
  qc.insert( QStringLiteral( "samples" ), static_cast<int>( samples.size() ) );
  qc.insert( QStringLiteral( "auto_fit" ), autoFit );
  qc.insert( QStringLiteral( "model" ), variogramExtra( model, fitR2, fitRmse ) );
  QVariantMap gridNode;
  gridNode.insert( QStringLiteral( "cols" ), grid.cols );
  gridNode.insert( QStringLiteral( "rows" ), grid.rows );
  gridNode.insert( QStringLiteral( "cell_size" ), job->cellSize );
  qc.insert( QStringLiteral( "grid" ), gridNode );
  qc.insert( QStringLiteral( "counts" ), counts );
  if ( !sgsNode.isEmpty() )
    qc.insert( QStringLiteral( "sgs" ), sgsNode );
  qc.insert( QStringLiteral( "parameters" ), qcParameters );
  if ( !writeJsonFile( job->qcPath, qc ) )
  {
    cleanupTemp();
    job->error = tr( "QC 写盘失败" );
    return false;
  }
  job->ok = true;
  return true;
}

bool ConstraintWorkflow::publishGeostatJob( const GeostatJob &job, QString *error )
{
  const auto discardTemp = [&job]() {
    if ( job.outputPath.contains( QStringLiteral( "paleo-gs-" ) ) )
      QDir( QFileInfo( job.outputPath ).absolutePath() ).removeRecursively();
  };
  if ( !job.ok || job.outputPath.isEmpty() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, job.error.isEmpty() ? tr( "地质统计计算未返回输出路径" ) : job.error );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次地质统计成果" ) );
    return false;
  }
  bool known = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( job.factorId, &known );
  if ( !known )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "未知单因素 id：%1" ).arg( job.factorId ) );
    return false;
  }
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, job.horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( job.factorId, job.horizon ), &regErr );
  if ( !st.isValid() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const QString stagedSupport = job.supportPath.isEmpty() ? QString()
                                    : fileStem( st.absolutePath ) + QStringLiteral( ".support.tif" );
  const QString stagedQc = fileStem( st.absolutePath ) + QStringLiteral( ".qc.json" );
  auto discard = [&]() {
    removeIfPresent( st.absolutePath );
    removeIfPresent( stagedSupport );
    removeIfPresent( stagedQc );
    discardTemp();
  };
  if ( job.generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次地质统计成果" ) );
    return false;
  }
  if ( !QFile::copy( job.qcPath, stagedQc ) )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "地质统计 QC 无法写入成果目录" ) );
    return false;
  }
  if ( !stagedSupport.isEmpty() && !QFile::copy( job.supportPath, stagedSupport ) )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "地质统计旁路文件无法写入成果目录" ) );
    return false;
  }
  QString jsonErr;
  const QVariantMap qc = readJsonObject( stagedQc, &jsonErr );
  QVariantMap hashParams = qc.value( QStringLiteral( "parameters" ) ).toMap();
  if ( hashParams.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, jsonErr.isEmpty() ? tr( "地质统计成果缺少参数指纹输入" ) : jsonErr );
    return false;
  }
  QStringList parentIds = registrar.parentVersionIdsFor( job.parentPaths );
  parentIds.append( job.params.value( QStringLiteral( "parentVersionIds" ) ).toStringList() );
  parentIds.removeDuplicates();
  parentIds.sort();
  QVariantList parentList;
  for ( const QString &id : parentIds )
    parentList << id;
  hashParams.insert( QStringLiteral( "horizon" ), job.horizon );
  hashParams.insert( QStringLiteral( "factor_id" ), job.factorId );
  hashParams.insert( QStringLiteral( "parent_version_ids" ), parentList );
  const paleo::singlefactor::ParameterHash hash = paleo::singlefactor::parameterHash( hashParams );
  if ( !hash.ok )
  {
    discard();
    paleo::workflow_detail::setError( error, hash.error.isEmpty() ? tr( "参数指纹计算失败" ) : hash.error );
    return false;
  }
  QString shaErr;
  const QString qcSha = DataCatalog::sha256FileHex( stagedQc, &shaErr );
  QString supportSha;
  if ( !stagedSupport.isEmpty() )
  {
    supportSha = DataCatalog::sha256FileHex( stagedSupport, &shaErr );
    if ( supportSha.isEmpty() )
    {
      discard();
      paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "旁路文件 sha256 计算失败" ) : shaErr );
      return false;
    }
  }
  if ( qcSha.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "QC sha256 计算失败" ) : shaErr );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次地质统计成果" ) );
    return false;
  }
  const QDir projectDir( registrar.projectDir() );
  const QVariantMap counts = qc.value( QStringLiteral( "counts" ) ).toMap();
  const QString methodActual = job.degraded ? QStringLiteral( "idw" ) : job.method;
  const QString algorithmId = job.degraded
                                  ? QStringLiteral( "paleo:paleo_constraint_idw" )
                                  : QStringLiteral( "paleo:geostat_%1" ).arg( job.method );
  QVariantMap extra;
  extra.insert( QStringLiteral( "mapping_product" ), true );
  extra.insert( QStringLiteral( "layer_id" ), QStringLiteral( "product." ) + st.versionId );
  extra.insert( QStringLiteral( "manifest_layer_id" ),
                QStringLiteral( "factor.%1.%2" ).arg( job.horizon, job.factorId ) );
  extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
  extra.insert( QStringLiteral( "title" ), tr( "%1·%2" ).arg( def.title, job.horizon ) );
  extra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor" ) );
  extra.insert( QStringLiteral( "factor_id" ), job.factorId );
  extra.insert( QStringLiteral( "field" ), job.field );
  extra.insert( QStringLiteral( "cell_size" ), job.cellSize );
  extra.insert( QStringLiteral( "horizon" ), job.horizon );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  extra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
  extra.insert( QStringLiteral( "provenance_schema_version" ), 1 );
  extra.insert( QStringLiteral( "method" ), job.method );
  extra.insert( QStringLiteral( "method_actual" ), methodActual );
  extra.insert( QStringLiteral( "algorithm_id" ), algorithmId );
  extra.insert( QStringLiteral( "samples" ), qc.value( QStringLiteral( "samples" ) ) );
  extra.insert( QStringLiteral( "variogram" ), qc.value( QStringLiteral( "model" ) ) );
  if ( qc.contains( QStringLiteral( "sgs" ) ) )
    extra.insert( QStringLiteral( "sgs" ), qc.value( QStringLiteral( "sgs" ) ) );
  extra.insert( QStringLiteral( "finite_cells" ), counts.value( QStringLiteral( "finite" ) ) );
  extra.insert( QStringLiteral( "nodata_cells" ), counts.value( QStringLiteral( "nodata" ) ) );
  extra.insert( QStringLiteral( "solver_failures" ), counts.value( QStringLiteral( "solver_failures" ) ) );
  if ( qc.contains( QStringLiteral( "parameters" ) ) )
  {
    const QVariantMap qcP = qc.value( QStringLiteral( "parameters" ) ).toMap();
    if ( qcP.contains( QStringLiteral( "cross_correlation" ) ) )
      extra.insert( QStringLiteral( "cross_correlation" ), qcP.value( QStringLiteral( "cross_correlation" ) ) );
  }
  if ( !stagedSupport.isEmpty() )
  {
    extra.insert( QStringLiteral( "support_path" ), projectDir.relativeFilePath( stagedSupport ) );
    extra.insert( QStringLiteral( "support_sha256" ), supportSha );
  }
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( stagedQc ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  insertStrategyId( job.params, extra ); // 方向67：策略包 id 进血缘（kriging 词表项）
  QString commitErr;
  if ( !registrar.commitExternal( st, job.outputPath, parentIds, algorithmId, extra, &commitErr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }
  if ( !declareFactorResult( layers, job.horizon, job.factorId, def, st.absolutePath, registrar.projectDir(),
                             st.assetId, error ) )
  {
    discardTemp();
    return false;
  }
  // 方向 47：SGS 成员集合入库——成员栅格由计算段写进同一临时目录，
  // 这里收编为 realization_set 成员版本，并派生均值/总体标准差/P10/P90
  // 统计面（成员 ≥2 才派生——单成员集合如实无不确定性）。
  if ( !job.memberPaths.isEmpty() )
  {
    RealizationWorkflow realizationWf;
    realizationWf.bind( m_catalog.data(), m_projectDir, layers );
    QVector<RealizationWorkflow::MemberInput> memberInputs;
    memberInputs.reserve( job.memberPaths.size() );
    for ( const QString &path : job.memberPaths )
      memberInputs.push_back( { path, job.memberSeed } );
    QVariantMap setExtra;
    setExtra.insert( QStringLiteral( "method" ), job.method );
    setExtra.insert( QStringLiteral( "method_actual" ), methodActual );
    setExtra.insert( QStringLiteral( "algorithm_id" ), algorithmId );
    setExtra.insert( QStringLiteral( "factor_id" ), job.factorId );
    setExtra.insert( QStringLiteral( "field" ), job.field );
    setExtra.insert( QStringLiteral( "cell_size" ), job.cellSize );
    setExtra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
    setExtra.insert( QStringLiteral( "variogram" ), qc.value( QStringLiteral( "model" ) ) );
    setExtra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor/Realizations" ) );
    QString setErr;
    const QString setId = realizationWf.publishSet(
        tr( "%1·%2" ).arg( def.title, job.horizon ), job.horizon, memberInputs,
        parentIds, setExtra, &setErr );
    if ( setId.isEmpty() )
    {
      discardTemp();
      // 单因素图+图层已登记成功——集合是增量产物，如实报告两半段状态。
      paleo::workflow_detail::setError( error,
          tr( "单因素图已登记；realization 集合入库失败：%1" ).arg( setErr ) );
      return false;
    }
    if ( job.memberPaths.size() >= 2 )
    {
      paleo::ensemble::StatsRequest want;
      want.mean = want.stddev = want.p10 = want.p90 = true;
      if ( !realizationWf.deriveStatistics( setId, want, &setErr ) )
      {
        discardTemp();
        paleo::workflow_detail::setError( error,
            tr( "realization 集合 %1 已入库；统计派生失败：%2" ).arg( setId, setErr ) );
        return false;
      }
    }
  }
  discardTemp();
  return true;
}

bool ConstraintWorkflow::generateGeostatFactor( const QString &horizon, const QString &factorId,
                                                const QString &method, const SingleFactorDefinition &def,
                                                const QVariantMap &params, QString *error )
{
  Q_UNUSED( def );
  GeostatJob job;
  if ( !prepareGeostatJob( horizon, factorId, method, params, &job, error ) )
    return false;
  if ( !computeGeostatJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishGeostatJob( job, error );
}

