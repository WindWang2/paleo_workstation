// 层：功能
#include "workflows.h"
#include "workflows_internal.h"

#include "../catalog/datacatalog.h"
#include "../domain/singlefactorrequest.h"  // 制图工作场不进融合/分相
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisstyleservice.h"       // applyFaciesBoundaryStyle
#include "../services/projectdata.h"
#include "../algorithms/faciesmapping/faciesqa.h" // 方向 39：边界核查引擎（只接不重写）
#include "boundaryeditrules.h"              // 方向 39：相界类型编辑语义门禁
#include "boundarysemantics.h"              // 相界地质语义类型词表
#include "derivedassets.h"
#include "mappingworkflow.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QSet>

#include <qgsabstractgeometry.h>
#include <qgscurve.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsmultipolygon.h>
#include <qgspoint.h>
#include <qgspolygon.h>
#include <qgsvectorlayer.h>

// 综合编图工作流（③）。
// 方向20 轮4：按类边界从 workflows.cpp 析出。头文件契约不动。


namespace
{
  // 方向 39：从图层现势数据装配某要素的边界语义事实（kind/transitionWidth
  // 由调用方用提案值覆写）。邻接 = 与他要素交集为线（共享边），非仅相交；
  // 相代码未知（字段缺/值非法）如实记 -1，不参与差异判定。
  BoundarySemantics::BoundaryEditFacts boundaryFactsOf( QgsVectorLayer *vl, const QgsFeature &f )
  {
    using namespace BoundarySemantics;
    BoundaryEditFacts facts;
    const int codeIdx = vl->fields().lookupField( QStringLiteral( "facies_code" ) );
    if ( codeIdx >= 0 && f.attribute( codeIdx ).isValid() )
    {
      bool ok = false;
      const int code = f.attribute( codeIdx ).toInt( &ok );
      facts.selfFaciesCode = ok ? code : -1;
    }

    const QgsGeometry geom = f.geometry();
    if ( geom.type() == Qgis::GeometryType::Polygon && !geom.isNull() )
    {
      if ( const QgsPolygon *poly = qgsgeometry_cast< const QgsPolygon * >( geom.constGet() ) )
      {
        // 环闭合：外环首尾精确重合（存储环按构造闭合；口径同 faciesqa closedCopy）。
        if ( const QgsCurve *ring = poly->exteriorRing() )
        {
          const QgsPoint first = ring->startPoint();
          const QgsPoint last = ring->endPoint();
          facts.ringClosed = first.x() == last.x() && first.y() == last.y();
        }
      }
      QgsFeature other;
      QgsFeatureIterator it = vl->getFeatures();
      while ( it.nextFeature( other ) )
      {
        if ( other.id() == f.id() )
          continue;
        const QgsGeometry og = other.geometry();
        if ( og.isNull() || !geom.intersects( og ) )
          continue;
        const QgsGeometry shared = geom.intersection( og );
        if ( shared.type() != Qgis::GeometryType::Line || shared.length() <= 0 )
          continue; // 相交成面（重叠）不算共享边邻接
        facts.hasAdjacent = true;
        int otherCode = -1;
        if ( codeIdx >= 0 && other.attribute( codeIdx ).isValid() )
        {
          bool ok = false;
          const int code = other.attribute( codeIdx ).toInt( &ok );
          otherCode = ok ? code : -1;
        }
        if ( otherCode >= 0 && facts.selfFaciesCode >= 0 && otherCode != facts.selfFaciesCode )
        {
          facts.adjacentFaciesDiffers = true;
          facts.adjacentFaciesCode = otherCode;
        }
      }
    }
    return facts;
  }
} // namespace

// ---------------------------------------------------------------------------
// CompositionWorkflow — ③综合编图
// ---------------------------------------------------------------------------

CompositionWorkflow::CompositionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_proc( proc ), m_layers( layers )
{
  // 方向 39：相多边形层重实例化（工程重开/层位组释放重建）后重挂相界语义
  // 符号——instantiate 总是从源新建图层，编辑会话里设置的渲染器不随实例
  // 存活；已提交的 boundary_kind/transition_width 字段在，样式就按词表
  // 重建（无字段 → applyFaciesBoundaryStyle 自身无操作）。
  if ( m_layers )
  {
    connect( m_layers, &QgisLayerService::layerInstantiated, this,
             [this]( const QString &layerId ) {
               if ( !layerId.startsWith( QLatin1String( "facies." ) ) )
                 return;
               if ( auto *vl = qobject_cast<QgsVectorLayer *>( m_layers->layer( layerId ) ) )
               {
                 QgisStyleService::applyFaciesBoundaryStyle( vl );
                 vl->triggerRepaint();
               }
             } );
  }
}

void CompositionWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

DataCatalog *CompositionWorkflow::catalog() const
{
  return m_catalog.data();
}

QgisProcessingService *CompositionWorkflow::processingService() const
{
  return m_proc.data();
}

QgisLayerService *CompositionWorkflow::layerService() const
{
  return m_layers.data();
}

bool CompositionWorkflow::fuseFactors( const QString &horizon, const QStringList &factorLayerIds,
                                       QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "composition workflow is not bound to services" ) );
    return false;
  }
  if ( factorLayerIds.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "no factor layers supplied for fusion" ) );
    return false;
  }

  QVector<LayerDeclaration> decls;
  QString readErr;
  if ( !layers->tryDeclared( &decls, &readErr ) )
  {
    paleo::workflow_detail::setError( error, readErr.isEmpty() ? tr( "无法读取图层清单" ) : readErr );
    return false;
  }

  QVariantList inputs;
  inputs.reserve( factorLayerIds.size() );
  QStringList parentPaths;
  for ( const QString &layerId : factorLayerIds )
  {
    const LayerDeclaration *decl = nullptr;
    for ( const LayerDeclaration &d : decls )
    {
      if ( d.layerId == layerId )
      {
        decl = &d;
        break;
      }
    }
    if ( !decl )
    {
      paleo::workflow_detail::setError( error, tr( "图层 %1 未在清单声明" ).arg( layerId ) );
      return false;
    }
    if ( paleo::singlefactor::isCartographicProductLayer( layerId, decl->group ) )
    {
      paleo::workflow_detail::setError( error, tr( "解释性制图工作场不能参与连续融合、分相或厚度统计" ) );
      return false;
    }
    QgsMapLayer *layer = layers->instantiate( layerId, error );
    if ( !layer )
      return false;
    inputs.append( QVariant::fromValue( layer ) );
    parentPaths.append( layer->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  // T26：融合栅格落 artifacts/derived + DERIVED 版本（父版本 = 各单因素栅格）。
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "facies_fusion_raster" ), tr( "%1 相融合" ).arg( horizon ),
      QStringLiteral( "FUSION_%1.tif" ).arg( horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }

  QVariantMap params;
  params.insert( QStringLiteral( "INPUTS" ), inputs );
  params.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_facies_fusion" ), params, error );
  if ( results.isEmpty() )
    return false;

  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "facies fusion returned no output path" ) );
    return false;
  }
  QVariantMap fusionExtra;
  fusionExtra.insert( QStringLiteral( "factors" ), factorLayerIds );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "paleo:paleo_facies_fusion" ), fusionExtra,
                                  &commitErr ) )
  {
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "composite.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "03_Composite" );
  if ( !layers->declare( decl, error ) )
    return false;

  paleo::workflow_detail::stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit compositionDone( horizon, decl.layerId );
  return true;
}

bool CompositionWorkflow::deriveFaciesPolygons( const QString &horizon, const QString &rasterLayerId,
                                               const QVariantMap &params, QString *error )
{
  const auto fail = [this, &horizon, error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    emit faciesPolygonsFailed( horizon, msg );
    return false;
  };

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( rasterLayerId.isEmpty() )
    return fail( tr( "no raster layer supplied for facies polygons" ) );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );

  QString declHorizon;
  QString declGroup;
  bool found = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == rasterLayerId )
    {
      declHorizon = d.horizon;
      declGroup = d.group;
      found = true;
      break;
    }
  }
  if ( !found )
    return fail( tr( "raster layer '%1' is not declared" ).arg( rasterLayerId ) );
  if ( paleo::singlefactor::isCartographicProductLayer( rasterLayerId, declGroup ) )
    return fail( tr( "解释性制图工作场不能参与分相或转面" ) );

  const QString h = !declHorizon.isEmpty() ? declHorizon : horizon;
  if ( h.isEmpty() )
    return fail( tr( "cannot derive facies polygons without a horizon" ) );

  QgsMapLayer *raster = layers->instantiate( rasterLayerId, error );
  if ( !raster )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to instantiate raster '%1'" ).arg( rasterLayerId ) );
  }

  // T26：相多边形 gpkg 落 artifacts/derived + DERIVED 版本（父版本 = 被多边形
  // 化的栅格与约束图层的文件源版本）。
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
    return fail( regErr );
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "facies_polygons" ), tr( "%1 相多边形" ).arg( h ),
      QStringLiteral( "FACIES_%1.gpkg" ).arg( h ), &regErr );
  if ( !st.isValid() )
    return fail( regErr );
  QStringList parentPaths{ raster->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap alg;
  alg.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( raster ) );
  alg.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );
  const QStringList forwarded = {
      QStringLiteral( "MIN_AREA" ), QStringLiteral( "SIMPLIFY" ),
      QStringLiteral( "SNAP_TOLERANCE" ), QStringLiteral( "ANGLE_TOLERANCE" ) };
  for ( const QString &key : forwarded )
  {
    if ( params.contains( key ) )
      alg.insert( key, params.value( key ) );
  }
  const QString constraintId = params.value( QStringLiteral( "CONSTRAINT_LAYER" ) ).toString();
  if ( !constraintId.isEmpty() )
  {
    QgsMapLayer *constraints = layers->instantiate( constraintId, error );
    if ( !constraints )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to instantiate constraints '%1'" ).arg( constraintId ) );
    }
    alg.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_facies_polygonize" ), alg, error );
  if ( results.isEmpty() )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "facies polygonize produced no results" ) );
  }
  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( outPath.isEmpty() )
    return fail( tr( "facies polygonize returned no output path" ) );
  QVariantMap faciesExtra;
  faciesExtra.insert( QStringLiteral( "raster_layer" ), rasterLayerId );
  faciesExtra.insert( QStringLiteral( "constrained" ), !constraintId.isEmpty() );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "paleo:paleo_facies_polygonize" ), faciesExtra,
                                  &commitErr ) )
    return fail( commitErr );

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "facies.%1" ).arg( h );
  decl.horizon = h;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( st.absolutePath );
  decl.group = QStringLiteral( "05_PaleoMap" );
  if ( !layers->declare( decl, error ) )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to declare facies layer '%1'" ).arg( decl.layerId ) );
  }

  paleo::workflow_detail::stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit faciesPolygonsReady( h, decl.layerId );
  return true;
}

// ---- m2(C)：相属性回写 -------------------------------------------------------
QString CompositionWorkflow::prepareFaciesForEditing( const QString &layerId, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    return QString();
  };

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( layerId.isEmpty() )
    return fail( tr( "no facies layer supplied" ) );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
  LayerDeclaration decl;
  bool found = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == layerId )
    {
      decl = d;
      found = true;
      break;
    }
  }
  if ( !found )
    return fail( tr( "layer '%1' is not declared" ).arg( layerId ) );

  const QString srcPath = decl.source.section( QLatin1Char( '|' ), 0, 0 );
  const QString srcSuffix = decl.source.contains( QLatin1Char( '|' ) )
                                ? decl.source.section( QLatin1Char( '|' ), 1 )
                                : QString();
  if ( !QFileInfo( srcPath ).isFile() )
    return layerId; // 非文件源（memory 等）——交给常规编辑路径
  const QFile::Permissions perms = QFile::permissions( srcPath );
  if ( perms & ( QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther ) )
    return layerId; // 已可写（不是 T26 只读派生件）——原样

  const QString projectDir = PaleoWorkflowDerivedProjectDir( this );
  if ( projectDir.isEmpty() )
    return fail( tr( "无法定位工程目录来铺相界工作副本（%1）" ).arg( layerId ) );
  QString safeId = layerId;
  safeId.replace( QLatin1Char( '/' ), QLatin1Char( '_' ) )
      .replace( QLatin1Char( '\\' ), QLatin1Char( '_' ) );
  const QString workDir =
      QDir( projectDir ).filePath( QStringLiteral( "artifacts/layers/facies" ) );
  const QString workPath = QDir( workDir ).filePath( QStringLiteral( "%1.gpkg" ).arg( safeId ) );
  if ( !QDir().mkpath( workDir ) )
    return fail( tr( "cannot create %1" ).arg( workDir ) );
  if ( QFileInfo::exists( workPath ) && !QFile::remove( workPath ) )
    return fail( tr( "cannot replace stale working copy %1" ).arg( workPath ) );
  if ( !QFile::copy( srcPath, workPath ) )
    return fail( tr( "cannot copy %1 → %2" ).arg( srcPath, workPath ) );
  QFile::setPermissions( workPath,
                         QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ReadUser | QFileDevice::WriteUser |
                             QFileDevice::ReadGroup | QFileDevice::ReadOther );

  // manifest 同 id 重指（layerId 不变——页面/导出/引用方无感）；只读原件
  // 留在 artifacts/derived，catalog 版本 sha 不动。
  LayerDeclaration work = decl;
  work.source = srcSuffix.isEmpty() ? workPath
                                    : QStringLiteral( "%1|%2" ).arg( workPath, srcSuffix );
  QString declErr;
  if ( !layers->declare( work, &declErr ) )
    return fail( declErr );

  // 实例缓存里可能压着只读旧层：按层位整组释放（manifest 是权威，图层按
  // 需重实例化；实例由 QgsProject 持有，removeMapLayer 即析构）。
  if ( layers->isInstantiated( layerId ) )
    layers->releaseHorizon( decl.horizon );
  return layerId;
}

bool CompositionWorkflow::saveFaciesAttributes( const QString &layerId, const QVariantMap &attrs,
                                                QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    return false;
  };

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( layerId.isEmpty() )
    return fail( tr( "no facies layer supplied" ) );

  QgsMapLayer *l = layers->instantiate( layerId, error );
  auto *vl = qobject_cast<QgsVectorLayer *>( l );
  if ( !vl )
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "layer '%1' is not a vector layer" ).arg( layerId ) );

  // 页面拿不到选中要素 id：以图层当前选中集为准（空集 → 拒绝并说明）。
  QgsFeatureIds selected = vl->selectedFeatureIds();
  if ( selected.isEmpty() )
    return fail( tr( "先在地图上选中要改相属性的要素（%1）" ).arg( layerId ) );

  // 只读派生件兜底：startEditing 失败且源只读 → 铺工作副本重指后重试一次
  // （fid 跨实例稳定，选中集随行迁移）。
  if ( !vl->isEditable() && !vl->startEditing() )
  {
    QString prepErr;
    const QString prepared = prepareFaciesForEditing( layerId, &prepErr );
    if ( prepared.isEmpty() )
      return fail( tr( "cannot start editing on '%1'（%2）" ).arg( layerId, prepErr ) );
    vl = qobject_cast<QgsVectorLayer *>( layers->instantiate( layerId, error ) );
    if ( !vl )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to re-instantiate '%1' after preparing edit copy" )
                             .arg( layerId ) );
    vl->select( selected );
    if ( !vl->startEditing() )
      return fail( tr( "cannot start editing on '%1'" ).arg( layerId ) );
  }

  // 字段词表：facies_code 是 polygonize 算法产物（int）；facies_type/comment
  // 为编辑面字段，缺则先补建（补建也在编辑会话内，各自成 undo 步）。
  // boundary_kind 是相界地质语义类型（C2，词表 BoundarySemantics）；值域
  // 校验在页面下拉侧 + 方向 39 语义门禁（见下）。transition_width 是相变
  // 渐变带宽度（double，图层地图单位；仅相变可携带——门禁拒绝其余类型）。
  struct FieldSpec
  {
    const char *key;
    const char *name;
    QVariant::Type type;
  };
  const FieldSpec wanted[] = {
    { "facies_code", "facies_code", QVariant::Int },
    { "facies_type", "facies_type", QVariant::String },
    { "comment", "comment", QVariant::String },
    { "boundary_kind", "boundary_kind", QVariant::String },
    { "transition_width", "transition_width", QVariant::Double },
  };

  // ---- 方向 39：相界类型编辑语义门禁（任一选中要素被拒 → 整单失败，
  // 未写任何字段——先于 beginEditCommand；拒绝原因含冻结词面回页面）。---
  // 带域门禁只判本次提交的带宽提案（attrs 携带 transition_width 时）；
  // 换类型不携带带宽键则不判旧值——旧带宽数据保留但惰性（非相变类目
  // 不渲染带、QA 不核查），不静默清零也不把用户锁死在旧类型上。
  if ( attrs.contains( QLatin1String( "boundary_kind" ) ) ||
       attrs.contains( QLatin1String( "transition_width" ) ) )
  {
    const bool kindInAttrs = attrs.contains( QLatin1String( "boundary_kind" ) );
    const int kindIdx = vl->fields().lookupField( QLatin1String( "boundary_kind" ) );
    for ( const QgsFeatureId fid : selected )
    {
      const QgsFeature current = vl->getFeature( fid );
      BoundarySemantics::BoundaryEditFacts facts = boundaryFactsOf( vl, current );
      if ( kindInAttrs )
        facts.kind = attrs.value( QLatin1String( "boundary_kind" ) ).toString();
      else if ( kindIdx >= 0 )
      {
        const QVariant kind = current.attribute( kindIdx );
        facts.kind = kind.isValid() ? kind.toString() : QString();
      }
      if ( attrs.contains( QLatin1String( "transition_width" ) ) )
      {
        bool ok = false;
        const double width = attrs.value( QLatin1String( "transition_width" ) ).toDouble( &ok );
        if ( !ok )
          return fail( tr( "渐变带宽度须是数值：%1" )
                           .arg( attrs.value( QLatin1String( "transition_width" ) ).toString() ) );
        facts.transitionWidth = width;
      }
      const BoundarySemantics::BoundaryEditVerdict verdicts[] = {
        BoundarySemantics::checkKindAssignment( facts ),
        BoundarySemantics::checkTransitionBand( facts ),
        BoundarySemantics::checkRingClosure( facts ),
      };
      for ( const BoundarySemantics::BoundaryEditVerdict &v : verdicts )
      {
        if ( !v.accepted )
          return fail( tr( "%1（要素 %2）" ).arg( v.reason ).arg( fid ) );
      }
    }
  }

  // 只写 attrs 携带的字段。
  QVector<QPair<int, QVariant>> writes; // resolved field index + value
  for ( const FieldSpec &f : wanted )
  {
    if ( !attrs.contains( QLatin1String( f.key ) ) )
      continue;
    QVariant value = attrs.value( QLatin1String( f.key ) );
    if ( value.type() == QVariant::String && f.type == QVariant::Int )
    {
      // 页面校验过整数，这里兜底：非整数串如实拒绝，不静默落 0。
      bool ok = false;
      const int code = value.toInt( &ok );
      if ( !ok )
        return fail( tr( "相代码须是整数：%1" ).arg( value.toString() ) );
      value = code;
    }
    int idx = vl->fields().lookupField( QLatin1String( f.name ) );
    if ( idx < 0 )
    {
      if ( !vl->addAttribute( QgsField( QLatin1String( f.name ), f.type ) ) )
        return fail( tr( "cannot add field '%1' to %2" ).arg( QLatin1String( f.name ), layerId ) );
      idx = vl->fields().lookupField( QLatin1String( f.name ) );
      if ( idx < 0 )
        return fail( tr( "field '%1' not visible after add" ).arg( QLatin1String( f.name ), layerId ) );
    }
    writes.append( qMakePair( idx, value ) );
  }

  vl->beginEditCommand( tr( "编辑相属性" ) );
  for ( const auto &w : writes )
    for ( const QgsFeatureId fid : selected )
      vl->changeAttributeValue( fid, w.first, w.second );
  vl->endEditCommand();

  // C2：boundary_kind 变更即时反映到图面（相界语义符号映射——渲染器按
  // boundary_kind 分类描边；无该字段的层为无操作）。transition_width 同样
  // 挂相变渐变带的 data-defined 带宽（方向 39）。
  if ( attrs.contains( QLatin1String( "boundary_kind" ) ) ||
       attrs.contains( QLatin1String( "transition_width" ) ) )
    QgisStyleService::applyFaciesBoundaryStyle( vl );
  vl->triggerRepaint();
  return true;
}

// ---- 方向 39：相界边界核查（faciesqa 只接不重写）---------------------------

QVariantList CompositionWorkflow::runFaciesBoundaryQa( const QString &layerId, QString *error )
{
  using paleo::faciesmapping::FaciesQaIssue;
  using paleo::faciesmapping::FaciesQaIssueType;
  using paleo::faciesmapping::FaciesMapUnit;
  using paleo::faciesmapping::runFaciesQa;

  const auto fail = [error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    return QVariantList();
  };

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  QgsMapLayer *l = layers->instantiate( layerId, error );
  auto *vl = qobject_cast<QgsVectorLayer *>( l );
  if ( !vl )
    return fail( ( error && !error->isEmpty() ) ? *error
                     : tr( "层 '%1' 不是矢量层，无法核查相界" ).arg( layerId ) );
  if ( vl->geometryType() != Qgis::GeometryType::Polygon )
    return fail( tr( "层 '%1' 不是面层——相界核查面向相多边形" ).arg( layerId ) );

  const int codeIdx = vl->fields().lookupField( QStringLiteral( "facies_code" ) );
  const int kindIdx = vl->fields().lookupField( QStringLiteral( "boundary_kind" ) );
  const int widthIdx = vl->fields().lookupField( QStringLiteral( "transition_width" ) );

  // 部件环 → QA 环点（存储环为直线环，pointAt 逐点取；无曲线展开）。
  const auto ringPoints = []( const QgsCurve *curve ) {
    std::vector<paleo::singlefactor::Point2> pts;
    if ( !curve )
      return pts;
    const int n = curve->numPoints();
    pts.reserve( static_cast<std::size_t>( std::max( n, 0 ) ) );
    for ( int k = 0; k < n; ++k )
    {
      QgsPoint p;
      Qgis::VertexType type = Qgis::VertexType::Segment;
      if ( curve->pointAt( k, p, type ) )
        pts.push_back( { p.x(), p.y() } );
    }
    return pts;
  };

  // 单元装配：单部件 = 外环+洞；多部件 = 逐部件成单元（regionId 后缀 .pN
  // ——每个部件都是真实边界环，不静默丢）。regionId → 单元索引备查。
  std::vector<FaciesMapUnit> units;
  QHash<QString, std::size_t> unitIndex;
  QgsFeature f;
  QgsFeatureIterator it = vl->getFeatures();
  while ( it.nextFeature( f ) )
  {
    FaciesMapUnit base;
    if ( codeIdx >= 0 && f.attribute( codeIdx ).isValid() )
    {
      bool ok = false;
      const int code = f.attribute( codeIdx ).toInt( &ok );
      base.faciesCode = ok ? code : -1;
    }
    if ( kindIdx >= 0 && f.attribute( kindIdx ).isValid() )
      base.boundaryKind = f.attribute( kindIdx ).toString().toStdString();
    if ( widthIdx >= 0 && f.attribute( widthIdx ).isValid() && !f.attribute( widthIdx ).isNull() )
    {
      bool ok = false;
      const double width = f.attribute( widthIdx ).toDouble( &ok );
      base.transitionWidth = ok ? width : 0.0;
    }

    const QString baseId = QString::number( f.id() );
    const QgsGeometry geom = f.geometry();
    const QgsAbstractGeometry *g = geom.constGet();
    const auto appendPart = [&]( const QgsPolygon *poly, const QString &regionId ) {
      if ( !poly )
        return;
      FaciesMapUnit part = base;
      part.regionId = regionId.toStdString();
      part.geometry.exterior.points = ringPoints( poly->exteriorRing() );
      for ( int h = 0; h < poly->numInteriorRings(); ++h )
      {
        paleo::singlefactor::Ring hole;
        hole.points = ringPoints( poly->interiorRing( h ) );
        part.geometry.holes.push_back( std::move( hole ) );
      }
      if ( part.geometry.exterior.points.size() >= 3 )
      {
        unitIndex.insert( regionId, units.size() );
        units.push_back( std::move( part ) );
      }
    };
    if ( const QgsMultiPolygon *mp = qgsgeometry_cast< const QgsMultiPolygon * >( g ) )
    {
      for ( int p = 0; p < mp->numGeometries(); ++p )
        appendPart( qgsgeometry_cast< const QgsPolygon * >( mp->geometryN( p ) ),
                    QStringLiteral( "%1.p%2" ).arg( baseId, QString::number( p ) ) );
    }
    else
    {
      appendPart( qgsgeometry_cast< const QgsPolygon * >( g ), baseId );
    }
  }

  if ( units.empty() )
    return fail( tr( "层 '%1' 没有可核查的面要素" ).arg( layerId ) );

  const paleo::faciesmapping::FaciesQaOptions qaOptions; // 默认容差；尖灭端点
  // 落位检测关（编图层无他可落位边界/约束线上下文时如实不报）。
  const paleo::faciesmapping::FaciesQaResult qa = runFaciesQa( units, {}, {}, qaOptions );
  if ( qa.status != paleo::singlefactor::Status::Ok )
    return fail( tr( "QA 检测失败：%1" ).arg( QString::fromStdString( qa.message ) ) );

  // 报告面 = 边界几何与类型核查项；井覆盖/孤岛/约束冲突归方向 27 的
  // draft QA（那里有井位/阈值/约束线上下文），这里无上下文不冒充检测。
  QVariantList report;
  const auto unitOf = [&]( const std::string &regionId ) -> const FaciesMapUnit * {
    const auto found = unitIndex.constFind( QString::fromStdString( regionId ) );
    return found != unitIndex.constEnd() ? &units[found.value()] : nullptr;
  };
  for ( const FaciesQaIssue &issue : qa.issues )
  {
    QString reason;
    switch ( issue.type )
    {
      case FaciesQaIssueType::UnclosedRing:
      {
        const FaciesMapUnit *unit = issue.regionIds.empty() ? nullptr : unitOf( issue.regionIds.front() );
        if ( unit && unit->geometry.exterior.points.size() < 4 )
          reason = tr( "环点数不足（%1 个点）" ).arg( unit->geometry.exterior.points.size() );
        else
          reason = tr( "环首尾不闭合（缺口 %1）" ).arg( issue.metric, 0, 'f', 2 );
        break;
      }
      case FaciesQaIssueType::Overlap:
        reason = tr( "相带两两重叠（交集面积 %1）" ).arg( issue.metric, 0, 'f', 2 );
        break;
      case FaciesQaIssueType::PinchoutTipDangling:
        reason = issue.metric >= 0
                     ? tr( "尖灭端点无落位——开放端悬空（距最近可落位边界 %1）" )
                           .arg( issue.metric, 0, 'f', 2 )
                     : tr( "尖灭端点无落位——场内无可落位边界" );
        break;
      case FaciesQaIssueType::TransitionBandMissing:
        reason = tr( "相变带无渐变范围——transition_width 未设置或为 0" );
        break;
      case FaciesQaIssueType::ConformableCutFacies:
      {
        const FaciesMapUnit *a = issue.regionIds.size() > 0 ? unitOf( issue.regionIds[0] ) : nullptr;
        const FaciesMapUnit *b = issue.regionIds.size() > 1 ? unitOf( issue.regionIds[1] ) : nullptr;
        reason = tr( "整合接触边界切两侧相——两侧相代码不同（%1/%2，共享边 %3）" )
                     .arg( a ? a->faciesCode : -1 )
                     .arg( b ? b->faciesCode : -1 )
                     .arg( issue.metric, 0, 'f', 2 );
        break;
      }
      case FaciesQaIssueType::SmallIsland:
      case FaciesQaIssueType::ConstraintConflict:
      case FaciesQaIssueType::NoWellCoverage:
        continue; // 归方向 27 draft QA（无井/阈值/约束线上下文不冒充检测）
    }
    QVariantMap row;
    row.insert( QStringLiteral( "name" ),
                QString::fromLatin1( paleo::faciesmapping::faciesQaIssueName( issue.type ) ) );
    row.insert( QStringLiteral( "reason" ), reason );
    QStringList regionIds;
    for ( const std::string &id : issue.regionIds )
      regionIds << QString::fromStdString( id );
    row.insert( QStringLiteral( "regionIds" ), regionIds );
    row.insert( QStringLiteral( "metric" ), issue.metric );
    row.insert( QStringLiteral( "x" ), issue.location.x );
    row.insert( QStringLiteral( "y" ), issue.location.y );
    report.append( row );
  }
  return report;
}
