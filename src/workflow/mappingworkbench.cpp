// 层：功能
#include "mappingworkbench.h"
#include "mapversionvalidation_internal.h"
#include "../algorithms/faciesmapping/formationdominance.h"
#include "../catalog/purgelease.h"
#include "../domain/faciescatalog.h"
#include "../domain/faciestaxonomy.h"
#include "../domain/interpretationevidence.h"
#include "../domain/mappinghorizons.h"
#include "../domain/singlefactorrequest.h"
#include "../io/constraintstore.h"
#include "../io/lasparser.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/faciespartitioneditor.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/layervocabulary.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/mappingfeatureidentity.h"
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
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgscoordinatereferencesystem.h>
#include <qgseditorwidgetsetup.h>
#include <qgsgeometry.h>
#include <qgsfeaturerequest.h>
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
QString filePath(const QString &source) { return source.section('|', 0, 0); }
void rememberNativeFailure(QgsVectorLayer *layer, const QString &target, const QString &error) {
  auto failures = layer->property("paleo.evidenceEditFailures").toMap();
  if (error.isEmpty()) failures.remove(target); else failures.insert(target, error);
  layer->setProperty("paleo.evidenceEditFailures", failures.isEmpty() ? QVariant() : QVariant(failures));
  const auto reason = failures.isEmpty() ? QString() : failures.cbegin().value().toString();
  layer->setProperty("paleo.evidenceEditFailure", reason.isEmpty() ? QVariant() : QVariant(reason));
  layer->setAllowCommit(failures.isEmpty());
}

bool validWorkingEvidence(QgsVectorLayer *layer, const QString &horizon,
                          const DataCatalog *catalog, QString *error) {
  if (MapVersionEvidenceCommitProof::validFor(layer, horizon, catalog)) return true;
  QSet<QString> identities;
  QgsFeatureRequest request;
  QgsAttributeList attributes;
  for (const auto &field : {MappingFeatureIdentity::UidField, QStringLiteral("facies_evidence"), QStringLiteral("paleo_interpretation")})
    if (const int index = layer->fields().indexOf(field); index >= 0) attributes << index;
  request.setSubsetOfAttributes(attributes);
  request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  auto features = layer->getFeatures(request); QgsFeature feature;
  while (features.nextFeature(feature)) {
    const auto identity = MappingFeatureIdentity::uid(feature);
    if (identity.isEmpty() || identities.contains(identity)) {
      fail(error, QObject::tr("图元身份缺失或重复，不能提交；请修复工作副本或撤销修改")); return false;
    }
    identities.insert(identity);
    QVariantList records; QVariantMap interpretation;
    if (layer->fields().indexOf("facies_evidence") >= 0 &&
        !paleo::evidence::decodeRecords(feature.attribute("facies_evidence").toString(), &records, error)) return false;
    if (layer->fields().indexOf("paleo_interpretation") >= 0 &&
        !paleo::evidence::decodeInterpretation(feature.attribute("paleo_interpretation").toString(), &interpretation, error)) return false;
    if (!interpretation.isEmpty() &&
        (interpretation.value("feature_uid").toString() != identity ||
         interpretation.value("horizon").toString() != horizon)) {
      fail(error, QObject::tr("解释身份或层位与图元不一致，不能提交")); return false;
    }
    for (const auto &entry : records) {
      const auto row = entry.toMap();
      if (!row.value("feature_uid").toString().isEmpty() && row.value("feature_uid").toString() != identity) {
        fail(error, QObject::tr("证据关联指向其他图元，不能提交")); return false;
      }
      if (catalog && !row.value("unlinked", row.value("tombstone", false)).toBool()) {
        const auto source = row.value("source_version").toString();
        if (!source.isEmpty() && catalog->versionById(source).id.isEmpty()) {
          fail(error, QObject::tr("证据来源版本不存在：%1").arg(source)); return false;
        }
      }
    }
    const QVariantMap target{{"horizon", horizon}, {"feature_uid", identity}};
    for (const auto &finding : paleo::evidence::check(records, interpretation, target))
      if (finding.toMap().value("severity").toString() == QStringLiteral("Blocking")) {
        fail(error, finding.toMap().value("message").toString()); return false;
      }
  }
  return true;
}

// uid/isLocalGridCrs/acceptsLocalGrid/wellAttributeUpdates 随所属函数析出至
// mappingworkbench_{predict,edit}.cpp（同名内部助手，各 TU 私有副本）。
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
  // 发布的成果（单因素栅格/等值线）一律激活：补 product.<v> 不可变别名、
  // 发 productReady（上图 + 选中 + 提示）。不能按 factorBusy() 门控——约束页
  // 的等值线走 m_taskSvc 手工任务而非 runFactorWork，m_factorJobTask 从未
  // 设置，门控会把那条链的成果挡在成果列表外。
  connect(m_constraints, &ConstraintWorkflow::factorGenerated, this,
          [this](const QString &h, const QString &, const QString &id) {
            publishFactorAlias(h, id);
          });
  connect(m_constraints, &ConstraintWorkflow::contoursGenerated, this,
          [this](const QString &h, const QString &, const QString &id) {
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
  for (const auto &adapter : std::as_const(m_evidenceSpatialAdapters)) delete adapter.data();
  m_evidenceSpatialAdapters.clear();
  m_attributionLayers.clear(); // shared_ptr 缓存：清表即释放缓存引用
  m_attributionLru.clear();
  // G08：反向索引 sidecar 绑定工程目录——切工程必须重置，否则反查打到
  // 旧工程的索引（头文件注释承诺的行为）。
  m_evidenceIndex.reset();
  if (m_indexSyncRunner)
    m_indexSyncRunner->requestCancel();
  m_evidenceIndexSyncedRevision = -1;
  m_evidenceIndexAttemptedRevision = -1;
  m_evidenceIndexSyncError.clear();
  m_evidenceIndexStats.clear();
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
      if (e.value("kind") == "constraint_snapshot" &&
          m_catalog->currentVersion(a.id).id != v.id) {
        m_layers->removeDeclaration(d.layerId);
        continue;
      }
      declareMissing(d);
      if (e.value("kind") == "constraint_snapshot")
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
  if (!v.id.isEmpty() && !v.extra.value("mapping_product").toBool()) {
    // Reuse an already declared spatial RAW/reference layer whose exact
    // catalog version matches. Never guess by display name or force an editor.
    for (const auto &decl : m_layers->declared())
      if ((decl.type == "vector" || decl.type == "raster") && !decl.layerId.startsWith("draft.") && versionForLayer(decl.layerId).id == v.id)
        return decl.layerId;
    fail(error, tr("该来源版本尚未作为空间图层加载；可在数据管理查看或加载原始资料")); return {};
  }
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
  if (!id.isEmpty()) {
    const auto stored =
        m_catalog->versionById(id).extra.value("facies").toList();
    // 旧分类读出即迁移进三级体系（未细分/未归类），项目里不出现未定义相。
    QVariantList upgraded;
    if (FaciesTaxonomy::upgrade(stored, &upgraded)) return upgraded;
    return stored;
  }
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
  QVariantList normalized;
  if (!FaciesTaxonomy::normalize(items, &normalized, error)) return false;
  QSet<int> codes;
  for (const auto &v : normalized) {
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
      fail(error, tr("纹理不在本工程纹理库中"));
      return false;
    }
    codes.insert(code);
  }
  DerivedAssetRegistrar r(m_catalog, m_dir);
  auto st = r.stage("facies_schema", QStringLiteral("facies-schema-%1").arg(h),
                    "facies.json", error);
  if (!st.isValid() || !writeJson(st.absolutePath, normalized, error))
    return false;
  QStringList parents;
  if (!schemaVersion(h).isEmpty())
    parents << schemaVersion(h);
  const bool ok = r.commit(st, parents, "facies-schema",
                           {{"horizon", h}, {"facies", normalized}, {"facies_schema", "paleo_facies_hierarchy_v1"}, {"prediction_level", "micro_facies"}}, error);
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
                        {"version", v.id.isEmpty() ? 0 : v.versionNumber},
                        {"unregistered_product", v.id.isEmpty() && (d.layerId.startsWith("product.") || d.layerId.startsWith("draft."))},
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
  DataCatalog::BatchSave transaction(m_catalog);
  DerivedAssetRegistrar r(m_catalog, m_dir);
  auto st = r.stage(
      kind,
      assetKey.isEmpty() ? QStringLiteral("%1-%2").arg(h, kind) : assetKey,
      QStringLiteral("result.%1").arg(QFileInfo(path).suffix()), error);
  if (!st.isValid())
    return {};
  bool published = false;
  QString registeredPath = st.absolutePath;
  const auto cleanup = qScopeGuard([&] {
    if (!published) {
      transaction.abort();
      QFile::setPermissions(registeredPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      QFile::remove(registeredPath);
    }
  });
  const auto id = QStringLiteral("product.%1").arg(st.versionId);
  const auto group =
      kind.contains("prediction")   ? QStringLiteral("02_Prediction")
      : kind.contains("constraint") ? PaleoLayerVocabulary::kConstraintsGroup
                                    : QStringLiteral("05_PaleoMap");
  if (extra.contains("spatial_edit_events")) {
    auto events = extra.value("spatial_edit_events").toList();
    for (auto &entry : events) {
      auto event = entry.toMap();
      if (event.value("version_id").toString().isEmpty()) {
        event.insert("version_id", st.versionId); event.insert("after_revision", st.versionNumber);
        entry = event;
      }
    }
    extra.insert("spatial_edit_events", events);
  }
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
  const auto registered = m_catalog->versionById(st.versionId);
  registeredPath = DataCatalog::resolvedVersionPath(m_dir, registered);
  LayerDeclaration d;
  d.layerId = id;
  d.horizon = h;
  d.type = type;
  d.source = registeredPath + suffix;
  d.group = group;
  d.title = title + tr(" · v%1").arg(registered.versionNumber);
  if (!m_layers->declare(d, error)) return {};
  if (!transaction.flush(error)) {
    m_layers->removeDeclaration(id);
    return {};
  }
  published = true;
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
    auto tops = data.topsForMapping(wellId);
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
      const auto hierarchy = FaciesCatalog::attributes(schema, d.dominantCode);
      row.insert("facies", hierarchy.value("facies_name"));
      row.insert("sub_facies", hierarchy.value("subfacies"));
      row.insert("micro_facies", hierarchy.value("microfacies"));
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
  // 在途任务由 factorGenerated→publishFactorAlias 收尾；同步路径同一条
  // 信号链已在调用内发射，这里不再自行 declare/productReady（否则双发）。
  if (m_constraints->factorBusy())
    return true;
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
  // 在途任务由 contoursGenerated→publishFactorAlias 收尾；同步路径同一条
  // 信号链已在调用内发射，这里不再自行 declare/productReady（否则双发）。
  if (m_constraints->factorBusy())
    return true;
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
      mode > 4) {
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
    if (id.startsWith("draft.") && vector->geometryType() == Qgis::GeometryType::Polygon) {
      QString error;
      if (!FaciesPartitionEditor::install(vector, v.extra.value("facies").toList(),
                                         v.extra.value("partition_footprint_wkb").toString(), &error)) {
        vector->setAllowCommit(false); emit errorOccurred(error); return;
      }
    }
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
    if (id.startsWith("draft.") && vector->fields().indexOf(MappingFeatureIdentity::UidField) >= 0) {
      QString identityError;
      if (auto *observer = MappingFeatureIdentity::Observer::attach(vector, &identityError)) {
        if (observer->property("paleo.workbenchBinding").value<QObject *>() != this) {
          observer->setProperty("paleo.workbenchBinding", QVariant::fromValue(static_cast<QObject *>(this)));
          const QPointer<QgsVectorLayer> safeVector(vector);
          connect(this, &QObject::destroyed, observer, [observer] {
            observer->setProperty("paleo.workbenchBinding", QVariant());
          });
          connect(observer, &MappingFeatureIdentity::Observer::businessEdit, this,
                  [this, safeVector, id](qint64 fid, const QString &kind) {
            if (!safeVector) return;
            QString error;
            if (!recordSpatialInterpretationEdit(id, fid, kind, &error)) {
              rememberNativeFailure(safeVector, "feature:" + QString::number(fid), error);
              emit errorOccurred(error);
            } else {
              // A successful B cannot erase A's failed audit in the same command.
              rememberNativeFailure(safeVector, "feature:" + QString::number(fid), {});
            }
          });
          connect(observer, &MappingFeatureIdentity::Observer::failed, this,
                  [this, safeVector](const QString &reason) {
            if (safeVector) {
              rememberNativeFailure(safeVector, QStringLiteral("observer"), reason);
            }
            emit errorOccurred(reason);
          });
          connect(vector, &QgsVectorLayer::afterRollBack, this, [this, safeVector, id] {
            if (!safeVector) return;
            QString error;
            const bool valid = validWorkingEvidence(safeVector, declaration(id).horizon, m_catalog, &error);
            safeVector->setProperty("paleo.evidenceEditFailures", QVariant());
            safeVector->setProperty("paleo.evidenceEditFailure", QVariant());
            if (!valid) rememberNativeFailure(safeVector, QStringLiteral("rollback"), error);
            else safeVector->setAllowCommit(true);
          });
        }
      } else {
        vector->setAllowCommit(false); emit errorOccurred(identityError);
      }
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
        if (auto *current = qobject_cast<QgsVectorLayer *>(m_layers->layer(id))) {
          // Release only our own coverage veto after native undo/repair. A
          // rejected commit must not permanently lock a now-valid edit buffer.
          if (current->property("paleo.partitionCommitRejected").toBool() &&
              current->property("paleo.evidenceEditFailure").toString().isEmpty() &&
              FaciesHierarchyRenderer::validateTopology(current, nullptr)) {
            current->setProperty("paleo.partitionCommitRejected", QVariant());
            current->setAllowCommit(true);
          }
          styleLayer(id);
          emit displayChanged(id);
        }
      });
      connect(vector, &QgsVectorLayer::beforeCommitChanges, this,
              [this, vector, id, schema = v.extra.value("facies").toList()] {
                const bool previouslyAllowed = vector->allowCommit();
                QString error = vector->property("paleo.evidenceEditFailure").toString();
                const bool evidenceOk = previouslyAllowed && error.isEmpty() &&
                    validWorkingEvidence(vector, declaration(id).horizon, m_catalog, &error);
                const bool topologyOk = evidenceOk && FaciesHierarchyRenderer::validateTopology(vector, &error);
                if (evidenceOk) vector->setProperty("paleo.partitionCommitRejected", !topologyOk);
                const bool ok = topologyOk &&
                    MappingArtifactWriter::syncFaciesAttributes(vector, schema, &error) &&
                    validWorkingEvidence(vector, declaration(id).horizon, m_catalog, &error);
                // Never override a rejection from an earlier beforeCommit consumer.
                vector->setAllowCommit(ok && vector->allowCommit());
                if (!ok && error.isEmpty()) error = tr("图层提交已被证据或图元身份校验拒绝，请修复或撤销修改");
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
