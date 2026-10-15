// 层：功能
// 方向57：从 constraintfactorjobs.cpp 按函数族析出。方法定义逐字搬迁，
// 类契约仍在 workflows.h（公共 API 零改动）；族间共享辅助经
// constraintfactorjobs_internal.h。
#include "../algorithms/ensemblestats.h" // StatsRequest（方向47 集合统计派生）
#include "../algorithms/geostat/kriging.h" // KrigingParams / KrigingResult（方向18）
#include "../algorithms/geostat/sgs.h"       // SgsParams / SgsResult（方向18）
#include "../algorithms/geostat/variogram.h" // 变差函数模型（方向18）
#include "../algorithms/rasterout.h" // PaleoRasterOut / createFloatRaster
#include "../algorithms/singlefactor/cartographicworkfile.h"
#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../domain/singlefactorrequest.h" // 制图工作场不进融合/分相
#include "../domain/singlefactorstrategy.h" // 方向67：策略包词表（strategy_id 校验）
#include "../io/constraintstore.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/factorcontour.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/qgiseditingservice.h" // 拓扑提交门（geometryCommitError）
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisstyleservice.h" // applyFaciesBoundaryStyle
#include "../services/futuretask.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include "../services/singlefactordef.h"
#include "boundarysemantics.h" // 相界地质语义类型词表
#include "constraintfactorjobs_internal.h"
#include "constraintworkflow_internal.h"
#include "derivedassets.h"
#include "mappingworkflow.h"
#include "realizationworkflow.h"
#include "workflows.h"
#include "workflows_internal.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariantList>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <cpl_conv.h>
#include <cstring>
#include <gdal.h>
#include <ogr_spatialref.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h> // WS-C：参与井点 samples GPKG 写出
#include <qgsvectorlayer.h>
#include <qgsvectorlayerfeatureiterator.h>
#include <variant>

using namespace paleo::constraint_detail;

// ---- m2(B) 单因素图页：generateFactor / generateContours ---------------------
bool ConstraintWorkflow::generateFactor( const QString &horizon, const QString &factorId,
                                         const QVariantMap &inputParams, QString *error )
{
  if (factorBusy()) {
    paleo::workflow_detail::setError(
        error, tr("已有单因素任务，请等待或在任务中心取消"));
    return false;
  }
  QVariantMap params = inputParams;
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

  if (!prepareFactorInputs(horizon, factorId, params, error))
    return false;
  if (def.processingAlgId == QLatin1String("paleo:paleo_constraint_idw") &&
      params.value("method").toString() != QLatin1String("structural_idw")) {
    const auto pointsId =
        params.value("pointsLayerId", wellsLayerIdFor(layers, horizon)).toString();
    auto *points = layers->instantiate(pointsId, error);
    if (!points)
      return false;
    const auto extent = points->extent();
    const double cell =
        params.value("cellSize", def.defaultParams.value("cellSize", 1.0)).toDouble();
    if (!std::isfinite(cell) || cell <= 0 ||
        std::ceil(std::max(extent.width() * 1.2, cell * 2) / cell) *
                std::ceil(std::max(extent.height() * 1.2, cell * 2) / cell) >
            4 * 1024 * 1024) {
      paleo::workflow_detail::setError(
          error, tr("单因素网格超过 400 万像元，请增大像元大小或缩小输入范围"));
      return false;
    }
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
  // 方向67：策略包 id 先于一切分派校验——词表（singlefactorstrategy）里查不到
  // 就拒绝，不回退到某个「差不多」的策略（未知 id 混进血缘比缺字段更糟）。
  const QString strategyId = params.value( QStringLiteral( "strategy_id" ) ).toString();
  if ( !strategyId.isEmpty() && !paleo::singlefactor::surfaceMethodPack( strategyId ) )
  {
    paleo::workflow_detail::setError(
        error, tr( "未知制图策略 id：%1（不在 singlefactorstrategy 词表内）" ).arg( strategyId ) );
    return false;
  }
  // structural_idw 按格网分辨率（gridResolution）成图，没有 cellSize 契约——
  // 分派先于像元大小校验。
  if ( method == QLatin1String( "structural_idw" ) )
    return generateStructuralFactor( horizon, factorId, def, params, error );
  if ( !( cellSize > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  // 方向41：local_direction_kriging 走同一本地方向三段式，只在插值面内切引擎
  //（METHOD=kriging）；克里金不成立时该任务自己如实回落 IDW 并记 method_actual。
  if ( method == QLatin1String( "local_direction_idw" ) || method == QLatin1String( "local_direction_kriging" ) )
    return generateLocalDirectionFactor( horizon, factorId, def, params, error );
  if ( method == QLatin1String( "kriging" ) || method == QLatin1String( "sgs" ) ||
       method == QLatin1String( "cokriging" ) )
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
  runParams.insert( QStringLiteral( "LOCAL_GRID_WKT" ), DataCatalog::localGridCrsWkt() );
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

  return runFactorAlgorithm(
      horizon, def.processingAlgId, runParams,
      [=, this](const QVariantMap &results, QString *error) mutable {
        const auto fingerprint = results.value("_derived_fingerprint")
                                     .value<DerivedFileFingerprint>();

        if (results.isEmpty()) {
          if (error && error->isEmpty())
            paleo::workflow_detail::setError(
                error, tr("单因素引擎未返回任何成果（计算失败或被取消）"));
          return false;
        }
        const QString outPath = paleo::workflow_detail::outputPathOf(results);
        if (outPath.isEmpty()) {
          paleo::workflow_detail::setError(error,
                                           tr("单因素生成未返回输出路径"));
          return false;
        }

        QVariantMap extra;
        extra.insert("mapping_product", true);
        extra.insert("layer_id", "product." + st.versionId);
        extra.insert(
            "manifest_layer_id",
            QStringLiteral("factor.%1.%2")
                .arg(horizon,
                     factorId)); // C4：catalog 侧权威反链（清单 layerId）
        extra.insert("layer_type", "raster");
        extra.insert("title", tr("%1·%2").arg(def.title, horizon));
        extra.insert("group", "04_SingleFactor");
        extra.insert(QStringLiteral("factor_id"), factorId);
        extra.insert(QStringLiteral("field"), field);
        extra.insert(QStringLiteral("cell_size"), cellSize);
        extra.insert(QStringLiteral("constrained"), hasConstraints);
        // P1-A：引擎回执（paleo_constraint_idw 的 CONSTRAINT_RECEIPTS）→ 血缘
        // 逐条记实际消费/未用原因；"constrained" 只表示传了约束层，不代表被用。
        paleo::constraint_detail::insertConstraintReceiptExtras(results, extra);
        paleo::constraint_detail::insertSampleReceiptExtras(results, extra);
        extra.insert(QStringLiteral("horizon"), horizon);
        extra.insert(QStringLiteral("kind"),
                     QStringLiteral("single_factor_raster"));
        parentPaths.append(
            property(
                ("paleo.constraint.snapshot." + horizon).toUtf8().constData())
                .toString());
        QStringList parentIds = registrar.parentVersionIdsFor(parentPaths);
        parentIds.append(params.value("parentVersionIds").toStringList());
        parentIds.removeDuplicates();
        if (auto *catalog = PaleoWorkflowDerivedCatalog(this))
          for (const auto &id : parentIds)
            if (catalog->versionById(id).extra.value("mock").toBool())
              extra.insert("mock", true);
        insertStrategyId(
            params,
            extra); // 方向67：策略包 id 进血缘（旧约束 IDW = 词表 "idw"）
        QString commitErr;
        if (!registrar.commitExternal(
                st, outPath, parentIds, def.processingAlgId, extra, &commitErr,
                fingerprint.sha256.isEmpty() ? nullptr : &fingerprint)) {
          paleo::workflow_detail::setError(error, commitErr);
          return false;
        }

        // 色带样式落盘
        // <projectDir>/styles/factor_<factorId>.qml（best-effort）与
        // 声明/盖章/通知收尾见 declareFactorResult（三引擎共用）。
        return declareFactorResult(layers, horizon, factorId, def, outPath,
                                   registrar.projectDir(), st.assetId, error);
      },
      error);
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
  runParams.insert( QStringLiteral( "LOCAL_GRID_WKT" ), DataCatalog::localGridCrsWkt() );
  // 倒置层序（底高于顶）多为重叠/误拾取——默认折 nodata（注册表默认同值）。
  runParams.insert( QStringLiteral( "NEGATIVE_TO_NODATA" ),
                    params.value( QStringLiteral( "negativeToNodata" ),
                                  def.defaultParams.value( QStringLiteral( "negativeToNodata" ), true ) ) );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QStringList parentPaths{top->source().section(QLatin1Char('|'), 0, 0),
                                base->source().section(QLatin1Char('|'), 0, 0)};
  return runFactorAlgorithm(
      horizon, def.processingAlgId, runParams,
      [=, this](const QVariantMap &results, QString *error) mutable {
        const auto fingerprint = results.value("_derived_fingerprint")
                                     .value<DerivedFileFingerprint>();

        // #415 同族：空成果必须带原因返回（引擎失败未设 error 时任务面会
        // 看起来像成功）。
        if (results.isEmpty()) {
          if (error && error->isEmpty())
            paleo::workflow_detail::setError(
                error, tr("等厚因子引擎未返回任何成果（计算失败或被取消）"));
          return false;
        }
        const QString outPath = paleo::workflow_detail::outputPathOf(results);
        if (outPath.isEmpty()) {
          paleo::workflow_detail::setError(error, tr("等厚生成未返回输出路径"));
          return false;
        }

        QVariantMap extra;
        extra.insert(QStringLiteral("factor_id"), factorId);
        extra.insert(QStringLiteral("engine"), def.processingAlgId);
        extra.insert(QStringLiteral("top_layer_id"), topId);
        extra.insert(QStringLiteral("base_layer_id"), baseId);
        QString commitErr;
        if (!registrar.commitExternal(
                st, outPath, registrar.parentVersionIdsFor(parentPaths),
                def.processingAlgId, extra, &commitErr,
                fingerprint.sha256.isEmpty() ? nullptr : &fingerprint)) {
          paleo::workflow_detail::setError(error, commitErr);
          return false;
        }

        return declareFactorResult(layers, horizon, factorId, def, outPath,
                                   registrar.projectDir(), st.assetId, error);
      },
      error);
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
  runParams.insert( QStringLiteral( "LOCAL_GRID_WKT" ), DataCatalog::localGridCrsWkt() );
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

  return runFactorAlgorithm(
      horizon, def.processingAlgId, runParams,
      [=, this](const QVariantMap &results, QString *error) mutable {
        const auto fingerprint = results.value("_derived_fingerprint")
                                     .value<DerivedFileFingerprint>();

        if (results.isEmpty()) {
          if (error && error->isEmpty())
            paleo::workflow_detail::setError(
                error, tr("距离因子引擎未返回任何成果（计算失败或被取消）"));
          return false;
        }
        const QString outPath = paleo::workflow_detail::outputPathOf(results);
        if (outPath.isEmpty()) {
          paleo::workflow_detail::setError(error,
                                           tr("单因素生成未返回输出路径"));
          return false;
        }

        QVariantMap extra;
        extra.insert(QStringLiteral("factor_id"), factorId);
        extra.insert(QStringLiteral("engine"), def.processingAlgId);
        extra.insert(QStringLiteral("cell_size"), cellSize);
        extra.insert(QStringLiteral("constrained"), hasConstraints);
        // P2：距离变换引擎回执 → 血缘（display_only/停用打断线不隔断，逐条留因）。
        paleo::constraint_detail::insertConstraintReceiptExtras(results, extra);
        extra.insert(QStringLiteral("horizon"), horizon);
        extra.insert(QStringLiteral("kind"),
                     QStringLiteral("single_factor_raster"));
        extra.insert(
            QStringLiteral("manifest_layer_id"),
            QStringLiteral("factor.%1.%2").arg(horizon, factorId)); // C4 反链
        extra.insert(QStringLiteral("points_layer_id"), pointsId);
        QString commitErr;
        if (!registrar.commitExternal(
                st, outPath, registrar.parentVersionIdsFor(parentPaths),
                def.processingAlgId, extra, &commitErr,
                fingerprint.sha256.isEmpty() ? nullptr : &fingerprint)) {
          paleo::workflow_detail::setError(error, commitErr);
          return false;
        }

        // 色带样式落盘（welldist 绿→灰预设已备；best-effort）与收尾同 IDW 路径
        // （declareFactorResult，三引擎共用）。
        return declareFactorResult(layers, horizon, factorId, def, outPath,
                                   registrar.projectDir(), st.assetId, error);
      },
      error);
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
  runParams.insert( QStringLiteral( "LOCAL_GRID_WKT" ), DataCatalog::localGridCrsWkt() );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QString stagedQc = fileStem( st.absolutePath ) + QStringLiteral( ".qc.json" );
  const QString stagedStructural = fileStem( st.absolutePath ) + QStringLiteral( ".structural.json" );
  auto discard = [st, stagedQc, stagedStructural]() {
    removeIfPresent( st.absolutePath );
    removeIfPresent( stagedQc );
    removeIfPresent( stagedStructural );
  };

  const auto boundaryCrs = boundary->crs();
  return runFactorAlgorithm(
      horizon, engineId, runParams,
      [=, this](const QVariantMap &results, QString *error) mutable {
        const auto fingerprint = results.value("_derived_fingerprint")
                                     .value<DerivedFileFingerprint>();

        const QString outPath = paleo::workflow_detail::outputPathOf(results);
        if (results.isEmpty() || outPath.isEmpty() ||
            !QFile::exists(stagedQc) || !QFile::exists(stagedStructural)) {
          discard();
          if (results.isEmpty() || outPath.isEmpty()) {
            // proc->run 通常已填 error；同步分支失败时兜底原因，避免
            // jobCompleted(false) 无错误串（#415 同族）。
            if (error && error->isEmpty())
              paleo::workflow_detail::setError(
                  error, tr("结构化插值引擎未返回任何成果（计算失败或被取消）"));
            return false;
          }
          paleo::workflow_detail::setError(
              error, tr("结构化插值未生成旁路成果（qc/structural json）"));
          return false;
        }

        QString jsonErr;
        const QVariantMap qc = m_factorTasks
                                   ? results.value("_qc").toMap()
                                   : readJsonObject(stagedQc, &jsonErr);
        QVariantMap hashParams = qc.value(QStringLiteral("parameters")).toMap();
        if (hashParams.isEmpty()) {
          discard();
          paleo::workflow_detail::setError(
              error,
              jsonErr.isEmpty() ? tr("结构化成果缺少参数指纹输入") : jsonErr);
          return false;
        }
        QStringList parentIds = registrar.parentVersionIdsFor(parentPaths);
        parentIds.append(
            params.value(QStringLiteral("parentVersionIds")).toStringList());
        parentIds.removeDuplicates();
        parentIds.sort();
        QVariantList parentList;
        for (const QString &id : parentIds)
          parentList << id;
        hashParams.insert(QStringLiteral("horizon"), horizon);
        hashParams.insert(QStringLiteral("factor_id"), factorId);
        hashParams.insert(QStringLiteral("parent_version_ids"), parentList);
        const paleo::singlefactor::ParameterHash hash =
            paleo::singlefactor::parameterHash(hashParams);
        if (!hash.ok) {
          discard();
          paleo::workflow_detail::setError(error, hash.error.isEmpty()
                                                      ? tr("参数指纹计算失败")
                                                      : hash.error);
          return false;
        }
        QString shaErr;
        const QString qcSha =
            m_factorTasks ? results.value("_qc_sha").toString()
                          : DataCatalog::sha256FileHex(stagedQc, &shaErr);
        const QString structuralSha =
            m_factorTasks
                ? results.value("_structural_sha").toString()
                : DataCatalog::sha256FileHex(stagedStructural, &shaErr);
        if (qcSha.isEmpty() || structuralSha.isEmpty()) {
          discard();
          paleo::workflow_detail::setError(
              error,
              shaErr.isEmpty() ? tr("旁路文件 sha256 计算失败") : shaErr);
          return false;
        }

        const QDir projectDir(registrar.projectDir());
        const QVariantMap counts = qc.value(QStringLiteral("counts")).toMap();
        const QVariantMap fieldModel =
            qc.value(QStringLiteral("field_model")).toMap();
        QVariantMap extra;
        extra.insert(QStringLiteral("mapping_product"), true);
        extra.insert(QStringLiteral("layer_id"),
                     QStringLiteral("product.") + st.versionId);
        extra.insert(QStringLiteral("manifest_layer_id"),
                     QStringLiteral("factor.%1.%2").arg(horizon, factorId));
        extra.insert(QStringLiteral("layer_type"), QStringLiteral("raster"));
        extra.insert(QStringLiteral("title"),
                     tr("%1·%2").arg(def.title, horizon));
        extra.insert(QStringLiteral("group"),
                     QStringLiteral("04_SingleFactor"));
        extra.insert(QStringLiteral("factor_id"), factorId);
        extra.insert(QStringLiteral("field"), field);
        extra.insert(QStringLiteral("method"),
                     QStringLiteral("structural_idw"));
        extra.insert(QStringLiteral("grid_resolution"),
                     runParams.value(QStringLiteral("GRID_RESOLUTION")));
        extra.insert(QStringLiteral("constrained"), hasConstraints);
        extra.insert(QStringLiteral("horizon"), horizon);
        extra.insert(QStringLiteral("kind"),
                     QStringLiteral("single_factor_raster"));
        extra.insert(QStringLiteral("value_source"),
                     QStringLiteral("analysis"));
        extra.insert(QStringLiteral("parameter_hash"), hash.sha256);
        extra.insert(QStringLiteral("provenance_schema_version"), 1);
        extra.insert(QStringLiteral("algorithm_id"), engineId);
        extra.insert(QStringLiteral("extent_source"),
                     qc.value(QStringLiteral("extent_source")));
        extra.insert(QStringLiteral("crs_mode"),
                     qc.value(QStringLiteral("crs_mode")));
        extra.insert(QStringLiteral("qc_path"),
                     projectDir.relativeFilePath(stagedQc));
        extra.insert(QStringLiteral("qc_sha256"), qcSha);
        extra.insert(QStringLiteral("structural_path"),
                     projectDir.relativeFilePath(stagedStructural));
        extra.insert(QStringLiteral("structural_sha256"), structuralSha);
        extra.insert(QStringLiteral("finite_cells"),
                     counts.value(QStringLiteral("finite")));
        extra.insert(QStringLiteral("nodata_cells"),
                     counts.value(QStringLiteral("nodata")));
        extra.insert(
            QStringLiteral("direction_coverage_percent"),
            fieldModel.value(QStringLiteral("direction_coverage_percent")));
        extra.insert(QStringLiteral("barrier_buffer_distance"),
                     qc.value(QStringLiteral("barrier_buffer_distance")));
        extra.insert(QStringLiteral("contour_stop_buffer_distance"),
                     qc.value(QStringLiteral("contour_stop_buffer_distance")));
        // P3 任务G：结构面引擎回执（QC constraint_receipts）进血缘——此前只记
        // constrained（是否传了约束层），看不到哪条约束实际参与。
        paleo::constraint_detail::insertQcConstraintReceiptExtras(qc, extra);
        inheritMockFlag(PaleoWorkflowDerivedCatalog(this), parentIds, extra);
        insertStrategyId(
            params, extra); // 方向67：策略包 id 进血缘（structural_idw 词表项）
        QString commitErr;
        if (!registrar.commitExternal(
                st, outPath, parentIds, engineId, extra, &commitErr,
                fingerprint.sha256.isEmpty() ? nullptr : &fingerprint)) {
          discard();
          paleo::workflow_detail::setError(error, commitErr);
          return false;
        }

        // 参与井点（非控制点）→ samples.<层位>.<因素>
        // 点图层（样式在后续工作流）。
        const QVariantList wells = qc.value(QStringLiteral("wells")).toList();
        if (!wells.isEmpty()) {
          const QString samplesPath =
              fileStem(st.absolutePath) + QStringLiteral(".samples.gpkg");
          QString samplesErr;
          if (m_factorTasks
                  ? QFile::exists(samplesPath)
                  : writeStructuralSamples(samplesPath, wells, boundaryCrs,
                                           &samplesErr)) {
            LayerDeclaration sdecl;
            sdecl.layerId =
                QStringLiteral("samples.%1.%2").arg(horizon, factorId);
            sdecl.horizon = horizon;
            sdecl.type = QStringLiteral("vector");
            sdecl.source = samplesPath + QStringLiteral("|layername=samples");
            sdecl.group = QStringLiteral("04_SingleFactor");
            sdecl.title = tr("参与井点");
            layers->declare(sdecl); // best-effort：井点层失败不打断因素成果
          }
        }

        return declareFactorResult(layers, horizon, factorId, def,
                                   st.absolutePath, registrar.projectDir(),
                                   st.assetId, error);
      },
      error);
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
  if (m_factorTasks) {
    auto shared = std::make_shared<LocalDirectionJob>(job);
    return runFactorWork(
        tr("%1 单因素计算").arg(horizon),
        [proc = m_proc.data(), shared](PaleoTask *task) {
          const bool ok = computeLocalDirectionJobWithService(
              proc, shared.get(), [task] { return task->cancelRequested(); },
              [task](double p) {
                task->reportStage("build", qBound(0, qRound(p), 95));
              });
          return ok ? QString() : shared->error;
        },
        [this, shared](QString *error) {
          return publishLocalDirectionJob(*shared, error);
        },
        [shared] {
          if (shared->outputPath.contains("paleo-sf-"))
            QDir(QFileInfo(shared->outputPath).absolutePath())
                .removeRecursively();
          if (shared->staging.isValid()) {
            const auto path = shared->staging.absolutePath;
            for (const auto &file :
                 QStringList{path, fileStem(path) + ".qc.json",
                             fileStem(path) + ".support.tif"})
              QFile::remove(file);
          }
        },
        error);
  }
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

bool ConstraintWorkflow::factorBusy() const {
  return m_factorJobTask && m_factorJobTask->running();
}
bool ConstraintWorkflow::runFactorWork(
    const QString &title, std::function<QString(PaleoTask *)> compute,
    std::function<bool(QString *)> publish, std::function<void()> cleanup,
    QString *error) {
  if (!m_factorTasks) {
    const auto err = compute(nullptr);
    if (!err.isEmpty()) {
      if (cleanup)
        cleanup();
      paleo::workflow_detail::setError(error, err);
      return false;
    }
    return publish(error);
  }
  if (factorBusy()) {
    paleo::workflow_detail::setError(error, tr("已有单因素任务"));
    return false;
  }
  struct Pending {
    std::function<void()> cleanup;
    bool published = false;
    ~Pending() {
      if (!published && cleanup)
        cleanup();
    }
  };
  auto pending = std::make_shared<Pending>();
  pending->cleanup = std::move(cleanup);
  const auto catalog = m_catalog;
  const auto dir = m_projectDir;
  const auto generation = m_publishGeneration;
  const auto sequence = m_catalog ? m_catalog->mutationSeq() : 0;
  const auto session = m_factorTasks->session();
  QPointer<PaleoTask> task = m_factorTasks->beginExternal(title);
  m_factorJobTask = task;
  emit factorJobStateChanged(true);
  connect(task, &PaleoTask::changed, this, [this, task] {
    if (task && task->running())
      emit factorJobProgress(task->percent(), task->stage());
  });
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(task, &PaleoTask::cancelAsked, watcher,
          [watcher] { watcher->future().cancel(); });
  connect(watcher, &QObject::destroyed, task, [task, pending] {
    if (task && task->running()) {
      task->requestCancel();
      task->finishExternal();
    }
  });
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, task, catalog, dir, generation, sequence, session,
           publish, pending] {
            QString error;
            if (!task || task->cancelRequested() ||
                watcher->future().isCanceled()) {
              if (task)
                task->requestCancel();
            } else if (catalog != m_catalog || dir != m_projectDir ||
                       generation != m_publishGeneration ||
                       (catalog && catalog->mutationSeq() != sequence) ||
                       session != m_factorTasks->session()) {
              task->requestCancel();
            } else {
              error = watcher->result();
              if (error.isEmpty()) {
                task->reportStage("publish", 98);
                pending->published = publish(&error);
              }
              if (!pending->published && error.isEmpty())
                error = tr("成果发布失败");
              if (!error.isEmpty())
                emit factorJobFailed(error);
            }
            if (pending->published)
              emit factorJobProgress(100, QStringLiteral("publish"));
            else if (task && task->cancelRequested())
              emit factorJobCancelled();
            if (task)
              task->finishExternal(error);
            emit factorJobStateChanged(false);
            // Disconnect destruction cancellation: terminal status is queued.
            disconnect(watcher, &QObject::destroyed, task, nullptr);
            watcher->deleteLater();
          });
  watcher->setFuture(QtConcurrent::run(paleo::tasks::pool(m_factorTasks),
                                       [task, compute, pending] {
                                         if (!task || task->cancelRequested())
                                           return QString();
                                         try {
                                           return compute(task);
                                         } catch (const std::exception &e) {
                                           return QString::fromUtf8(e.what());
                                         } catch (...) {
                                           return QStringLiteral("计算异常");
                                         }
                                       }));
  return true;
}
bool ConstraintWorkflow::runFactorAlgorithm(
    const QString &horizon, const QString &engine, const QVariantMap &params,
    std::function<bool(const QVariantMap &, QString *)> publish,
    QString *error) {
  if (!m_factorTasks) {
    const auto results = m_proc->run(engine, params, error);
    return !results.isEmpty() && publish(results, error);
  }
  struct Source {
    QString key, uri, provider, type, crs;
    std::shared_ptr<QgsVectorLayerFeatureSource> features;
    Qgis::WkbType wkb;
  };
  std::vector<Source> sources;
  auto options = params;
  for (auto it = params.cbegin(); it != params.cend(); ++it) {
    auto *layer = it.value().value<QgsMapLayer *>();
    if (!layer)
      continue;
    Source source{it.key(),
                  layer->source(),
                  layer->providerType(),
                  qobject_cast<QgsRasterLayer *>(layer) ? "raster" : "vector",
                  layer->crs().toWkt(),
                  {},
                  Qgis::WkbType::Unknown};
    if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
      source.features = std::make_shared<QgsVectorLayerFeatureSource>(vector);
      source.wkb = vector->wkbType();
    }
    sources.push_back(std::move(source));
    options.remove(it.key());
  }
  auto results = std::make_shared<QVariantMap>();
  const QString path = params.value("OUTPUT").toString();
  auto *proc = m_proc.data();
  return runFactorWork(
      tr("%1 单因素计算").arg(horizon),
      [proc, sources, options, engine, results, path](PaleoTask *task) mutable {
        const auto cancel = [task] { return task && task->cancelRequested(); };
        std::vector<std::unique_ptr<QgsMapLayer>> layers;
        for (const auto &source : sources) {
          if (cancel())
            return QString();
          std::unique_ptr<QgsMapLayer> layer;
          if (source.type == "raster")
            layer = std::make_unique<QgsRasterLayer>(source.uri, source.key,
                                                     source.provider);
          else {
            auto vector = std::make_unique<QgsVectorLayer>(
                QgsWkbTypes::displayString(source.wkb), source.key, "memory");
            auto *provider = vector->dataProvider();
            if (!provider ||
                !provider->addAttributes(source.features->fields().toList()))
              return QStringLiteral("无法准备矢量输入");
            vector->updateFields();
            auto iterator = source.features->getFeatures();
            QgsFeature feature;
            QgsFeatureList batch;
            while (iterator.nextFeature(feature)) {
              if (cancel())
                return QString();
              batch << feature;
              if (batch.size() == 1024) {
                if (!provider->addFeatures(batch))
                  return QStringLiteral("矢量输入准备失败");
                batch.clear();
              }
            }
            if (!batch.isEmpty() && !provider->addFeatures(batch))
              return QStringLiteral("矢量输入准备失败");
            vector->updateExtents();
            layer = std::move(vector);
          }
          if (!layer->isValid())
            return QStringLiteral("无法读取计算输入：") + source.uri;
          layer->setCrs(QgsCoordinateReferenceSystem::fromWkt(source.crs));
          options.insert(source.key, QVariant::fromValue(layer.get()));
          layers.push_back(std::move(layer));
        }
        QgisProcessingService::ProcessingHooks hooks;
        hooks.cancelled = cancel;
        hooks.progress = [task](double p) {
          if (task)
            task->reportStage("build", qBound(0, qRound(p), 95));
        };
        QString error;
        *results = proc->run(engine, options, &error, hooks);
        if (results->isEmpty() && error.isEmpty() && !cancel())
          error = QStringLiteral("计算没有返回成果");
        if (!error.isEmpty() || cancel() || results->isEmpty())
          return error;
        const auto actual = paleo::workflow_detail::outputPathOf(*results);
        if (QDir::cleanPath(actual) != QDir::cleanPath(path)) {
          if (!QFile::copy(actual, path))
            return QStringLiteral("无法暂存计算成果");
          results->insert("OUTPUT", path);
        }
        task->reportStage("hash", 96);
        const auto fingerprint =
            DerivedFileFingerprint::read(path, cancel, &error);
        if (fingerprint.sha256.isEmpty())
          return error;
        results->insert("_derived_fingerprint",
                        QVariant::fromValue(fingerprint));

        if (engine == QLatin1String("paleo:paleo_structural_idw")) {
          const auto stem = fileStem(path);
          results->insert("_qc_sha", DataCatalog::sha256FileHex(
                                         stem + ".qc.json", &error, cancel));
          results->insert("_structural_sha",
                          DataCatalog::sha256FileHex(stem + ".structural.json",
                                                     &error, cancel));
          const auto qc = readJsonObject(stem + ".qc.json", &error);
          results->insert("_qc", qc);
          const auto wells = qc.value("wells").toList();
          if (!cancel() && !wells.isEmpty()) {
            const auto *boundary =
                options.value("BOUNDARY").value<QgsMapLayer *>();
            QString sampleError;
            writeStructuralSamples(stem + ".samples.gpkg", wells,
                                   boundary ? boundary->crs()
                                            : QgsCoordinateReferenceSystem(),
                                   &sampleError);
          }
        }

        return error;
      },
      [results, publish](QString *error) { return publish(*results, error); },
      [path] {
        if (path.isEmpty())
          return;
        const auto stem = fileStem(path);
        for (const QString &file :
             QStringList{path, stem + ".qc.json", stem + ".structural.json",
                         stem + ".support.tif", stem + ".samples.gpkg"})
          QFile::remove(file);
      },
      error);
}

void ConstraintWorkflow::cancelFactorTask() {
  if (m_factorJobTask)
    m_factorJobTask->requestCancel();
}
