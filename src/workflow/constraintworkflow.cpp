// 层：功能
#include "../qgis/layervocabulary.h"
#include "../services/paleotaskservice.h"
#include "workflows.h"
#include "constraintjobs.h" // #558：job 族完整定义（原内嵌 workflows.h）
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
#include "algorithmpublish.h"
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
#include <qgsvectorlayer.h>

#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <variant>


#include "constraintworkflow_internal.h"

// 跨 TU 内部辅助（fileStem / sameFile / catalogPathMatches / removeIfPresent /
// readJsonObject）来自内部头——本文件与拆出的作业面文件共用同一 using。
using namespace paleo::constraint_detail;

// 约束与单因素工作流（②）。
// 方向20 轮4：按类边界从 workflows.cpp 析出——四个类里最大的一个（约 3730 行），
// 含约束 CRUD、三组单因素作业的 prepare/compute/publish 三段式，以及它们在
// JobRunner 上的统一异步面（ConstraintJob + variant 分派）。头文件契约不动。
//
// 共享面已就位（workflows_internal.h）：setError / stampLayerAssetLink /
// derivedRegistrarOf / outputPathOf / isFileBackedSource / stamp 与 ORT 工区
// 网格辅助。约束作业的 fileStem / sameFile / catalogPathMatches /
// removeIfPresent / readJsonObject 单点定义在 constraintworkflow_internal.h，
// 供约束编排与拆分作业复用；不再在 TU 内重复实现。


// ---------------------------------------------------------------------------
// ConstraintWorkflow — ②约束与单因素
// ---------------------------------------------------------------------------

ConstraintWorkflow::ConstraintWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_proc( proc ), m_layers( layers )
{
}

ConstraintWorkflow::~ConstraintWorkflow() = default;

void ConstraintWorkflow::setConstraintStore( ConstraintStore *store )
{
  m_externalConstraintStore = store;
}

void ConstraintWorkflow::setThicknessSamples( const QVariantList &rows, const QString &message )
{
  m_thicknessRows = rows;
  m_thicknessMessage = message;
}

// L-01：替代 paleo.constraint.snapshot.* 动态属性暗道（ARCH-02/06 家族）。
// 空路径按「无快照」处理——镜像旧 setProperty(key, QVariant()) 的移除语义。
void ConstraintWorkflow::setConstraintSnapshotPath( const QString &horizon, const QString &path )
{
  if ( path.isEmpty() )
    m_constraintSnapshots.remove( horizon );
  else
    m_constraintSnapshots.insert( horizon, path );
}

void ConstraintWorkflow::clearConstraintSnapshots()
{
  m_constraintSnapshots.clear();
}

void ConstraintWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  ++m_publishGeneration;
  if (m_factorJobTask)
    m_factorJobTask->requestCancel();
  m_catalog = catalog;
  m_projectDir = projectDir;
  m_preparedWellFactors.clear();
  m_wellFactorRows.clear();
  m_wellFactorMessage.clear();
}

void ConstraintWorkflow::setStore( PaleoProjectStore *store )
{
  ++m_publishGeneration;
  if ( m_projectStore != store )
  {
    m_projectStore = store;
    m_ownedConstraintStore.reset();
  }
}

DataCatalog *ConstraintWorkflow::catalog() const
{
  return m_catalog.data();
}

QgisProcessingService *ConstraintWorkflow::processingService() const
{
  return m_proc.data();
}

QgisLayerService *ConstraintWorkflow::layerService() const
{
  return m_layers.data();
}

PaleoProjectStore *ConstraintWorkflow::projectStore() const
{
  return m_projectStore.data();
}

ConstraintStore *ConstraintWorkflow::constraintStore() const
{
  if ( m_externalConstraintStore )
    return m_externalConstraintStore;

  if ( m_projectStore && !m_projectStore->gpkgPath().isEmpty() )
  {
    if ( m_ownedConstraintStore && m_ownedConstraintStore->gpkgPath() == m_projectStore->gpkgPath() )
      return m_ownedConstraintStore.get();

    m_ownedConstraintStore = std::make_unique<ConstraintStore>( m_projectStore->gpkgPath(), m_projectStore.data() );
    return m_ownedConstraintStore.get();
  }

  m_ownedConstraintStore.reset();
  return nullptr;
}

namespace
{
QString semanticForStoredType( const QString &type, const QVariantMap &lineParams )
{
  const QString given = lineParams.value( QStringLiteral( "semantic" ) ).toString();
  if ( !given.isEmpty() )
    return given;
  if ( type == QLatin1String( "break_line" ) || type == QLatin1String( "hard_barrier" ) )
    return QStringLiteral( "hard_barrier" );
  if ( type == QLatin1String( "direction_line" ) || type == QLatin1String( "direction_guide" ) )
    return QStringLiteral( "direction_guide" );
  return type;
}

QString storageTypeForSemantic( const QString &semantic )
{
  if ( semantic == QLatin1String( "hard_barrier" ) || semantic == QLatin1String( "break_line" ) )
    return QStringLiteral( "break_line" );
  if ( semantic == QLatin1String( "direction_guide" ) || semantic == QLatin1String( "direction_line" ) )
    return QStringLiteral( "direction_line" );
  return semantic;
}

bool isPersistedConstraintType( const QString &type )
{
  return type == QLatin1String( "break_line" ) || type == QLatin1String( "direction_line" ) ||
         type == QLatin1String( "hard_barrier" ) || type == QLatin1String( "direction_guide" ) ||
         type == QLatin1String( "interpretive_boundary" ) || type == QLatin1String( "contour_stop" ) ||
         type == QLatin1String( "cartographic_detour" );
}

bool knownConstraintSemantic( const QString &semantic )
{
  return semantic == QLatin1String( "hard_barrier" ) || semantic == QLatin1String( "direction_guide" ) ||
         semantic == QLatin1String( "interpretive_boundary" ) || semantic == QLatin1String( "contour_stop" ) ||
         semantic == QLatin1String( "cartographic_detour" );
}

QString lineParamsJson( const QString &type, const QVariantMap &lineParams, QString *error )
{
  const int schema = lineParams.value( QStringLiteral( "schemaVersion" ), 1 ).toInt();
  if ( schema > 1 )
  {
    if ( error )
      *error = QObject::tr( "不认识的约束参数版本：%1" ).arg( schema );
    return QString();
  }
  const QString semantic = semanticForStoredType( type, lineParams );
  if ( !knownConstraintSemantic( semantic ) )
  {
    if ( error )
      *error = QObject::tr( "未知约束语义：%1" ).arg( semantic );
    return QString();
  }
  QVariantMap stored = lineParams;
  stored.insert( QStringLiteral( "schemaVersion" ), 1 );
  stored.insert( QStringLiteral( "semantic" ), semantic );
  if ( !stored.contains( QStringLiteral( "enabled" ) ) )
    stored.insert( QStringLiteral( "enabled" ), true );
  // 上游 BLK_MODE 缺省 = full_block（constraint_semantics.py：未知/缺省
  // 一律按硬隔断处理）。只挂到硬隔断语义上，方向线不写 blockMode。
  if ( semantic == QLatin1String( "hard_barrier" ) &&
       !stored.contains( QStringLiteral( "blockMode" ) ) )
    stored.insert( QStringLiteral( "blockMode" ), QStringLiteral( "full_block" ) );
  if ( !stored.contains( QStringLiteral( "ratio" ) ) )
    stored.insert( QStringLiteral( "ratio" ), semantic == QLatin1String( "direction_guide" ) ? 8.0 : 1.0 );
  if ( !stored.contains( QStringLiteral( "influenceRadius" ) ) )
    stored.insert( QStringLiteral( "influenceRadius" ), 0.0 );
  if ( !stored.contains( QStringLiteral( "coreRadius" ) ) )
    stored.insert( QStringLiteral( "coreRadius" ), 0.0 );
  if ( !stored.contains( QStringLiteral( "softStrength" ) ) )
    stored.insert( QStringLiteral( "softStrength" ), 0.35 );
  if ( !stored.contains( QStringLiteral( "softRadius" ) ) )
    stored.insert( QStringLiteral( "softRadius" ), 0.0 );
  if ( !stored.contains( QStringLiteral( "displayBuffer" ) ) )
    stored.insert( QStringLiteral( "displayBuffer" ), 0.0 );
  if ( !stored.contains( QStringLiteral( "cartographicBuffer" ) ) )
    stored.insert( QStringLiteral( "cartographicBuffer" ), 0.0 );
  return QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( stored ) ).toJson( QJsonDocument::Compact ) );
}

QString constraintLayerTitle(const QString &horizon)
{
  return ConstraintWorkflow::tr("%1 约束线").arg(horizon);
}

QString constraintSource(ConstraintStore *store, const QString &horizon, const QString &subset)
{
  QSet<Qgis::GeometryType> types;
  for (const auto &row : store->load(horizon))
    types.insert(QgsGeometry::fromWkt(row.value(QStringLiteral("wkt")).toString()).type());
  QString source = QStringLiteral("%1|layername=constraints").arg(store->gpkgPath());
  // OGR 从全表首要素推断类型；单一几何层位显式定型，空约束层默认是线。
  if (types.isEmpty() || types == QSet<Qgis::GeometryType>{Qgis::GeometryType::Line})
    source += QStringLiteral("|geometrytype=LineString");
  else if (types == QSet<Qgis::GeometryType>{Qgis::GeometryType::Point})
    source += QStringLiteral("|geometrytype=Point");
  else if (types == QSet<Qgis::GeometryType>{Qgis::GeometryType::Polygon})
    source += QStringLiteral("|geometrytype=Polygon");
  return source + subset;
}

// 混合几何层位的主层不按类型过滤，以覆盖全部行；伴随矢量视图补齐
// 主层渲染类型之外的点/线/面。单一几何层位则使用明确的类型，避免空表推断。
QString constraintGeometryViewSuffix( Qgis::GeometryType type )
{
  switch ( type )
  {
    case Qgis::GeometryType::Point:
      return QStringLiteral( "point" );
    case Qgis::GeometryType::Line:
      return QStringLiteral( "line" );
    case Qgis::GeometryType::Polygon:
      return QStringLiteral( "polygon" );
    default:
      return {};
  }
}

void syncConstraintGeometryViews( QgisLayerService *layers, ConstraintStore *store, const QString &horizon )
{
  if ( !layers || !store || horizon.isEmpty() )
    return;
  const QString base = QStringLiteral( "%1|layername=constraints" ).arg( store->gpkgPath() );
  const QString subset = paleo::workflow_detail::constraintHorizonSubsetSuffix( horizon );
  if ( subset.isEmpty() )
  {
    // 层位名含 '|' 等无法安全进入 OGR URI 的字符：主约束层本就无法声明，
    // 伴随视图同步无从谈起——如实告警，绝不退化成无过滤全表层。
    qWarning() << "[ConstraintWorkflow] horizon not representable in subset URI, skip view sync:" << horizon;
    return;
  }
  QSet<Qgis::GeometryType> present;
  for ( const QVariantMap &row : store->load( horizon ) )
  {
    const QgsGeometry geometry = QgsGeometry::fromWkt( row.value( QStringLiteral( "wkt" ) ).toString() );
    if ( !geometry.isNull() && !geometry.isEmpty() )
      present.insert( geometry.type() );
  }
  Qgis::GeometryType mainType = Qgis::GeometryType::Unknown;
  if ( !present.isEmpty() )
  {
    const QgsVectorLayer probe( constraintSource(store, horizon, subset), QStringLiteral( "probe" ), QStringLiteral( "ogr" ) );
    if ( probe.isValid() )
      mainType = probe.geometryType();
  }
  if (auto *live = qobject_cast<QgsVectorLayer *>(layers->layer(QStringLiteral("constraints.%1").arg(horizon))))
    mainType = live->geometryType();
  static const struct
  {
    Qgis::GeometryType type;
    const char *ogrName;
    const char *title;
  } kViews[] = { { Qgis::GeometryType::Point, "Point", "约束（点）" },
                 { Qgis::GeometryType::Line, "LineString", "约束（线）" },
                 { Qgis::GeometryType::Polygon, "Polygon", "约束（面）" } };
  for ( const auto &view : kViews )
  {
    const QString layerId =
        QStringLiteral( "%1%2.%3" ).arg( PaleoLayerVocabulary::kConstraintGeometryViewPrefix, horizon, constraintGeometryViewSuffix( view.type ) );
    if ( present.contains( view.type ) && view.type != mainType )
    {
      LayerDeclaration decl;
      decl.layerId = layerId;
      decl.horizon = horizon;
      decl.type = QStringLiteral( "vector" );
      decl.source = base + QStringLiteral( "|geometrytype=%1" ).arg( QLatin1String( view.ogrName ) ) + subset;
      decl.group = PaleoLayerVocabulary::kConstraintsGroup;
      decl.title = QStringLiteral( "%1 · %2" ).arg( QString::fromUtf8( view.title ), horizon );
      const LayerDeclaration existing = layers->declaration( layerId );
      if ( existing.source != decl.source || existing.title != decl.title )
        layers->declare( decl );
    }
    else if ( !layers->declaration( layerId ).layerId.isEmpty() )
    {
      layers->removeDeclaration( layerId );
    }
  }
}

} // namespace

void ConstraintWorkflow::setEditingService( QgisEditingService *editing )
{
  if ( m_editingService )
    disconnect( m_editingService.data(), nullptr, this, nullptr );
  m_editingService = editing;
  if ( !editing )
    return;
  // P4：会话保存/取消后 store 内容可能变了（取消恢复会话起点），伴随视图重同步。
  // 信号只带 QGIS 图层 id，层位数通常很少：逐个已声明约束层位同步。
  const auto resync = [this]( const QString & ) { syncAllConstraintGeometryViews(); };
  connect( editing, &QgisEditingService::editCommitted, this, resync );
  connect( editing, &QgisEditingService::editRolledBack, this, resync );
}

void ConstraintWorkflow::syncAllConstraintGeometryViews()
{
  ConstraintStore *cs = constraintStore();
  QgisLayerService *layers = m_layers.data();
  if ( !cs || !layers )
    return;
  QVector<LayerDeclaration> declared;
  if ( !layers->tryDeclared( &declared, nullptr ) )
    return;
  const QString mainPrefix = QStringLiteral( "constraints." );
  QSet<QString> horizons;
  for ( const LayerDeclaration &d : declared )
    if ( !d.horizon.isEmpty() && ( d.layerId.startsWith( mainPrefix ) ||
                                   d.layerId.startsWith( PaleoLayerVocabulary::kConstraintGeometryViewPrefix ) ) )
      horizons.insert( d.horizon );
  for ( const QString &h : std::as_const( horizons ) ) {
    loadConstraints(h); // 保存/取消后重定型主矢量层，再同步伴随几何视图。
    syncConstraintGeometryViews(layers, cs, h);
  }
}

bool ConstraintWorkflow::canStartConstraintSession() const
{
  return constraintStore() && m_layers && ( m_drawSessionStarter || m_editingService );
}

bool ConstraintWorkflow::ensureConstraintSession( const QString &horizon, QString *error )
{
  ConstraintStore *cs = constraintStore();
  QgisLayerService *layers = m_layers.data();
  if ( !cs || !layers || horizon.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "约束编辑会话缺少约束库或图层服务" ) );
    return false;
  }
  const QString layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const auto liveSession = [&]() {
    auto *layer = qobject_cast<QgsVectorLayer *>( layers->layer( layerId ) );
    return layer && layer->isEditable() && layer->property( "paleoConstraintEditSession" ).toBool();
  };
  if ( liveSession() )
    return true;
  // 空层位主动编辑：先建表（不改既有行），主图层才可实例化并开会话。
  if ( !cs->ensureSchema( error ) )
    return false;
  if ( layers->declaration( layerId ).layerId.isEmpty() )
  {
    LayerDeclaration decl;
    decl.layerId = layerId;
    decl.horizon = horizon;
    decl.type = QStringLiteral( "vector" );
    QString subsetError;
    const QString subset = paleo::workflow_detail::constraintHorizonSubsetSuffix( horizon, &subsetError );
    if ( subset.isEmpty() )
    {
      if ( error )
        *error = subsetError;
      return false; // 层位名进不了安全过滤器：拒绝（不声明无过滤全表层）
    }
    decl.source = constraintSource(cs, horizon, subset);
    decl.group = PaleoLayerVocabulary::kConstraintsGroup;
    if ( !layers->declare( decl, error ) )
      return false;
  }
  QString startError;
  bool started = false;
  if ( m_drawSessionStarter )
  {
    started = m_drawSessionStarter( horizon, &startError );
  }
  else if ( QgisEditingService *editing = m_editingService.data() )
  {
    auto *layer = qobject_cast<QgsVectorLayer *>( layers->instantiate( layerId, &startError ) );
    started = layer && editing->beginEdit( layer, &startError );
  }
  else
  {
    startError = tr( "未绑定编辑服务" );
  }
  if ( !started || !liveSession() )
  {
    paleo::workflow_detail::setError(
        error, tr( "无法开始约束编辑会话，未写入：%1" )
                   .arg( startError.isEmpty() ? tr( "约束图层未进入编辑会话" ) : startError ) );
    return false;
  }
  return true;
}

bool ConstraintWorkflow::drawConstraint( const QString &horizon, const QString &wkt, const QString &type,
                                         int faciesCode, QString *error, QString *constraintIdOut,
                                         const QVariantMap &lineParams )
{
  // 完成绘制即持久化；只有用户已开启的矢量编辑会话才保留撤销/取消语义。
  if (horizon.isEmpty()) {
    paleo::workflow_detail::setError(error, tr("没有活动层位，无法绘制约束"));
    return false;
  }
  if (!addConstraint(horizon, wkt, type, faciesCode, error, constraintIdOut, lineParams))
    return false;
  if (constraintStore() && m_layers) {
    QString layerError;
    auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(QStringLiteral("constraints.%1").arg(horizon), &layerError));
    if (!layer) {
      paleo::workflow_detail::setError(error, tr("约束已保存，但矢量图层加载失败：%1").arg(layerError));
      return false;
    }
    QgisStyleService::applyConstraintLayerStyle(layer);
    layer->triggerRepaint();
  }
  return true;
}

bool ConstraintWorkflow::saveConstraintEdits(const QString &horizon, QString *error)
{
  auto *layer = m_layers ? qobject_cast<QgsVectorLayer *>(m_layers->layer(QStringLiteral("constraints.%1").arg(horizon))) : nullptr;
  if (!layer || !layer->isEditable())
    return true;
  if (!m_editingService || !layer->property("paleoConstraintEditSession").toBool()) {
    paleo::workflow_detail::setError(error, tr("约束图层未接入工程编辑服务，无法保存当前修改"));
    return false;
  }
  return m_editingService->commitEdit(layer, error);
}

bool ConstraintWorkflow::addConstraintThroughSession(QgsVectorLayer *layer, ConstraintStore *store,
                                                     const QString &horizon, const QString &id, const QString &wkt,
                                                     const QString &storedType, int faciesCode,
                                                     const QString &paramsJson, bool extended, QString *error)
{
  QgsFeature feature(layer->fields());
  const auto set = [&](const QString &name, const QVariant &value) {
    const int index = layer->fields().indexFromName(name);
    if (index >= 0)
      feature.setAttribute(index, value);
    return index >= 0;
  };
  if (!set(QStringLiteral("id"), id) || !set(QStringLiteral("horizon"), horizon) ||
      !set(QStringLiteral("type"), storedType) ||
      (extended && (!set(QStringLiteral("params_json"), paramsJson) || !set(QStringLiteral("schema_version"), 1))))
  {
    paleo::workflow_detail::setError(error, tr("约束编辑缓冲区缺少身份或语义字段"));
    return false;
  }
  set(QStringLiteral("facies_code"), faciesCode);
  set(QStringLiteral("weight"), 1.0); // 与 ConstraintStore::append 默认权重一致
  feature.setGeometry(QgsGeometry::fromWkt(wkt));
  layer->beginEditCommand(tr("绘制约束"));
  if (!layer->addFeature(feature))
  {
    layer->destroyEditCommand();
    paleo::workflow_detail::setError(error, tr("约束写入编辑缓冲区失败"));
    return false;
  }
  layer->endEditCommand();
  // 会话在命令结束时同步写 store；被拒时它已撤回该命令，这里如实报错。
  for (const QVariantMap &row : store->load(horizon))
    if (row.value(QStringLiteral("id")).toString() == id &&
        row.value(QStringLiteral("type")).toString() == storedType &&
        (!extended || row.value(QStringLiteral("params_json")).toString() == paramsJson) &&
        QgsGeometry::fromWkt(row.value(QStringLiteral("wkt")).toString()).equals(QgsGeometry::fromWkt(wkt)))
      return true;
  paleo::workflow_detail::setError(error, tr("约束未写入 store（编辑会话已撤回该次绘制）"));
  return false;
}

bool ConstraintWorkflow::addConstraint( const QString &horizon, const QString &wkt,
                                        const QString &type, int faciesCode, QString *error,
                                        QString *constraintIdOut, const QVariantMap &lineParams )
{
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  if ( wkt.trimmed().isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "constraint geometry WKT is empty" ) );
    return false;
  }
  // 拓扑提交门（QGIS_NATIVE_ADOPTION）：约束几何进 IDW 掩膜前经原生验证
  // ——自相交多边形会让掩膜语义失真，如实拒收不静默修形。
  if ( const QString geomErr = QgisEditingService::geometryCommitError(
           QgsGeometry::fromWkt( wkt ), tr( "constraint geometry" ) );
       !geomErr.isEmpty() )
  {
    paleo::workflow_detail::setError( error, geomErr );
    return false;
  }

  auto *editing = qobject_cast<QgsVectorLayer *>(layers->layer(QStringLiteral("constraints.%1").arg(horizon)));
  if (editing && !editing->isEditable())
    editing = nullptr;
  // 无 store 同步会话的编辑缓冲区不能接收新约束（写 store 会被会话回滚覆盖）。
  if (editing && (!constraintStore() || !editing->property("paleoConstraintEditSession").toBool()))
  {
    paleo::workflow_detail::setError(error, tr("先保存或取消约束编辑，再绘制新约束"));
    return false;
  }

  // 编号在全库唯一。只看当前层位会从 c-1 重新计，撞上其他层位已有的
  // c-1 后，保存被拒并撤回，空子集又被 QGIS 读成无几何属性表。
  if ( ConstraintStore *existing = constraintStore() )
  {
    for ( const QVariantMap &row : existing->load() )
    {
      const QString id = row.value( QStringLiteral( "id" ) ).toString();
      if ( !id.startsWith( QLatin1String( "c-" ) ) )
        continue;
      bool ok = false;
      const int num = id.mid( 2 ).toInt( &ok );
      if ( ok && num > m_inMemorySeq )
        m_inMemorySeq = num;
    }
  }
  Constraint c;
  const int seq = ++m_inMemorySeq;
  c.id = QStringLiteral( "c-%1" ).arg( seq );
  c.type = type;
  c.wkt = wkt;
  c.targetFaciesCode = faciesCode;

  ConstraintStore *cs = constraintStore();
  if ( cs )
  {
    const bool persist = !lineParams.isEmpty() || isPersistedConstraintType( type );
    QString storedType = type;
    QString paramsJson;
    if ( persist )
    {
      paramsJson = lineParamsJson( type, lineParams, error );
      if ( paramsJson.isEmpty() )
        return false;
      storedType = storageTypeForSemantic( semanticForStoredType( type, lineParams ) );
    }
    if ( editing )
    {
      // P2：编辑会话内绘制 → 原生 edit command，进入撤销栈；会话在命令结束时
      // 原子写 store（失败则同步撤回该命令）。撤销/重做/取消会话均可回到绘制前。
      if ( !addConstraintThroughSession( editing, cs, horizon, c.id, wkt, persist ? storedType : type, faciesCode,
                                         persist ? paramsJson : QString(), persist, error ) )
        return false;
      // 显式编辑中的绘制按命令原子写 store，伴随视图即时同步。
      syncConstraintGeometryViews( layers, cs, horizon );
      if ( persist )
        c.type = storedType;
      QVariantMap rec = c.toMap();
      if ( persist )
      {
        rec.insert( QStringLiteral( "params_json" ), paramsJson );
        rec.insert( QStringLiteral( "schema_version" ), 1 );
      }
      rec.insert( QStringLiteral( "horizon" ), horizon );
      rec.insert( QStringLiteral( "facies_code" ), faciesCode );
      m_inMemoryConstraints.append( rec );
      if ( constraintIdOut )
        *constraintIdOut = c.id;
      emit constraintAdded( c.id );
      return true;
    }
    if ( persist )
    {
      if ( !cs->appendExtended( horizon, c.id, wkt, storedType, faciesCode, paramsJson, 1, error ) )
        return false;
      c.type = storedType;
    }
    else if ( !cs->append( horizon, c.id, wkt, type, faciesCode, error ) )
    {
      return false;
    }

    QVariantMap rec = c.toMap();
    if ( persist )
    {
      rec.insert( QStringLiteral( "params_json" ), paramsJson );
      rec.insert( QStringLiteral( "schema_version" ), 1 );
    }
    rec.insert( QStringLiteral( "horizon" ), horizon );
    rec.insert( QStringLiteral( "facies_code" ), faciesCode );
    m_inMemoryConstraints.append( rec );

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
    decl.horizon = horizon;
    decl.title = constraintLayerTitle(horizon);
    decl.type = QStringLiteral( "vector" );
    QString subsetError;
    const QString subset = paleo::workflow_detail::constraintHorizonSubsetSuffix( horizon, &subsetError );
    if ( subset.isEmpty() )
    {
      if ( error )
        *error = subsetError;
      cs->remove( c.id );
      return false; // 已回滚刚写入的行，不留半截
    }
    decl.source = constraintSource(cs, horizon, subset);
    decl.group = PaleoLayerVocabulary::kConstraintsGroup;
    if ( !layers->declare( decl, error ) )
    {
      cs->remove( c.id );
      return false;
    }
    syncConstraintGeometryViews( layers, cs, horizon );

    if ( constraintIdOut )
      *constraintIdOut = c.id;
    emit constraintAdded( c.id );
    return true;
  }

  // Fallback when no store is configured (preserves memory-layer compatibility)
  QVariantMap rec = c.toMap();
  rec.insert( QStringLiteral( "horizon" ), horizon );
  if ( !lineParams.isEmpty() || isPersistedConstraintType( type ) )
  {
    const QString paramsJson = lineParamsJson( type, lineParams, error );
    if ( paramsJson.isEmpty() )
      return false;
    c.type = storageTypeForSemantic( semanticForStoredType( type, lineParams ) );
    rec.insert( QStringLiteral( "type" ), c.type );
    rec.insert( QStringLiteral( "params_json" ), paramsJson );
    rec.insert( QStringLiteral( "schema_version" ), 1 );
  }
  m_inMemoryConstraints.append( rec );

  QStringList wkts;
  for ( const QVariantMap &m : m_inMemoryConstraints )
  {
    if ( m.value( QStringLiteral( "horizon" ) ).toString() == horizon )
      wkts << m.value( QStringLiteral( "wkt" ) ).toString();
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.title = constraintLayerTitle(horizon);
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "memory|%1" ).arg( wkts.join( QLatin1Char( '|' ) ) );
  decl.group = PaleoLayerVocabulary::kConstraintsGroup;
  if ( !layers->declare( decl, error ) )
  {
    m_inMemoryConstraints.removeLast();
    return false;
  }

  if ( constraintIdOut )
    *constraintIdOut = c.id;
  emit constraintAdded( c.id );
  return true;
}

bool ConstraintWorkflow::updateConstraintLine(const QString &id, const QVariantMap &lineParams, QString *error)
{
  return updateConstraintLines({id}, lineParams, error);
}

bool ConstraintWorkflow::updateConstraintLines(const QStringList &ids, const QVariantMap &patch, QString *error)
{
  if (ids.isEmpty() || ids.contains(QString()))
  {
    paleo::workflow_detail::setError(error, tr("缺少约束 id"));
    return false;
  }
  ConstraintStore *cs = constraintStore();
  auto rows = cs ? cs->load() : m_inMemoryConstraints;
  QString horizon;
  QSet<QString> pending(ids.cbegin(), ids.cend());
  QHash<QString, QVariantMap> updates;
  for (QVariantMap &row : rows)
  {
    const QString id = row.value(QStringLiteral("id")).toString();
    if (!pending.contains(id))
      continue;
    const QString h = row.value(QStringLiteral("horizon")).toString();
    if (!horizon.isEmpty() && horizon != h)
    {
      paleo::workflow_detail::setError(error, tr("批量编辑仅允许同一层位的约束"));
      return false;
    }
    horizon = h;
    if (QgsGeometry::fromWkt(row.value(QStringLiteral("wkt")).toString()).type() != Qgis::GeometryType::Line)
    {
      paleo::workflow_detail::setError(error, tr("约束语义改型仅适用于线要素"));
      return false;
    }
    const QByteArray json = row.value(QStringLiteral("params_json")).toString().toUtf8();
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(json, &parseError);
    if (!json.isEmpty() && (parseError.error != QJsonParseError::NoError || !doc.isObject()))
    {
      paleo::workflow_detail::setError(error, tr("约束参数 JSON 无效：%1").arg(id));
      return false;
    }
    QVariantMap params = doc.object().toVariantMap();
    for (auto it = patch.cbegin(); it != patch.cend(); ++it)
      if (it.key() != QLatin1String("geometryAzimuth"))
        params.insert(it.key(), it.value());
    if (patch.contains(QStringLiteral("geometryAzimuth")))
    {
      QgsGeometry geometry = QgsGeometry::fromWkt(row.value(QStringLiteral("wkt")).toString());
      const auto line = geometry.asPolyline();
      const double angle = patch.value(QStringLiteral("geometryAzimuth")).toDouble();
      if (line.size() < 2 || !std::isfinite(angle) || angle < 0 || angle > 360)
      {
        paleo::workflow_detail::setError(error, tr("方向角仅适用于非空约束折线，范围为 0–360 度"));
        return false;
      }
      const auto &a = line.first(), &b = line.last();
      if (a == b)
      {
        paleo::workflow_detail::setError(error, tr("闭合约束线没有唯一方向角"));
        return false;
      }
      const double current = std::atan2(b.x()-a.x(), b.y()-a.y()) * 180.0 / std::acos(-1.0);
      geometry.rotate(angle - current, geometry.centroid().asPoint());
      row.insert(QStringLiteral("wkt"), geometry.asWkt(17));
    }
    const QString text = lineParamsJson(row.value(QStringLiteral("type")).toString(), params, error);
    if (text.isEmpty())
      return false;
    row.insert(QStringLiteral("type"), storageTypeForSemantic(semanticForStoredType(row.value(QStringLiteral("type")).toString(), params)));
    row.insert(QStringLiteral("params_json"), text);
    row.insert(QStringLiteral("schema_version"), 1);
    updates.insert(id, row);
    pending.remove(id);
  }
  if (!pending.isEmpty())
  {
    paleo::workflow_detail::setError(error, tr("找不到约束 %1").arg(*pending.cbegin()));
    return false;
  }
  // P4：可开会话时（UI 启动器或已绑定编辑服务）语义改型也走会话，进撤销栈；
  // 未绑定任何会话来源的裸用路径（测试壳/批处理）维持事务化直写。
  if (canStartConstraintSession() && !ensureConstraintSession(horizon, error))
    return false;
  auto *layer = m_layers ? qobject_cast<QgsVectorLayer *>(m_layers->layer(QStringLiteral("constraints.%1").arg(horizon))) : nullptr;
  if (layer && layer->isEditable())
  {
    if (!cs || !layer->property("paleoConstraintEditSession").toBool())
    {
      paleo::workflow_detail::setError(error, tr("约束编辑尚未接入 store 同步会话"));
      return false;
    }
    layer->beginEditCommand(tr("批量修改约束参数"));
    for (const QVariantMap &row : updates)
    {
      const qlonglong fid = row.value(QStringLiteral("fid")).toLongLong();
      QgsGeometry geometry = QgsGeometry::fromWkt(row.value(QStringLiteral("wkt")).toString());
      if (patch.contains(QStringLiteral("geometryAzimuth")) &&
          !layer->changeGeometry(fid, geometry))
      {
        layer->destroyEditCommand();
        paleo::workflow_detail::setError(error, tr("修改约束方向角失败"));
        return false;
      }
      for (const QString &key : {QStringLiteral("type"), QStringLiteral("params_json"), QStringLiteral("schema_version")})
        if (!layer->changeAttributeValue(fid, layer->fields().indexFromName(key), row.value(key)))
        {
          layer->destroyEditCommand();
          paleo::workflow_detail::setError(error, tr("修改约束编辑缓冲区失败"));
          return false;
        }
    }
    layer->endEditCommand();
    // A rejected store transaction reverts the native command synchronously.
    const auto persisted = cs->load(horizon);
    QSet<QString> verified;
    for (const QVariantMap &row : persisted)
    {
      const QString id = row.value(QStringLiteral("id")).toString();
      if (!updates.contains(id))
        continue;
      const auto expected = updates.value(id);
      if (row.value(QStringLiteral("params_json")) != expected.value(QStringLiteral("params_json")) ||
          row.value(QStringLiteral("type")) != expected.value(QStringLiteral("type")) ||
          !QgsGeometry::fromWkt(row.value(QStringLiteral("wkt")).toString()).equals(
              QgsGeometry::fromWkt(expected.value(QStringLiteral("wkt")).toString())))
      {
        paleo::workflow_detail::setError(error, tr("约束参数未写入 store"));
        return false;
      }
      verified.insert(id);
    }
    if (verified.size() != updates.size())
    {
      paleo::workflow_detail::setError(error, tr("约束参数未写入 store"));
      return false;
    }
  }
  else if (cs)
  {
    QVector<QVariantMap> horizonRows;
    for (const QVariantMap &row : rows)
      if (row.value(QStringLiteral("horizon")).toString() == horizon)
        horizonRows.append(row);
    if (!cs->replaceHorizon(horizon, horizonRows, error))
      return false;
    if (layer)
    {
      layer->reload();
      layer->triggerRepaint();
    }
  }
  else
    m_inMemoryConstraints = rows;
  for (auto it = updates.cbegin(); it != updates.cend(); ++it)
    emit constraintLineUpdated(it.key());
  return true;
}

bool ConstraintWorkflow::switchConstraintSemantic(const QString &id, const QString &semantic, QString *error)
{
  return updateConstraintLines({id}, {{QStringLiteral("semantic"), semantic}}, error);
}

bool ConstraintWorkflow::removeConstraint( const QString &constraintId, QString *error )
{
  if ( constraintId.trimmed().isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "缺少约束 id" ) );
    return false;
  }
  ConstraintStore *cs = constraintStore();
  if ( !cs )
  {
    const auto removed = std::remove_if(
        m_inMemoryConstraints.begin(), m_inMemoryConstraints.end(),
        [&constraintId]( const QVariantMap &rec ) {
          return rec.value( QStringLiteral( "id" ) ).toString() == constraintId;
        } );
    if ( removed == m_inMemoryConstraints.end() )
    {
      paleo::workflow_detail::setError( error, tr( "找不到约束 %1" ).arg( constraintId ) );
      return false;
    }
    m_inMemoryConstraints.erase( removed, m_inMemoryConstraints.end() );
    emit constraintRemoved( constraintId );
    return true;
  }
  bool found = false;
  for ( const QVariantMap &row : cs->load() )
  {
    if ( row.value( QStringLiteral( "id" ) ).toString() == constraintId )
    {
      found = true;
      break;
    }
  }
  if ( !found )
  {
    paleo::workflow_detail::setError( error, tr( "找不到约束 %1" ).arg( constraintId ) );
    return false;
  }
  for (const auto &row : cs->load())
  {
    if (row.value(QStringLiteral("id")).toString() != constraintId)
      continue;
    const QString rowHorizon = row.value(QStringLiteral("horizon")).toString();
    // P4：同 updateConstraintLines——可开会话时删除也走会话（可撤销）。
    if (canStartConstraintSession() && !ensureConstraintSession(rowHorizon, error))
      return false;
    auto *layer = m_layers ? qobject_cast<QgsVectorLayer *>(m_layers->layer(
        QStringLiteral("constraints.%1").arg(row.value(QStringLiteral("horizon")).toString()))) : nullptr;
    if (!layer || !layer->isEditable())
      break;
    if (!layer->property("paleoConstraintEditSession").toBool())
    {
      paleo::workflow_detail::setError(error, tr("约束编辑尚未接入 store 同步会话"));
      return false;
    }
    layer->beginEditCommand(tr("删除约束"));
    if (!layer->deleteFeature(row.value(QStringLiteral("fid")).toLongLong()))
    {
      layer->destroyEditCommand();
      // 删除失败不写 error，调用方只拿到静默 false（#188）——与其余路径一致
      // 如实报错。
      paleo::workflow_detail::setError(error, tr("删除约束失败"));
      return false;
    }
    layer->endEditCommand();
    for (const auto &remaining : cs->load())
      if (remaining.value(QStringLiteral("id")).toString() == constraintId)
      {
        paleo::workflow_detail::setError(error, tr("删除约束未写入 store"));
        return false;
      }
    syncConstraintGeometryViews(m_layers.data(), cs, rowHorizon);
    emit constraintRemoved(constraintId);
    return true;
  }
  QString removedHorizon;
  for ( const QVariantMap &row : cs->load() )
    if ( row.value( QStringLiteral( "id" ) ).toString() == constraintId )
      removedHorizon = row.value( QStringLiteral( "horizon" ) ).toString();
  if ( !cs->remove( constraintId, error ) )
    return false;
  syncConstraintGeometryViews( m_layers.data(), cs, removedHorizon );
  const auto removed = std::remove_if(
      m_inMemoryConstraints.begin(), m_inMemoryConstraints.end(),
      [&constraintId]( const QVariantMap &rec ) {
        return rec.value( QStringLiteral( "id" ) ).toString() == constraintId;
      } );
  m_inMemoryConstraints.erase( removed, m_inMemoryConstraints.end() );
  emit constraintRemoved( constraintId );
  return true;
}

int ConstraintWorkflow::constraintLayerCrsState() const
{
  // 0=无层/无库 1=legacy CRS-less 2=已登记 SRS（master #488 的存储契约探测）。
  ConstraintStore *store = constraintStore();
  if ( !store )
    return 0;
  const ConstraintStore::CrsStorageStatus status = store->inspectCrsStorage();
  if ( !status.layerExists )
    return 0;
  return status.legacyNoCrs ? 1 : 2;
}

bool ConstraintWorkflow::migrateConstraintLayerCrs( QString *error, QString *backupPath )
{
  ConstraintStore *store = constraintStore();
  if ( !store )
  {
    if ( error )
      *error = tr( "约束库未绑定" );
    return false;
  }
  return store->migrateLegacyCrs( error, backupPath );
}

QRectF ConstraintWorkflow::lastFactorComputeDomain( const QString &horizon ) const
{
  // 已提取井点样本的层位：点层包围盒 ±10%（与 paleo_constraint_idw 的
  // 网格 pad 同口径）作为「最近计算域」。域是运行时刻从井点算出的下界，
  // 页面只把它当作已知的最近域——未提取/换层位时返回空，保持域未知。
  if ( horizon.isEmpty() || m_preparedWellFactors.value( "horizon" ) != horizon )
    return QRectF();
  QgisLayerService *layers = m_layers.data();
  const QString pointsLayerId = m_preparedWellFactors.value( "points_layer_id" ).toString();
  if ( !layers || pointsLayerId.isEmpty() )
    return QRectF();
  auto *layer = qobject_cast<QgsVectorLayer *>( layers->layer( pointsLayerId ) );
  if ( !layer || !layer->isValid() )
    return QRectF();
  const QgsRectangle extent = layer->extent();
  if ( extent.isEmpty() || !std::isfinite( extent.width() ) || !std::isfinite( extent.height() ) )
    return QRectF();
  const double xPad = extent.width() > 0.0 ? extent.width() * 0.1 : 1.0;
  const double yPad = extent.height() > 0.0 ? extent.height() * 0.1 : 1.0;
  return QRectF( extent.xMinimum() - xPad, extent.yMinimum() - yPad,
                 extent.width() + 2 * xPad, extent.height() + 2 * yPad );
}

QVector<QVariantMap> ConstraintWorkflow::loadConstraints( const QString &horizon )
{
  ConstraintStore *cs = constraintStore();
  if ( !cs )
  {
    QVector<QVariantMap> res;
    for ( const QVariantMap &m : m_inMemoryConstraints )
    {
      if ( horizon.isEmpty() || m.value( QStringLiteral( "horizon" ) ).toString() == horizon )
        res.append( m );
    }
    return res;
  }

  QVector<QVariantMap> loaded = cs->load( horizon );

  if ( horizon.isEmpty() )
  {
    m_inMemoryConstraints.clear();
  }
  else
  {
    for ( int i = m_inMemoryConstraints.size() - 1; i >= 0; --i )
    {
      if ( m_inMemoryConstraints.at( i ).value( QStringLiteral( "horizon" ) ).toString() == horizon )
        m_inMemoryConstraints.removeAt( i );
    }
  }

  int maxSeq = m_inMemorySeq;
  for ( const QVariantMap &rec : loaded )
  {
    m_inMemoryConstraints.append( rec );
    const QString id = rec.value( QStringLiteral( "id" ) ).toString();
    if ( id.startsWith( QStringLiteral( "c-" ) ) )
    {
      bool ok = false;
      int num = id.mid( 2 ).toInt( &ok );
      if ( ok && num > maxSeq )
        maxSeq = num;
    }
  }
  m_inMemorySeq = maxSeq;

  QgisLayerService *layers = m_layers.data();
  if ( layers && !loaded.isEmpty() )
  {
    if ( !horizon.isEmpty() )
    {
      if (auto *active = layers->layer(QStringLiteral("constraints.%1").arg(horizon));
          active && active->property("paleoConstraintEditSession").toBool())
        return loaded; // the temporary provider belongs to the live session
      LayerDeclaration decl;
      decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
      decl.horizon = horizon;
      decl.title = constraintLayerTitle(horizon);
      decl.type = QStringLiteral( "vector" );
      const QString subset = paleo::workflow_detail::constraintHorizonSubsetSuffix( horizon );
      if ( !subset.isEmpty() )
      {
        decl.source = constraintSource(cs, horizon, subset);
        decl.group = PaleoLayerVocabulary::kConstraintsGroup;
        layers->declare( decl );
      }
      syncConstraintGeometryViews( layers, cs, horizon );
    }
    else
    {
      QSet<QString> horizons;
      for ( const QVariantMap &rec : loaded )
      {
        const QString h = rec.value( QStringLiteral( "horizon" ) ).toString();
        if ( !h.isEmpty() )
          horizons.insert( h );
      }
      for ( const QString &h : horizons )
      {
        if (auto *active = layers->layer(QStringLiteral("constraints.%1").arg(h));
            active && active->property("paleoConstraintEditSession").toBool())
          continue;
        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "constraints.%1" ).arg( h );
        decl.horizon = h;
        decl.title = constraintLayerTitle(h);
        decl.type = QStringLiteral( "vector" );
        const QString subset = paleo::workflow_detail::constraintHorizonSubsetSuffix( h );
        if ( !subset.isEmpty() )
        {
          decl.source = constraintSource(cs, h, subset);
          decl.group = PaleoLayerVocabulary::kConstraintsGroup;
          layers->declare( decl );
        }
        syncConstraintGeometryViews( layers, cs, h );
      }
    }
  }

  return loaded;
}

bool ConstraintWorkflow::runConstraintIDW( const QString &horizon, const QString &pointsLayerId,
                                          const QString &field, double cellSize, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if (factorBusy()) {
    paleo::workflow_detail::setError(error, tr("已有单因素任务"));
    return false;
  }
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  if ( pointsLayerId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "没有井点图层" ) );
    return false;
  }
  if ( field.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "插值字段为空" ) );
    return false;
  }
  if ( !( cellSize > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  // INPUT is a QgsProcessingParameterFeatureSource: it accepts a QgsMapLayer*
  // variant, so resolve the declared points layer through the layer service.
  QgsMapLayer *points = layers->instantiate( pointsLayerId, error );
  if ( !points )
    return false;

  // T26：IDW 单因素栅格落 artifacts/derived + DERIVED 版本（父版本 = 井点图层
  // 与约束图层的文件源版本）。
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "constraint_idw_raster" ), tr( "%1 约束 IDW" ).arg( horizon ),
      QStringLiteral( "IDW_%1.tif" ).arg( horizon ), &regErr );
  if ( !st.isValid() )
  {
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap params;
  params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  params.insert( QStringLiteral( "FIELD" ), field );
  params.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  params.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

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
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  return runFactorAlgorithm(
      horizon, QStringLiteral("paleo:paleo_constraint_idw"), params,
      [=, this](const QVariantMap &results, QString *error) mutable {
        const auto fingerprint = results.value("_derived_fingerprint")
                                     .value<DerivedFileFingerprint>();

        if (results.isEmpty())
          return false;

        const QString outPath = paleo::workflow_detail::outputPathOf(results);
        if (outPath.isEmpty()) {
          paleo::workflow_detail::setError(
              error, tr("constraint IDW returned no output path"));
          return false;
        }
        QVariantMap idwExtra;
        idwExtra.insert(QStringLiteral("field"), field);
        idwExtra.insert(QStringLiteral("cell_size"), cellSize);
        idwExtra.insert(QStringLiteral("constrained"), hasConstraints);
        // algo-atom P1：经统一 publishArtifacts（等价 commitExternal）
        const AlgorithmRunResult runResult =
            paleo::runResultFromProcessingOutput( results );
        QString commitErr;
        if ( !paleo::publishArtifacts(
                 registrar, st, runResult,
                 registrar.parentVersionIdsFor( parentPaths ),
                 QStringLiteral( "paleo:paleo_constraint_idw" ), idwExtra,
                 &commitErr,
                 fingerprint.sha256.isEmpty() ? nullptr : &fingerprint ) )
        {
          paleo::workflow_detail::setError( error, commitErr );
          return false;
        }

        LayerDeclaration decl;
        decl.layerId = QStringLiteral("factor.%1.idw").arg(horizon);
        decl.horizon = horizon;
        decl.type = QStringLiteral("raster");
        decl.source = st.absolutePath;
        decl.group = QStringLiteral("04_SingleFactor");
        // 声明 factor.<层位>.idw 之前抬代次。同一字节的新文件也算改指向。
        ++m_publishGeneration;
        if (!layers->declare(decl, error))
          return false;

        paleo::workflow_detail::stampLayerAssetLink(
            layers, decl.layerId, st.assetId); // C4：已实例化层补盖资产关联
        emit factorDone(horizon, decl.layerId);
        return true;
      },
      error);
}
