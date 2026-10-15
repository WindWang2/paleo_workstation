// 层：QGIS 封装
#include "qgisconstrainteditsession.h"
#include "constraintcrs.h"
#include "qgiseditingservice.h"
#include "../io/constraintstore.h"
#include "../metadata/paleoprojectstore.h"
#include <QFileInfo>
#include <QUndoStack>
#include <QTemporaryDir>
#include <gdal.h>
#include <ogr_api.h>
#include <algorithm>
#include <gdal_utils.h>
#include <cpl_error.h>
#include <qgsfeature.h>
#include <qgsvectorlayer.h>
#include <qgsproject.h>
#include <qgsproviderregistry.h>
#include <qgsprovidermetadata.h>
#include <qgsreadwritecontext.h>
#include <QDomDocument>

QgisConstraintEditSession::QgisConstraintEditSession(QgsVectorLayer *layer, PaleoProjectStore *store,
                                                   std::function<void(const QString &)> failed,
                                                   const ConstraintStore::AuditContext &context)
  : m_layer(layer), m_store(store), m_failed(std::move(failed)), m_audit(context)
{
}

QgisConstraintEditSession::~QgisConstraintEditSession()
{
  if (m_layer && m_workingDir && !m_finished)
  {
    disconnect(m_connection);
    QString error;
    if (!persist(m_initial, &error, QStringLiteral("cancel")) && m_failed)
      m_failed(tr("结束约束会话时恢复失败：%1").arg(error));
    m_layer->rollBack();
    restoreSource();
  }
}

void QgisConstraintEditSession::restoreSource()
{
  if (!m_layer || !m_workingDir)
    return;
  const auto selected = m_layer->selectedFeatureIds();
  QgsDataProvider::ProviderOptions options;
  options.transformContext = m_layer->transformContext();
  const QgsCoordinateReferenceSystem crs = m_layer->crs();
  m_layer->setDataSource(m_source, m_name, QStringLiteral("ogr"), options);
  // 无 SRS 的约束层换源后 CRS 不得丢（P1-C）：沿用会话前的 CRS。
  if (!m_layer->crs().isValid() && crs.isValid())
    m_layer->setCrs(crs);
  m_layer->setSubsetString(m_subset);
  m_layer->selectByIds(selected);
  m_layer->setProperty("paleoConstraintEditSession", false);
}

QVector<QVariantMap> QgisConstraintEditSession::snapshot(QString *error) const
{
  QVector<QVariantMap> rows;
  if (!m_layer)
  {
    *error = tr("约束编辑图层已不可用");
    return rows;
  }
  QgsFeature feature;
  auto features = m_layer->getFeatures();
  while (features.nextFeature(feature))
  {
    const QString invalid = QgisEditingService::geometryCommitError(feature.geometry(), tr("约束几何"));
    if (!invalid.isEmpty())
    {
      *error = invalid;
      return {};
    }
    QVariantMap row;
    for (int i = 0; i < feature.fields().count(); ++i)
      row.insert(feature.fields().at(i).name(), feature.attribute(i));
    row.insert(QStringLiteral("wkt"), feature.geometry().asWkt(17));
    const QString id = row.value(QStringLiteral("id")).toString();
    // 新增要素（缓冲区负 fid）只接受带稳定身份与语义类型的约束（ConstraintWorkflow
    // 绘制路径写入）；工具栏裸画的无语义要素仍拒收。
    const bool added = feature.id() < 0;
    if (row.value(QStringLiteral("horizon")).toString() != m_horizon ||
        (added && (id.isEmpty() || row.value(QStringLiteral("type")).toString().isEmpty())))
    {
      *error = tr("约束编辑不能新增无语义要素或改动层位");
      return {};
    }
    if (added)
    {
      if (!m_newFids.contains(feature.id()))
      {
        m_newFids.insert(feature.id(), m_nextFid++);
        m_newIdentities.insert(feature.id(), id);
      }
      if (m_newIdentities.value(feature.id()) != id)
      {
        *error = tr("约束编辑不能改动要素身份");
        return {};
      }
      row.insert(QStringLiteral("fid"), m_newFids.value(feature.id()));
    }
    else
    {
      row.insert(QStringLiteral("fid"), feature.id());
      if (!m_identities.isEmpty() && m_identities.value(feature.id()) != id)
      {
        *error = tr("约束编辑不能改动要素身份");
        return {};
      }
    }
    rows.append(row);
  }
  return rows;
}

bool QgisConstraintEditSession::initialize(QString *error)
{
  if (!m_layer) { *error = tr("约束编辑图层已不可用"); return false; }
  m_path = m_layer->source().section(QLatin1Char('|'), 0, 0);
  m_horizon = m_layer->customProperty(QStringLiteral("paleoLayerId")).toString().mid(12);
  if (!m_store || m_store->isReadOnly() || m_horizon.isEmpty() ||
      QFileInfo(m_path).canonicalFilePath().isEmpty() ||
      QFileInfo(m_path).canonicalFilePath() != QFileInfo(m_store->gpkgPath()).canonicalFilePath() ||
      !m_layer->source().contains(QLatin1String("|layername=constraints")))
  {
    *error = tr("约束编辑需要可写的工程 constraint store");
    return false;
  }
  QString reason;
  m_initial = snapshot(&reason);
  ConstraintStore store(m_path, m_store);
  if (!store.setAuditContext(m_audit, error)) return false;
  if (!reason.isEmpty() || m_initial.size() != store.load(m_horizon).size())
  {
    *error = reason.isEmpty() ? tr("约束图层与 store 的层位范围不一致") : reason;
    return false;
  }
  for (const auto &row : m_initial)
    m_identities.insert(row.value(QStringLiteral("fid")).toLongLong(), row.value(QStringLiteral("id")).toString());
  // 新绘约束的 store fid 从全表（所有层位）最大 fid 之后分配，避免撞到其他层位的行。
  {
    qlonglong maxFid = 0;
    if (GDALDatasetH ds = GDALOpenEx(m_path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr))
    {
      if (OGRLayerH layer = GDALDatasetGetLayerByName(ds, "constraints"))
      {
        OGR_L_ResetReading(layer);
        while (OGRFeatureH raw = OGR_L_GetNextFeature(layer))
        {
          maxFid = std::max<qlonglong>(maxFid, OGR_F_GetFID(raw));
          OGR_F_Destroy(raw);
        }
      }
      GDALClose(ds);
    }
    else
    {
      *error = tr("无法读取约束 store：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
      return false;
    }
    for (auto it = m_identities.cbegin(); it != m_identities.cend(); ++it)
      maxFid = std::max(maxFid, it.key());
    m_nextFid = maxFid + 1;
  }
  // Keep the native provider's baseline stable while the authoritative store
  // changes after every command. Otherwise provider count minus deleted FIDs
  // double-counts deletes, and native undo can reread newly persisted values.
  m_source = m_layer->source();
  m_subset = m_layer->subsetString();
  m_name = m_layer->name();
  m_workingDir = std::make_unique<QTemporaryDir>();
  GDALDatasetH source = GDALOpenEx(m_path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
  const QString workingPath = m_workingDir->filePath(QStringLiteral("constraints.gpkg"));
  char *arguments[] = {const_cast<char *>("-f"), const_cast<char *>("GPKG"),
                       const_cast<char *>("-preserve_fid"), const_cast<char *>("constraints"), nullptr};
  auto *options = GDALVectorTranslateOptionsNew(arguments, nullptr);
  GDALDatasetH copy = source && m_workingDir->isValid()
      ? GDALVectorTranslate(workingPath.toUtf8().constData(), nullptr, 1, &source, options, nullptr) : nullptr;
  GDALVectorTranslateOptionsFree(options);
  if (source)
    GDALClose(source);
  if (!copy)
  {
    *error = tr("无法建立约束编辑工作副本：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
    m_workingDir.reset();
    return false;
  }
  GDALClose(copy);
  const auto selected = m_layer->selectedFeatureIds();
  QgsDataProvider::ProviderOptions providerOptions;
  providerOptions.transformContext = m_layer->transformContext();
  const QgsCoordinateReferenceSystem sessionCrs = m_layer->crs();
  m_layer->setDataSource(workingPath + QStringLiteral("|layername=constraints"), m_name, QStringLiteral("ogr"), providerOptions);
  if (!m_layer->crs().isValid())
  {
    if (sessionCrs.isValid())
      m_layer->setCrs(sessionCrs); // 工作副本同样无 SRS：沿用原图层 CRS
    else
      paleo::constraintcrs::assignStoreCrsIfMissing(m_layer);
  }
  m_layer->setSubsetString(m_subset);
  m_layer->selectByIds(selected);
  if (!m_layer->isValid() || !m_layer->startEditing())
  {
    *error = tr("无法开始约束工作副本编辑");
    restoreSource();
    m_workingDir.reset();
    return false;
  }
  m_layer->setProperty("paleoConstraintEditSession", true);
  if (auto *project = m_layer->project())
    connect(project, &QgsProject::writeMapLayer, this,
            [this, project](QgsMapLayer *layer, QDomElement &element, QDomDocument &document) {
      if (m_finished || layer != m_layer)
        return;
      // Saving a project during a session must never serialize the temporary
      // provider path. Geometry and attributes already reside in the store.
      auto datasource = element.firstChildElement(QStringLiteral("datasource"));
      while (!datasource.firstChild().isNull())
        datasource.removeChild(datasource.firstChild());
      QgsReadWriteContext context;
      context.setPathResolver(project->pathResolver());
      auto *metadata = QgsProviderRegistry::instance()->providerMetadata(QStringLiteral("ogr"));
      const QString source = metadata ? metadata->absoluteToRelativeUri(m_source, context) : m_source;
      datasource.appendChild(document.createTextNode(source));
    });
  m_current = m_initial;
  connect(m_layer, &QgsVectorLayer::editCommandStarted, this, [this] { m_newCommand = true; });
  // A command is not durable until it ends. Index changes while it is open
  // include the push and the undo performed by destroyEditCommand; persisting
  // those would record a cancelled gesture as two edits.
  connect(m_layer, &QgsVectorLayer::editCommandEnded, this, [this] {
    m_newCommand = false;
    if (m_layer) synchronize(m_layer->undoStack()->index(), QStringLiteral("edit"));
  });
  connect(m_layer, &QgsVectorLayer::editCommandDestroyed, this, [this] { m_newCommand = false; });
  connect(m_layer, &QObject::destroyed, this, [this] {
    if (!m_finished)
    {
      QString error;
      if (!persist(m_initial, &error, QStringLiteral("cancel")) && m_failed)
        m_failed(tr("约束编辑图层移除时恢复失败：%1").arg(error));
    }
  });
  m_index = m_layer->undoStack()->index();
  m_connection = connect(m_layer->undoStack(), &QUndoStack::indexChanged, this,
                         [this](int index) { synchronize(index); });
  return true;
}

bool QgisConstraintEditSession::setAuditContext(const ConstraintStore::AuditContext &context, QString *error)
{
  ConstraintStore store(m_path, m_store);
  if (!store.setAuditContext(context, error)) return false;
  m_audit = context;
  return true;
}

bool QgisConstraintEditSession::persist(const QVector<QVariantMap> &rows, QString *error, const QString &action)
{
  if (!m_store)
  {
    *error = tr("约束 store 已不可用");
    return false;
  }
  ConstraintStore store(m_path, m_store);
  auto context = m_audit;
  context.action = action;
  if (context.reason.isEmpty()) context.reason = action == QLatin1String("undo") ? tr("撤销约束修改") :
      action == QLatin1String("redo") ? tr("重做约束修改") : action == QLatin1String("cancel") ? tr("放弃约束编辑并恢复会话起点") :
      action == QLatin1String("restore") ? tr("恢复约束保存失败前状态") : tr("原生约束几何或语义编辑");
  if (!store.setAuditContext(context, error)) return false;
  return store.replaceHorizon(m_horizon, rows, error);
}

void QgisConstraintEditSession::synchronize(int index, const QString &forcedAction)
{
  if (m_writing || !m_layer)
    return;
  // Wait until the open command ends or is destroyed. See editCommandEnded.
  if (m_newCommand && forcedAction.isEmpty())
    return;
  m_writing = true;
  QString error;
  const auto rows = snapshot(&error);
  const auto action = !forcedAction.isEmpty() ? forcedAction
      : index < m_index ? QStringLiteral("undo") : QStringLiteral("redo");
  if (error.isEmpty() && rows == m_current)
  {
    m_index = index;
    m_writing = false;
    return;
  }
  if (error.isEmpty() && persist(rows, &error, action))
  {
    m_current = rows;
    m_index = index;
  }
  else
  {
    // Transaction refused: native buffer returns to the last durable state.
    // At a full undo limit a push keeps the numeric index unchanged.
    if (index == m_index)
    {
      if (forcedAction == QLatin1String("edit"))
        m_layer->undoStack()->undo();
    }
    else
      m_layer->undoStack()->setIndex(m_index);
    m_index = m_layer->undoStack()->index();
    if (m_failed) m_failed(tr("约束编辑未保存：%1").arg(error));
  }
  m_writing = false;
}

bool QgisConstraintEditSession::finish(bool save, QString *error)
{
  if (!m_layer) { *error = tr("约束编辑图层已不可用"); return false; }
  QString reason;
  const auto rows = save ? snapshot(&reason) : m_initial;
  if (!reason.isEmpty() || !persist(rows, &reason, save ? QStringLiteral("edit") : QStringLiteral("cancel")))
  {
    *error = reason;
    return false;
  }
  disconnect(m_connection);
  // Store already owns the durable result. Discard the matching buffer;
  // flushing it again through OGR would repeat deletes and alter FIDs.
  if (!m_layer->rollBack())
  {
    const bool restored = persist(m_current, &reason, QStringLiteral("restore"));
    m_connection = connect(m_layer->undoStack(), &QUndoStack::indexChanged, this,
                           [this](int index) { synchronize(index); });
    *error = restored ? tr("无法结束约束编辑缓冲区") : tr("无法结束约束编辑缓冲区，恢复失败：%1").arg(reason);
    return false;
  }
  m_finished = true;
  restoreSource();
  m_workingDir.reset();
  m_layer->reload();
  m_layer->triggerRepaint();
  return true;
}
