// 层：功能
#include "../qgis/layervocabulary.h"
#include "workflows.h"
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

void ConstraintWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
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
} // namespace

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

  if (auto *existing = qobject_cast<QgsVectorLayer *>(layers->layer(QStringLiteral("constraints.%1").arg(horizon))); existing && existing->isEditable())
  {
    paleo::workflow_detail::setError(error, tr("先保存或取消约束编辑，再绘制新约束"));
    return false;
  }

  // In-memory constraint record (member-equivalent state via property).
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
    decl.type = QStringLiteral( "vector" );
    decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                      .arg( cs->gpkgPath(), horizon );
    decl.group = PaleoLayerVocabulary::kConstraintsGroup;
    if ( !layers->declare( decl, error ) )
    {
      cs->remove( c.id );
      return false;
    }

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
      return false;
    }
    layer->endEditCommand();
    for (const auto &remaining : cs->load())
      if (remaining.value(QStringLiteral("id")).toString() == constraintId)
      {
        paleo::workflow_detail::setError(error, tr("删除约束未写入 store"));
        return false;
      }
    emit constraintRemoved(constraintId);
    return true;
  }
  if ( !cs->remove( constraintId, error ) )
    return false;
  const auto removed = std::remove_if(
      m_inMemoryConstraints.begin(), m_inMemoryConstraints.end(),
      [&constraintId]( const QVariantMap &rec ) {
        return rec.value( QStringLiteral( "id" ) ).toString() == constraintId;
      } );
  m_inMemoryConstraints.erase( removed, m_inMemoryConstraints.end() );
  emit constraintRemoved( constraintId );
  return true;
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
      decl.type = QStringLiteral( "vector" );
      decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                        .arg( cs->gpkgPath(), horizon );
      decl.group = PaleoLayerVocabulary::kConstraintsGroup;
      layers->declare( decl );
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
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                          .arg( cs->gpkgPath(), h );
        decl.group = PaleoLayerVocabulary::kConstraintsGroup;
        layers->declare( decl );
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

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_constraint_idw" ), params, error );
  if ( results.isEmpty() )
    return false;

  const QString outPath = paleo::workflow_detail::outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "constraint IDW returned no output path" ) );
    return false;
  }
  QVariantMap idwExtra;
  idwExtra.insert( QStringLiteral( "field" ), field );
  idwExtra.insert( QStringLiteral( "cell_size" ), cellSize );
  idwExtra.insert( QStringLiteral( "constrained" ), hasConstraints );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "paleo:paleo_constraint_idw" ), idwExtra,
                                  &commitErr ) )
  {
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  // 声明 factor.<层位>.idw 之前抬代次。同一字节的新文件也算改指向。
  ++m_publishGeneration;
  if ( !layers->declare( decl, error ) )
    return false;

  paleo::workflow_detail::stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit factorDone( horizon, decl.layerId );
  return true;
}
