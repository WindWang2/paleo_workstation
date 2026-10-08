// 层：功能
// 方向57：从 constraintfactorjobs.cpp 按函数族析出。方法定义逐字搬迁，
// 类契约仍在 workflows.h（公共 API 零改动）；族间共享辅助经
// constraintfactorjobs_internal.h。
#include "workflows.h"
#include "constraintworkflow_internal.h"
#include "workflows_internal.h"
#include "../algorithms/geostat/kriging.h"     // KrigingParams / KrigingResult（方向18）
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

bool ConstraintWorkflow::prepareLocalDirectionJob( const QString &horizon, const QString &factorId,
                                                    const QVariantMap &inputParams, LocalDirectionJob *job,
                                                    QString *error, const QString &engineId )
{
  QVariantMap params = inputParams;
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少本地方向任务" ) );
    return false;
  }
  if (!prepareFactorInputs(horizon, factorId, params, error))
    return false;
  *job = LocalDirectionJob();
  job->generation = ++m_publishGeneration;
  job->horizon = horizon;
  job->factorId = factorId;
  job->params = params;
  job->engineId = params.value(QStringLiteral("method")).toString() == QLatin1String("surfer_idw")
      ? QStringLiteral("paleo:paleo_surfer_idw") : engineId;

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  if ( !proc->algorithmIds().contains( job->engineId ) )
  {
    paleo::workflow_detail::setError( error, tr( "单因素引擎尚未注册：%1" ).arg( job->engineId ) );
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
  const auto sourceOf = [&declared]( const QString &id ) {
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.layerId == id )
        return d.source;
    }
    return QString();
  };
  job->wellUri = sourceOf( pointsId );
  if ( job->wellUri.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "井点图层 %1 没有数据源" ).arg( pointsId ) );
    return false;
  }
  job->parentPaths << job->wellUri.section( QLatin1Char( '|' ), 0, 0 );

  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  job->hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  if ( job->hasConstraints )
  {
    const QString frozen = property( ( "paleo.constraint.snapshot." + horizon ).toUtf8().constData() ).toString();
    job->constraintUri = frozen.isEmpty() ? sourceOf( constraintLayerId )
                                           : frozen + QStringLiteral( "|layername=features" );
    if ( job->constraintUri.isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "无法加载约束图层 %1" ).arg( constraintLayerId ) );
      return false;
    }
    job->parentPaths << job->constraintUri.section( QLatin1Char( '|' ), 0, 0 );
  }
  // 方向84（D3）：协克里金协变量栅格——仅 METHOD=cokriging 时解析（其他方法
  // 残留 covariateLayerId 不接线，避免未参与的资产进血缘 parentPaths——review L7）；
  // cokriging 而缺图层由这里与算法层双重如实拒绝。
  const QString methodText = params.value( QStringLiteral( "method" ) ).toString();
  const QString covariateLayerId = params.value( QStringLiteral( "covariateLayerId" ) ).toString();
  if ( methodText == QLatin1String( "cokriging" ) )
  {
    bool covariateDeclared = false;
    QString covariateSource;
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.layerId == covariateLayerId )
      {
        covariateDeclared = true;
        covariateSource = d.source;
        break;
      }
    }
    if ( covariateLayerId.isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "协克里金需要协变量图层（栅格资产，如地震属性或已算因子面）" ) );
      return false;
    }
    if ( !covariateDeclared )
    {
      paleo::workflow_detail::setError( error, tr( "协变量图层 %1 不在图层清单" ).arg( covariateLayerId ) );
      return false;
    }
    if ( covariateSource.isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "协变量图层 %1 没有数据源" ).arg( covariateLayerId ) );
      return false;
    }
    job->covariateUri = covariateSource;
    job->parentPaths << covariateSource.section( QLatin1Char( '|' ), 0, 0 );
  }
  const QString snapshot = property( ( "paleo.constraint.snapshot." + horizon ).toUtf8().constData() ).toString();
  if ( !snapshot.isEmpty() )
    job->parentPaths << snapshot;
  job->prepared = true;
  return true;
}

bool ConstraintWorkflow::computeLocalDirectionJob( LocalDirectionJob *job, const std::function<bool()> &cancelled,
                                                   const std::function<void( double )> &progress )
{
  if ( !job || !job->prepared )
  {
    if ( job )
      job->error = tr( "本地方向任务尚未准备" );
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
  if ( !wells->isValid() )
  {
    job->error = tr( "井点图层无法读取" );
    return false;
  }
  const QString tempDir = QDir( QDir::tempPath() )
                              .filePath( QStringLiteral( "paleo-sf-%1" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
  if ( !QDir().mkpath( tempDir ) )
  {
    job->error = tr( "无法创建临时计算目录" );
    return false;
  }
  job->outputPath = QDir( tempDir ).filePath( QStringLiteral( "factor.tif" ) );

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
  runParams.insert( QStringLiteral( "FIELD" ), job->field );
  runParams.insert( QStringLiteral( "CELL_SIZE" ), job->cellSize );
  runParams.insert( QStringLiteral( "POWER" ), job->params.value( QStringLiteral( "power" ), 2.0 ) );
  runParams.insert( QStringLiteral( "COVERAGE" ),
                    job->params.value( QStringLiteral( "coverage" ), QStringLiteral( "well_supported" ) ) );
  runParams.insert( QStringLiteral( "CLUSTER" ), job->params.value( QStringLiteral( "wellClusterLocality" ), false ) );
  runParams.insert( QStringLiteral( "LOCAL_GRID" ), job->params.value( QStringLiteral( "localGrid" ), false ) );
  // ARCH-05：规范局部网格 WKT 由 workflow 注入（算法核不问 catalog），
  // 局部网格输出的 GeoTIFF 保 EDATUM 往返等价。
  runParams.insert( QStringLiteral( "LOCAL_GRID_WKT" ), DataCatalog::localGridCrsWkt() );
  runParams.insert( QStringLiteral( "REQUIRE_FULL_COVERAGE" ),
                    job->params.value( QStringLiteral( "requireFullCoverage" ), false ) );
  runParams.insert( QStringLiteral( "PERCENT_TO_FRACTION" ),
                    job->params.value( QStringLiteral( "percentToFraction" ), false ) );
  runParams.insert( QStringLiteral( "MIN_POINTS" ), job->params.value( QStringLiteral( "minPoints" ), 3 ) );
  runParams.insert( QStringLiteral( "MAX_POINTS" ), job->params.value( QStringLiteral( "maxPoints" ), 12 ) );
  runParams.insert( QStringLiteral( "SEARCH_RADIUS" ), job->params.value( QStringLiteral( "searchRadius" ), 0.0 ) );
  // 方向41：克里金请求与变差参数（只在本地方向面内切引擎，不改缺省 IDW 行为）。
  const QString localMethod = job->params.value( QStringLiteral( "method" ) ).toString();
  const bool localKriging = localMethod == QLatin1String( "local_direction_kriging" ) ||
                            localMethod == QLatin1String( "kriging" ) ||
                            localMethod == QLatin1String( "cokriging" );
  runParams.insert( QStringLiteral( "METHOD" ),
                    localMethod == QLatin1String( "cokriging" )
                        ? QStringLiteral( "cokriging" )
                        : ( localKriging ? QStringLiteral( "kriging" )
                                         : QStringLiteral( "local_direction_idw" ) ) );
  if ( localKriging )
  {
    runParams.insert( QStringLiteral( "VARIAGRAM_MODEL" ),
                      job->params.value( QStringLiteral( "variogramModel" ), QStringLiteral( "spherical" ) ) );
    runParams.insert( QStringLiteral( "NUGGET" ), job->params.value( QStringLiteral( "nugget" ), 0.0 ) );
    runParams.insert( QStringLiteral( "SILL" ), job->params.value( QStringLiteral( "sill" ), 0.0 ) );
    runParams.insert( QStringLiteral( "RANGE" ), job->params.value( QStringLiteral( "range" ), 0.0 ) );
    runParams.insert( QStringLiteral( "VARIAGRAM_AZIMUTH" ),
                      job->params.value( QStringLiteral( "azimuth" ), -1.0 ) );
    runParams.insert( QStringLiteral( "KRIGING_MAX_POINTS" ),
                      job->params.value( QStringLiteral( "krigingMaxPoints" ), 16 ) );
    if ( localMethod == QLatin1String( "cokriging" ) )
    {
      // 方向84（D3）：协克里金——协变量栅格与 MM1 交叉相关系数 ρ。
      runParams.insert( QStringLiteral( "CROSS_CORRELATION" ),
                        job->params.value( QStringLiteral( "crossCorrelation" ), 0.0 ) );
    }
  }
  if ( job->engineId == QLatin1String( "paleo:paleo_surfer_idw" ) )
  {
    // 各向异性是 surfer 引擎专属参数；其余引擎不识别。
    runParams.insert( QStringLiteral( "ANISOTROPY_RATIO" ),
                      job->params.value( QStringLiteral( "anisotropyRatio" ), 1.0 ) );
    runParams.insert( QStringLiteral( "ANISOTROPY_ANGLE" ),
                      job->params.value( QStringLiteral( "anisotropyAngle" ), 0.0 ) );
  }
  if ( job->params.contains( QStringLiteral( "valueUnit" ) ) )
    runParams.insert( QStringLiteral( "VALUE_UNIT" ), job->params.value( QStringLiteral( "valueUnit" ) ) );
  runParams.insert( QStringLiteral( "OUTPUT" ), job->outputPath );

  std::unique_ptr<QgsVectorLayer> constraints;
  if ( job->hasConstraints )
  {
    constraints = std::make_unique<QgsVectorLayer>( job->constraintUri, QStringLiteral( "constraints" ),
                                                    QStringLiteral( "ogr" ) );
    if ( !constraints->isValid() )
    {
      job->error = tr( "约束快照无法读取" );
      QDir( tempDir ).removeRecursively();
      return false;
    }
    runParams.insert( QStringLiteral( "CONSTRAINTS" ),
                      QVariant::fromValue( static_cast<QgsMapLayer *>( constraints.get() ) ) );
  }
  // 方向84（D3）：协克里金协变量栅格（prepare 已解析数据源；无效由算法层拒绝）。
  std::unique_ptr<QgsRasterLayer> covariate;
  if ( !job->covariateUri.isEmpty() )
  {
    covariate = std::make_unique<QgsRasterLayer>( job->covariateUri, QStringLiteral( "covariate" ) );
    if ( !covariate->isValid() )
    {
      job->error = tr( "协变量栅格无法读取：%1" ).arg( job->covariateUri );
      QDir( tempDir ).removeRecursively();
      return false;
    }
    runParams.insert( QStringLiteral( "COVARIATE" ),
                      QVariant::fromValue( static_cast<QgsMapLayer *>( covariate.get() ) ) );
  }

  QgisProcessingService::ProcessingHooks hooks;
  hooks.cancelled = cancelled;
  hooks.progress = progress;
  QString runErr;
  const QVariantMap results = proc->run( job->engineId, runParams, &runErr, hooks );
  job->supportPath = results.value( QStringLiteral( "SUPPORT" ) ).toString();
  job->qcPath = results.value( QStringLiteral( "QC" ) ).toString();
  const QString outPath = results.value( QStringLiteral( "OUTPUT" ) ).toString();
  if ( !outPath.isEmpty() )
    job->outputPath = outPath;
  if ( job->supportPath.isEmpty() )
    job->supportPath = fileStem( job->outputPath ) + QStringLiteral( ".support.tif" );
  if ( job->qcPath.isEmpty() )
    job->qcPath = fileStem( job->outputPath ) + QStringLiteral( ".qc.json" );
  if ( results.isEmpty() || outPath.isEmpty() || !QFile::exists( job->supportPath ) || !QFile::exists( job->qcPath ) )
  {
    removeIfPresent( job->outputPath );
    removeIfPresent( job->supportPath );
    removeIfPresent( job->qcPath );
    QDir( tempDir ).removeRecursively();
    job->outputPath.clear();
    job->error = runErr.isEmpty() ? tr( "本地方向插值未返回输出路径" ) : runErr;
    job->ok = false;
    return false;
  }
  job->ok = true;
  return true;
}

bool ConstraintWorkflow::publishLocalDirectionJob( const LocalDirectionJob &job, QString *error )
{
  const auto discardTemp = [&job]() {
    if ( job.outputPath.contains( QStringLiteral( "paleo-sf-" ) ) )
      QDir( QFileInfo( job.outputPath ).absolutePath() ).removeRecursively();
  };
  if ( !job.ok || job.outputPath.isEmpty() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, job.error.isEmpty() ? tr( "本地方向插值未返回输出路径" ) : job.error );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次本地方向成果" ) );
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
  const QString stagedSupport = fileStem( st.absolutePath ) + QStringLiteral( ".support.tif" );
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
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次本地方向成果" ) );
    return false;
  }
  if ( !QFile::copy( job.supportPath, stagedSupport ) || !QFile::copy( job.qcPath, stagedQc ) )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "本地方向旁路文件无法写入成果目录" ) );
    return false;
  }
  QString jsonErr;
  const QVariantMap qc = readJsonObject( stagedQc, &jsonErr );
  QVariantMap hashParams = qc.value( QStringLiteral( "parameters" ) ).toMap();
  if ( hashParams.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, jsonErr.isEmpty() ? tr( "本地方向成果缺少参数指纹输入" ) : jsonErr );
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
  const QString supportSha = DataCatalog::sha256FileHex( stagedSupport, &shaErr );
  const QString qcSha = DataCatalog::sha256FileHex( stagedQc, &shaErr );
  if ( supportSha.isEmpty() || qcSha.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "旁路文件 sha256 计算失败" ) : shaErr );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次本地方向成果" ) );
    return false;
  }
  const QDir projectDir( registrar.projectDir() );
  const QVariantMap counts = qc.value( QStringLiteral( "counts" ) ).toMap();
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
  extra.insert( QStringLiteral( "constrained" ), job.hasConstraints );
  extra.insert( QStringLiteral( "horizon" ), job.horizon );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  extra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
  extra.insert( QStringLiteral( "provenance_schema_version" ), 1 );
  // 方向41：algorithm_id 只认 QC 里实际执行的引擎（克里金回落时是 constraint/
  // local_direction_idw），engine_id 记请求的引擎，两者不混称。
  const QString actualAlgorithm = qc.value( QStringLiteral( "algorithm_id" ) ).toString();
  const QString actualMethod = qc.value( QStringLiteral( "method_actual" ) ).toString();
  const QString fallbackReason = qc.value( QStringLiteral( "fallback_reason" ) ).toString();
  const QString committedAlgorithm = actualAlgorithm.isEmpty() ? job.engineId : actualAlgorithm;
  extra.insert( QStringLiteral( "algorithm_id" ), committedAlgorithm );
  extra.insert( QStringLiteral( "engine_id" ), job.engineId );
  extra.insert( QStringLiteral( "method_actual" ),
                actualMethod.isEmpty() ? QStringLiteral( "local_direction_idw" ) : actualMethod );
  if ( qc.contains( QStringLiteral( "variogram" ) ) )
    extra.insert( QStringLiteral( "variogram" ), qc.value( QStringLiteral( "variogram" ) ) );
  if ( qc.contains( QStringLiteral( "issues" ) ) )
    extra.insert( QStringLiteral( "issues" ), qc.value( QStringLiteral( "issues" ) ) );
  if ( !fallbackReason.isEmpty() )
    extra.insert( QStringLiteral( "fallback_reason" ), fallbackReason );
  // 计数只在 QC 里有对应键时写（旧工程 QC 没有 kriging/idw_fallback 计数）。
  if ( counts.contains( QStringLiteral( "kriging" ) ) )
    extra.insert( QStringLiteral( "kriging_cells" ), counts.value( QStringLiteral( "kriging" ) ) );
  if ( counts.contains( QStringLiteral( "idw_fallback" ) ) )
    extra.insert( QStringLiteral( "idw_fallback_cells" ), counts.value( QStringLiteral( "idw_fallback" ) ) );
  extra.insert( QStringLiteral( "extent_source" ), qc.value( QStringLiteral( "extent_source" ) ) );
  extra.insert( QStringLiteral( "crs_mode" ), qc.value( QStringLiteral( "crs_mode" ) ) );
  extra.insert( QStringLiteral( "support_path" ), projectDir.relativeFilePath( stagedSupport ) );
  extra.insert( QStringLiteral( "support_sha256" ), supportSha );
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( stagedQc ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  extra.insert( QStringLiteral( "finite_cells" ), counts.value( QStringLiteral( "finite" ) ) );
  extra.insert( QStringLiteral( "nodata_cells" ), counts.value( QStringLiteral( "nodata" ) ) );
  extra.insert( QStringLiteral( "extrapolated_cells" ), counts.value( QStringLiteral( "extrapolated" ) ) );
  extra.insert( QStringLiteral( "barrier_cells" ), counts.value( QStringLiteral( "barrier" ) ) );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  insertStrategyId( job.params, extra ); // 方向67：策略包 id 进血缘（覆盖本地方向两引擎与 surfer 委托）
  QString commitErr;
  if ( !registrar.commitExternal( st, job.outputPath, parentIds, committedAlgorithm, extra, &commitErr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }
  discardTemp();
  return declareFactorResult( layers, job.horizon, job.factorId, def, st.absolutePath, registrar.projectDir(),
                              st.assetId, error );
}

bool ConstraintWorkflow::generateLocalDirectionFactor( const QString &horizon, const QString &factorId,
                                                       const SingleFactorDefinition &def,
                                                       const QVariantMap &params, QString *error )
{
  Q_UNUSED( def );
  LocalDirectionJob job;
  if ( !prepareLocalDirectionJob( horizon, factorId, params, &job, error ) )
    return false;
  if ( !computeLocalDirectionJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishLocalDirectionJob( job, error );
}

