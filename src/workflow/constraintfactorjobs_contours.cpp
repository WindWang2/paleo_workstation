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

bool ConstraintWorkflow::generateContours( const QString &horizon, const QString &factorLayerId,
                                           double interval, QString *error )
{
  AnalysisContourJob job;
  if ( !prepareAnalysisContourJob( horizon, factorLayerId, interval, {}, false, &job, error ) )
    return false;
  if ( !computeAnalysisContourJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishAnalysisContourJob( job, error );
}

bool ConstraintWorkflow::prepareAnalysisContourJob( const QString &horizon, const QString &factorLayerId,
                                                    double interval, const QVector<double> &levels,
                                                    bool fixedLevels, AnalysisContourJob *job, QString *error )
{
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少等值线任务" ) );
    return false;
  }
  *job = AnalysisContourJob();
  if ( fixedLevels )
  {
    if ( levels.isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "等值级别为空" ) );
      return false;
    }
    for ( double level : levels )
    {
      if ( !std::isfinite( level ) )
      {
        paleo::workflow_detail::setError( error, tr( "等值级别必须为有限数值" ) );
        return false;
      }
    }
  }

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  if ( !fixedLevels )
  {
    if ( factorLayerId.isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "缺少单因素图层 id" ) );
      return false;
    }
    // interval<=0 不在此早退：structural 因素走自动级别，判定依赖 rasterPath
    // 就绪后的 catalog 扫描，故正间距校验移到下方检测之后。
  }

  QString rasterPath;
  if ( !resolveDeclaredRaster( layers, factorLayerId, &rasterPath, error ) )
    return false;
  if ( factorLayerId.startsWith( QStringLiteral( "cartographic." ) ) )
  {
    paleo::workflow_detail::setError( error, tr( "解释性制图成果不能当作分析场提取等值线" ) );
    return false;
  }

  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }

  // WS-C part2：algorithm_id=paleo:paleo_structural_idw 的因素版本 → 上游
  // field_contours 提线路径；interval<=0 = 自动级别（其余引擎仍要求正间距）。
  if ( auto *catalog = PaleoWorkflowDerivedCatalog( this ) )
  {
    for ( const CatalogVersion &version : catalog->versions() )
    {
      if ( !catalogPathMatches( registrar.projectDir(), version.path, rasterPath ) )
        continue;
      if ( version.extra.value( QStringLiteral( "algorithm_id" ) ).toString() !=
           QStringLiteral( "paleo:paleo_structural_idw" ) )
        continue;
      job->structural = true;
      const QString rel =
          version.extra.value( QStringLiteral( "structural_path" ) ).toString();
      job->structuralPath = QDir::isAbsolutePath( rel )
                                ? rel
                                : QDir( registrar.projectDir() ).absoluteFilePath( rel );
      break;
    }
  }
  if ( job->structural && ( job->structuralPath.isEmpty() ||
                            !QFile::exists( job->structuralPath ) ) )
  {
    paleo::workflow_detail::setError( error, tr( "结构面侧卡缺失，无法提取等值线：%1" ).arg( rasterPath ) );
    return false;
  }
  if ( !fixedLevels && !job->structural && !( interval > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "等值线间距必须是正数" ) );
    return false;
  }

  QString shaErr;
  const QString analysisSha = DataCatalog::sha256FileHex( rasterPath, &shaErr );
  if ( analysisSha.isEmpty() )
  {
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "分析场 sha256 计算失败" ) : shaErr );
    return false;
  }

  job->generation = ++m_publishGeneration;
  job->horizon = horizon;
  job->factorLayerId = factorLayerId;
  job->factorId = factorIdOf( horizon, factorLayerId );
  job->rasterPath = rasterPath;
  job->analysisSha = analysisSha;
  job->parentPaths << rasterPath;
  job->interval = interval;
  job->levels = levels;
  job->fixedLevels = fixedLevels;
  job->prepared = true;
  return true;
}

bool ConstraintWorkflow::computeAnalysisContourJob( AnalysisContourJob *job, const std::function<bool()> &cancelled )
{
  // 只写临时 GPKG。不碰 catalog、清单和发布代次。
  if ( !job || !job->prepared )
  {
    if ( job )
      job->error = tr( "等值线任务尚未准备" );
    return false;
  }
  const auto discard = [&job]() {
    discardTempTree( job->outputPath );
    job->ok = false;
  };
  if ( cancelled && cancelled() )
  {
    job->error = tr( "已取消" );
    job->ok = false;
    return false;
  }

  const QString tempDir = QDir( QDir::tempPath() )
                              .filePath( QStringLiteral( "paleo-sf-contour-%1" )
                                             .arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
  if ( !QDir().mkpath( tempDir ) )
  {
    job->error = tr( "无法创建临时计算目录" );
    job->ok = false;
    return false;
  }
  job->outputPath = QDir( tempDir ).filePath( QStringLiteral( "contours.gpkg" ) );

  QString contourErr;
  // structural 因素必须走上游 field_contours 提取；绝不静默降级 GDAL。
  const bool wrote = job->structural
                         ? FactorContourService::generateStructuralContours(
                               job->rasterPath, job->structuralPath, job->outputPath,
                               job->fixedLevels ? job->levels : QVector<double>(),
                               job->interval, &job->resolvedLevels, &contourErr )
                     : job->fixedLevels
                         ? FactorContourService::generateFixedContours( job->rasterPath, job->outputPath, job->levels,
                                                                        &contourErr )
                         : FactorContourService::generateContours( job->rasterPath, job->outputPath, job->interval,
                                                                   &contourErr );
  if ( cancelled && cancelled() )
  {
    discard();
    job->error = tr( "已取消" );
    return false;
  }
  if ( !wrote )
  {
    discard();
    job->error = contourErr;
    return false;
  }
  job->ok = true;
  job->error.clear();
  return true;
}

bool ConstraintWorkflow::publishAnalysisContourJob( const AnalysisContourJob &job, QString *error )
{
  const auto discardTemp = [&job]() { discardTempTree( job.outputPath ); };
  if ( !job.ok || job.outputPath.isEmpty() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, job.error.isEmpty() ? tr( "等值线生成失败" ) : job.error );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次等值线" ) );
    return false;
  }
  QString shaErr;
  if ( !analysisShaMatches( job.rasterPath, job.analysisSha, &shaErr ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "分析场在等值线期间被改写，丢弃这次等值线" ) );
    return false;
  }
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  if ( !declaredFactorPathMatches( layers, job.factorLayerId, job.rasterPath ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "分析场在等值线期间被改写，丢弃这次等值线" ) );
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
      QStringLiteral( "contour_lines" ), tr( "等值线·%1" ).arg( SingleFactorRegistry::titleFor( job.factorId ) ),
      QStringLiteral( "CONTOURS_%1_%2.gpkg" ).arg( job.factorId, job.horizon ), &regErr );
  if ( !st.isValid() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  if ( sameFile( st.absolutePath, job.rasterPath ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "等值线不能覆盖分析场文件" ) );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    if ( !sameFile( st.absolutePath, job.rasterPath ) )
      removeIfPresent( st.absolutePath );
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次等值线" ) );
    return false;
  }
  if ( !analysisShaMatches( job.rasterPath, job.analysisSha, &shaErr ) ||
       !declaredFactorPathMatches( layers, job.factorLayerId, job.rasterPath ) )
  {
    if ( !sameFile( st.absolutePath, job.rasterPath ) )
      removeIfPresent( st.absolutePath );
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "分析场在等值线期间被改写，丢弃这次等值线" ) );
    return false;
  }

  const QStringList parents = registrar.parentVersionIdsFor( { job.rasterPath } );
  QVariantMap extra = contourMetadata( QStringLiteral( "product." ) + st.versionId, job.horizon, job.factorLayerId );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "contour_lines" ) );
  extra.insert( QStringLiteral( "title" ), tr( "%1 等值线" ).arg( job.horizon ) );
  extra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor/Contours" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  extra.insert( QStringLiteral( "analysis_sha256" ), job.analysisSha );
  extra.insert( QStringLiteral( "manifest_layer_id" ),
                QStringLiteral( "contours.%1.%2" ).arg( job.horizon, job.factorId ) );
  if ( job.fixedLevels )
  {
    QVariantList levelList;
    for ( double level : job.levels )
      levelList << level;
    extra.insert( QStringLiteral( "levels" ), levelList );
  }
  else
  {
    extra.insert( QStringLiteral( "interval" ), job.interval );
  }
  if ( job.structural )
  {
    extra.insert( QStringLiteral( "algorithm_id" ),
                  QStringLiteral( "paleo:paleo_structural_idw" ) );
    QVariantList used;
    for ( double level : job.resolvedLevels )
      used << level;
    extra.insert( QStringLiteral( "levels_used" ), used );
  }
  QVariantList parentList;
  for ( const QString &id : parents )
    parentList << id;
  extra.insert( QStringLiteral( "parent_version_ids" ), parentList );
  if ( !job.fixedLevels )
    inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parents, extra );

  QString commitErr;
  if ( !registrar.commitExternal( st, job.outputPath, parents,
                                  job.structural ? QStringLiteral( "paleo_field_contours" )
                                                 : QStringLiteral( "gdal_contour_c_api" ),
                                  extra, &commitErr ) )
  {
    if ( !sameFile( st.absolutePath, job.rasterPath ) )
      removeIfPresent( st.absolutePath );
    discardTemp();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }
  discardTemp();

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "contours.%1.%2" ).arg( job.horizon, job.factorId );
  decl.horizon = job.horizon;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "%1|layername=contours" ).arg( st.absolutePath );
  decl.group = QStringLiteral( "04_SingleFactor/Contours" );
  decl.title = tr( "等值线·%1" ).arg( SingleFactorRegistry::titleFor( job.factorId ) );
  if ( !layers->declare( decl, error ) )
    return false;

  paleo::workflow_detail::stampLayerAssetLink( layers, decl.layerId, st.assetId );
  emit contoursGenerated( job.horizon, job.factorLayerId, decl.layerId );
  return true;
}

bool ConstraintWorkflow::generateCartographicWork( const QString &horizon, const QString &factorLayerId,
                                                   const QVector<double> &levels, QString *error,
                                                   bool refuseUnresolved, int *unresolvedOut )
{
  const quint64 generation = ++m_publishGeneration;
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  if ( !proc->algorithmIds().contains( QStringLiteral( "paleo:paleo_cartographic_work" ) ) )
  {
    paleo::workflow_detail::setError( error, tr( "制图工作场引擎尚未注册" ) );
    return false;
  }
  if ( factorLayerId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "缺少单因素图层 id" ) );
    return false;
  }
  if ( levels.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "等值级别为空" ) );
    return false;
  }
  for ( double level : levels )
  {
    if ( !std::isfinite( level ) )
    {
      paleo::workflow_detail::setError( error, tr( "等值级别必须为有限数值" ) );
      return false;
    }
  }

  QVector<LayerDeclaration> declared;
  QString readErr;
  if ( !layers->tryDeclared( &declared, &readErr ) )
  {
    paleo::workflow_detail::setError( error, readErr.isEmpty() ? tr( "无法读取图层清单" ) : readErr );
    return false;
  }
  const LayerDeclaration *factorDecl = nullptr;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == factorLayerId )
    {
      factorDecl = &d;
      break;
    }
  }
  if ( !factorDecl )
  {
    paleo::workflow_detail::setError( error, tr( "图层 %1 未在清单声明" ).arg( factorLayerId ) );
    return false;
  }
  if ( paleo::singlefactor::isCartographicProductLayer( factorLayerId, factorDecl->group ) )
  {
    paleo::workflow_detail::setError( error, tr( "解释性制图工作场不能当作分析场" ) );
    return false;
  }
  if ( factorDecl->type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
  {
    paleo::workflow_detail::setError( error, tr( "制图工作场输入必须是栅格图层：%1" ).arg( factorLayerId ) );
    return false;
  }
  const QString rasterPath = factorDecl->source.section( QLatin1Char( '|' ), 0, 0 );
  if ( !QFile::exists( rasterPath ) )
  {
    paleo::workflow_detail::setError( error, tr( "分析场文件不存在：%1" ).arg( rasterPath ) );
    return false;
  }

  QString factorId = factorLayerId;
  const QString factorPrefix = QStringLiteral( "factor.%1." ).arg( horizon );
  if ( factorLayerId.startsWith( factorPrefix ) )
    factorId = factorLayerId.mid( factorPrefix.size() );

  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  if ( DataCatalog *catalog = PaleoWorkflowDerivedCatalog( this ) )
  {
    for ( const CatalogVersion &version : catalog->versions() )
    {
      if ( !catalogPathMatches( registrar.projectDir(), version.path, rasterPath ) )
        continue;
      const QString kind = version.extra.value( QStringLiteral( "kind" ) ).toString();
      const QString valueSource = version.extra.value( QStringLiteral( "value_source" ) ).toString();
      if ( paleo::singlefactor::rejectsQuantitativeUse( kind, valueSource ) )
      {
        paleo::workflow_detail::setError( error, tr( "解释性制图工作场不能当作分析场" ) );
        return false;
      }
    }
  }

  QString shaErr;
  const QString analysisSha = DataCatalog::sha256FileHex( rasterPath, &shaErr );
  if ( analysisSha.isEmpty() )
  {
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "分析场 sha256 计算失败" ) : shaErr );
    return false;
  }

  bool knownFactor = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( factorId, &knownFactor );
  const QString title = SingleFactorRegistry::titleFor( factorId );
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_cartographic_work" ), tr( "制图工作场·%1" ).arg( title ),
      QStringLiteral( "CARTOGRAPHIC_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  if ( sameFile( st.absolutePath, rasterPath ) )
  {
    paleo::workflow_detail::setError( error, tr( "制图工作场不能覆盖分析场文件" ) );
    return false;
  }

  QgsMapLayer *raster = layers->instantiate( factorLayerId, error );
  if ( !raster )
    return false;

  QStringList levelText;
  QVariantList levelList;
  for ( double level : levels )
  {
    levelText << QString::number( level, 'g', 17 );
    levelList << level;
  }
  QStringList parentPaths{ rasterPath };
  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( raster ) );
  runParams.insert( QStringLiteral( "LEVELS" ), levelText.join( QLatin1Char( ',' ) ) );
  runParams.insert( QStringLiteral( "TRANSITION" ), 0.0 );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  std::unique_ptr<QgsVectorLayer> frozenConstraints;
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    const auto frozenPath = property( ( "paleo.constraint.snapshot." + horizon ).toUtf8().constData() ).toString();
    if ( !frozenPath.isEmpty() )
    {
      frozenConstraints = std::make_unique<QgsVectorLayer>( frozenPath + QStringLiteral( "|layername=features" ),
                                                           QStringLiteral( "constraints" ), QStringLiteral( "ogr" ) );
      if ( !frozenConstraints->isValid() )
      {
        paleo::workflow_detail::setError( error, tr( "约束快照无法读取" ) );
        return false;
      }
      constraints = frozenConstraints.get();
    }
    if ( !constraints )
    {
      paleo::workflow_detail::setError( error, constraintErr.isEmpty() ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId ) : constraintErr );
      return false;
    }
    runParams.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_cartographic_work" ), runParams, error );
  const QString outPath = results.value( QStringLiteral( "OUTPUT" ) ).toString();
  QString qcPath = results.value( QStringLiteral( "QC" ) ).toString();
  if ( qcPath.isEmpty() && !outPath.isEmpty() )
    qcPath = fileStem( outPath ) + QStringLiteral( ".qc.json" );
  auto discard = [&]() {
    if ( !sameFile( outPath, rasterPath ) )
      removeIfPresent( outPath );
    if ( !sameFile( st.absolutePath, rasterPath ) )
      removeIfPresent( st.absolutePath );
    if ( !sameFile( qcPath, rasterPath ) )
      removeIfPresent( qcPath );
  };
  if ( results.isEmpty() || outPath.isEmpty() )
  {
    discard();
    if ( error && error->isEmpty() )
      paleo::workflow_detail::setError( error, tr( "制图工作场未返回输出路径" ) );
    return false;
  }
  if ( sameFile( outPath, rasterPath ) )
  {
    paleo::workflow_detail::setError( error, tr( "制图工作场不能覆盖分析场文件" ) );
    return false;
  }
  if ( generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次制图工作场" ) );
    return false;
  }
  if ( !QFile::exists( qcPath ) )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "制图工作场缺少 QC" ) );
    return false;
  }

  if ( !analysisShaMatches( rasterPath, analysisSha, &shaErr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "分析场在制图期间被改写，丢弃工作场" ) );
    return false;
  }

  QString jsonErr;
  const QVariantMap qc = readJsonObject( qcPath, &jsonErr );
  if ( qc.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, jsonErr.isEmpty() ? tr( "制图工作场 QC 无法读取" ) : jsonErr );
    return false;
  }
  const int unresolved = qc.value( QStringLiteral( "unresolved_crossings" ) ).toInt();
  if ( unresolvedOut )
    *unresolvedOut = unresolved;
  if ( refuseUnresolved && unresolved > 0 )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "未解决穿线 %1 条，严格模式不发布" ).arg( unresolved ) );
    return false;
  }
  QStringList parentIds = registrar.parentVersionIdsFor( parentPaths );
  parentIds.removeDuplicates();
  parentIds.sort();
  QVariantList parentList;
  for ( const QString &id : parentIds )
    parentList << id;
  QVariantMap hashParams = cartographicHashParameters( qc, levelList, analysisSha );
  hashParams.insert( QStringLiteral( "horizon" ), horizon );
  hashParams.insert( QStringLiteral( "factor_id" ), factorId );
  hashParams.insert( QStringLiteral( "parent_version_ids" ), parentList );
  const paleo::singlefactor::ParameterHash hash = paleo::singlefactor::parameterHash( hashParams );
  if ( !hash.ok )
  {
    discard();
    paleo::workflow_detail::setError( error, hash.error.isEmpty() ? tr( "参数指纹计算失败" ) : hash.error );
    return false;
  }
  const QString qcSha = DataCatalog::sha256FileHex( qcPath, &shaErr );
  if ( qcSha.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "旁路文件 sha256 计算失败" ) : shaErr );
    return false;
  }
  if ( generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次制图工作场" ) );
    return false;
  }

  const QString layerId = QStringLiteral( "cartographic.%1.%2" ).arg( horizon, factorId );
  const QDir projectDir( registrar.projectDir() );
  QVariantMap extra;
  extra.insert( QStringLiteral( "mapping_product" ), true );
  extra.insert( QStringLiteral( "layer_id" ), layerId );
  extra.insert( QStringLiteral( "manifest_layer_id" ), layerId );
  extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
  extra.insert( QStringLiteral( "title" ), tr( "制图工作场·%1" ).arg( title ) );
  extra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor/Cartographic" ) );
  extra.insert( QStringLiteral( "horizon" ), horizon );
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "factor_layer_id" ), factorLayerId );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_cartographic_work" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "cartographic_work" ) );
  extra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
  extra.insert( QStringLiteral( "provenance_schema_version" ), 1 );
  extra.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "paleo:paleo_cartographic_work" ) );
  extra.insert( QStringLiteral( "analysis_sha256" ), analysisSha );
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( qcPath ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  extra.insert( QStringLiteral( "modified_cells" ), qc.value( QStringLiteral( "modified_cells" ) ) );
  extra.insert( QStringLiteral( "unchanged" ), qc.value( QStringLiteral( "unchanged" ) ) );
  extra.insert( QStringLiteral( "unresolved_crossings" ), qc.value( QStringLiteral( "unresolved_crossings" ) ) );
  extra.insert( QStringLiteral( "levels" ), hashParams.value( QStringLiteral( "levels" ) ) );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, parentIds, QStringLiteral( "paleo:paleo_cartographic_work" ), extra,
                                  &commitErr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  if ( !registrar.projectDir().isEmpty() )
  {
    QString styleErr;
    FactorStyleWriter::writeStyleQml( factorId, st.absolutePath,
                                      QDir( registrar.projectDir() ).filePath( QStringLiteral( "styles" ) ),
                                      &styleErr );
  }

  LayerDeclaration decl;
  decl.layerId = layerId;
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "04_SingleFactor/Cartographic" );
  decl.styleRef = knownFactor ? def.styleRef : QString();
  decl.title = tr( "制图工作场·%1" ).arg( title );
  if ( !layers->declare( decl, error ) )
    return false;

  paleo::workflow_detail::stampLayerAssetLink( layers, decl.layerId, st.assetId );
  emit cartographicWorkGenerated( horizon, factorLayerId, decl.layerId );
  return true;
}

bool ConstraintWorkflow::generateContoursAtLevels( const QString &horizon, const QString &factorLayerId,
                                                   const QVector<double> &levels, QString *error )
{
  AnalysisContourJob job;
  if ( !prepareAnalysisContourJob( horizon, factorLayerId, 0.0, levels, true, &job, error ) )
    return false;
  if ( !computeAnalysisContourJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishAnalysisContourJob( job, error );
}

