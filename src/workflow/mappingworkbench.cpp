// 层：功能
#include "mappingworkbench.h"
#include "../algorithms/faciesmapping/formationdominance.h"
#include "../catalog/purgelease.h"
#include "../domain/faciescatalog.h"
#include "../domain/mappinghorizons.h"
#include "../domain/singlefactorrequest.h"
#include "../io/constraintstore.h"
#include "../io/lasparser.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/layervocabulary.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../qgis/wellattributestore.h"
#include "../services/futuretask.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include "assetops.h"
#include "constraintimport.h"
#include "derivedassets.h"
#include "evidencerecord.h"
#include "services/projectdata.h"
#include "services/seismichorizoncluster.h"
#include "wellpredictionservice.h"
#include "workflows.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgscoordinatereferencesystem.h>
#include <qgseditorwidgetsetup.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsunittypes.h>
#include <qgsvectorlayer.h>

namespace {
void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}
QString uid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString filePath(const QString &source) { return source.section('|', 0, 0); }
// 无大地基准的工程/局部直角米制 CRS（LOCAL_CS / ENGCRS，均映射到
// Qgis::CrsType::Engineering）与本工程局部网格等价；带基准的投影或
// 地理 CRS 仍拒绝。约束线与测区边界导入共用同一坐标门。
bool isLocalGridCrs(const QgsCoordinateReferenceSystem &c) {
  return c.isValid() && !c.isGeographic() &&
         c.mapUnits() == Qgis::DistanceUnit::Meters &&
         c.type() == Qgis::CrsType::Engineering;
}
bool acceptsLocalGrid(const QgsCoordinateReferenceSystem &c) {
  const auto local = QgsCoordinateReferenceSystem::fromWkt(
      DataCatalog::localGridCrsWkt());
  return c == local || isLocalGridCrs(c);
}
bool writeJson(const QString &path, const QVariant &value, QString *error) {
  QSaveFile f(path);
  const auto bytes = QJsonDocument::fromVariant(value).toJson();
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() ||
      !f.commit()) {
    fail(error, f.errorString());
    return false;
  }
  return true;
}
QVariantList wellAttributeUpdates(const QVariantList &points, const QVariantList &schema,
                                  const QString &horizon, bool predicted, const QString &method = {}, const QString &jobId = {}) {
  QVariantList updates;
  for (const auto &v : points) {
    const auto point = v.toMap();
    if (point.value("depth_mock").toBool()) continue; // 合成深度只供旧结果展示，不能维护真实井段。
    const auto intervals = QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8()).toVariant().toList();
    for (const auto &value : intervals) {
      const auto interval = value.toMap();
      const auto code = interval.value("code");
      const auto name = FaciesCatalog::find(schema, code).value("name").toString();
      const auto hierarchy = FaciesCatalog::attributes(schema, code);
      QVariantMap row{{"well_id", point.value("id")}, {"horizon", horizon},
        {"top_md", interval.value("top")}, {"base_md", interval.value("bottom")},
        {"facies_code", code}, {"facies", hierarchy.value("facies_name")},
        {"sub_facies", hierarchy.value("subfacies")},
        {"micro_facies", hierarchy.value("microfacies")}, {"facies_pattern", QStringLiteral("")}};
      if (predicted) { row.insert("predicted_facies", name); row.insert("model", point.value("prediction_model", method)); row.insert("job_id", point.value("prediction_job_id", jobId)); }
      if (predicted && point.contains("prediction_status")) {
        row.insert("prediction_status", point.value("prediction_status"));
        row.insert("prediction_reason", point.value("prediction_reason"));
      }
      updates << row;
    }
  }
  return updates;
}
} // namespace
MappingWorkbench::MappingWorkbench(QgisLayerService *layers,
                                   QgisProcessingService *processing,
                                   QgisProjectService *project,
                                   ConstraintWorkflow *constraints,
                                   QObject *parent)
    : QObject(parent), m_layers(layers), m_processing(processing),
      m_project(project), m_constraints(constraints) {
  // 方向51：不再自装测试替身（原先这里 setPredictionService(new <替身>(this))，
  // 产品里的远端预测就这样悄悄跑着假数据）。
  // 远端预测由装配根显式注入（app/aiwiring.cpp）；本地层位窗聚类是用户
  // 显式选择的 Mock 模式，产物记录真实反射特征来源与 Mock 类别解释。
  connect(m_constraints, &ConstraintWorkflow::factorJobFailed, this,
          &MappingWorkbench::errorOccurred);
  connect(m_constraints, &ConstraintWorkflow::factorGenerated, this,
          [this](const QString &h, const QString &, const QString &id) {
            if (m_constraints->factorBusy())
              publishFactorAlias(h, id);
          });
  connect(m_constraints, &ConstraintWorkflow::contoursGenerated, this,
          [this](const QString &h, const QString &, const QString &id) {
            if (m_constraints->factorBusy())
              publishFactorAlias(h, id);
          });
  connect(m_layers, &QgisLayerService::layerInstantiated, this,
          &MappingWorkbench::styleLayer);
  // .qgz 恢复的图层由 adoptProjectLayers 收编、不发 layerInstantiated，
  // styleLayer 永远不会跑——旧版本烘焙进图层的渲染/标注（相面默认带 ID
  // 序号）原样残留，draft 层的编辑接线同样缺席。工程打开后对在场声明
  // 图层补一次样式：显式选过的按工程条目，没选过的回当前默认口径。
  // 排队连接：等 store/目录在 projectOpened 各处理器里重绑完再跑，
  // versionForLayer 需要新工程的版本库。
  if (m_project)
    connect(m_project, &QgisProjectService::projectOpened, this,
            [this] {
              QVector<LayerDeclaration> declarations;
              if (!m_layers->tryDeclared(&declarations, nullptr))
                return;
              for (const auto &d : declarations)
                if (m_layers->layer(d.layerId))
                  styleLayer(d.layerId);
            },
            Qt::QueuedConnection);
  connect(m_layers, &QgisLayerService::layerDeclared, this,
          [this] { m_productsValid = false; emit changed(); });
  connect(this, &MappingWorkbench::predictionProgress, this, [this](int p) {
    if (m_task)
      m_task->reportBytes(std::clamp(p, 0, 100), 100);
  });
  connect(this, &MappingWorkbench::predictionDetailChanged, this, [this](const QString &detail) {
    if (m_task) m_task->reportDetail(detail);
  });
  // 不在 constraintAdded 上自动快照：每画一条线出一个「约束过程·vN」版本
  // 会刷屏图层树。快照只在使用点做——generateFactor/importConstraints
  // 前置 snapshotConstraints（谱系语义：被计算的约束状态）；实时显示走
  // constraints.<horizon> 声明图层（直读 store，reload 即新）。
}
MappingWorkbench::~MappingWorkbench() {
  if (m_mockCancelled)
    m_mockCancelled->store(true);
  if (m_prepareCancelled)
    m_prepareCancelled->store(true);
}
void MappingWorkbench::setTaskService(PaleoTaskService *tasks) {
  m_taskSvc = tasks;
  if (m_wellRemote)
    m_wellRemote->setTaskService(tasks);
}
void MappingWorkbench::beginPredictionTask(const QString &title) {
  endPredictionTask();
  if (!m_taskSvc)
    return;
  auto *task = m_taskSvc->beginExternal(title);
  m_task = task;
  connect(task, &PaleoTask::cancelAsked, this, [this, task] {
    if (m_task == task)
      cancelPrediction();
  });
}
void MappingWorkbench::endPredictionTask(const QString &error) {
  if (PaleoTask *task = m_task) {
    m_task = nullptr;
    if (error.isEmpty())
      task->reportBytes(100, 100);
    task->finishExternal(error);
  }
}
void MappingWorkbench::cancelPredictionTask() {
  if (PaleoTask *task = m_task) {
    m_task = nullptr; // 先摘：requestCancel 发的 cancelAsked 不再回调取消
    if (!task->cancelRequested())
      task->requestCancel();
    task->finishExternal();
  }
}
void MappingWorkbench::bindCatalog(DataCatalog *catalog, const QString &dir) {
  cancelPrediction();
  if (m_composeRunner)
    m_composeRunner->requestCancel();
  cancelPolygonize(); // 旧工程的转面结果由 commit 会话守卫丢弃
  if (m_catalog)
    disconnect(m_catalog, nullptr, this, nullptr);
  for (const auto &name : m_constraints->dynamicPropertyNames())
    if (name.startsWith("paleo.constraint.snapshot."))
      m_constraints->setProperty(name.constData(), QVariant());
  m_catalog = catalog;
  m_catalogSyncQueued = false;
  m_dir = dir;
  m_versionIndexValid = m_productsValid = false;
  m_aliasVersions.clear(); m_pathVersions.clear(); m_productsCache.clear();
  if (m_wellRemote) m_wellRemote->bindCatalog(catalog, dir);
  m_logVersion.clear();
  m_logCache = {};
  if (catalog)
    connect(catalog, &DataCatalog::changed, this, [this] {
      m_versionIndexValid = m_productsValid = false;
      if (m_catalogSyncQueued)
        return;
      m_catalogSyncQueued = true;
      const QPointer<DataCatalog> bound = m_catalog;
      QMetaObject::invokeMethod(
          this,
          [this, bound] {
            if (m_catalog != bound)
              return;
            m_catalogSyncQueued = false;
            synchronizeCatalogLayers();
            emit changed();
          },
          Qt::QueuedConnection);
    });
  synchronizeCatalogLayers();
  emit catalogBound();
  emit changed();
}
void MappingWorkbench::synchronizeCatalogLayers() {
  if (!m_catalog || !m_catalog->isOpen())
    return;
  QVector<LayerDeclaration> declarations;
  QString error;
  if (!m_layers->tryDeclared(&declarations, &error)) {
    emit errorOccurred(error);
    return;
  }
  QHash<QString, LayerDeclaration> known;
  for (const auto &d : declarations)
    known.insert(d.layerId, d);
  const auto declareMissing = [this, &known](const LayerDeclaration &d) {
    const auto old = known.value(d.layerId);
    if (old.source == d.source && old.horizon == d.horizon &&
        old.type == d.type && old.title == d.title && old.group == d.group)
      return;
    QString error;
    if (m_layers->declare(d, &error))
      known.insert(d.layerId, d);
    else
      emit errorOccurred(error);
  };
  // Catalog also receives products from the single-factor advanced tools.
  // Every saved version gets its own immutable layer, including products
  // whose original manifest_layer_id or layer_id is a moving current alias.
  for (const auto &a : m_catalog->assets())
    for (const auto &v : m_catalog->versionsForAsset(a.id)) {
      const auto e = v.extra;
      if (!e.value("mapping_product").toBool() ||
          (e.value("layer_type") != "vector" &&
           e.value("layer_type") != "raster"))
        continue;
      LayerDeclaration d;
      d.layerId = QStringLiteral("product.%1").arg(v.id);
      d.horizon = e.value("horizon").toString();
      d.type = e.value("layer_type").toString();
      d.source = DataCatalog::resolvedVersionPath(m_dir, v) +
                 e.value("source_suffix").toString();
      d.group = e.value("group").toString();
      d.title = e.value("title", a.displayName).toString() +
                tr(" · v%1").arg(v.versionNumber);
      d.title.replace(QStringLiteral("测井优势相"), QStringLiteral("测井相"));
      declareMissing(d);
      if (e.value("kind") == "constraint_snapshot" &&
          m_catalog->currentVersion(a.id).id == v.id)
        m_constraints->setProperty(
            ("paleo.constraint.snapshot." + d.horizon).toUtf8().constData(),
            filePath(d.source));
      const auto draftId = e.value("draft_id").toString();
      const auto working = e.value("working_path").toString();
      if (!draftId.isEmpty() && !working.isEmpty()) {
        CatalogVersion pathCheck;
        pathCheck.managed = true;
        pathCheck.path = working;
        const auto wp = DataCatalog::resolvedVersionPath(m_dir, pathCheck);
        if (!wp.isEmpty() && QFileInfo::exists(wp)) {
          d.layerId = draftId;
          d.source = wp + QStringLiteral("|layername=features");
          d.title = e.value("title").toString() + tr(" · 工作副本");
          d.title.replace(QStringLiteral("测井优势相"), QStringLiteral("测井相"));
          declareMissing(d);
        }
      }
    }
}
QString MappingWorkbench::layerForVersion(const QString &versionId,
                                          QString *error) {
  const auto v =
      m_catalog ? m_catalog->versionById(versionId) : CatalogVersion();
  if (v.id.isEmpty() || !v.extra.value("mapping_product").toBool() ||
      (v.extra.value("layer_type") != "vector" &&
       v.extra.value("layer_type") != "raster")) {
    fail(error, tr("所选版本不是可联动的编图或单因素图件"));
    return {};
  }
  const auto id = QStringLiteral("product.%1").arg(v.id);
  synchronizeCatalogLayers();
  if (declaration(id).layerId.isEmpty()) {
    fail(error, tr("所选图件版本尚未登记到图层目录"));
    return {};
  }
  return id;
}
void MappingWorkbench::setPredictionService(RemotePredictionService *service) {
  cancelPrediction();
  if (m_remote)
    disconnect(m_remote, nullptr, this, nullptr);
  m_remote = service;
  connectPredictionService(service);
}
void MappingWorkbench::setWellPredictionService(WellPredictionService *service) {
  cancelPrediction();
  if (m_wellRemote) disconnect(m_wellRemote, nullptr, this, nullptr);
  m_wellRemote = service;
  if (service) {
    service->setTaskService(m_taskSvc);
    service->bindCatalog(m_catalog, m_dir);
    connect(service, &WellPredictionService::detailChanged, this,
            [this](const QString &id, const QString &detail) {
      if (id != m_request.id) return;
      emit predictionDetailChanged(detail);
    });
  }
  connectPredictionService(service);
  emit predictionStatusChanged();
}
QString MappingWorkbench::wellPredictionStatusHint() const {
  return m_wellRemote ? m_wellRemote->statusHint() : predictionStatusHint();
}
void MappingWorkbench::connectPredictionService(RemotePredictionService *service) {
  if (!service)
    return;
  connect(service, &RemotePredictionService::completed, this,
          &MappingWorkbench::finishPrediction);
  connect(service, &RemotePredictionService::progress, this,
          [this](const QString &id, int p) {
            if (id == m_request.id)
              emit predictionProgress(p);
          });
  connect(service, &RemotePredictionService::failed, this,
          [this](const QString &id, const QString &message) {
            if (id != m_request.id)
              return;
            m_request = {};
            endPredictionTask(message);
            emit predictionBusyChanged(false);
            emit errorOccurred(message);
          });
}
void MappingWorkbench::setPredictionStatusHint(const QString &hint) {
  if (m_predictionHint == hint)
    return;
  m_predictionHint = hint;
  emit predictionStatusChanged();
}
bool MappingWorkbench::ready(const QString &h, QString *error) const {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开可写工程"));
    return false;
  }
  if (h.isEmpty() || !DataCatalog::isSafePathSegment(h) ||
      !isMappingHorizon(h)) {
    fail(error, tr("请先选择有效编图层位"));
    return false;
  }
  return true;
}
QString MappingWorkbench::schemaVersion(const QString &h) const {
  if (m_catalog)
    for (const auto &a : m_catalog->assets())
      if (a.type == QLatin1String("facies_schema") &&
          a.displayName == QStringLiteral("facies-schema-%1").arg(h))
        return m_catalog->currentVersion(a.id).id;
  return {};
}
QVariantList MappingWorkbench::facies(const QString &h) const {
  const auto id = schemaVersion(h);
  if (!id.isEmpty())
    return m_catalog->versionById(id).extra.value("facies").toList();
  return FaciesCatalog::defaults();
}
bool MappingWorkbench::saveFacies(const QString &h, const QVariantList &items,
                                  QString *error) {
  if (!ready(h, error))
    return false;
  if (items.isEmpty() || items.size() > 32) {
    fail(error, tr("每个层位需要 1–32 个相类别"));
    return false;
  }
  QSet<int> codes;
  for (const auto &v : items) {
    const auto f = v.toMap();
    bool ok = false;
    int code = f.value("code").toInt(&ok);
    if (!ok || code < 1 || code > 32767 || codes.contains(code) ||
        f.value("name").toString().trimmed().isEmpty() ||
        !QColor(f.value("color").toString()).isValid()) {
      fail(error, tr("相编码须为不重复的正整数，名称与颜色不能为空"));
      return false;
    }
    if (!f.value("texture").toString().isEmpty() &&
        FaciesCatalog::resourcePath(f.value("texture").toString()).isEmpty()) {
      fail(error, tr("纹理不在本项目纹理库中"));
      return false;
    }
    codes.insert(code);
  }
  DerivedAssetRegistrar r(m_catalog, m_dir);
  auto st = r.stage("facies_schema", QStringLiteral("facies-schema-%1").arg(h),
                    "facies.json", error);
  if (!st.isValid() || !writeJson(st.absolutePath, items, error))
    return false;
  QStringList parents;
  if (!schemaVersion(h).isEmpty())
    parents << schemaVersion(h);
  QVariantList normalized;
  for (const auto &v : items) {
    auto f = v.toMap();
    f.insert("code", f.value("code").toInt());
    normalized << f;
  }
  const bool ok = r.commit(st, parents, "facies-schema",
                           {{"horizon", h}, {"facies", normalized}}, error);
  if (ok) {
    // facies-ux 3：在场的测井相点层跟随新分类重着色。
    const auto schemas = WellAttributeStore::faciesSchemas(m_catalog);
    if (auto *formations = qobject_cast<QgsVectorLayer *>(
            m_layers->layer(WellAttributeStore::formationLayerId())))
      QgisStyleService::applyWellFormationLayerStyle(formations, schemas);
    if (auto *intervals = qobject_cast<QgsVectorLayer *>(
            m_layers->layer(WellAttributeStore::intervalLayerId())))
      QgisStyleService::applyWellAttributeLayerStyle(intervals, schemas);
    emit faciesChanged(h);
  }
  return ok;
}
QVariantList MappingWorkbench::inputs(const QString &kind) const {
  QVariantList out;
  if (!m_catalog || !m_catalog->isOpen())
    return out;
  if (kind == QLatin1String("wells")) {
    for (const auto &well : m_catalog->entities("well")) {
      QStringList parents;
      QString logVersion;
      for (const auto &link : m_catalog->linksForEntity(well.id))
        if (!link.unresolved &&
            QStringList{"well_log", "well_head", "tops", "time_depth", "interpretation", "core"}.contains(
                link.role)) {
          const auto v = m_catalog->currentVersion(link.assetId);
          if (!v.id.isEmpty() &&
              QFileInfo::exists(DataCatalog::resolvedVersionPath(m_dir, v))) {
            parents << v.id;
            if (link.role == "well_log" &&
                (logVersion.isEmpty() || link.isPrimary))
              logVersion = v.id;
          }
        }
      if (well.hasSurface && std::isfinite(well.surfaceX) &&
          std::isfinite(well.surfaceY))
        out << QVariantMap{
            {"id", well.id},      {"name", well.name},
            {"x", well.surfaceX}, {"y", well.surfaceY},
            {"parents", parents}, {"log_version_id", logVersion}};
    }
  } else
    for (const auto &a : m_catalog->assets()) {
      bool volume = a.type == QLatin1String("seismic") ||
                    a.type == QLatin1String("seismic_volume");
      for (const auto &l : m_catalog->linksForAsset(a.id))
        volume = volume ||
                 (!l.unresolved && l.role == QLatin1String("seismic_volume"));
      const auto v = m_catalog->currentVersion(a.id);
      if (volume && !v.id.isEmpty() &&
          QFileInfo::exists(DataCatalog::resolvedVersionPath(m_dir, v)))
        out << QVariantMap{{"id", a.id},
                           {"name", a.displayName},
                           {"parents", QStringList{v.id}}};
    }
  return out;
}
LayerDeclaration MappingWorkbench::declaration(const QString &id) const {
  auto result = m_layers->declaration(id);
  result.title.replace(QStringLiteral("测井优势相"), QStringLiteral("测井相"));
  return result;
}
void MappingWorkbench::ensureVersionIndex() const {
  if (!m_catalog) return;
  if (m_versionIndexValid && m_versionIndexSeq == m_catalog->mutationSeq() &&
      m_versionIndexRevision == m_catalog->catalogRevision()) return;
  m_aliasVersions.clear(); m_pathVersions.clear(); m_productsValid = false;
  const auto insert = [](auto &index, const QString &key, const CatalogVersion &v) {
    if (key.isEmpty()) return;
    auto it = index.find(key);
    if (it == index.end() || it->versionNumber < v.versionNumber) index.insert(key, v);
  };
  for (const auto &v : m_catalog->versions()) {
    insert(m_aliasVersions, v.extra.value("layer_id").toString(), v);
    insert(m_aliasVersions, v.extra.value("draft_id").toString(), v);
    insert(m_pathVersions, DataCatalog::resolvedVersionPath(m_dir, v), v);
  }
  m_versionIndexSeq = m_catalog->mutationSeq();
  m_versionIndexRevision = m_catalog->catalogRevision();
  m_versionIndexValid = true;
}
CatalogVersion MappingWorkbench::versionForLayer(const QString &id) const {
  if (!m_catalog || id.isEmpty()) return {};
  if (id.startsWith("product.")) {
    const auto exact = m_catalog->versionById(id.mid(8));
    if (!exact.id.isEmpty()) return exact;
  }
  ensureVersionIndex();
  auto found = m_aliasVersions.value(id);
  const auto byPath = m_pathVersions.value(filePath(declaration(id).source));
  if (found.id.isEmpty() || byPath.versionNumber > found.versionNumber) found = byPath;
  return found;
}
QVariantList MappingWorkbench::products(const QString &h) const {
  ensureVersionIndex();
  const auto filter = [&h](const QVariantList &all) {
    if (h.isEmpty()) return all;
    QVariantList out;
    for (const auto &v : all) {
      const auto horizon = v.toMap().value("horizon").toString();
      if (horizon.isEmpty() || horizon == h) out << v;
    }
    return out;
  };
  if (m_productsValid) return filter(m_productsCache);
  QHash<QString, QStringList> children;
  if (m_catalog)
    for (const auto &v : m_catalog->versions()) {
      const auto asset = m_catalog->assetById(v.assetId);
      const auto title = tr("%1 · v%2").arg(v.extra.value("title", asset.displayName).toString()).arg(v.versionNumber);
      for (const auto &parent : v.parentVersionIds) children[parent] << title;
    }
  QVariantList rows;
  for (const auto &d : m_layers->declared()) {
    // h 为空 = 不按层位圈定（全部层位的成果都列出）；有 h 时精确匹配，
    // 无层位声明（井位/测区/底图）在任何视图都保留。
    const auto v = versionForLayer(d.layerId);
    const auto e = v.extra;
    // Current aliases remain in the layer tree; the result list shows one row
    // per immutable version plus each writable draft.
    if (e.value("mapping_product").toBool() &&
        d.layerId != QStringLiteral("product.%1").arg(v.id) &&
        !d.layerId.startsWith("draft."))
      continue;
    QStringList parentNames, childNames;
    if (m_catalog) {
      for (const auto &parentId : v.parentVersionIds) {
        const auto p = m_catalog->versionById(parentId);
        const auto asset = m_catalog->assetById(p.assetId);
        parentNames
            << tr("%1 · v%2")
                   .arg(p.extra.value("title", asset.displayName).toString())
                   .arg(p.versionNumber);
      }
      childNames = children.value(v.id);
    }
    rows << QVariantMap{{"id", d.layerId},
                        {"name", d.title.isEmpty() ? d.layerId : QString(d.title).replace(QStringLiteral("测井优势相"), QStringLiteral("测井相"))},
                        {"horizon", d.horizon},
                        {"type", d.type},
                        {"version", v.versionNumber},
                        {"version_id", v.id},
                        {"asset_id", v.assetId},
                        {"parents", v.parentVersionIds},
                        {"parent_names", parentNames},
                        {"children", childNames},
                        {"kind", e.value("kind")},
                        {"mock", e.value("mock")},
                        {"path", d.source},
                        {"method", v.sourceUri},
                        {"parameters", e},
                        {"draft", d.layerId.startsWith("draft.")}};
  }
  m_productsCache = rows;
  m_productsValid = true;
  return filter(rows);
}
QString MappingWorkbench::record(const QString &path, const QString &h,
                                 const QString &kind, const QString &title,
                                 const QString &type,
                                 const QStringList &parents, QVariantMap extra,
                                 QString *error, const QString &suffix,
                                 const QString &assetKey,
                                 const DerivedFileFingerprint *fingerprint) {
  DerivedAssetRegistrar r(m_catalog, m_dir);
  auto st = r.stage(
      kind,
      assetKey.isEmpty() ? QStringLiteral("%1-%2").arg(h, kind) : assetKey,
      QStringLiteral("result.%1").arg(QFileInfo(path).suffix()), error);
  if (!st.isValid())
    return {};
  const auto id = QStringLiteral("product.%1").arg(st.versionId);
  const auto group =
      kind.contains("prediction")   ? QStringLiteral("02_Prediction")
      : kind.contains("constraint") ? PaleoLayerVocabulary::kConstraintsGroup
                                    : QStringLiteral("05_PaleoMap");
  extra.insert("mapping_product", true);
  extra.insert("layer_id", id);
  extra.insert("horizon", h);
  extra.insert("kind", kind);
  extra.insert("title", title);
  extra.insert("layer_type", type);
  extra.insert("source_suffix", suffix);
  extra.insert("group", group);
  extra.insert("created_at",
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  for (const auto &parent : parents)
    if (m_catalog->versionById(parent).extra.value("mock").toBool())
      extra.insert("mock", true);
  if (!extra.contains("facies"))
    extra.insert("facies", facies(h));
  // facies-ux 5：证据链（输入版本 + 模型/参数指纹 + 支撑井/地震属性 + 软件版本）。
  if (!extra.contains("evidence"))
    extra.insert("evidence", paleo::workflow_detail::evidenceRecord(m_catalog, parents, extra));
  auto attested = fingerprint ? *fingerprint : DerivedFileFingerprint{};
  QString actual = path;
  if (fingerprint) {
    if (!fingerprint->matches(path) || !QFile::rename(path, st.absolutePath)) {
      fail(error, tr("无法发布已校验的计算成果"));
      return {};
    }
    actual = st.absolutePath;
    attested.path = actual;
  }
  if (!r.commitExternal(st, actual, parents,
                        extra.value("method", kind).toString(), extra, error,
                        fingerprint ? &attested : nullptr)) {
    if (fingerprint)
      QFile::remove(actual);
    return {};
  }
  LayerDeclaration d;
  d.layerId = id;
  d.horizon = h;
  d.type = type;
  d.source = st.absolutePath + suffix;
  d.group = group;
  d.title = title + tr(" · v%1").arg(st.versionNumber);
  if (!m_layers->declare(d, error))
    return {};
  if (QDir::cleanPath(path).startsWith(
          QDir(m_dir).filePath("artifacts/staging/"))) {
    QFile::remove(path);
    QFile::remove(path + "-wal");
    QFile::remove(path + "-shm");
    QFile::remove(path + ".aux.xml");
  }
  emit changed();
  emit productReady(h, id);
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("图件已登记，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return id;
}
QString MappingWorkbench::openWellAttributes(QString *error) {
  return WellAttributeStore::open(m_catalog, m_dir, m_layers, false, true, error)
             ? WellAttributeStore::intervalLayerId() : QString();
}
QString MappingWorkbench::openWellFormations(QString *error) {
  return WellAttributeStore::open(m_catalog, m_dir, m_layers,
                                  WellAttributeStore::Table::Formations, true, error)
             ? WellAttributeStore::formationLayerId() : QString();
}
void MappingWorkbench::ensureWellBaseLayers() {
  if (!m_layers)
    return;
  QString error;
  for (const auto table : {WellAttributeStore::Table::Intervals,
                           WellAttributeStore::Table::Formations,
                           WellAttributeStore::Table::Factors})
    WellAttributeStore::open(m_catalog, m_dir, m_layers, table,
                             /*create=*/false, &error);
  if (!m_layers->layer(QStringLiteral("wells")))
    m_layers->instantiate(QStringLiteral("wells"), &error);
}
QVariantList MappingWorkbench::wellFormationUpdates(
    const QVariantList &points, const QVariantList &schema, bool predicted,
    const QString &method, const QString &jobId) const {
  QVariantList updates;
  if (!m_catalog)
    return updates;
  ProjectDataFacade data;
  data.setCatalog(m_catalog.data(), m_dir);
  for (const auto &v : points) {
    const auto point = v.toMap();
    if (point.value("depth_mock").toBool())
      continue; // 合成深度只供旧结果展示，不能维护真实地层。
    const QString wellId = point.value("id").toString();
    QVector<FaciesSpan> spans;
    double bottom = 0;
    for (const auto &value :
         QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8())
             .toVariant().toList()) {
      const auto interval = value.toMap();
      const auto code = interval.value("code");
      spans.append({interval.value("top").toDouble(), interval.value("bottom").toDouble(),
                    FaciesCatalog::find(schema, code).value("name").toString(), code.toInt()});
      bottom = std::max(bottom, interval.value("bottom").toDouble());
    }
    auto tops = data.topsFor(wellId);
    tops.erase(std::remove_if(tops.begin(), tops.end(),
                              [](const WellTop &t) { return !std::isfinite(t.md); }),
               tops.end());
    std::stable_sort(tops.begin(), tops.end(),
                     [](const WellTop &a, const WellTop &b) { return a.md < b.md; });
    QVector<WellComposite::FormationInterval> formations;
    QSet<QString> names;
    for (qsizetype i = 0; i < tops.size(); ++i) {
      const double base = i + 1 < tops.size() ? tops[i + 1].md : bottom;
      if (tops[i].horizon.trimmed().isEmpty() || !(base > tops[i].md) ||
          names.contains(tops[i].horizon.trimmed()))
        continue;
      names.insert(tops[i].horizon.trimmed());
      WellComposite::FormationInterval f;
      f.name = tops[i].horizon.trimmed();
      f.topDepth = float(tops[i].md);
      f.bottomDepth = float(base);
      f.unitType = f.name.endsWith(QStringLiteral("组")) ? QStringLiteral("组")
                                                         : QStringLiteral("段");
      formations.append(f);
    }
    if (point.contains("formation_intervals")) {
      formations.clear();
      const auto stored = point.value("formation_intervals");
      const auto frozen = stored.metaType().id() == QMetaType::QVariantList
          ? stored.toList()
          : QJsonDocument::fromJson(stored.toString().toUtf8()).toVariant().toList();
      for (const auto &value : frozen) {
        const auto row = value.toMap();
        WellComposite::FormationInterval formation;
        formation.name = row.value("name").toString();
        formation.unitType = row.value("unit_type").toString();
        formation.topDepth = float(row.value("top").toDouble());
        formation.bottomDepth = float(row.value("bottom").toDouble());
        formations.append(formation);
      }
    }
    for (const auto &d : paleo::faciesmapping::dominantFaciesByFormation(formations, spans)) {
      QVariantMap row{{"well_id", wellId}, {"formation", d.formation},
        {"unit_type", d.unitType}, {"top_md", d.top}, {"base_md", d.bottom},
        {"thickness_md", d.thickness}, {"dominant_facies", d.dominant},
        {"facies_code", d.dominantCode > 0 ? QVariant(d.dominantCode) : QVariant()},
        {"dominant_thickness_md", d.dominantThickness}, {"dominant_ratio", d.ratio},
        {"coverage", d.coverage}, {"tie", d.tie}, {"facies_composition", d.composition}};
      if (predicted) {
        row.insert("predicted_dominant", d.dominant);
        if (point.contains("prediction_status")) {
          row.insert("prediction_status", point.value("prediction_status"));
          row.insert("prediction_reason", point.value("prediction_reason"));
        }
        row.insert("source", QStringLiteral("wells_prediction"));
        row.insert("model", point.value("prediction_model", method));
        row.insert("job_id", point.value("prediction_job_id", jobId));
      }
      updates << row;
    }
  }
  return updates;
}
bool MappingWorkbench::predict(const QString &h, const QString &kind,
                               const QStringList &ids, QString *error,
                               const QString &horizonFile) {
  if (!ready(h, error))
    return false;
  const bool mock = kind == QLatin1String("seismic_mock");
  const bool seismicInput = kind == QLatin1String("seismic") || mock;
  if (kind != "wells" && !seismicInput) {
    fail(error, tr("未知预测类型"));
    return false;
  }
  if (busy()) {
    fail(error, tr("已有预测运行中"));
    return false;
  }
  if (!m_remote && !(kind == "wells" && m_wellRemote) && !mock) {
    // 远端预测需要装配服务；本地层位窗聚类独立运行。
    fail(error, tr("远端预测未配置，走本地引擎"));
    return false;
  }
  if (ids.isEmpty() || (seismicInput && ids.size() != 1)) {
    fail(error, tr("地震预测选择一个地震体；测井预测至少选择一口井"));
    return false;
  }
  if (kind == QLatin1String("wells")) {
    for (const auto &id : {WellAttributeStore::intervalLayerId(), WellAttributeStore::formationLayerId()}) {
      auto *attributes = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
      if (attributes && attributes->isEditable()) { fail(error, tr("测井属性表正在编辑，请先保存或放弃编辑")); return false; }
    }
  }
  if (schemaVersion(h).isEmpty() && !saveFacies(h, facies(h), error))
    return false;
  RemotePredictionRequest request;
  request.id = uid();
  request.horizon = h;
  request.kind = seismicInput ? QStringLiteral("seismic") : kind;
  request.facies = facies(h);
  const auto choices = inputs(kind);
  QSet<QString> seen;
  // 测井曲线的深度范围放到后台读（整份 LAS 解析，多井时主线程会卡住）；
  // 这里只在 catalog 所属线程解析出文件路径。
  QStringList logPaths;
  for (const auto &v : choices) {
    const auto row = v.toMap();
    if (!ids.contains(row.value("id").toString()))
      continue;
    seen.insert(row.value("id").toString());
    request.sourceVersionIds << row.value("parents").toStringList();
    if (kind == "wells") {
      auto well = row;
      const auto logVersion = row.value("log_version_id").toString();
      logPaths << (logVersion.isEmpty()
                       ? QString()
                       : DataCatalog::resolvedVersionPath(
                             m_dir, m_catalog->versionById(logVersion)));
      well.insert("vector_intervals", WellAttributeStore::rows(m_dir, m_layers, false, row.value("id").toString()));
      well.insert("vector_factors", WellAttributeStore::rows(m_dir, m_layers, true, row.value("id").toString(), h));
      well.insert("vector_formations", WellAttributeStore::rows(m_dir, m_layers, WellAttributeStore::Table::Formations, row.value("id").toString()));
      request.wells << well;
    }
  }
  if (seen.size() != QSet<QString>(ids.cbegin(), ids.cend()).size()) {
    fail(error, tr("输入缺少可读源文件、测井曲线或有效井坐标，请检查数据管理"));
    return false;
  }
  request.sourceVersionIds << schemaVersion(h);
  request.sourceVersionIds.removeDuplicates();
  if (seismicInput && !mock) {
    for (const auto &l : m_catalog->linksForAsset(ids.first()))
      if (!l.unresolved) {
        const auto entity = m_catalog->entityById(l.entityId);
        for (const auto &p : entity.corners) {
          QRectF pt(p.first, p.second, 0.001, 0.001);
          request.extent =
              request.extent.isNull() ? pt : request.extent.united(pt);
        }
      }
    if (request.extent.width() < 1 || request.extent.height() < 1) {
      for (const auto &d : m_layers->declared())
        if (d.horizon == h && d.type == "raster") {
          auto *r =
              qobject_cast<QgsRasterLayer *>(m_layers->instantiate(d.layerId));
          if (r) {
            auto e = r->extent();
            request.extent =
                QRectF(e.xMinimum(), e.yMinimum(), e.width(), e.height());
            break;
          }
        }
    }
    if (request.extent.width() < 1 || request.extent.height() < 1) {
      fail(error,
           tr("地震体缺少工区角点，且当前层位没有可用网格，请先完成空间配准"));
      return false;
    }
  }
  if (mock) {
    // 原始层位的道号/TWT 可直接提取；时间栅格只作为已有工程的兼容输入。
    QString horizonPath = horizonFile, horizonVersion;
    auto source = SeismicHorizonSource::Scatter;
    const QString selectedName = QFileInfo(horizonFile).completeBaseName().toUpper();
    if (!horizonFile.isEmpty() && isMappingHorizon(selectedName) && selectedName != h) {
      fail(error, tr("所选文件属于 %1，当前编图层位是 %2，请选择对应层位的文件").arg(selectedName, h));
      return false;
    }
    if (horizonPath.isEmpty()) {
      QMap<QString, QString> originals;
      CatalogVersion snapshot;
      for (const auto &asset : m_catalog->assets()) {
        if (asset.type != QLatin1String("horizon")) continue;
        bool linked = false;
        for (const auto &link : m_catalog->linksForAsset(asset.id))
          if (!link.unresolved && link.role == QLatin1String("horizon") &&
              m_catalog->entityById(link.entityId).name.compare(h, Qt::CaseInsensitive) == 0)
            linked = true;
        CatalogVersion raw;
        for (const auto &version : m_catalog->versionsForAsset(asset.id)) {
          const QString path = DataCatalog::resolvedVersionPath(m_dir, version);
          if (!QFileInfo(path).isFile()) continue;
          if (version.extra.value("horizon_input_snapshot").toBool() &&
              version.extra.value("horizon").toString() == h) {
            if (snapshot.id.isEmpty() || version.versionNumber > snapshot.versionNumber)
              snapshot = version;
          } else if (version.stage == QLatin1String("RAW") &&
                     (linked || QFileInfo(version.fileName).completeBaseName().compare(h, Qt::CaseInsensitive) == 0) &&
                     (raw.id.isEmpty() || version.versionNumber > raw.versionNumber)) {
            raw = version;
          }
        }
        if (!raw.id.isEmpty())
          originals.insert(DataCatalog::resolvedVersionPath(m_dir, raw), raw.id);
      }
      if (originals.size() > 1) {
        fail(error, tr("%1 有多个原始层位文件，请在「层位文件」选择本次使用的文件").arg(h));
        return false;
      }
      if (!originals.isEmpty()) {
        horizonPath = originals.firstKey(); horizonVersion = originals.first();
      } else if (!snapshot.id.isEmpty()) {
        horizonPath = DataCatalog::resolvedVersionPath(m_dir, snapshot);
        horizonVersion = snapshot.id;
      } else {
        source = SeismicHorizonSource::TimeRaster;
        for (const auto &d : m_layers->declared())
          if (d.horizon == h && d.type == QLatin1String("raster") &&
              d.layerId.startsWith(QLatin1String("horizon.")) &&
              QFileInfo(filePath(d.source)).isFile()) {
            if (horizonPath.isEmpty() || d.layerId == QStringLiteral("horizon.") + h) {
              horizonPath = filePath(d.source);
              horizonVersion = versionForLayer(d.layerId).id;
            }
          }
      }
    }
    if (!QFileInfo(horizonPath).isFile() || !QFileInfo(horizonPath).isReadable()) {
      fail(error, tr("未找到 %1 的可读层位数据，请在「层位文件」直接选择 DAT，或在数据管理中导入层位资料").arg(h));
      return false;
    }
    if (!horizonVersion.isEmpty() && source == SeismicHorizonSource::TimeRaster)
      request.sourceVersionIds << horizonVersion;
    request.sourceVersionIds.removeDuplicates();
    QVector<int> codes;
    for (const auto &f : request.facies)
      codes << f.toMap().value("code").toInt();
    const auto seismicVersion = m_catalog->currentVersion(ids.first());
    const QString seismicPath = DataCatalog::resolvedVersionPath(m_dir, seismicVersion);
    const QString cacheDir = QDir(m_dir).filePath("artifacts/cache/segy-index");
    std::shared_ptr<QTemporaryDir> inputSnapshot;
    if (source == SeismicHorizonSource::Scatter) {
      const QString staging = QDir(m_dir).filePath("artifacts/staging");
      QDir().mkpath(staging);
      inputSnapshot = std::make_shared<QTemporaryDir>(staging + "/horizon-input-XXXXXX");
      if (!inputSnapshot->isValid()) {
        fail(error, tr("无法创建层位输入快照目录"));
        return false;
      }
    }
    m_request = request;
    m_predictionParameters = {
      {"horizon_source_format", source == SeismicHorizonSource::Scatter ? "smi_xyz_inline_crossline" : "time_raster"},
      {"horizon_source_uri", QFileInfo(horizonPath).absoluteFilePath()}};
    beginPredictionTask(tr("智能预测 · %1 · 地震层位窗聚类（Mock）").arg(h));
    if (m_task)
      m_task->reportDetail(tr("后台读取地震道与层位窗"));
    m_mockCancelled = std::make_shared<std::atomic_bool>(false);
    auto cancelled = m_mockCancelled;
    auto *watcher = new QFutureWatcher<SeismicClusterResult>(this);
    connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
            [this, request](int value) {
      if (m_request.id == request.id)
        emit predictionProgress(value);
    });
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, request, cancelled, inputSnapshot, horizonPath, horizonVersion] {
      const auto clustered = watcher->result();
      watcher->deleteLater();
      if (m_request.id != request.id || cancelled->load())
        return;
      if (!clustered.error.isEmpty()) {
        m_request = {};
        m_predictionParameters.clear();
        endPredictionTask(clustered.error);
        emit predictionBusyChanged(false);
        emit errorOccurred(clustered.error);
        return;
      }
      if (inputSnapshot) {
        // 保存本次实际使用的原始字节，形成可追溯输入；不生成时间栅格。
        DerivedAssetRegistrar registrar(m_catalog, m_dir);
        QString error;
        const auto st = registrar.stage("horizon", request.horizon + "-prediction-input",
                                        request.horizon + ".dat", &error);
        const QStringList parents = horizonVersion.isEmpty() ? QStringList() : QStringList{horizonVersion};
        if (!st.isValid() || !registrar.commitExternal(st, inputSnapshot->filePath("horizon.dat"),
            parents, horizonPath, {{"horizon", request.horizon}, {"horizon_input_snapshot", true},
                                  {"z_units", "ms"}}, &error)) {
          m_request = {};
          m_predictionParameters.clear();
          endPredictionTask(error);
          emit predictionBusyChanged(false);
          emit errorOccurred(error);
          return;
        }
        m_request.sourceVersionIds << st.versionId;
      }
      m_predictionParameters.insert("valid_cells", clustered.validCells);
      m_request.extent = clustered.extent;
      m_request.rows = clustered.rows;
      m_request.columns = clustered.columns;
      RemotePredictionResult result;
      result.request = m_request;
      result.cells = clustered.cells;
      result.mock = true;
      result.method = QStringLiteral("mock-horizon-window-kmeans-v1");
      finishPrediction(result);
    });
    emit predictionBusyChanged(true);
    watcher->setFuture(QtConcurrent::run(
        paleo::tasks::pool(m_taskSvc),
        [seismicPath, horizonPath, cacheDir, codes, cancelled, source,
         inputSnapshot](QPromise<SeismicClusterResult> &promise) {
          promise.setProgressRange(0, 100);
          QString inputPath = horizonPath;
          if (inputSnapshot) {
            inputPath = inputSnapshot->filePath("horizon.dat");
            const QFileInfo before(horizonPath);
            const qint64 sourceSize = before.size();
            const auto modified = before.lastModified();
            if (!QFile::copy(horizonPath, inputPath) ||
                sourceSize != QFileInfo(inputPath).size() ||
                sourceSize != QFileInfo(horizonPath).size() ||
                modified != QFileInfo(horizonPath).lastModified()) {
              SeismicClusterResult failed;
              failed.error = tr("层位文件复制失败或复制期间被修改，请重试");
              promise.addResult(failed);
              return;
            }
          }
          promise.addResult(clusterSeismicHorizon(seismicPath, inputPath, cacheDir, codes,
              [&promise](int value) { promise.setProgressValue(value); },
              [cancelled] { return cancelled->load(); }, source));
        }));
    return true;
  }
  m_mockCancelled.reset();
  m_predictionParameters.clear();
  m_request = request;
  beginPredictionTask(request.kind == "wells"
                          ? tr("智能预测 · %1 · 测井 %2 口井（每井一个预测相）")
                                .arg(h).arg(request.wells.size())
                          : tr("智能预测 · %1 · 地震相（预测服务）").arg(h));
  emit predictionBusyChanged(true);
  emit predictionDetailChanged(tr("准备预测输入"));
  if (request.kind == "wells" && m_wellRemote) {
    m_wellRemote->start(request);
    return true;
  }
  if (request.kind != "wells") {
    if (m_task)
      m_task->reportDetail(tr("已提交预测服务，等待返回"));
    m_remote->start(request);
    return true;
  }
  // 测井：后台读各井曲线深度范围 → 回主线程补进请求 → 提交预测服务。
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  m_prepareCancelled = cancelled;
  if (m_task)
    m_task->reportDetail(tr("后台读取 %1 口井的测井曲线").arg(logPaths.size()));
  auto *watcher = new QFutureWatcher<QVector<QPair<double, double>>>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
          [this, id = request.id, total = logPaths.size()](int done) {
    if (m_task && m_request.id == id)
      m_task->reportDetail(tr("读取测井曲线 %1/%2").arg(done).arg(total));
  });
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, id = request.id, cancelled] {
    const auto ranges = watcher->result();
    watcher->deleteLater();
    if (m_request.id != id || cancelled->load())
      return;
    m_prepareCancelled.reset();
    for (qsizetype i = 0; i < m_request.wells.size() && i < ranges.size(); ++i) {
      auto well = m_request.wells[i].toMap();
      const bool measured = std::isfinite(ranges[i].first) &&
                            ranges[i].second > ranges[i].first;
      well.insert("depth_top", measured ? ranges[i].first : 0.0);
      well.insert("depth_bottom", measured ? ranges[i].second : 120.0);
      well.insert("depth_mock", !measured);
      m_request.wells[i] = well;
    }
    if (m_task)
      m_task->reportDetail(
          tr("已提交 %1 口井，等待预测服务返回").arg(m_request.wells.size()));
    m_remote->start(m_request);
  });
  watcher->setFuture(QtConcurrent::run(
      paleo::tasks::pool(m_taskSvc),
      [logPaths, cancelled](QPromise<QVector<QPair<double, double>>> &promise) {
        promise.setProgressRange(0, int(logPaths.size()));
        QVector<QPair<double, double>> ranges;
        for (const auto &path : logPaths) {
          if (cancelled->load())
            break;
          double top = std::numeric_limits<double>::infinity(), bottom = -top;
          QStringList names;
          QList<LasCurve> curves;
          if (!path.isEmpty() &&
              LasParser::parse(path, names, curves, nullptr) &&
              !curves.isEmpty())
            for (double d : curves.first().values)
              if (std::isfinite(d)) {
                top = std::min(top, d);
                bottom = std::max(bottom, d);
              }
          ranges.append({top, bottom});
          promise.setProgressValue(int(ranges.size()));
        }
        promise.addResult(ranges);
      }));
  return true;
}
void MappingWorkbench::cancelPrediction() {
  cancelPredictionTask();
  if (m_mockCancelled)
    m_mockCancelled->store(true);
  else if (m_prepareCancelled)
    m_prepareCancelled->store(true); // 还在读曲线，预测服务尚未提交
  else if (m_request.kind == "wells" && m_wellRemote)
    m_wellRemote->cancel();
  else if (m_remote)
    m_remote->cancel();
  m_prepareCancelled.reset();
  bool was = busy();
  m_request = {};
  m_predictionParameters.clear();
  if (was)
    emit predictionBusyChanged(false);
}
void MappingWorkbench::finishPrediction(const RemotePredictionResult &result) {
  if (result.request.id != m_request.id || !m_catalog || !m_catalog->isOpen())
    return;
  const auto request = m_request;
  // Validate the response against the frozen request before publishing any
  // artifact.
  const auto schema = result.facies.isEmpty() ? request.facies : result.facies;
  QSet<int> codes;
  bool schemaValid = !schema.isEmpty() && schema.size() <= 32;
  for (const auto &f : schema) {
    const auto row = f.toMap();
    bool ok = false;
    const int code = row.value("code").toInt(&ok);
    schemaValid &= ok && code > 0 && code <= 32767 && !codes.contains(code) &&
        !row.value("name").toString().trimmed().isEmpty() &&
        QColor(row.value("color").toString()).isValid();
    codes.insert(code);
  }
  for (int i = 0; i < request.facies.size(); ++i)
    schemaValid &= i < schema.size() && schema.value(i) == request.facies[i];
  // 只有真实测井适配器可返回明确的逐井跳过记录；仍严格核对井 ID 和坐标。
  const auto isSkipped = [&result](const QVariantMap &point) {
    return result.partialWellCoverage && !result.mock &&
        result.method == QLatin1String("remote-well-facies-api-v2") &&
        point.value("prediction_status").toString() == QStringLiteral("未预测") &&
        !point.value("prediction_reason").toString().trimmed().isEmpty() &&
        point.contains("facies_code") && point.value("facies_code").isNull();
  };
  bool valid = schemaValid && result.request.horizon == request.horizon &&
               result.request.kind == request.kind;
  if (result.currentFormationOnly)
    valid = valid && request.kind == "wells" && result.partialWellCoverage &&
        !result.mock && result.method == QLatin1String("remote-well-facies-api-v2");
  if (request.kind == "seismic") {
    valid = valid && result.cells.size() == request.rows * request.columns;
    for (int code : result.cells)
      valid = valid && (codes.contains(code) ||
          (result.mock && result.method == QLatin1String("mock-horizon-window-kmeans-v1") && code == -9999));
  } else {
    valid = valid && result.points.size() == request.wells.size();
    for (int i = 0; i < result.points.size() && valid; ++i) {
      auto p = result.points[i].toMap();
      auto w = request.wells[i].toMap();
      valid = (codes.contains(p.value("facies_code").toInt()) || isSkipped(p)) &&
              p.value("id") == w.value("id") && p.value("x") == w.value("x") &&
              p.value("y") == w.value("y");
    }
  }
  QVariantList points = result.points;
  if (request.kind == "wells")
    for (int i = 0; i < points.size() && valid; ++i) {
      auto point = points[i].toMap();
      const auto intervals =
          QJsonDocument::fromJson(
              point.value("facies_intervals").toString().toUtf8())
              .toVariant()
              .toList();
      if (isSkipped(point)) {
        const auto json = QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8());
        valid = json.isArray() && json.array().isEmpty();
        point.insert("horizon", request.horizon);
        point.insert("depth_mock", false);
        point.insert("predicted_intervals", QStringLiteral("[]"));
        for (const auto &key : {"facies_name", "subfacies", "microfacies", "dominant_facies", "predicted_dominant"})
          point.insert(key, QVariant());
        point.insert("facies_label", tr("未预测"));
        point.insert("facies_scope", QStringLiteral("formation"));
        point.insert("formation", request.horizon);
        point.insert("coverage", 0.0);
        points[i] = point;
        continue;
      }
      const double rangeTop = result.partialWellCoverage
          ? point.value("prediction_depth_top").toDouble()
          : request.wells[i].toMap().value("depth_top").toDouble();
      const double rangeBase = result.partialWellCoverage
          ? point.value("prediction_depth_bottom").toDouble()
          : request.wells[i].toMap().value("depth_bottom").toDouble();
      if (result.currentFormationOnly) {
        const auto frozen = QJsonDocument::fromJson(point.value("formation_intervals").toString().toUtf8()).toVariant().toList();
        const auto formation = frozen.value(0).toMap();
        valid = frozen.size() == 1 &&
            formation.value("name").toString().compare(request.horizon, Qt::CaseInsensitive) == 0 &&
            formation.value("top").toDouble() == rangeTop &&
            formation.value("bottom").toDouble() == rangeBase;
      }
      double last = rangeTop;
      valid = valid && !intervals.isEmpty() && intervals.size() <= 10000;
      for (const auto &v : intervals) {
        const auto interval = v.toMap();
        bool topOk = false, bottomOk = false;
        double top = interval.value("top").toDouble(&topOk),
               bottom = interval.value("bottom").toDouble(&bottomOk);
        valid = valid && topOk && bottomOk && std::isfinite(top) &&
                std::isfinite(bottom) && bottom > top &&
                (result.partialWellCoverage ? top >= last - 1e-6 && bottom <= rangeBase + 1e-6
                                            : std::abs(top - last) < 1e-6) &&
                codes.contains(interval.value("code").toInt());
        last = bottom;
      }
      valid =
          valid &&
          std::isfinite(rangeTop) && std::isfinite(rangeBase) && rangeBase > rangeTop &&
          (result.partialWellCoverage || std::abs(last - rangeBase) < 1e-6);
      for (const auto &key : {"log_version_id", "horizon", "depth_mock"})
        point.insert(key, key == QStringLiteral("horizon")
                              ? QVariant(request.horizon)
                              : key == QStringLiteral("depth_mock") && result.partialWellCoverage
                                    ? QVariant(false) : request.wells[i].toMap().value(key));
      point.insert("predicted_intervals", point.value("facies_intervals"));
      const auto attrs =
          FaciesCatalog::attributes(schema, point.value("facies_code"));
      for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
        point.insert(a.key(), a.value());
      points[i] = point;
    }
  // 智能预测的井点相 = 该井在本层位地层（tops：本层位顶 → 下一层位顶）内
  // 厚度最大的相；统计后仅发布每井每地层的一条预测相。不能预测的井保留 NULL。
  QVariantList formationRows, skippedIntervalRows;
  int skippedWells = 0;
  if (valid && request.kind == "wells") {
    emit predictionProgress(96);
    emit predictionDetailChanged(tr("统计当前地层的预测相"));
    if (m_task)
      m_task->reportDetail(tr("按地层统计每井的相并写入图件"));
    formationRows = wellFormationUpdates(points, schema, true,
                                         result.method, request.id);
    for (auto &value : points) {
      auto point = value.toMap();
      const auto intervals =
          QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8())
              .toVariant().toList();
      if (isSkipped(point)) {
        ++skippedWells;
        QVariantMap failedFormation;
        const auto frozen = QJsonDocument::fromJson(point.value("formation_intervals").toString().toUtf8()).toVariant().toList();
        for (const auto &v : frozen) {
          const auto f = v.toMap();
          if (f.value("name").toString().compare(request.horizon, Qt::CaseInsensitive) == 0)
            failedFormation = {{"well_id", point.value("id")}, {"formation", request.horizon},
                {"unit_type", f.value("unit_type")}, {"top_md", f.value("top")}, {"base_md", f.value("bottom")}};
        }
        // 缺少分层的井也清除工区中该层位的旧预测，不能沿用上次成功值。
        if (failedFormation.isEmpty()) {
          const auto old = WellAttributeStore::rows(m_dir, m_layers, WellAttributeStore::Table::Formations,
              point.value("id").toString(), request.horizon);
          if (!old.isEmpty()) failedFormation = old.first().toMap();
        }
        double top = 0, base = 0;
        FaciesCatalog::formationRange(failedFormation.value("top_md"), failedFormation.value("base_md"), &top, &base);
        if (std::isfinite(top) && std::isfinite(base) && base > top) {
          for (const auto &key : {"dominant_facies", "predicted_dominant", "facies_code", "dominant_ratio", "dominant_thickness_md", "facies_composition", "tie"})
            failedFormation.insert(key, QVariant());
          failedFormation.insert("thickness_md", base - top);
          failedFormation.insert("coverage", 0.0);
          failedFormation.insert("source", QStringLiteral("wells_prediction"));
          failedFormation.insert("model", QVariant());
          failedFormation.insert("job_id", request.id);
          failedFormation.insert("prediction_status", point.value("prediction_status"));
          failedFormation.insert("prediction_reason", point.value("prediction_reason"));
          formationRows << failedFormation;
          point.insert("formation_top", top);
          point.insert("formation_base", base);
        }
        auto oldIntervals = WellAttributeStore::rows(m_dir, m_layers, false,
            point.value("id").toString(), request.horizon);
        if (oldIntervals.isEmpty() && std::isfinite(top) && std::isfinite(base) && base > top)
          oldIntervals << QVariantMap{{"top_md", top}, {"base_md", base}};
        for (const auto &v : oldIntervals) {
          const auto old = v.toMap();
          skippedIntervalRows << QVariantMap{{"well_id", point.value("id")}, {"horizon", request.horizon},
              {"top_md", old.value("top_md")}, {"base_md", old.value("base_md")},
              {"predicted_facies", QVariant()}, {"model", QVariant()}, {"job_id", request.id},
              {"prediction_status", point.value("prediction_status")}, {"prediction_reason", point.value("prediction_reason")}};
        }
        value = point;
        continue;
      }
      QVariantMap formation;
      for (const auto &row : formationRows)
        if (row.toMap().value("well_id") == point.value("id") &&
            row.toMap().value("formation").toString().compare(
                request.horizon, Qt::CaseInsensitive) == 0)
          formation = row.toMap();
      double top = 0, base = 0;
      FaciesCatalog::formationRange(formation.value("top_md"),
                                    formation.value("base_md"), &top, &base);
      const int code = formation.isEmpty()
                           ? 0
                           : result.partialWellCoverage
                                 ? formation.value("facies_code").toInt()
                                 : FaciesCatalog::dominantCode(intervals, top, base);
      if (code) {
        point.insert("facies_code", code);
        point.insert("facies_scope", QStringLiteral("formation"));
        point.insert("formation", formation.value("formation"));
        point.insert("formation_top", top);
        point.insert("formation_base", base);
        point.insert("facies_ratio", formation.value("dominant_ratio"));
        if (result.partialWellCoverage)
          for (const auto &key : {"dominant_facies", "predicted_dominant", "dominant_thickness_md",
                                "dominant_ratio", "coverage", "tie", "facies_composition"})
            point.insert(key, formation.value(key));
        const auto attrs = FaciesCatalog::attributes(schema, code);
        for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
          point.insert(a.key(), a.value());
      } else {
        if (result.partialWellCoverage) valid = false;
        point.insert("facies_scope", QStringLiteral("well"));
      }
      value = point;
    }
  }
  if (valid && request.kind == "wells") {
    for (auto &point : points)
      point = FaciesCatalog::formationPrediction(point.toMap(), schema, request.horizon);
    for (auto &value : formationRows) {
      auto row = value.toMap();
      row.insert("facies_composition", QVariant());
      value = row;
    }
  }
  QString error;
  if (!valid)
    error = tr("预测服务返回的网格、井集合或相编码与请求不一致，未发布");
  const auto stageDir = QDir(m_dir).filePath("artifacts/staging/" + request.id);
  QDir().mkpath(stageDir);
  const auto path = QDir(stageDir).filePath(
      request.kind == "seismic" ? "prediction.tif" : "prediction.geojson");
  if (error.isEmpty()) {
    const bool ok = request.kind == "seismic"
                        ? MappingArtifactWriter::raster(
                              path, result.cells, request.columns, request.rows,
                              request.extent, &error)
                        : MappingArtifactWriter::points(path, points, &error);
    bool maintained = ok;
    if (ok && request.kind == "wells")
    {
      emit predictionProgress(97);
      emit predictionDetailChanged(tr("更新工区测井及地层预测信息"));
      maintained = WellAttributeStore::mergeIntervals(m_catalog, m_dir, m_layers,
          wellAttributeUpdates(points, schema, request.horizon, true, result.method, request.id) + skippedIntervalRows, &error) &&
        WellAttributeStore::mergeFormations(m_catalog, m_dir, m_layers,
          formationRows, &error);
    }
    // 首次预测刚落盘的属性/地层表立刻进图层列表（无需重开工程或切页）。
    if (ok && request.kind == "wells")
      ensureWellBaseLayers();
    if (maintained) {
      emit predictionProgress(99);
      emit predictionDetailChanged(tr("登记并显示预测图件"));
      auto parameters = m_predictionParameters;
      if (result.currentFormationOnly) {
        parameters.insert("result_scope", QStringLiteral("current_formation"));
        parameters.insert("result_kind", QStringLiteral("formation_prediction"));
      }
      if (request.kind == "wells") {
        parameters.insert("predicted_well_count", points.size() - skippedWells);
        parameters.insert("skipped_well_count", skippedWells);
        // facies-ux 5：支撑井与模型名进证据链（逐井：预测相 / 跳过原因）。
        QVariantList supporting, skipped;
        QStringList models;
        for (const auto &value : points) {
          const auto point = value.toMap();
          const auto model = point.value("prediction_model").toString();
          if (!model.isEmpty())
            for (const auto &name : model.split(QStringLiteral("；"), Qt::SkipEmptyParts))
              if (!models.contains(name))
                models << name;
          const QVariantMap well{{"id", point.value("id")},
                                 {"name", point.value("name")},
                                 {"facies_code", point.value("facies_code")},
                                 {"log_version_id", point.value("log_version_id")}};
          if (point.value("facies_code").isNull() ||
              point.value("prediction_status").toString() == QStringLiteral("未预测"))
            skipped << QVariantMap{{"id", point.value("id")},
                                   {"name", point.value("name")},
                                   {"reason", point.value("prediction_reason")}};
          else
            supporting << well;
        }
        parameters.insert("supporting_wells", supporting);
        parameters.insert("skipped_wells", skipped);
        if (!models.isEmpty())
          parameters.insert("model_names", models);
      }
      const QVariantMap common =
             {{"mock", result.mock},
              {"method", result.method},
              {"window_half_ms", result.method == QLatin1String("mock-horizon-window-kmeans-v1") ? 12.0 : 0.0},
              {"request_id", request.id},
              {"facies", schema},
              {"wells", request.wells},
              {"columns", request.columns},
              {"rows", request.rows},
              {"extent", QVariantList{request.extent.left(), request.extent.top(),
                                      request.extent.width(), request.extent.height()}}};
      for (auto it = common.cbegin(); it != common.cend(); ++it)
        parameters.insert(it.key(), it.value());
      auto sourceVersions = request.sourceVersionIds;
      if (request.kind == "wells" && result.partialWellCoverage)
        for (const auto &source : result.request.sourceVersionIds)
          if (!m_catalog->versionById(source).id.isEmpty()) sourceVersions << source;
      sourceVersions.removeDuplicates();
      record(path, request.horizon, request.kind + "_prediction",
             tr("%1 %2预测%3")
                 .arg(request.horizon,
                      request.kind == "seismic" ? tr("地震相")
                          : tr("测井相"),
                      result.mock ? tr("（Mock）") : QString()),
             request.kind == "seismic" ? "raster" : "vector",
             sourceVersions,
             parameters,
             &error);
    }
  }
  m_request = {};
  m_predictionParameters.clear();
  if (error.isEmpty()) {
    emit predictionDetailChanged(request.kind == "wells"
        ? tr("预测完成：成功 %1 口，未预测 %2 口").arg(points.size() - skippedWells).arg(skippedWells)
        : tr("预测完成"));
    emit predictionProgress(100);
  }
  endPredictionTask(error);
  emit predictionBusyChanged(false);
  if (!error.isEmpty())
    emit errorOccurred(error);
}
QString MappingWorkbench::ensureInputVersion(const QString &id,
                                             QString *error) {
  const auto existing = versionForLayer(id);
  if (!existing.id.isEmpty() && !id.startsWith("draft."))
    return existing.id;
  const auto d = declaration(id);
  if (d.layerId.isEmpty()) {
    fail(error, tr("未找到输入图件"));
    return {};
  }
  auto *layer = m_layers->instantiate(id, error);
  if (!layer)
    return {};
  QString path = filePath(d.source), suffix;
  if (auto *vl = qobject_cast<QgsVectorLayer *>(layer)) {
    QDir().mkpath(m_dir + "/artifacts/staging");
    path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
    if (!MappingArtifactWriter::vectorSnapshot(vl, path, error))
      return {};
    suffix = "|layername=features";
  }
  QStringList parents;
  if (!existing.id.isEmpty())
    parents << existing.id;
  auto snapshot =
      record(path, d.horizon, "input_snapshot",
             tr("输入快照 %1").arg(d.title.isEmpty() ? id : d.title), d.type,
             parents, {}, error, suffix, QStringLiteral("input-%1").arg(id));
  return versionForLayer(snapshot).id;
}
QString MappingWorkbench::copyForEditing(const QString &id,
                                         const QStringList &references,
                                         QString *error) {
  auto d = declaration(id);
  if (!ready(d.horizon, error))
    return {};
  if (d.type != "vector") {
    fail(error, tr("请先将相栅格转为矢量面，再创建编辑副本"));
    return {};
  }
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer)
    return {};
  if (layer->fields().indexOf("facies_code") < 0) {
    fail(error,
         tr("编辑底图需要 facies_code 相编码字段；请选择相面或预测相点"));
    return {};
  }
  QStringList parents;
  auto parent = ensureInputVersion(id, error);
  if (parent.isEmpty())
    return {};
  parents << parent;
  for (const auto &ref : references) {
    auto version = ensureInputVersion(ref, error);
    if (version.isEmpty())
      return {};
    parents << version;
  }
  const auto draft = QStringLiteral("draft.%1").arg(uid());
  const auto relative =
      QStringLiteral("artifacts/working/%1/features.gpkg").arg(draft);
  const auto path = QDir(m_dir).filePath(relative);
  QDir().mkpath(QFileInfo(path).absolutePath());
  if (!MappingArtifactWriter::vectorSnapshot(layer, path, error))
    return {};
  const auto source = m_catalog->versionById(parent);
  {
    QgsVectorLayer copy(path + "|layername=features", "copy", "ogr");
    if (!MappingArtifactWriter::syncFaciesAttributes(
            &copy, source.extra.value("facies", facies(d.horizon)).toList(),
            error))
      return {};
  }
  const QVariantMap extra{
      {"draft_id", draft},
      {"working_path", relative},
      {"facies", source.extra.value("facies", facies(d.horizon))},
      {"reference_layers", references},
      {"display_mode", displayMode(id)},
      {"hierarchy_model", "shared-polygon-partition-v1"},
      {"mock", source.extra.value("mock")},
      {"result_scope", layer->fields().indexOf("facies_intervals") >= 0 ? QStringLiteral("current_formation") : source.extra.value("result_scope").toString()},
      {"result_kind", layer->fields().indexOf("facies_intervals") >= 0 ? QStringLiteral("formation_prediction") : source.extra.value("result_kind").toString()},
      {"method", "manual-edit-copy"}};
  const auto editTitle = layer->fields().indexOf("facies_intervals") >= 0
                             ? tr("%1 测井预测相修订").arg(d.horizon)
                             : tr("%1 相图修订").arg(d.horizon);
  const auto snapshot =
      record(path, d.horizon, "edited_facies", editTitle, "vector", parents,
             extra, error, "|layername=features", draft);
  if (snapshot.isEmpty())
    return {};
  d.layerId = draft;
  d.source = path + "|layername=features";
  d.title = editTitle + tr(" · 工作副本");
  d.group = "05_PaleoMap";
  if (!m_layers->declare(d, error))
    return {};
  emit productReady(d.horizon, draft);
  return draft;
}
bool MappingWorkbench::saveEditingVersion(const QString &id, QString *error) {
  if (!id.startsWith("draft.")) {
    fail(error, tr("只有人工编辑副本可以保存编辑版本"));
    return false;
  }
  const auto d = declaration(id);
  if (!ready(d.horizon, error))
    return false;
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer)
    return false;
  if (layer->isEditable()) {
    fail(error, tr("请先点击“保存编辑”提交当前编辑，再保存图件版本"));
    return false;
  }
  if (!FaciesHierarchyRenderer::validateTopology(layer, error))
    return false;
  const auto previous = versionForLayer(id);
  if (previous.id.isEmpty()) {
    fail(error, tr("找不到编辑副本的来源版本"));
    return false;
  }
  auto features = layer->getFeatures();
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    bool ok = false;
    const double code = feature.attribute("facies_code").toDouble(&ok);
    if ((!feature.attribute("facies_code").isNull() &&
         (!ok || !std::isfinite(code) || code < 1 || code > 32767 ||
          code != std::round(code))) ||
        feature.geometry().isEmpty() || !feature.geometry().isGeosValid()) {
      fail(error, tr("工作副本含空几何、无效几何或非法相编码。请修正属性 / "
                     "几何后再保存图件版本；历史版本未改变。"));
      return false;
    }
  }
  if (!MappingArtifactWriter::syncFaciesAttributes(
          layer, previous.extra.value("facies").toList(), error))
    return false;
  auto extra = previous.extra;
  extra.insert("method", "manual-edit-save");
  extra.insert("display_mode", displayMode(id));
  extra.insert("hierarchy_model", "shared-polygon-partition-v1");
  QStringList evidenceParents{previous.id};
  auto evidenceFeatures = layer->getFeatures();
  QgsFeature evidenceFeature;
  while (evidenceFeatures.nextFeature(evidenceFeature)) {
    if (layer->fields().indexOf("facies_evidence") < 0)
      break;
    for (const auto &entry :
         QJsonDocument::fromJson(
             evidenceFeature.attribute("facies_evidence").toString().toUtf8())
             .toVariant()
             .toList()) {
      const auto source = entry.toMap().value("source_version").toString();
      if (!source.isEmpty() && !evidenceParents.contains(source))
        evidenceParents << source;
    }
  }
  if (layer->fields().indexOf("facies_intervals") >= 0) {
    for (const auto &id : {WellAttributeStore::intervalLayerId(), WellAttributeStore::formationLayerId()}) {
      auto *attributes = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
      if (attributes && attributes->isEditable()) { fail(error, tr("测井属性表正在编辑，请先保存或放弃编辑")); return false; }
    }
  }
  const auto snapshotPath = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(layer, snapshotPath, error))
    return false;
  const auto output =
      record(snapshotPath, d.horizon, "edited_facies",
             previous.extra.value("title").toString(), "vector",
             evidenceParents, extra, error, "|layername=features", id);
  if (!output.isEmpty()) {
    styleLayer(id);
    if (layer->fields().indexOf("facies_intervals") >= 0) {
      QVariantList points;
      auto it = layer->getFeatures(); QgsFeature feature;
      while (it.nextFeature(feature)) {
        QVariantMap point;
        for (const auto &field : {"id", "facies_intervals", "depth_mock", "formation_intervals"})
          if (layer->fields().indexOf(field) >= 0) point.insert(field, feature.attribute(field));
        points << point;
      }
      if (!WellAttributeStore::mergeIntervals(m_catalog, m_dir, m_layers,
          wellAttributeUpdates(points, previous.extra.value("facies").toList(), d.horizon, false), error) ||
          !WellAttributeStore::mergeFormations(m_catalog, m_dir, m_layers,
          wellFormationUpdates(points, previous.extra.value("facies").toList(), false), error)) return false;
    }
  }
  return !output.isEmpty();
}
QVariantList MappingWorkbench::removalProducts(const QString &layerId, QString *error) const {
  if (!layerId.startsWith("product.") && !layerId.startsWith("draft.")) {
    fail(error, tr("只有生成的图件与编辑副本可在此删除"));
    return {};
  }
  if (!m_catalog || !m_layers) {
    fail(error, tr("请先打开可写工程"));
    return {};
  }
  const auto target = versionForLayer(layerId);
  if (target.assetId.isEmpty()) {
    fail(error, tr("所选图件尚未登记到数据管理"));
    return {};
  }
  // 一次建反向谱系，按资产广度遍历；每条版本边只处理一次。
  QHash<QString, QVector<CatalogVersion>> childrenByAsset;
  for (const auto &child : m_catalog->versions()) {
    QSet<QString> parents;
    for (const auto &parent : child.parentVersionIds) {
      const auto asset = m_catalog->versionById(parent).assetId;
      if (!asset.isEmpty()) parents.insert(asset);
    }
    for (const auto &asset : parents) childrenByAsset[asset] << child;
  }
  QStringList assets{target.assetId};
  QSet<QString> selected{target.assetId};
  for (qsizetype i = 0; i < assets.size(); ++i)
    for (const auto &child : childrenByAsset.value(assets[i])) {
      if (selected.contains(child.assetId)) continue;
      if (!child.extra.value("mapping_product").toBool()) {
        fail(error, tr("图件被数据管理中的「%1」引用，请先处理该派生资产")
                        .arg(m_catalog->assetById(child.assetId).displayName));
        return {};
      }
      selected.insert(child.assetId);
      assets << child.assetId;
    }
  QVariantList products;
  for (const auto &asset : assets) {
    const auto version = m_catalog->currentVersion(asset);
    products << QVariantMap{{"asset_id", asset},
      {"title", version.extra.value("title", m_catalog->assetById(asset).displayName)},
      {"versions", m_catalog->versionsForAsset(asset).size()}};
  }
  return products;
}
bool MappingWorkbench::removeProduct(const QString &layerId, QString *error, bool includeDerived) {
  if (m_catalog && CatalogPurgeLease::isHeld(m_catalog->catalogPath())) {
    fail(error, tr("受管文件正在清理，请完成后再删除图件"));
    return false;
  }
  const auto products = removalProducts(layerId, error);
  if (products.isEmpty()) return false;
  if (products.size() > 1 && !includeDerived) {
    fail(error, tr("下游图件「%1」以它为来源，请确认同时删除关联派生图件")
                    .arg(products[1].toMap().value("title").toString()));
    return false;
  }
  QStringList assetIds, workingPaths;
  for (const auto &product : products) {
    const auto assetId = product.toMap().value("asset_id").toString();
    assetIds << assetId;
    for (const auto &version : m_catalog->versionsForAsset(assetId)) {
      const auto working = version.extra.value("working_path").toString();
      CatalogVersion check; check.path = working; check.managed = true;
      const auto resolved = DataCatalog::resolvedVersionPath(m_dir, check);
      if (!resolved.isEmpty() && QDir::cleanPath(resolved).startsWith(
              QDir(m_dir).absoluteFilePath("artifacts/working/")))
        workingPaths << QFileInfo(resolved).absolutePath();
    }
  }
  // 同资产的全部声明一并下架（同一资产多版本各自成行）。
  QVector<LayerDeclaration> declared;
  if (!m_layers->tryDeclared(&declared, error)) return false;
  QVector<LayerDeclaration> declarations;
  QStringList declIds;
  const QSet<QString> selectedAssets(assetIds.begin(), assetIds.end());
  for (const auto &d : declared)
    if (selectedAssets.contains(versionForLayer(d.layerId).assetId)) {
      declarations << d;
      declIds << d.layerId;
    }
  // 编辑中的实例不可销毁（removeDeclaration 自身也拦，前置校验让整组
  // 声明要么全下架要么不动）。
  const QSet<QString> selectedDeclarations(declIds.begin(), declIds.end());
  if (auto *project = m_project ? m_project->project() : nullptr)
    for (auto *layer : project->mapLayers())
      if (selectedDeclarations.contains(
              layer->customProperty("paleoLayerId").toString()))
        if (auto *vector = qobject_cast<QgsVectorLayer *>(layer);
            vector && vector->isEditable()) {
          fail(error, tr("请先保存或取消该图层的编辑，再删除图件"));
          return false;
        }
  // 先下架声明、释放 provider，再原子删除整套资产；目录提交失败恢复
  // 声明，避免留下消失的列表项或下次打开又冒出的图件。
  const auto restoreDeclarations = [this, &declarations] {
    for (const auto &d : declarations) m_layers->declare(d);
    emit changed();
  };
  if (!m_layers->removeDeclarations(declIds, error)) return false;
  const auto outcome = paleo::assetops::purgeAssets(m_catalog, m_dir, assetIds, true, true);
  if (outcome.purgedAssetIds.size() != assetIds.size()) {
    restoreDeclarations();
    fail(error, outcome.failedAssets.isEmpty() ? tr("图件删除失败") : outcome.failedAssets.constFirst());
    return false;
  }
  // 主线程只提交目录并释放 QGIS provider。物理清理使用值快照，后台不访问活 catalog。
  if (layerId.startsWith("draft."))
    workingPaths << QDir(m_dir).filePath(QStringLiteral("artifacts/working/") + layerId);
  workingPaths.removeDuplicates();
  const auto projectDir = m_dir;
  const auto retained = m_catalog->versions();
  const auto lease = CatalogPurgeLease::acquire(m_catalog->catalogPath());
  if (!lease) {
    emit errorOccurred(tr("图件记录已删除，后台文件清理未启动，请检查残留文件"));
  } else {
    auto *cleanup = new QFutureWatcher<paleo::assetops::PurgeOutcome>(this);
    connect(cleanup, &QFutureWatcher<paleo::assetops::PurgeOutcome>::finished, this,
        [this, cleanup, projectDir] {
          const auto result = cleanup->result();
          cleanup->deleteLater();
          if (!result.leftoverFiles.isEmpty()) {
            const auto message = tr("图件记录已删除，以下受管文件未能移除：%1").arg(result.leftoverFiles.join("；"));
            if (m_dir == projectDir) emit errorOccurred(message);
            else qWarning("%s", qPrintable(message));
          }
        });
    const auto future = QtConcurrent::run(
        paleo::tasks::pool(m_taskSvc),
        [projectDir, paths = outcome.pendingFiles, retained, workingPaths,
         lease] {
          auto result =
              paleo::assetops::purgeManagedFiles(projectDir, paths, retained);
          for (const auto &working : workingPaths)
            if (QDir(working).exists() && !QDir(working).removeRecursively())
              result.leftoverFiles << working;
          return result;
        });
    paleo::tasks::track(
        m_taskSvc, cleanup, future, tr("回收已删除图件的文件"), false,
        [future] {
          return future.result().leftoverFiles.isEmpty()
                     ? QString()
                     : QStringLiteral("部分图件文件未能回收");
        },
        false);
    cleanup->setFuture(future);
  }
  emit changed();
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("图件已删除，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return true;
}
QString MappingWorkbench::snapshotConstraints(const QString &h,
                                              const QStringList &parents,
                                              QString *error) {
  if (!ready(h, error))
    return {};
  auto *store = m_constraints->constraintStore();
  if (!store || m_constraints->loadConstraints(h).isEmpty())
    return {};
  QString escaped = h;
  escaped.replace("'", "''");
  QgsVectorLayer layer(store->gpkgPath() +
                           "|layername=constraints|subset=horizon='" + escaped +
                           "'",
                       "constraints", "ogr");
  layer.setCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(&layer, path, error))
    return {};
  QStringList upstream = parents;
  const auto old =
      m_constraints
          ->property(("paleo.constraint.snapshot." + h).toUtf8().constData())
          .toString();
  if (!old.isEmpty())
    upstream
        << DerivedAssetRegistrar(m_catalog, m_dir).parentVersionIdsFor({old});
  const auto id =
      record(path, h, "constraint_snapshot", tr("%1 约束过程").arg(h), "vector",
             upstream, {{"method", "constraint-snapshot"}}, error,
             "|layername=features");
  if (!id.isEmpty())
    m_constraints->setProperty(
        ("paleo.constraint.snapshot." + h).toUtf8().constData(),
        filePath(declaration(id).source));
  return id;
}
bool MappingWorkbench::importConstraints(const QString &h, const QString &path,
                                         const QString &role, QString *error) {
  if (!ready(h, error))
    return false;
  // 与既有导入同一坐标门：约束几何必须落在本工程局部米制网格。
  {
    QgsVectorLayer probe(path, QStringLiteral("constraints"),
                         QStringLiteral("ogr"));
    if (probe.isValid() && !acceptsLocalGrid(probe.crs())) {
      fail(error, tr("约束线必须采用本工程的局部米制坐标；请先完成配准"));
      return false;
    }
  }
  const auto records =
      paleo::readConstraintImportFeatures(path, role, nullptr, error);
  if (records.isEmpty())
    return false;
  // Copy the entire source into a managed GPKG (including shapefile sidecars).
  QgsVectorLayer source(path, "constraints", "ogr");
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto input = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(&source, input, error))
    return false;
  const auto inputId =
      record(input, h, "constraint_source", tr("%1 导入约束").arg(h), "vector",
             {}, {{"source_file", path}, {"method", "constraint-import"}},
             error, "|layername=features");
  if (inputId.isEmpty())
    return false;
  QStringList created;
  bool ok = true;
  for (const auto &record : records) {
    QString cid;
    if (!m_constraints->addConstraint(h, record.wkt, record.type, -1, error,
                                      &cid, record.params)) {
      ok = false;
      break;
    }
    created << cid;
  }
  if (!ok) {
    if (auto *store = m_constraints->constraintStore())
      for (const auto &cid : created)
        store->remove(cid);
    m_constraints->loadConstraints(h);
    return false;
  }
  return !snapshotConstraints(h, {versionForLayer(inputId).id}, error)
              .isEmpty();
}

// 测区范围/成图边界导入（WS-B/WS-C）：面图层快照进 artifacts/layers/，
// 声明为 horizon 无关的 00_Data 共享层（图层树布局器置顶）。layerId 按
// 文件名稳定——同名重导是原位更新（declare upsert 换源重挂），不同名
// 边界并存供切换。
bool MappingWorkbench::importBoundaryLayer(const QString &path,
                                           QString *error) {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开可写工程"));
    return false;
  }
  QgsVectorLayer source(path, QStringLiteral("boundary"), QStringLiteral("ogr"));
  if (!source.isValid()) {
    fail(error, tr("边界文件无法读取：%1").arg(path));
    return false;
  }
  if (source.geometryType() != Qgis::GeometryType::Polygon) {
    fail(error, tr("测区边界必须是面图层（多边形）"));
    return false;
  }
  if (!acceptsLocalGrid(source.crs())) {
    fail(error, tr("测区边界必须采用本工程的局部米制坐标；请先完成配准"));
    return false;
  }
  QDir().mkpath(m_dir + "/artifacts/layers");
  const QString copy = QDir(m_dir).filePath(
      QStringLiteral("artifacts/layers/boundary-%1.gpkg").arg(uid().left(8)));
  if (!MappingArtifactWriter::vectorSnapshot(&source, copy, error))
    return false;

  QString stem = QFileInfo(path).completeBaseName();
  stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
               QStringLiteral("_"));
  LayerDeclaration decl;
  decl.layerId = QStringLiteral("boundary.%1").arg(
      stem.isEmpty() ? uid().left(8) : stem);
  decl.horizon.clear();          // 共享层——不随层位切换释放
  decl.type = QStringLiteral("vector");
  decl.source = copy;
  decl.group = QStringLiteral("00_Data");
  decl.title = QFileInfo(path).completeBaseName();
  if (decl.title.isEmpty())
    decl.title = tr("测区边界");
  if (!m_layers->declare(decl, error))
    return false;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layers->instantiate(decl.layerId, error));
  if (!layer)
    return false;
  QgisStyleService::applyBoundaryLayerStyle(layer);
  layer->triggerRepaint();
  emit changed();
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("边界已导入，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return true;
}

bool MappingWorkbench::generateFactor(const QString &h, const QString &factor,
                                      const QVariantMap &params,
                                      QString *error) {
  if (!ready(h, error))
    return false;
  if (!m_constraints->loadConstraints(h).isEmpty() &&
      snapshotConstraints(h, {}, error).isEmpty())
    return false;
  const auto points = params.value("pointsLayerId").toString();
  auto options = params;
  if (!points.isEmpty()) {
    auto parent = ensureInputVersion(points, error);
    if (parent.isEmpty())
      return false;
    options.insert("parentVersionIds", QStringList{parent});
    const auto snapshot =
        m_catalog->versionById(parent).extra.value("layer_id").toString();
    if (!snapshot.isEmpty())
      options.insert("pointsLayerId", snapshot);
  }
  // structural_idw 按格网分辨率成图，没有 cellSize 契约——跳过像元数闸门
  //（其域规模由边界多边形 + GRID_RESOLUTION 控制，引擎自限）。
  const bool structural =
      options.value("method").toString() == QLatin1String("structural_idw");
  if (!structural) {
    if (auto *samples = m_layers->instantiate(
            options.value("pointsLayerId").toString(), error)) {
      const auto extent = samples->extent();
      const double cell = options.value("cellSize", 100).toDouble();
      if (!std::isfinite(cell) || cell <= 0 ||
          std::ceil(std::max(extent.width() * 1.2, cell * 2) / cell) *
                  std::ceil(std::max(extent.height() * 1.2, cell * 2) / cell) >
              4 * 1024 * 1024) {
        fail(error,
             tr("单因素网格超过 400 万像元，请增大网格间距或缩小输入范围"));
        return false;
      }
    } else
      return false;
  }
  if (!m_constraints->generateFactor(h, factor, options, error))
    return false;
  if (m_taskSvc && m_constraints->factorBusy())
    return true;
  const auto id = QStringLiteral("factor.%1.%2").arg(h, factor);
  const auto v = versionForLayer(id);
  auto d = declaration(id);
  d.layerId = v.extra.value("layer_id").toString();
  d.title += tr(" · v%1").arg(v.versionNumber);
  if (!d.layerId.isEmpty())
    m_layers->declare(d, error);
  emit changed();
  emit productReady(h, d.layerId.isEmpty() ? id : d.layerId);
  if (m_project && !m_project->projectPath().isEmpty())
    m_project->writeProject();
  return true;
}
bool MappingWorkbench::generateContours(const QString &h, const QString &id,
                                        double interval, QString *error) {
  if (!ready(h, error) || declaration(id).horizon != h) {
    fail(error, tr("等值线输入必须属于当前层位"));
    return false;
  }
  const auto contourExtra = versionForLayer(id).extra;
  const auto contourKind = contourExtra.value("kind").toString();
  const auto contourSource = contourExtra.value("value_source").toString();
  if (paleo::singlefactor::rejectsQuantitativeUse(contourKind, contourSource)) {
    fail(error, tr("解释性制图成果不能当作分析场提取等值线"));
    return false;
  }
  if (contourKind != QLatin1String("single_factor_raster") &&
      !id.startsWith("factor.")) {
    fail(error, tr("请选择连续单因素栅格，类别相图不能生成等值线"));
    return false;
  }
  if (!m_constraints->generateContours(h, id, interval, error))
    return false;
  if (m_taskSvc && m_constraints->factorBusy())
    return true;
  const auto prefix = QStringLiteral("factor.%1.").arg(h);
  const auto key = id.startsWith(prefix) ? id.mid(prefix.size()) : id;
  const auto output = QStringLiteral("contours.%1.%2").arg(h, key);
  const auto v = versionForLayer(output);
  auto d = declaration(output);
  d.layerId = v.extra.value("layer_id").toString();
  d.title += tr(" · v%1").arg(v.versionNumber);
  if (!d.layerId.isEmpty())
    m_layers->declare(d, error);
  emit changed();
  emit productReady(h, d.layerId.isEmpty() ? output : d.layerId);
  if (m_project && !m_project->projectPath().isEmpty())
    m_project->writeProject();
  return true;
}
int MappingWorkbench::labelMode(const QString &id) const {
  // 只有「应用标注」显式选过才写工程条目；没选过按几何给默认（面只标相名，
  // 不带 ID 序号——2026-10-10 用户要求）。
  const int fallback = MappingArtifactWriter::defaultFaciesLabelMode(
      qobject_cast<QgsVectorLayer *>(m_layers ? m_layers->layer(id) : nullptr));
  return m_project && m_project->project()
             ? m_project->project()->readNumEntry("paleo/faciesLabels", id,
                                                  fallback)
             : fallback;
}
bool MappingWorkbench::setLabelMode(const QString &id, int mode,
                                    QString *error) {
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id));
  if (!layer || layer->fields().indexOf("facies_code") < 0 || mode < 0 ||
      mode > 3) {
    fail(error, tr("请选择井相点图或矢量相面；栅格请先转为相面。"));
    return false;
  }
  MappingArtifactWriter::applyFaciesLabels(layer, mode);
  if (m_project && m_project->project())
    m_project->project()->writeEntry("paleo/faciesLabels", id, mode);
  emit changed();
  return true;
}
void MappingWorkbench::styleLayer(const QString &id) {
  // 约束线图层（constraints.<层位>）按 type 语义分类渲染——方向线/打断线
  // /软边界等在图面可辨（版本目录不入册，mapping_product 分支管不着）。
  if (id.startsWith(QLatin1String("constraints."))) {
    if (auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id)))
      QgisStyleService::applyConstraintLayerStyle(v);
    return;
  }
  const auto v = versionForLayer(id);
  if (!v.extra.value("mapping_product").toBool())
    return;
  auto *layer = m_layers->layer(id);
  if (!layer)
    return;
  if (layer->name().contains(QStringLiteral("测井优势相")))
    layer->setName(QString(layer->name()).replace(QStringLiteral("测井优势相"), QStringLiteral("测井相")));
  layer->setCustomProperty("paleoAssetId", v.assetId);
  layer->setCustomProperty("paleoVersionId", v.id);
  const auto styledKind = v.extra.value("kind").toString();
  const auto styledSource = v.extra.value("value_source").toString();
  const bool analysisRaster =
      paleo::singlefactor::isAnalysisFactorRaster(styledKind, styledSource);
  const bool cartographic =
      paleo::singlefactor::rejectsQuantitativeUse(styledKind, styledSource);
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    vector->setReadOnly(!id.startsWith("draft."));
    // facies-ux 4：属性表里 facies_code 用相名称下拉（值映射到编码），
    // 地震相 / 测井相工作副本可直接在列表中改相（改动由 DraftAutosave 随时保存）。
    if (const int codeField = vector->fields().indexOf("facies_code"); codeField >= 0) {
      const auto schema = v.extra.value("facies").toList();
      if (!schema.isEmpty()) {
        QVariantList map;
        for (const auto &entry : schema) {
          const auto f = entry.toMap();
          map << QVariantMap{{QStringLiteral("%1 · %2").arg(f.value("code").toInt()).arg(f.value("name").toString()),
                              f.value("code").toInt()}};
        }
        vector->setEditorWidgetSetup(
            codeField, QgsEditorWidgetSetup(QStringLiteral("ValueMap"), {{QStringLiteral("map"), map}}));
      }
    }
    if (!vector->property("faciesSelectionAttached").toBool()) {
      vector->setProperty("faciesSelectionAttached", true);
      connect(vector, &QgsVectorLayer::selectionChanged, this,
              [this, id] { emit displayChanged(id); });
    }
    if (id.startsWith("draft.") &&
        !vector->property("faciesSyncAttached").toBool()) {
      vector->setProperty("faciesSyncAttached", true);
      vector->setCustomProperty("paleo/requireFaciesTopology",
                                vector->geometryType() ==
                                    Qgis::GeometryType::Polygon);
      connect(vector, &QgsVectorLayer::editCommandEnded, this, [this, id] {
        styleLayer(id);
        emit displayChanged(id);
      });
      connect(vector, &QgsVectorLayer::afterRollBack, this, [this, id] {
        styleLayer(id);
        emit displayChanged(id);
      });
      FaciesHierarchyRenderer::watchUndo(vector, this, [this, id] {
        if (m_layers->layer(id)) {
          styleLayer(id);
          emit displayChanged(id);
        }
      });
      connect(vector, &QgsVectorLayer::beforeCommitChanges, this,
              [this, vector, schema = v.extra.value("facies").toList()] {
                QString error;
                const bool ok =
                    FaciesHierarchyRenderer::validateTopology(vector, &error) &&
                    MappingArtifactWriter::syncFaciesAttributes(vector, schema,
                                                                &error);
                vector->setAllowCommit(ok);
                if (!ok)
                  emit errorOccurred(error);
              });
    }
  }
  if (analysisRaster || cartographic)
    FactorStyleWriter::applyTo(qobject_cast<QgsRasterLayer *>(layer),
                               v.extra.value("factor_id").toString());
  if (styledKind.startsWith(QLatin1String("constraint")) || analysisRaster ||
      cartographic || styledKind == QLatin1String("contour_lines") ||
      styledKind == QLatin1String("well_factor_points") ||
      styledKind == QLatin1String("single_factor_cartographic_contour")) {
    // 分析与制图绕行等值线共用双描边和数字 halo，兼容 ELEV/level。
    if ((styledKind == QLatin1String("contour_lines") ||
         styledKind == QLatin1String("single_factor_cartographic_contour")))
      if (auto *vector = qobject_cast<QgsVectorLayer *>(layer))
        QgisStyleService::applyContourLayerStyle(vector);
    return;
  }
  if (!layer->customProperty("paleo/faciesDisplayMode").isValid())
    layer->setCustomProperty("paleo/faciesDisplayMode",
                             v.extra.value("display_mode", "auto"));
  FaciesHierarchyRenderer::apply(layer, v.extra.value("facies").toList(),
                                 resolvedLevel(id));
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    MappingArtifactWriter::applyFaciesLabels(vector, labelMode(id));
    vector->setReadOnly(!id.startsWith("draft."));
  }
}

bool MappingWorkbench::prepareCompose(const QString &h, const QStringList &ids,
                                      const QVariantMap &params,
                                      ComposeJob &job, QString *error) {
  if (!ready(h, error))
    return false;
  if (ids.isEmpty()) {
    fail(error, tr("请选择编图输入，列表从上到下为优先级"));
    return false;
  }
  job.horizon = h;
  job.ids = ids;
  job.params = params;
  job.dir = m_dir;
  job.catalog = m_catalog;
  job.facies = facies(h);
  QVariantList sources;
  QStringList parents;
  QSet<QString> continuous;
  for (const auto &id : ids) {
    const auto d = declaration(id);
    if (d.horizon != h) {
      fail(error, tr("自动编图输入必须属于当前层位；其他层位可在参考窗口查看"));
      return false;
    }
    auto *layer = m_layers->instantiate(id, error);
    if (!layer)
      return false;
    auto version = ensureInputVersion(id, error);
    if (version.isEmpty())
      return false;
    parents << version;

    const auto v = m_catalog->versionById(version);
    const auto inputKind = v.extra.value("kind").toString();
    const auto inputSource = v.extra.value("value_source").toString();
    if (paleo::singlefactor::rejectsQuantitativeUse(inputKind, inputSource)) {
      fail(error, tr("解释性制图工作场不能参与连续融合、分相或厚度统计"));
      return false;
    }
    if (id.startsWith("factor.") || v.extra.contains("factor_id") ||
        paleo::singlefactor::isAnalysisFactorRaster(inputKind, inputSource))
      continuous.insert(id);
    else if (v.extra.contains("facies") &&
             v.extra.value("facies").toList() != facies(h)) {
      fail(error, tr("输入相分类与当前层位定义不同，请使用对应版本的相分类，或"
                     "仅作为参考"));
      return false;
    }
    const auto frozenId = v.extra.value("layer_id").toString();
    const auto frozen = frozenId.isEmpty() ? d : declaration(frozenId);
    sources << QVariantMap{{"source", frozen.source},
                           {"provider", layer->providerType()},
                           {"type", frozen.type},
                           {"crs", layer->crs().toWkt()},
                           {"continuous", continuous.contains(id)}};
  }
  QList<double> thresholds;
  for (const auto &v : params.value("thresholds").toList()) {
    bool ok;
    double n = v.toDouble(&ok);
    if (!ok) {
      fail(error, tr("分相阈值必须为数字"));
      return false;
    }
    thresholds << n;
  }
  if (schemaVersion(h).isEmpty() && !saveFacies(h, job.facies, error))
    return false;
  parents << schemaVersion(h);
  if (!QDir().mkpath(m_dir + "/artifacts/staging")) {
    fail(error, tr("无法创建暂存目录"));
    return false;
  }
  job.outputPath = m_dir + "/artifacts/staging/" + uid() + ".tif";
  job.sources = sources;
  job.parents = parents;
  job.thresholds = thresholds;
  return true;
}

bool MappingWorkbench::computeCompose(
    ComposeJob &job, const std::function<bool()> &cancel,
    const std::function<void(double)> &progress) {
  std::vector<std::unique_ptr<QgsMapLayer>> owned;
  QList<QgsMapLayer *> layers;
  QSet<QString> continuous;
  for (const auto &entry : job.sources) {
    if (cancel && cancel())
      return false;
    const auto source = entry.toMap();
    std::unique_ptr<QgsMapLayer> layer;
    if (source.value("type").toString() == "raster")
      layer = std::make_unique<QgsRasterLayer>(
          source.value("source").toString(), "compose", "gdal");
    else
      layer = std::make_unique<QgsVectorLayer>(
          source.value("source").toString(), "compose",
          source.value("provider", "ogr").toString());
    layer->setCrs(
        QgsCoordinateReferenceSystem::fromWkt(source.value("crs").toString()));
    if (source.value("continuous").toBool())
      continuous.insert(layer->id());
    layers << layer.get();
    owned.push_back(std::move(layer));
  }
  if (!MappingArtifactWriter::composeRaster(job.outputPath, layers, job.facies,
                                            job.thresholds, continuous,
                                            &job.error, cancel, progress))
    return false;
  job.fingerprint =
      DerivedFileFingerprint::read(job.outputPath, cancel, &job.error);
  return !job.fingerprint.sha256.isEmpty();
}

bool MappingWorkbench::commitCompose(ComposeJob &job, QString *error) {
  if (!job.error.isEmpty()) {
    fail(error, job.error);
    return false;
  }
  if (m_catalog != job.catalog || m_dir != job.dir) {
    fail(error, tr("工程已切换，综合编图结果已丢弃"));
    return false;
  }
  for (const auto &parent : job.parents)
    if (m_catalog->versionById(parent).id.isEmpty()) {
      fail(error, tr("输入已删除，综合编图结果已丢弃"));
      return false;
    }
  job.productId = record(
      job.outputPath, job.horizon, "composed_facies",
      tr("%1 综合相图").arg(job.horizon), "raster", job.parents,
      {{"method",
        "priority-first-valid; categorical-nearest; threshold-classification"},
       {"input_layers", job.ids},
       {"thresholds", job.params.value("thresholds")},
       {"facies", job.facies}},
      error, {}, {}, &job.fingerprint);
  return !job.productId.isEmpty();
}

QString MappingWorkbench::compose(const QString &h, const QStringList &ids,
                                  const QVariantMap &params, QString *error) {
  ComposeJob job;
  if (!prepareCompose(h, ids, params, job, error))
    return {};
  if (!computeCompose(job, {}, {}) || !commitCompose(job, error)) {
    QFile::remove(job.outputPath);
    if (!job.error.isEmpty())
      fail(error, job.error);
    return {};
  }
  return polygonize(job.productId, error);
}

bool MappingWorkbench::startCompose(const QString &h, const QStringList &ids,
                                    const QVariantMap &params, QString *error) {
  if (!m_taskSvc)
    return !compose(h, ids, params, error).isEmpty();
  using Runner = paleo::jobs::JobRunner<ComposeJob>;
  if (!m_composeRunner)
    m_composeRunner = std::make_unique<Runner>(this);
  m_composeRunner->setTaskService(m_taskSvc);
  if (m_composeRunner->busy()) {
    fail(error, tr("已有综合编图任务，请等待或在任务中心取消"));
    return false;
  }
  auto job = std::make_shared<ComposeJob>();
  if (!prepareCompose(h, ids, params, *job, error))
    return false;
  Runner::Callbacks cb;
  cb.compute = [](ComposeJob &j, const paleo::jobs::CancelFn &cancel,
                  const paleo::jobs::ProgressFn &report) {
    return computeCompose(j, cancel,
                          [report](double p) { report(p, "build"); });
  };
  cb.commit = [this](ComposeJob &j, QString *err) {
    if (!commitCompose(j, err)) {
      emit errorOccurred(err ? *err : j.error);
      return false;
    }
    QString polyError;
    if (!startPolygonize(j.productId, &polyError))
      emit errorOccurred(polyError);
    return true;
  };
  cb.cleanup = [](ComposeJob &j) { QFile::remove(j.outputPath); };
  return m_composeRunner->start(tr("%1 综合编图").arg(h), job, cb) != nullptr;
}

void MappingWorkbench::publishFactorAlias(const QString &h, const QString &id) {
  const auto v = versionForLayer(id);
  auto d = declaration(id);
  const auto alias = v.extra.value("layer_id").toString();
  if (!alias.isEmpty()) {
    d.layerId = alias;
    d.title += tr(" · v%1").arg(v.versionNumber);
  }
  QString error;
  if (!d.layerId.isEmpty() && !m_layers->declare(d, &error)) {
    emit errorOccurred(error);
    return;
  }
  emit changed();
  emit productReady(h, d.layerId.isEmpty() ? id : d.layerId);
}

MappingWorkbench::ComposeJob::~ComposeJob() {
  if (!outputPath.isEmpty())
    QFile::remove(outputPath);
}
