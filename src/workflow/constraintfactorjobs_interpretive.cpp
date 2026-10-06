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

bool ConstraintWorkflow::prepareInterpretiveContourJob( const QString &horizon, const QString &factorLayerId,
                                                        const QVector<double> &levels, bool strict,
                                                        InterpretiveContourJob *job, QString *error )
{
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少解释性等值线任务" ) );
    return false;
  }
  *job = InterpretiveContourJob();

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

  QStringList parentPaths{ rasterPath };
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  QString constraintUri;
  bool frozenConstraints = false;
  if ( hasConstraints )
  {
    const QString frozen = property( ( "paleo.constraint.snapshot." + horizon ).toUtf8().constData() ).toString();
    if ( !frozen.isEmpty() )
    {
      constraintUri = frozen + QStringLiteral( "|layername=features" );
      frozenConstraints = true;
    }
    else
    {
      for ( const LayerDeclaration &d : declared )
      {
        if ( d.layerId == constraintLayerId )
        {
          constraintUri = d.source;
          break;
        }
      }
      if ( constraintUri.isEmpty() )
      {
        paleo::workflow_detail::setError( error, tr( "无法加载约束图层 %1" ).arg( constraintLayerId ) );
        return false;
      }
    }
    parentPaths.append( constraintUri.section( QLatin1Char( '|' ), 0, 0 ) );
  }

  std::vector<paleo::singlefactor::ConstraintLine> constraintLines;
  std::vector<std::string> ignoredConstraints;
  if ( hasConstraints )
  {
    auto analysis = std::make_unique<QgsRasterLayer>( rasterPath, QStringLiteral( "analysis" ),
                                                      QStringLiteral( "gdal" ) );
    if ( !analysis->isValid() )
    {
      paleo::workflow_detail::setError( error, tr( "分析场无法读取" ) );
      return false;
    }
    const paleo::singlefactor::CartographicConstraintParse parsed =
        paleo::singlefactor::parseCartographicConstraints( constraintUri, analysis->crs() );
    if ( !parsed.ok )
    {
      const QString fallback = frozenConstraints ? tr( "约束快照无法读取" )
                                                 : tr( "无法加载约束图层 %1" ).arg( constraintLayerId );
      paleo::workflow_detail::setError( error, parsed.error.isEmpty() ? fallback : parsed.error );
      return false;
    }
    constraintLines = std::move( parsed.lines );
    ignoredConstraints = std::move( parsed.ignored );
  }

  job->generation = ++m_publishGeneration;
  job->strict = strict;
  job->horizon = horizon;
  job->factorLayerId = factorLayerId;
  job->factorId = factorIdOf( horizon, factorLayerId );
  job->rasterPath = rasterPath;
  job->constraintUri = constraintUri;
  job->hasConstraints = hasConstraints;
  job->frozenConstraints = frozenConstraints;
  job->analysisSha = analysisSha;
  job->parentPaths = parentPaths;
  job->levels = levels;
  job->constraintLines = std::move( constraintLines );
  job->ignoredConstraints = std::move( ignoredConstraints );
  job->prepared = true;
  return true;
}

bool ConstraintWorkflow::computeInterpretiveContourJob( InterpretiveContourJob *job,
                                                        const std::function<bool()> &cancelled )
{
  // 工作场和等值线都落在临时目录。不读发布代次，不登记。
  // 不构造 QgsMapLayer，不调用 Processing。约束线已在准备阶段解析。
  if ( !job || !job->prepared )
  {
    if ( job )
      job->error = tr( "解释性等值线任务尚未准备" );
    return false;
  }
  const auto discard = [&job]() {
    if ( !sameFile( job->workPath, job->rasterPath ) )
      discardTempTree( job->workPath );
    if ( !sameFile( job->contourPath, job->rasterPath ) )
      discardTempTree( job->contourPath );
    job->ok = false;
  };
  if ( cancelled && cancelled() )
  {
    job->error = tr( "已取消" );
    job->ok = false;
    return false;
  }
  const QString tempDir = QDir( QDir::tempPath() )
                              .filePath( QStringLiteral( "paleo-sf-carto-%1" )
                                             .arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
  if ( !QDir().mkpath( tempDir ) )
  {
    job->error = tr( "无法创建临时计算目录" );
    job->ok = false;
    return false;
  }
  const QString tempWork = QDir( tempDir ).filePath( QStringLiteral( "work.tif" ) );
  const QString analysisCopy = QDir( tempDir ).filePath( QStringLiteral( "analysis.tif" ) );
  job->workPath = tempWork;
  job->contourPath = QDir( tempDir ).filePath( QStringLiteral( "contours.gpkg" ) );
  if ( cancelled && cancelled() )
  {
    discard();
    job->error = tr( "已取消" );
    return false;
  }
  if ( !QFile::copy( job->rasterPath, analysisCopy ) )
  {
    discard();
    job->error = tr( "分析场无法读取" );
    return false;
  }

  paleo::singlefactor::CartographicWorkWrite request;
  request.analysisPath = analysisCopy;
  request.outputPath = tempWork;
  request.lines = job->constraintLines;
  request.ignored = job->ignoredConstraints;
  request.levels.assign( job->levels.cbegin(), job->levels.cend() );
  request.transition = 0.0;
  request.deriveCrsFromDataset = true;
  request.cancelled = cancelled;
  const paleo::singlefactor::CartographicWorkWritten written =
      paleo::singlefactor::writeCartographicWorkFile( request );
  if ( ( cancelled && cancelled() ) || written.cancelled )
  {
    discard();
    job->error = tr( "已取消" );
    return false;
  }
  if ( !written.ok )
  {
    discard();
    job->error = written.error.isEmpty() ? tr( "制图工作场未返回输出路径" ) : written.error;
    return false;
  }
  job->workPath = written.outputPath;
  job->qcPath = written.qcPath;
  if ( sameFile( job->workPath, job->rasterPath ) )
  {
    discard();
    job->error = tr( "制图工作场不能覆盖分析场文件" );
    return false;
  }
  const QString qcPath = job->qcPath;
  if ( !QFile::exists( qcPath ) )
  {
    discard();
    job->error = tr( "制图工作场缺少 QC" );
    return false;
  }

  QString jsonErr;
  const QVariantMap qc = readJsonObject( qcPath, &jsonErr );
  if ( qc.isEmpty() )
  {
    discard();
    job->error = jsonErr.isEmpty() ? tr( "制图工作场 QC 无法读取" ) : jsonErr;
    return false;
  }
  job->unresolved = qc.value( QStringLiteral( "unresolved_crossings" ) ).toInt();
  if ( cancelled && cancelled() )
  {
    discard();
    job->error = tr( "已取消" );
    return false;
  }
  if ( job->strict && job->unresolved > 0 )
  {
    discard();
    job->error = tr( "未解决穿线 %1 条，严格模式不发布" ).arg( job->unresolved );
    return false;
  }

  QString contourErr;
  if ( !FactorContourService::generateFixedContours( job->workPath, job->contourPath, job->levels, &contourErr ) )
  {
    discard();
    job->error = contourErr;
    return false;
  }
  if ( cancelled && cancelled() )
  {
    discard();
    job->error = tr( "已取消" );
    return false;
  }
  job->ok = true;
  job->error.clear();
  return true;
}

bool ConstraintWorkflow::publishInterpretiveContourJob( const InterpretiveContourJob &job, QString *error )
{
  const auto discardTemp = [&job]() {
    if ( !sameFile( job.workPath, job.rasterPath ) )
      discardTempTree( job.workPath );
    if ( !sameFile( job.contourPath, job.rasterPath ) )
      discardTempTree( job.contourPath );
  };
  if ( !job.ok || job.workPath.isEmpty() || job.contourPath.isEmpty() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, job.error.isEmpty() ? tr( "解释性等值线生成失败" ) : job.error );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次制图工作场" ) );
    return false;
  }

  QString shaErr;
  if ( !analysisShaMatches( job.rasterPath, job.analysisSha, &shaErr ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "分析场在制图期间被改写，丢弃工作场" ) );
    return false;
  }
  if ( sameFile( job.workPath, job.rasterPath ) || sameFile( job.contourPath, job.rasterPath ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "制图工作场不能覆盖分析场文件" ) );
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
    paleo::workflow_detail::setError( error, tr( "分析场在制图期间被改写，丢弃工作场" ) );
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

  bool knownFactor = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( job.factorId, &knownFactor );
  const QString title = SingleFactorRegistry::titleFor( job.factorId );
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_cartographic_work" ), tr( "制图工作场·%1" ).arg( title ),
      QStringLiteral( "CARTOGRAPHIC_%1_%2.tif" ).arg( job.factorId, job.horizon ), &regErr );
  if ( !st.isValid() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const QString stagedQc = fileStem( st.absolutePath ) + QStringLiteral( ".qc.json" );
  if ( sameFile( st.absolutePath, job.rasterPath ) || sameFile( stagedQc, job.rasterPath ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "制图工作场不能覆盖分析场文件" ) );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次制图工作场" ) );
    return false;
  }
  if ( QFile::exists( stagedQc ) )
    QFile::remove( stagedQc );
  if ( !QFile::copy( job.qcPath, stagedQc ) )
  {
    removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "制图工作场缺少 QC" ) );
    return false;
  }

  QString jsonErr;
  const QVariantMap qc = readJsonObject( stagedQc, &jsonErr );
  if ( qc.isEmpty() )
  {
    removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, jsonErr.isEmpty() ? tr( "制图工作场 QC 无法读取" ) : jsonErr );
    return false;
  }

  QVariantList levelList;
  for ( double level : job.levels )
    levelList << level;
  QStringList parentIds = registrar.parentVersionIdsFor( job.parentPaths );
  parentIds.removeDuplicates();
  parentIds.sort();
  QVariantList parentList;
  for ( const QString &id : parentIds )
    parentList << id;
  QVariantMap hashParams = cartographicHashParameters( qc, levelList, job.analysisSha );
  hashParams.insert( QStringLiteral( "horizon" ), job.horizon );
  hashParams.insert( QStringLiteral( "factor_id" ), job.factorId );
  hashParams.insert( QStringLiteral( "parent_version_ids" ), parentList );
  const paleo::singlefactor::ParameterHash hash = paleo::singlefactor::parameterHash( hashParams );
  if ( !hash.ok )
  {
    removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, hash.error.isEmpty() ? tr( "参数指纹计算失败" ) : hash.error );
    return false;
  }
  const QString qcSha = DataCatalog::sha256FileHex( stagedQc, &shaErr );
  if ( qcSha.isEmpty() )
  {
    removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "旁路文件 sha256 计算失败" ) : shaErr );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次制图工作场" ) );
    return false;
  }
  if ( !declaredFactorPathMatches( layers, job.factorLayerId, job.rasterPath ) )
  {
    removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "分析场在制图期间被改写，丢弃工作场" ) );
    return false;
  }

  const QString workLayerId = QStringLiteral( "cartographic.%1.%2" ).arg( job.horizon, job.factorId );
  const QDir projectDir( registrar.projectDir() );
  QVariantMap extra;
  extra.insert( QStringLiteral( "mapping_product" ), true );
  extra.insert( QStringLiteral( "layer_id" ), workLayerId );
  extra.insert( QStringLiteral( "manifest_layer_id" ), workLayerId );
  extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
  extra.insert( QStringLiteral( "title" ), tr( "制图工作场·%1" ).arg( title ) );
  extra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor/Cartographic" ) );
  extra.insert( QStringLiteral( "horizon" ), job.horizon );
  extra.insert( QStringLiteral( "factor_id" ), job.factorId );
  extra.insert( QStringLiteral( "factor_layer_id" ), job.factorLayerId );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_cartographic_work" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "cartographic_work" ) );
  extra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
  extra.insert( QStringLiteral( "provenance_schema_version" ), 1 );
  extra.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "paleo:paleo_cartographic_work" ) );
  extra.insert( QStringLiteral( "analysis_sha256" ), job.analysisSha );
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( stagedQc ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  extra.insert( QStringLiteral( "modified_cells" ), qc.value( QStringLiteral( "modified_cells" ) ) );
  extra.insert( QStringLiteral( "unchanged" ), qc.value( QStringLiteral( "unchanged" ) ) );
  extra.insert( QStringLiteral( "unresolved_crossings" ), qc.value( QStringLiteral( "unresolved_crossings" ) ) );
  extra.insert( QStringLiteral( "levels" ), hashParams.value( QStringLiteral( "levels" ) ) );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  QString commitErr;
  if ( !registrar.commitExternal( st, job.workPath, parentIds, QStringLiteral( "paleo:paleo_cartographic_work" ), extra,
                                  &commitErr ) )
  {
    if ( !sameFile( st.absolutePath, job.rasterPath ) )
      removeIfPresent( st.absolutePath );
    if ( !sameFile( stagedQc, job.rasterPath ) )
      removeIfPresent( stagedQc );
    discardTemp();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  if ( !registrar.projectDir().isEmpty() )
  {
    QString styleErr;
    FactorStyleWriter::writeStyleQml( job.factorId, st.absolutePath,
                                      QDir( registrar.projectDir() ).filePath( QStringLiteral( "styles" ) ),
                                      &styleErr );
  }

  LayerDeclaration workDecl;
  workDecl.layerId = workLayerId;
  workDecl.horizon = job.horizon;
  workDecl.type = QStringLiteral( "raster" );
  workDecl.source = st.absolutePath;
  workDecl.group = QStringLiteral( "04_SingleFactor/Cartographic" );
  workDecl.styleRef = knownFactor ? def.styleRef : QString();
  workDecl.title = tr( "制图工作场·%1" ).arg( title );
  if ( !layers->declare( workDecl, error ) )
  {
    discardTemp();
    return false;
  }
  paleo::workflow_detail::stampLayerAssetLink( layers, workDecl.layerId, st.assetId );
  emit cartographicWorkGenerated( job.horizon, job.factorLayerId, workDecl.layerId );

  const QString contourTitle = tr( "解释性等值线·%1" ).arg( title );
  const QString contourLayerId = QStringLiteral( "cartographic.%1.%2.contours" ).arg( job.horizon, job.factorId );
  const DerivedStaging contourSt = registrar.stage(
      QStringLiteral( "single_factor_cartographic_contour" ), contourTitle,
      QStringLiteral( "CARTO_CONTOURS_%1_%2.gpkg" ).arg( job.factorId, job.horizon ), &regErr );
  if ( !contourSt.isValid() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  if ( sameFile( contourSt.absolutePath, job.rasterPath ) )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "制图工作场不能覆盖分析场文件" ) );
    return false;
  }
  QStringList contourParents = registrar.parentVersionIdsFor( { job.rasterPath, st.absolutePath } );
  contourParents.removeDuplicates();
  contourParents.sort();
  QVariantList contourParentList;
  for ( const QString &id : contourParents )
    contourParentList << id;
  QVariantMap contourExtra = contourMetadata( contourLayerId, job.horizon, job.factorLayerId );
  contourExtra.insert( QStringLiteral( "manifest_layer_id" ), contourLayerId );
  contourExtra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_cartographic_contour" ) );
  contourExtra.insert( QStringLiteral( "value_source" ), QStringLiteral( "cartographic_work" ) );
  contourExtra.insert( QStringLiteral( "title" ), contourTitle );
  contourExtra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor/Cartographic" ) );
  contourExtra.insert( QStringLiteral( "levels" ), levelList );
  contourExtra.insert( QStringLiteral( "work_layer_id" ), workLayerId );
  contourExtra.insert( QStringLiteral( "parent_version_ids" ), contourParentList );
  contourExtra.insert( QStringLiteral( "unresolved_crossings" ), job.unresolved );
  if ( !registrar.commitExternal( contourSt, job.contourPath, contourParents, QStringLiteral( "gdal_contour_c_api" ),
                                  contourExtra, &commitErr ) )
  {
    if ( !sameFile( contourSt.absolutePath, job.rasterPath ) )
      removeIfPresent( contourSt.absolutePath );
    discardTemp();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  LayerDeclaration contourDecl;
  contourDecl.layerId = contourLayerId;
  contourDecl.horizon = job.horizon;
  contourDecl.type = QStringLiteral( "vector" );
  contourDecl.source = QStringLiteral( "%1|layername=contours" ).arg( contourSt.absolutePath );
  contourDecl.group = QStringLiteral( "04_SingleFactor/Cartographic" );
  contourDecl.title = contourTitle;
  if ( !layers->declare( contourDecl, error ) )
  {
    discardTemp();
    return false;
  }
  paleo::workflow_detail::stampLayerAssetLink( layers, contourDecl.layerId, contourSt.assetId );
  emit interpretiveContoursGenerated( job.horizon, job.factorLayerId, contourDecl.layerId );
  discardTemp();
  return true;
}

bool ConstraintWorkflow::generateInterpretiveContours( const QString &horizon, const QString &factorLayerId,
                                                       const QVector<double> &levels, QString *error, bool strict )
{
  InterpretiveContourJob job;
  if ( !prepareInterpretiveContourJob( horizon, factorLayerId, levels, strict, &job, error ) )
    return false;
  if ( !computeInterpretiveContourJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishInterpretiveContourJob( job, error );
}
