// 层：功能
#include "workflows.h"
#include "constraintworkflow_internal.h"
#include "workflows_internal.h"
#include "../algorithms/geostat/kriging.h"     // KrigingParams / KrigingResult（方向18）
#include "../algorithms/geostat/sgs.h"         // SgsParams / SgsResult（方向18）
#include "../algorithms/geostat/variogram.h"   // 变差函数模型（方向18）
#include "../algorithms/rasterout.h"          // PaleoRasterOut / createFloatRaster
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

// 约束与单因素工作流（②）的**单因素作业面**部分。
// 方向20 轮4 从 constraintworkflow.cpp 二次拆出：约束 CRUD（构造 / setter /
// 增删改查约束）留在 constraintworkflow.cpp；三组单因素作业的生成链与三段式
//（局部方向 / 克里金-SGS / 等值线 / 解释性等值线 + JobRunner 统一面）在此。
// 实测两段**零跨段调用**（各自只经 m_proc / m_layers / m_catalog 等成员取数据），
// 故按语义边界切分安全。头文件契约不动。
using namespace paleo::constraint_detail;


// ---- m2(B) 单因素图页：generateFactor / generateContours ---------------------
namespace
{
  // SHA 只比较输入字节；报错、代次/声明守卫与清理仍留在各发布路径，保持顺序。
  bool analysisShaMatches( const QString &path, const QString &expected, QString *error )
  {
    return DataCatalog::sha256FileHex( path, error ) == expected;
  }

  void inheritMockFlag( DataCatalog *catalog, const QStringList &parents, QVariantMap &extra )
  {
    if ( !catalog )
      return;
    for ( const QString &id : parents )
      if ( catalog->versionById( id ).extra.value( QStringLiteral( "mock" ) ).toBool() )
        extra.insert( QStringLiteral( "mock" ), true );
  }

  QVariantMap contourMetadata( const QString &layerId, const QString &horizon,
                               const QString &factorLayerId )
  {
    QVariantMap extra;
    extra.insert( QStringLiteral( "mapping_product" ), true );
    extra.insert( QStringLiteral( "layer_id" ), layerId );
    extra.insert( QStringLiteral( "horizon" ), horizon );
    extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "vector" ) );
    extra.insert( QStringLiteral( "source_suffix" ), QStringLiteral( "|layername=contours" ) );
    extra.insert( QStringLiteral( "factor_layer_id" ), factorLayerId );
    return extra;
  }

  // 同步/异步制图工作场的相同 QC 指纹输入。层位/因素/父版本由调用方补齐。
  QVariantMap cartographicHashParameters( const QVariantMap &qc, const QVariantList &levels,
                                         const QString &analysisSha )
  {
    QVariantMap params;
    params.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "paleo:paleo_cartographic_work" ) );
    params.insert( QStringLiteral( "value_source" ), QStringLiteral( "cartographic_work" ) );
    params.insert( QStringLiteral( "levels" ), qc.contains( QStringLiteral( "levels" ) )
                                               ? qc.value( QStringLiteral( "levels" ) ) : QVariant( levels ) );
    params.insert( QStringLiteral( "transition_distance" ), qc.value( QStringLiteral( "transition_distance" ) ) );
    params.insert( QStringLiteral( "ignored" ), qc.value( QStringLiteral( "ignored" ) ) );
    params.insert( QStringLiteral( "used_constraints" ), qc.value( QStringLiteral( "used_constraints" ) ) );
    params.insert( QStringLiteral( "analysis_sha256" ), analysisSha );
    return params;
  }

  // 原壳（paleomainwindow runIdwRequested 接线）里的井点图层查找，挪进
  // workflow 侧：vector 声明、layerId 以 "wells" 开头，优先当前层位，其次
  // 层位无关（horizon-agnostic）声明。
  QString wellsLayerIdFor( QgisLayerService *layers, const QString &horizon )
  {
    if ( !layers )
      return QString();
    QVector<LayerDeclaration> declared;
    if ( !layers->tryDeclared( &declared ) )
      return QString();
    QString agnostic;
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) != 0 )
        continue;
      if ( !d.layerId.startsWith( QStringLiteral( "wells" ) ) )
        continue;
      if ( !horizon.isEmpty() && d.horizon == horizon )
        return d.layerId;
      if ( agnostic.isEmpty() && d.horizon.isEmpty() )
        agnostic = d.layerId;
    }
    return agnostic;
  }
} // namespace
bool ConstraintWorkflow::generateFactor( const QString &horizon, const QString &factorId,
                                         const QVariantMap &params, QString *error )
{
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

  // 主线6 + C5（wave/deepen-perf）：按引擎分派参数整形。welldist 距离变换
  // 引擎已按冻结契约实现（src/algorithms/distancetransform.cpp）——注册面
  // 缺失时仍显式拒绝（不静默降级成 IDW，那会产出语义错误的栅格）；confidence
  // 的前置在 onnxpredictionservice（只读首个输出张量，无置信度通道），维持
  // 冻结拒绝。契约全文见 docs/progress/mapping.md。
  if ( def.processingAlgId == SingleFactorContracts::welldistEngineId() )
    return generateDistanceFactor( horizon, factorId, def, params, error );
  if ( def.processingAlgId == SingleFactorContracts::confidenceEngineId() )
  {
    paleo::workflow_detail::setError( error, tr( "单因素 %1 的引擎 %2 尚未接入（前置在 ONNX 置信度"
                         "通道，当前会话只读首个输出张量；见 docs/progress/mapping.md）" )
                         .arg( factorId, def.processingAlgId ) );
    return false;
  }
  if ( def.processingAlgId == QStringLiteral( "paleo:paleo_isopach" ) )
    return generateIsopachFactor( horizon, factorId, def, params, error );
  const QString field = params.value( QStringLiteral( "field" ),
                                      def.defaultParams.value( QStringLiteral( "field" ) ) )
                            .toString();
  const double cellSize = params.value( QStringLiteral( "cellSize" ),
                                        def.defaultParams.value( QStringLiteral( "cellSize" ), 1.0 ) )
                              .toDouble();
  if ( field.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "插值字段为空" ) );
    return false;
  }
  const QString method = params.value( QStringLiteral( "method" ) ).toString();
  // structural_idw 按格网分辨率（gridResolution）成图，没有 cellSize 契约——
  // 分派先于像元大小校验。
  if ( method == QLatin1String( "structural_idw" ) )
    return generateStructuralFactor( horizon, factorId, def, params, error );
  if ( !( cellSize > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  if ( method == QLatin1String( "local_direction_idw" ) )
    return generateLocalDirectionFactor( horizon, factorId, def, params, error );
  if ( method == QLatin1String( "kriging" ) || method == QLatin1String( "sgs" ) )
    return generateGeostatFactor( horizon, factorId, method, def, params, error );
  if ( method == QLatin1String( "surfer_idw" ) )
    return generateSurferIdwFactor( horizon, factorId, def, params, error );
  if ( !method.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "未知单因素方法：%1" ).arg( method ) );
    return false;
  }

  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon )
                                                    : overridePoints;
  if ( pointsId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }

  // INPUT is a QgsProcessingParameterFeatureSource: resolve the declared points
  // layer through the layer service (same contract as runConstraintIDW).
  QgsMapLayer *points = layers->instantiate( pointsId, error );
  if ( !points )
    return false;

  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  runParams.insert( QStringLiteral( "FIELD" ), field );
  runParams.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  // 约束线屏障随行（同 runConstraintIDW：清单里有该层位的约束图层就带上）。
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    paleo::workflow_detail::setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  std::unique_ptr<QgsVectorLayer> frozenConstraints;
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    const auto frozenPath=property(("paleo.constraint.snapshot."+horizon).toUtf8().constData()).toString();
    if(!frozenPath.isEmpty()) {
      frozenConstraints=std::make_unique<QgsVectorLayer>(frozenPath+"|layername=features","constraints","ogr");
      if(!frozenConstraints->isValid()){paleo::workflow_detail::setError(error,tr("约束快照无法读取"));return false;}
      constraints=frozenConstraints.get();
    }
    if ( !constraints )
    {
      paleo::workflow_detail::setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    runParams.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( def.processingAlgId, runParams, error );
  if ( results.isEmpty() )
    return false;
  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "单因素生成未返回输出路径" ) );
    return false;
  }

  QVariantMap extra;
  extra.insert("mapping_product",true);extra.insert("layer_id","product."+st.versionId);
  extra.insert("manifest_layer_id",QStringLiteral("factor.%1.%2").arg(horizon,factorId)); // C4：catalog 侧权威反链（清单 layerId）
  extra.insert("layer_type","raster");extra.insert("title",tr("%1·%2").arg(def.title,horizon));extra.insert("group","04_SingleFactor");
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "field" ), field );
  extra.insert( QStringLiteral( "cell_size" ), cellSize );
  extra.insert( QStringLiteral( "constrained" ), hasConstraints );
  extra.insert(QStringLiteral("horizon"), horizon);
  extra.insert(QStringLiteral("kind"), QStringLiteral("single_factor_raster"));
  parentPaths.append(property(("paleo.constraint.snapshot."+horizon).toUtf8().constData()).toString());
  QStringList parentIds = registrar.parentVersionIdsFor(parentPaths);
  parentIds.append(params.value("parentVersionIds").toStringList());
  parentIds.removeDuplicates();
  if(auto *catalog=PaleoWorkflowDerivedCatalog(this))for(const auto &id:parentIds)
    if(catalog->versionById(id).extra.value("mock").toBool())extra.insert("mock",true);
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, parentIds,
                                  def.processingAlgId, extra, &commitErr ) )
  {
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  // 色带样式落盘 <projectDir>/styles/factor_<factorId>.qml（best-effort）与
  // 声明/盖章/通知收尾见 declareFactorResult（三引擎共用）。
  return declareFactorResult( layers, horizon, factorId, def, outPath,
                              registrar.projectDir(), st.assetId, error );
}

bool ConstraintWorkflow::generateIsopachFactor( const QString &horizon, const QString &factorId,
                                                const SingleFactorDefinition &def,
                                                const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }

  // 等厚引擎的输入是两个已声明的构造面栅格（页面 topLayerId/baseLayerId）。
  const QString topId = params.value( QStringLiteral( "topLayerId" ),
                                      def.defaultParams.value( QStringLiteral( "topLayerId" ) ) )
                            .toString();
  const QString baseId = params.value( QStringLiteral( "baseLayerId" ),
                                       def.defaultParams.value( QStringLiteral( "baseLayerId" ) ) )
                             .toString();
  if ( topId.isEmpty() || baseId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "等厚引擎需要顶/底构造面图层（topLayerId/baseLayerId）" ) );
    return false;
  }

  QgsMapLayer *top = layers->instantiate( topId, error );
  if ( !top )
    return false;
  QgsMapLayer *base = layers->instantiate( baseId, error );
  if ( !base )
    return false;
  const auto isRaster = []( QgsMapLayer *l ) {
    return l->type() == Qgis::LayerType::Raster;
  };
  if ( !isRaster( top ) || !isRaster( base ) )
  {
    paleo::workflow_detail::setError( error, tr( "顶/底输入必须是栅格图层：%1 / %2" ).arg( topId, baseId ) );
    return false;
  }

  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT_TOP" ), QVariant::fromValue( top ) );
  runParams.insert( QStringLiteral( "INPUT_BASE" ), QVariant::fromValue( base ) );
  // 倒置层序（底高于顶）多为重叠/误拾取——默认折 nodata（注册表默认同值）。
  runParams.insert( QStringLiteral( "NEGATIVE_TO_NODATA" ),
                    params.value( QStringLiteral( "negativeToNodata" ),
                                  def.defaultParams.value( QStringLiteral( "negativeToNodata" ), true ) ) );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QVariantMap results = proc->run( def.processingAlgId, runParams, error );
  if ( results.isEmpty() )
    return false;
  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "等厚生成未返回输出路径" ) );
    return false;
  }

  const QStringList parentPaths{
      top->source().section( QLatin1Char( '|' ), 0, 0 ),
      base->source().section( QLatin1Char( '|' ), 0, 0 ) };
  QVariantMap extra;
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "engine" ), def.processingAlgId );
  extra.insert( QStringLiteral( "top_layer_id" ), topId );
  extra.insert( QStringLiteral( "base_layer_id" ), baseId );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  def.processingAlgId, extra, &commitErr ) )
  {
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  return declareFactorResult( layers, horizon, factorId, def, outPath,
                              registrar.projectDir(), st.assetId, error );
}

bool ConstraintWorkflow::generateDistanceFactor( const QString &horizon, const QString &factorId,
                                                 const SingleFactorDefinition &def,
                                                 const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }

  // 引擎按冻结契约 id 解析；注册面缺失 → 显式拒绝（不静默降级 IDW）。
  if ( !proc->algorithmIds().contains( def.processingAlgId ) )
  {
    paleo::workflow_detail::setError( error, tr( "单因素 %1 的引擎 %2 尚未注册（参数契约已冻结；"
                         "见 docs/progress/mapping.md）" )
                         .arg( factorId, def.processingAlgId ) );
    return false;
  }

  const double cellSize = params.value( QStringLiteral( "cellSize" ),
                                        def.defaultParams.value( QStringLiteral( "cellSize" ), 1.0 ) )
                              .toDouble();
  if ( !( cellSize > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon )
                                                    : overridePoints;
  if ( pointsId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }
  QgsMapLayer *points = layers->instantiate( pointsId, error );
  if ( !points )
    return false;

  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  runParams.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  // 约束图层随行（绕障语义在算法侧按 type 分拣：仅 break_line 阻断）。
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    paleo::workflow_detail::setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    if ( !constraints )
    {
      paleo::workflow_detail::setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    runParams.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( def.processingAlgId, runParams, error );
  if ( results.isEmpty() )
    return false;
  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "单因素生成未返回输出路径" ) );
    return false;
  }

  QVariantMap extra;
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "engine" ), def.processingAlgId );
  extra.insert( QStringLiteral( "cell_size" ), cellSize );
  extra.insert( QStringLiteral( "constrained" ), hasConstraints );
  extra.insert( QStringLiteral( "horizon" ), horizon );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  extra.insert( QStringLiteral( "manifest_layer_id" ),
                QStringLiteral( "factor.%1.%2" ).arg( horizon, factorId ) ); // C4 反链
  extra.insert( QStringLiteral( "points_layer_id" ), pointsId );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  def.processingAlgId, extra, &commitErr ) )
  {
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  // 色带样式落盘（welldist 绿→灰预设已备；best-effort）与收尾同 IDW 路径
  // （declareFactorResult，三引擎共用）。
  return declareFactorResult( layers, horizon, factorId, def, outPath,
                              registrar.projectDir(), st.assetId, error );
}
bool ConstraintWorkflow::prepareLocalDirectionJob( const QString &horizon, const QString &factorId,
                                                    const QVariantMap &params, LocalDirectionJob *job,
                                                    QString *error, const QString &engineId )
{
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少本地方向任务" ) );
    return false;
  }
  *job = LocalDirectionJob();
  job->generation = ++m_publishGeneration;
  job->horizon = horizon;
  job->factorId = factorId;
  job->params = params;
  job->engineId = engineId;

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
  runParams.insert( QStringLiteral( "REQUIRE_FULL_COVERAGE" ),
                    job->params.value( QStringLiteral( "requireFullCoverage" ), false ) );
  runParams.insert( QStringLiteral( "PERCENT_TO_FRACTION" ),
                    job->params.value( QStringLiteral( "percentToFraction" ), false ) );
  runParams.insert( QStringLiteral( "MIN_POINTS" ), job->params.value( QStringLiteral( "minPoints" ), 3 ) );
  runParams.insert( QStringLiteral( "MAX_POINTS" ), job->params.value( QStringLiteral( "maxPoints" ), 12 ) );
  runParams.insert( QStringLiteral( "SEARCH_RADIUS" ), job->params.value( QStringLiteral( "searchRadius" ), 0.0 ) );
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
  extra.insert( QStringLiteral( "algorithm_id" ), job.engineId );
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
  QString commitErr;
  if ( !registrar.commitExternal( st, job.outputPath, parentIds, job.engineId, extra, &commitErr ) )
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

// ---- WS-C：structural_idw（上游 Drawing structural_idw 移植）----
// 同步三段式（准备/计算/发布在同一函数内）；旁路约定同 local_direction_idw
//（.qc.json 参数指纹 + sha256 入 extra），另落 .structural.json 与
// samples.<层位>.<因素> 参与井点点图层。

namespace
{
  // 参与井点（非控制点）写成 GPKG 点图层；fields 契约：well_id / factor_value。
  bool writeStructuralSamples( const QString &path, const QVariantList &wells,
                               const QgsCoordinateReferenceSystem &crs, QString *error )
  {
    QgsVectorLayer layer( QStringLiteral( "Point" ), QStringLiteral( "samples" ),
                          QStringLiteral( "memory" ) );
    if ( !layer.isValid() )
    {
      paleo::workflow_detail::setError( error, QObject::tr( "参与井点图层无法创建" ) );
      return false;
    }
    layer.setCrs( crs );
    layer.startEditing();
    if ( !layer.addAttribute( QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ) )
         || !layer.addAttribute( QgsField( QStringLiteral( "factor_value" ), QMetaType::Type::Double ) ) )
    {
      paleo::workflow_detail::setError( error, QObject::tr( "参与井点字段创建失败" ) );
      return false;
    }
    layer.updateFields();
    for ( const QVariant &v : wells )
    {
      const QVariantMap well = v.toMap();
      if ( well.value( QStringLiteral( "is_control_point" ) ).toBool() )
        continue;
      QgsFeature feature( layer.fields() );
      feature.setGeometry( QgsGeometry::fromPointXY(
          QgsPointXY( well.value( QStringLiteral( "x" ) ).toDouble(),
                      well.value( QStringLiteral( "y" ) ).toDouble() ) ) );
      feature.setAttribute( QStringLiteral( "well_id" ),
                            well.value( QStringLiteral( "well_id" ) ).toString() );
      feature.setAttribute( QStringLiteral( "factor_value" ),
                            well.value( QStringLiteral( "value" ) ).toDouble() );
      if ( !layer.addFeature( feature ) )
      {
        paleo::workflow_detail::setError( error, QObject::tr( "参与井点写入失败" ) );
        return false;
      }
    }
    if ( !layer.commitChanges() )
    {
      paleo::workflow_detail::setError( error, QObject::tr( "参与井点提交失败" ) );
      return false;
    }
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = QStringLiteral( "GPKG" );
    options.layerName = QStringLiteral( "samples" );
    options.fileEncoding = QStringLiteral( "UTF-8" );
    options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
    return QgsVectorFileWriter::writeAsVectorFormatV3(
               &layer, path, QgsCoordinateTransformContext(), options, error )
           == QgsVectorFileWriter::NoError;
  }
} // namespace

bool ConstraintWorkflow::generateStructuralFactor( const QString &horizon, const QString &factorId,
                                                   const SingleFactorDefinition &def,
                                                   const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  const QString engineId = QStringLiteral( "paleo:paleo_structural_idw" );
  if ( !proc->algorithmIds().contains( engineId ) )
  {
    paleo::workflow_detail::setError( error, tr( "单因素 %1 的引擎 %2 尚未注册" ).arg( factorId, engineId ) );
    return false;
  }

  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon )
                                                    : overridePoints;
  if ( pointsId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }
  const QString field = params.value( QStringLiteral( "field" ),
                                      def.defaultParams.value( QStringLiteral( "field" ) ) )
                            .toString();
  if ( field.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "插值字段为空" ) );
    return false;
  }
  // 上游 data_mode=current_layers 的边界是必选项；缺省即拒绝（不静默降级）。
  const QString boundaryId = params.value( QStringLiteral( "boundaryLayerId" ) ).toString();
  if ( boundaryId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "请先选择成图边界图层" ) );
    return false;
  }

  QgsMapLayer *points = layers->instantiate( pointsId, error );
  if ( !points )
    return false;
  QgsMapLayer *boundary = layers->instantiate( boundaryId, error );
  if ( !boundary )
    return false;
  QgsMapLayer *interpArea = nullptr;
  const QString interpId = params.value( QStringLiteral( "interpolationAreaLayerId" ) ).toString();
  if ( !interpId.isEmpty() )
  {
    interpArea = layers->instantiate( interpId, error );
    if ( !interpArea )
      return false;
  }

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    paleo::workflow_detail::setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );

  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ),
                           boundary->source().section( QLatin1Char( '|' ), 0, 0 ) };
  if ( interpArea )
    parentPaths << interpArea->source().section( QLatin1Char( '|' ), 0, 0 );

  std::unique_ptr<QgsVectorLayer> frozenConstraints;
  QgsMapLayer *constraints = nullptr;
  if ( hasConstraints )
  {
    QString constraintErr;
    constraints = layers->instantiate( constraintLayerId, &constraintErr );
    const QString frozenPath =
        property( ( "paleo.constraint.snapshot." + horizon ).toUtf8().constData() ).toString();
    if ( !frozenPath.isEmpty() )
    {
      frozenConstraints = std::make_unique<QgsVectorLayer>(
          frozenPath + QStringLiteral( "|layername=features" ),
          QStringLiteral( "constraints" ), QStringLiteral( "ogr" ) );
      if ( !frozenConstraints->isValid() )
      {
        paleo::workflow_detail::setError( error, tr( "约束快照无法读取" ) );
        return false;
      }
      constraints = frozenConstraints.get();
      parentPaths << frozenPath;
    }
    if ( !constraints )
    {
      paleo::workflow_detail::setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    parentPaths << constraints->source().section( QLatin1Char( '|' ), 0, 0 );
  }

  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "WELLS" ), QVariant::fromValue( points ) );
  runParams.insert( QStringLiteral( "WELL_ID_FIELD" ),
                    params.value( QStringLiteral( "wellIdField" ), QStringLiteral( "well_id" ) ) );
  runParams.insert( QStringLiteral( "VALUE_FIELD" ), field );
  runParams.insert( QStringLiteral( "FACTOR_NAME" ),
                    params.value( QStringLiteral( "factorName" ), def.factorId ) );
  runParams.insert( QStringLiteral( "FACTOR_MODE" ),
                    params.value( QStringLiteral( "factorMode" ), QStringLiteral( "direct" ) ) );
  if ( params.contains( QStringLiteral( "numeratorField" ) ) )
    runParams.insert( QStringLiteral( "NUMERATOR_FIELD" ), params.value( QStringLiteral( "numeratorField" ) ) );
  if ( params.contains( QStringLiteral( "denominatorField" ) ) )
    runParams.insert( QStringLiteral( "DENOMINATOR_FIELD" ), params.value( QStringLiteral( "denominatorField" ) ) );
  runParams.insert( QStringLiteral( "VALUE_RANGE" ),
                    params.value( QStringLiteral( "valueRange" ), QStringLiteral( "ratio_0_1" ) ) );
  runParams.insert( QStringLiteral( "BOUNDARY" ), QVariant::fromValue( boundary ) );
  if ( interpArea )
    runParams.insert( QStringLiteral( "INTERPOLATION_AREA" ), QVariant::fromValue( interpArea ) );
  if ( constraints )
    runParams.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
  runParams.insert( QStringLiteral( "GRID_RESOLUTION" ),
                    params.value( QStringLiteral( "gridResolution" ),
                                  def.defaultParams.value( QStringLiteral( "gridResolution" ), 339 ) ) );
  runParams.insert( QStringLiteral( "POWER" ), params.value( QStringLiteral( "power" ), 2.0 ) );
  runParams.insert( QStringLiteral( "MIN_POINTS" ), params.value( QStringLiteral( "minPoints" ), 3 ) );
  runParams.insert( QStringLiteral( "MAX_POINTS" ), params.value( QStringLiteral( "maxPoints" ), 12 ) );
  runParams.insert( QStringLiteral( "SEARCH_RADIUS" ), params.value( QStringLiteral( "searchRadius" ), 0.0 ) );
  runParams.insert( QStringLiteral( "EXTEND_TO_BOUNDARY" ),
                    params.value( QStringLiteral( "extendToBoundary" ), true ) );
  runParams.insert( QStringLiteral( "ENABLE_BARRIERS" ),
                    params.value( QStringLiteral( "enableBarriers" ), true ) );
  runParams.insert( QStringLiteral( "ENABLE_DIRECTIONS" ),
                    params.value( QStringLiteral( "enableDirections" ), true ) );
  runParams.insert( QStringLiteral( "INTERPRETIVE_STRENGTH" ),
                    params.value( QStringLiteral( "interpretiveStrength" ), 0.35 ) );
  runParams.insert( QStringLiteral( "BARRIER_BUFFER" ),
                    params.value( QStringLiteral( "barrierBuffer" ), 150.0 ) );
  runParams.insert( QStringLiteral( "BARRIER_BUFFER_AUTO" ),
                    params.value( QStringLiteral( "barrierBufferAuto" ), false ) );
  runParams.insert( QStringLiteral( "BARRIER_SHAPE_RADIUS" ),
                    params.value( QStringLiteral( "barrierShapeRadius" ), 0.0 ) );
  runParams.insert( QStringLiteral( "BARRIER_SHAPE_STRENGTH" ),
                    params.value( QStringLiteral( "barrierShapeStrength" ), 1.0 ) );
  runParams.insert( QStringLiteral( "BARRIER_EXTEND" ),
                    params.value( QStringLiteral( "barrierExtend" ), true ) );
  runParams.insert( QStringLiteral( "BARRIER_EXTENSION_LIMIT" ),
                    params.value( QStringLiteral( "barrierExtensionLimit" ), -1.0 ) );
  runParams.insert( QStringLiteral( "CONTOUR_STOP_BUFFER" ),
                    params.value( QStringLiteral( "contourStopBuffer" ), -1.0 ) );
  runParams.insert( QStringLiteral( "CLUSTER" ),
                    params.value( QStringLiteral( "wellClusterLocality" ), true ) );
  runParams.insert( QStringLiteral( "LOCAL_GRID" ),
                    params.value( QStringLiteral( "localGrid" ), false ) );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QString stagedQc = fileStem( st.absolutePath ) + QStringLiteral( ".qc.json" );
  const QString stagedStructural = fileStem( st.absolutePath ) + QStringLiteral( ".structural.json" );
  auto discard = [&]() {
    removeIfPresent( st.absolutePath );
    removeIfPresent( stagedQc );
    removeIfPresent( stagedStructural );
  };

  const QVariantMap results = proc->run( engineId, runParams, error );
  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( results.isEmpty() || outPath.isEmpty() || !QFile::exists( stagedQc )
       || !QFile::exists( stagedStructural ) )
  {
    discard();
    if ( results.isEmpty() || outPath.isEmpty() )
      return false; // proc->run 已填 error
    paleo::workflow_detail::setError( error, tr( "结构化插值未生成旁路成果（qc/structural json）" ) );
    return false;
  }

  QString jsonErr;
  const QVariantMap qc = readJsonObject( stagedQc, &jsonErr );
  QVariantMap hashParams = qc.value( QStringLiteral( "parameters" ) ).toMap();
  if ( hashParams.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, jsonErr.isEmpty() ? tr( "结构化成果缺少参数指纹输入" ) : jsonErr );
    return false;
  }
  QStringList parentIds = registrar.parentVersionIdsFor( parentPaths );
  parentIds.append( params.value( QStringLiteral( "parentVersionIds" ) ).toStringList() );
  parentIds.removeDuplicates();
  parentIds.sort();
  QVariantList parentList;
  for ( const QString &id : parentIds )
    parentList << id;
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
  QString shaErr;
  const QString qcSha = DataCatalog::sha256FileHex( stagedQc, &shaErr );
  const QString structuralSha = DataCatalog::sha256FileHex( stagedStructural, &shaErr );
  if ( qcSha.isEmpty() || structuralSha.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "旁路文件 sha256 计算失败" ) : shaErr );
    return false;
  }

  const QDir projectDir( registrar.projectDir() );
  const QVariantMap counts = qc.value( QStringLiteral( "counts" ) ).toMap();
  const QVariantMap fieldModel = qc.value( QStringLiteral( "field_model" ) ).toMap();
  QVariantMap extra;
  extra.insert( QStringLiteral( "mapping_product" ), true );
  extra.insert( QStringLiteral( "layer_id" ), QStringLiteral( "product." ) + st.versionId );
  extra.insert( QStringLiteral( "manifest_layer_id" ),
                QStringLiteral( "factor.%1.%2" ).arg( horizon, factorId ) );
  extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
  extra.insert( QStringLiteral( "title" ), tr( "%1·%2" ).arg( def.title, horizon ) );
  extra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor" ) );
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "field" ), field );
  extra.insert( QStringLiteral( "method" ), QStringLiteral( "structural_idw" ) );
  extra.insert( QStringLiteral( "grid_resolution" ),
                runParams.value( QStringLiteral( "GRID_RESOLUTION" ) ) );
  extra.insert( QStringLiteral( "constrained" ), hasConstraints );
  extra.insert( QStringLiteral( "horizon" ), horizon );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  extra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
  extra.insert( QStringLiteral( "provenance_schema_version" ), 1 );
  extra.insert( QStringLiteral( "algorithm_id" ), engineId );
  extra.insert( QStringLiteral( "extent_source" ), qc.value( QStringLiteral( "extent_source" ) ) );
  extra.insert( QStringLiteral( "crs_mode" ), qc.value( QStringLiteral( "crs_mode" ) ) );
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( stagedQc ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  extra.insert( QStringLiteral( "structural_path" ), projectDir.relativeFilePath( stagedStructural ) );
  extra.insert( QStringLiteral( "structural_sha256" ), structuralSha );
  extra.insert( QStringLiteral( "finite_cells" ), counts.value( QStringLiteral( "finite" ) ) );
  extra.insert( QStringLiteral( "nodata_cells" ), counts.value( QStringLiteral( "nodata" ) ) );
  extra.insert( QStringLiteral( "direction_coverage_percent" ),
                fieldModel.value( QStringLiteral( "direction_coverage_percent" ) ) );
  extra.insert( QStringLiteral( "barrier_buffer_distance" ),
                qc.value( QStringLiteral( "barrier_buffer_distance" ) ) );
  extra.insert( QStringLiteral( "contour_stop_buffer_distance" ),
                qc.value( QStringLiteral( "contour_stop_buffer_distance" ) ) );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, parentIds, engineId, extra, &commitErr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  // 参与井点（非控制点）→ samples.<层位>.<因素> 点图层（样式在后续工作流）。
  const QVariantList wells = qc.value( QStringLiteral( "wells" ) ).toList();
  if ( !wells.isEmpty() )
  {
    const QString samplesPath = fileStem( st.absolutePath ) + QStringLiteral( ".samples.gpkg" );
    QString samplesErr;
    if ( writeStructuralSamples( samplesPath, wells, boundary->crs(), &samplesErr ) )
    {
      LayerDeclaration sdecl;
      sdecl.layerId = QStringLiteral( "samples.%1.%2" ).arg( horizon, factorId );
      sdecl.horizon = horizon;
      sdecl.type = QStringLiteral( "vector" );
      sdecl.source = samplesPath + QStringLiteral( "|layername=samples" );
      sdecl.group = QStringLiteral( "04_SingleFactor" );
      sdecl.title = tr( "参与井点" );
      layers->declare( sdecl ); // best-effort：井点层失败不打断因素成果
    }
  }

  return declareFactorResult( layers, horizon, factorId, def, st.absolutePath,
                              registrar.projectDir(), st.assetId, error );
}


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
                       const double geoTransform[6], const QgsCoordinateReferenceSystem &crs )
{
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( path, cols, rows, geoTransform, crs, -9999.0 );
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
                                            const QString &method, const QVariantMap &params,
                                            GeostatJob *job, QString *error )
{
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少地质统计任务" ) );
    return false;
  }
  if ( method != QLatin1String( "kriging" ) && method != QLatin1String( "sgs" ) )
  {
    paleo::workflow_detail::setError( error, tr( "未知地质统计方法：%1" ).arg( method ) );
    return false;
  }
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
    if ( !writeFloatRaster( job->outputPath, result.estimate, grid.cols, grid.rows, geoTransform, crs ) ||
         !writeFloatRaster( job->supportPath, result.variance, grid.cols, grid.rows, geoTransform, crs ) )
    {
      cleanupTemp();
      job->error = tr( "克里金栅格写盘失败" );
      return false;
    }
    counts.insert( QStringLiteral( "finite" ), result.finiteCells );
    counts.insert( QStringLiteral( "nodata" ), result.nodataCells );
    counts.insert( QStringLiteral( "solver_failures" ), result.solverFailures );
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
    if ( !writeFloatRaster( job->outputPath, mean, grid.cols, grid.rows, geoTransform, crs ) ||
         !writeFloatRaster( job->supportPath, spread, grid.cols, grid.rows, geoTransform, crs ) )
    {
      cleanupTemp();
      job->error = tr( "SGS 栅格写盘失败" );
      return false;
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
  if ( !stagedSupport.isEmpty() )
  {
    extra.insert( QStringLiteral( "support_path" ), projectDir.relativeFilePath( stagedSupport ) );
    extra.insert( QStringLiteral( "support_sha256" ), supportSha );
  }
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( stagedQc ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  QString commitErr;
  if ( !registrar.commitExternal( st, job.outputPath, parentIds, algorithmId, extra, &commitErr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }
  discardTemp();
  return declareFactorResult( layers, job.horizon, job.factorId, def, st.absolutePath, registrar.projectDir(),
                              st.assetId, error );
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

bool ConstraintWorkflow::generateSurferIdwFactor( const QString &horizon, const QString &factorId,
                                                  const SingleFactorDefinition &def,
                                                  const QVariantMap &params, QString *error )
{
  Q_UNUSED( def );
  LocalDirectionJob job;
  if ( !prepareLocalDirectionJob( horizon, factorId, params, &job, error,
                                  QStringLiteral( "paleo:paleo_surfer_idw" ) ) )
    return false;
  if ( !computeLocalDirectionJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishLocalDirectionJob( job, error );
}

// 三个单因素引擎（IDW / paleo_isopach / paleo_distance_transform）共用同一份
// 收尾：样式 best-effort 落盘（写失败不拦栅格成果）+ factor.<horizon>.<factorId>
// 栅格声明 + C4 资产关联补盖 + factorGenerated。声明失败不发成功信号。
bool ConstraintWorkflow::declareFactorResult( QgisLayerService *layers,
                                              const QString &horizon, const QString &factorId,
                                              const SingleFactorDefinition &def,
                                              const QString &outPath, const QString &projectDir,
                                              const QString &assetId, QString *error )
{
  if ( !projectDir.isEmpty() )
  {
    QString styleErr; // 降级不打断生成链
    FactorStyleWriter::writeStyleQml( factorId, outPath,
                                      QDir( projectDir ).filePath( QStringLiteral( "styles" ) ),
                                      &styleErr );
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.%1.%2" ).arg( horizon, factorId );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = outPath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  decl.styleRef = def.styleRef;
  decl.title = tr( "%1·%2" ).arg( def.title, horizon );
  // 新成果路径即使字节与旧栅格相同，也已经换了图层源。先抬代次，再改声明。
  ++m_publishGeneration;
  if ( !layers->declare( decl, error ) )
    return false;

  paleo::workflow_detail::stampLayerAssetLink( layers, decl.layerId, assetId ); // C4：已实例化层补盖资产关联
  emit factorGenerated( horizon, factorId, decl.layerId );
  return true;
}

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

namespace
{
bool resolveDeclaredRaster( QgisLayerService *layers, const QString &layerId, QString *path, QString *error );
bool declaredFactorPathMatches( QgisLayerService *layers, const QString &factorLayerId, const QString &rasterPath );

void discardTempTree( const QString &path )
{
  if ( path.contains( QStringLiteral( "paleo-sf-" ) ) )
    QDir( QFileInfo( path ).absolutePath() ).removeRecursively();
}

QString factorIdOf( const QString &horizon, const QString &factorLayerId )
{
  const QString factorPrefix = QStringLiteral( "factor.%1." ).arg( horizon );
  if ( factorLayerId.startsWith( factorPrefix ) )
    return factorLayerId.mid( factorPrefix.size() );
  return factorLayerId;
}
} // namespace

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

namespace
{
bool resolveDeclaredRaster( QgisLayerService *layers, const QString &layerId, QString *path, QString *error )
{
  QVector<LayerDeclaration> declared;
  QString readErr;
  if ( !layers->tryDeclared( &declared, &readErr ) )
  {
    if ( error )
      *error = readErr.isEmpty() ? QObject::tr( "无法读取图层清单" ) : readErr;
    return false;
  }
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId != layerId )
      continue;
    if ( d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
    {
      if ( error )
        *error = QObject::tr( "等值线输入必须是栅格图层：%1" ).arg( layerId );
      return false;
    }
    *path = d.source.section( QLatin1Char( '|' ), 0, 0 );
    return true;
  }
  if ( error )
    *error = QObject::tr( "图层 %1 未在清单声明" ).arg( layerId );
  return false;
}

bool declaredFactorPathMatches( QgisLayerService *layers, const QString &factorLayerId, const QString &rasterPath )
{
  if ( !layers || factorLayerId.isEmpty() || rasterPath.isEmpty() )
    return false;
  QString live;
  if ( !resolveDeclaredRaster( layers, factorLayerId, &live, nullptr ) )
    return false;
  return sameFile( live, rasterPath );
}
} // namespace

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
// ---- m2(B) end ----------------------------------------------------------------

// ---------------------------------------------------------------------------
// 方向20：三组作业的统一异步面（JobRunner 迁移）
//
// 上面 9 个 prepare/compute/publish 一行未改；这里只做「同一个协议的统一
// 入口」：把三组分派到各自的既有实现上，并把忙则互斥、取消接线、进度回包、
// 临时产物清理交给框架。行为等价点见 docs/progress/job-framework.md 的迁移
// 映射表。
// ---------------------------------------------------------------------------

namespace
{
/// 现状三处内联的 dropTemp 判据：只清 `paleo-sf-` 前缀的临时目录，不碰用户
/// 数据。搬到这里，三组共用同一条判据（原先是三份逐字复制的 lambda）。
void dropConstraintTemp( const QString &path )
{
  if ( path.contains( QStringLiteral( "paleo-sf-" ) ) )
    QDir( QFileInfo( path ).absolutePath() ).removeRecursively();
}
} // namespace

bool ConstraintWorkflow::prepareConstraintJob( ConstraintJob &job, const QVariantMap &params,
                                               QString *error )
{
  // params 约定（与各调用点现状一致）：
  //   local_direction : {horizon, factorId, ...} + params
  //   analysis_contour: {horizon, factorLayerId, interval, levels, fixedLevels}
  //   interpretive   : {horizon, factorLayerId, levels, strict}
  const QString kind = params.value( QStringLiteral( "kind" ) ).toString();
  const QString horizon = params.value( QStringLiteral( "horizon" ) ).toString();
  const QString factorId = params.value( QStringLiteral( "factorId" ) ).toString();
  const QString factorLayerId = params.value( QStringLiteral( "factorLayerId" ) ).toString();
  // QVariant 没有 toVector（那是 QVariantList→QVector<double> 的事）：逐个转，
  // 元素非数值的按 0 计入——与各调用点现状「参数面由 UI 构造、类型已定」一致。
  QVector<double> levels;
  const QVariantList levelList = params.value( QStringLiteral( "levels" ) ).toList();
  levels.reserve( levelList.size() );
  for ( const QVariant &v : levelList )
    levels.append( v.toDouble() );

  if ( kind == QLatin1String( "local_direction" ) )
  {
    LocalDirectionJob payload;
    if ( !prepareLocalDirectionJob( horizon, factorId, params, &payload, error ) )
      return false;
    job.kind = ConstraintJobKind::LocalDirection;
    job.payload = payload;
    // 现状局部方向的 stage 词表：<10 prepare / <25 geometry / >=80 encode，
    // 其余 interpolate。原样搬过来，不改阈值。
    job.stageOf = []( double percent ) -> QString {
      const int pct = qBound( 0, qRound( percent ), 100 );
      if ( pct < 10 )
        return QStringLiteral( "prepare" );
      if ( pct < 25 )
        return QStringLiteral( "geometry" );
      if ( pct >= 80 )
        return QStringLiteral( "encode" );
      return QStringLiteral( "interpolate" );
    };
    return true;
  }
  if ( kind == QLatin1String( "analysis_contour" ) )
  {
    AnalysisContourJob payload;
    const double interval = params.value( QStringLiteral( "interval" ) ).toDouble();
    const bool fixedLevels = params.value( QStringLiteral( "fixedLevels" ) ).toBool();
    if ( !prepareAnalysisContourJob( horizon, factorLayerId, interval, levels, fixedLevels,
                                     &payload, error ) )
      return false;
    job.kind = ConstraintJobKind::AnalysisContour;
    job.payload = payload;
    job.stageOf = []( double ) -> QString { return QStringLiteral( "contour" ); };
    return true;
  }
  if ( kind == QLatin1String( "interpretive_contour" ) )
  {
    InterpretiveContourJob payload;
    const bool strict = params.value( QStringLiteral( "strict" ), true ).toBool();
    if ( !prepareInterpretiveContourJob( horizon, factorLayerId, levels, strict, &payload, error ) )
      return false;
    job.kind = ConstraintJobKind::InterpretiveContour;
    job.payload = payload;
    job.stageOf = []( double ) -> QString { return QStringLiteral( "contour" ); };
    return true;
  }
  paleo::workflow_detail::setError( error, tr( "未知的单因素作业类型：%1" ).arg( kind ) );
  return false;
}

bool ConstraintWorkflow::computeConstraintJob( ConstraintJob &job, const std::function<bool()> &cancelled,
                                               const std::function<void(double)> &progress )
{
  // 进度口径对齐现状：各 compute 的 progress 回调收的是百分数（0..100）。
  const std::function<void(double)> pct = [progress]( double p ) {
    if ( progress )
      progress( p );
  };
  switch ( job.kind )
  {
    case ConstraintJobKind::LocalDirection:
      return computeLocalDirectionJob( &std::get<LocalDirectionJob>( job.payload ), cancelled, pct );
    case ConstraintJobKind::AnalysisContour:
      return computeAnalysisContourJob( &std::get<AnalysisContourJob>( job.payload ), cancelled );
    case ConstraintJobKind::InterpretiveContour:
      return computeInterpretiveContourJob( &std::get<InterpretiveContourJob>( job.payload ), cancelled );
  }
  return false;
}

bool ConstraintWorkflow::publishConstraintJob( const ConstraintJob &job, QString *error )
{
  switch ( job.kind )
  {
    case ConstraintJobKind::LocalDirection:
      return publishLocalDirectionJob( std::get<LocalDirectionJob>( job.payload ), error );
    case ConstraintJobKind::AnalysisContour:
      return publishAnalysisContourJob( std::get<AnalysisContourJob>( job.payload ), error );
    case ConstraintJobKind::InterpretiveContour:
      return publishInterpretiveContourJob( std::get<InterpretiveContourJob>( job.payload ), error );
  }
  paleo::workflow_detail::setError( error, tr( "未知的单因素作业类型" ) );
  return false;
}

QStringList ConstraintWorkflow::constraintJobTempPaths( const ConstraintJob &job )
{
  // 各组的临时产物路径：取消/失败时框架 cleanup 钩子据此清理。路径集合与
  // 现状三处 dropTemp 的调用点一一对应。
  QStringList paths;
  switch ( job.kind )
  {
    case ConstraintJobKind::LocalDirection:
    {
      const auto &p = std::get<LocalDirectionJob>( job.payload );
      paths << p.outputPath;
      break;
    }
    case ConstraintJobKind::AnalysisContour:
    {
      const auto &p = std::get<AnalysisContourJob>( job.payload );
      paths << p.outputPath;
      break;
    }
    case ConstraintJobKind::InterpretiveContour:
    {
      const auto &p = std::get<InterpretiveContourJob>( job.payload );
      paths << p.workPath << p.contourPath;
      break;
    }
  }
  return paths;
}

PaleoTask *ConstraintWorkflow::startConstraintJob( paleo::jobs::JobRunner<ConstraintJob> &runner,
                                                  ConstraintJob job, QObject *progressSink )
{
  using paleo::jobs::JobRunner;

  auto shared = std::make_shared<ConstraintJob>( std::move( job ) );
  const QString title = shared->title.isEmpty() ? tr( "单因素作业" ) : shared->title;

  JobRunner<ConstraintJob>::Callbacks cb;

  // prepare 已在调用方完成（各调用点原本就把 prepare 放在 GUI 线程做，且需要
  // 把失败直接呈到页面 statusLabel——这属于 UI 语义，留在 UI 侧）。故框架的
  // prepare 段不重复抓快照，只做一次空转确认。
  cb.prepare = []( ConstraintJob &, QString * ) { return true; };

  // compute 跑在 worker 线程，但它要调用的 computeConstraintJob 是非静态成员
  // （要访问 catalog / layers）。捕获 this 是安全的：这三组的 compute 段本来
  // 就是「读层位文件 + GDAL 栅格运算」，不写活 catalog（#106 owner-thread 写
  // 守卫只覆盖 commit 段的 registrar 写面），与现状 worker 直调
  // computeXxxJob 的线程模型完全一致。
  cb.compute = [this, shared]( ConstraintJob &j, const paleo::jobs::CancelFn &cancelled,
                              const paleo::jobs::ProgressFn &progress ) {
    // 现状三处都是「把百分数映射成 stage 再 reportStage」。这里把 stageOf 的
    // 映射结果交给框架的 ProgressFn，由框架统一节流（50ms）后经
    // PaleoTask::reportStage 上任务面板——不再自行节流。
    return computeConstraintJob(
        j, cancelled,
        [shared, progress]( double percent ) {
          if ( !progress )
            return;
          const int pct = qBound( 0, qRound( percent ), 100 );
          const QString stage = shared->stageOf ? shared->stageOf( percent )
                                                : QStringLiteral( "interpolate" );
          progress( static_cast<double>( pct ), stage );
        } );
  };

  // commit：owner 线程，publish 段——「发布是临界区」，框架此时已不接受取消。
  cb.commit = [this]( ConstraintJob &j, QString * ) {
    QString err;
    return publishConstraintJob( j, &err );
  };

  // cleanup：未成功发布时清临时产物。现状是三处内联 dropTemp，这里换成框架
  // 钩子，判据（只清 paleo-sf- 前缀）保持一致。
  cb.cleanup = []( ConstraintJob &j ) {
    const QStringList paths = constraintJobTempPaths( j );
    for ( const QString &path : paths )
      dropConstraintTemp( path );
  };

  Q_UNUSED( progressSink );
  return runner.start( title, shared, cb, QString(), /*quiet=*/true );
}
