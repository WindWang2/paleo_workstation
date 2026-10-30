// 层：功能
#include "evidenceinteraction.h"
#include "linkage/unitselectionbridge.h"
#include "mappingworkbench.h"
#include "../qgis/evidencemapadapter.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/mappingfeatureidentity.h"
#include <QScopedValueRollback>
#include <QTimer>
#include <QJsonDocument>
#include <qgsproject.h>
#include <qgslayertree.h>
#include <qgsvectorlayer.h>

EvidenceInteraction::EvidenceInteraction(MappingWorkbench *workbench, SelectionContext *selection,
    QgisLayerService *layers, QgisProjectService *project, QgisCanvasController *canvas, QObject *parent)
    : QObject(parent), m_workbench(workbench), m_selection(selection), m_layers(layers), m_project(project), m_canvas(canvas),
      m_primary(new EvidenceMapAdapter(canvas ? canvas->canvas() : nullptr, this)),
      m_source(new EvidenceMapAdapter(canvas ? canvas->canvas() : nullptr, this)) {
  if (selection) {
    connect(selection, &SelectionContext::targetChanged, this, &EvidenceInteraction::receive);
    connect(selection, &SelectionContext::activeHorizonChanged, this, [this] {
      m_back.clear(); m_lastPrimary = SelectionTarget(); emit historyChanged(false);
      m_source->clear(); m_primary->clear(); refresh();
    });
    connect(selection, &SelectionContext::targetChanged, this, [this](const SelectionTarget &target, const QString &origin, quint64) {
      // 回退栈只收主研究目标的变更：来源高亮（evidence）、快照刷新、会话
      // 恢复与 back 自身都不构成新的研究目标。
      static const QSet<QString> noPush{QStringLiteral("refresh"), QStringLiteral("restore"),
          QStringLiteral("project"), QStringLiteral("horizon"), QStringLiteral("back"), QStringLiteral("evidence")};
      const bool was = canGoBack();
      if (!noPush.contains(origin) && !m_lastPrimary.isEmpty() &&
          (m_lastPrimary.layerId != target.layerId ||
           m_lastPrimary.versionId != target.versionId ||
           m_lastPrimary.featureUid != target.featureUid)) {
        m_back.append(m_lastPrimary);
        if (m_back.size() > 32) m_back.removeFirst();
      }
      m_lastPrimary = target;
      if (canGoBack() != was) emit historyChanged(canGoBack());
    });
    connect(selection, &SelectionContext::targetChanged, this, [this](const SelectionTarget &target, const QString &origin, quint64) {
      if (!m_project || !m_project->project() || target.isEmpty() || target.projectId != projectId() || origin == "project" || origin == "horizon" || origin == "restore") return;
      // Display selection belongs to the QGIS project, not the evidence audit.
      const QVariantMap saved{{"project_id", target.projectId}, {"horizon", target.horizon}, {"layer_id", target.layerId},
        {"asset_id", target.assetId}, {"version_id", target.versionId}, {"feature_uid", target.featureUid},
        {"evidence_id", target.evidenceId}, {"assertion_id", target.assertionId}, {"interpretation_id", target.interpretationId}, {"revision", target.revision}};
      m_project->project()->writeEntry("paleo/evidenceTarget", "selection", QString::fromUtf8(QJsonDocument::fromVariant(saved).toJson(QJsonDocument::Compact)));
    });
  }
  if (project) {
    connect(project, &QgisProjectService::projectAboutToClose, this, &EvidenceInteraction::reset);
    connect(project, &QgisProjectService::projectOpened, this, [this] {
      const auto session = m_project->sessionId();
      QTimer::singleShot(0, this, [this, session] {
        if (!m_project || !m_selection || !m_workbench || session != m_project->sessionId()) return;
        // Adopted qgz layers already exist and do not emit layerInstantiated.
        if (m_layers) for (const auto &declaration : m_layers->declared()) observe(declaration.layerId);
        const auto json = m_project->project()->readEntry("paleo/evidenceTarget", "selection");
        const auto saved = QJsonDocument::fromJson(json.toUtf8()).toVariant().toMap();
        if (saved.value("project_id").toString() != projectId())
          return;
        // #493 后 reset() 在关工程时连 activeHorizon 一起清空（工程作用域），
        // 重开恢复时层位为空应采纳保存值——否则同工程重开永远恢复失败
        //（tst_mappingworkbench mapEvidenceAndGraphUseOneStableTarget 回归）。
        const auto savedHorizon = saved.value("horizon").toString();
        if (!savedHorizon.isEmpty() && savedHorizon != m_selection->activeHorizon()) {
          if (!m_selection->activeHorizon().isEmpty())
            return; // 已有不同层位：以当前上下文为准，不覆盖
          m_selection->setActiveHorizon(savedHorizon);
        }
        const auto layer = saved.value("layer_id").toString();
        if (layer.isEmpty() || m_workbench->versionForLayer(layer).id != saved.value("version_id").toString()) return;
        SelectionTarget target;
        target.projectId = projectId(); target.horizon = saved.value("horizon").toString(); target.layerId = layer;
        target.assetId = saved.value("asset_id").toString(); target.versionId = saved.value("version_id").toString();
        target.featureUid = saved.value("feature_uid").toString(); target.evidenceId = saved.value("evidence_id").toString();
        target.assertionId = saved.value("assertion_id").toString(); target.interpretationId = saved.value("interpretation_id").toString();
        target.revision = saved.value("revision").toULongLong();
        QString error;
        if (!m_layers || !m_layers->instantiate(layer, &error)) { emit navigationFailed(error); return; }
        m_selection->setTarget(target, "restore");
      });
    });
  }
  if (layers) {
    connect(layers, &QgisLayerService::layerInstantiated, this, [this](const QString &id) {
      observe(id);
      if (m_selection && m_selection->target().layerId == id) receive(m_selection->target(), "restore", m_selection->targetGeneration());
    });
    for (const auto &declaration : layers->declared()) observe(declaration.layerId);
  }
  if (workbench) {
    connect(workbench, &MappingWorkbench::catalogBound, this, &EvidenceInteraction::reset);
    connect(workbench, &MappingWorkbench::faciesEdited, this, [this](const QString &id) {
      if (m_selection && m_selection->target().layerId == id) refresh();
    });
    connect(workbench, &MappingWorkbench::displayChanged, this, [this](const QString &id) {
      if (m_selection && m_selection->target().layerId == id && !m_receiving) refresh();
    });
  }
}
QString EvidenceInteraction::projectId() const {
  return m_project ? m_project->mapConfiguration().projectId : QString();
}
void EvidenceInteraction::reset() {
  m_primary->clear(); m_source->clear(); m_snapshot.clear();
  m_back.clear(); m_lastPrimary = SelectionTarget();
  emit historyChanged(false);
  if (m_selection) {
    m_selection->clearTarget("project");
    // opt-02：选择状态是工程作用域——ids 与 activeHorizon 一并复位。旧实
    // 现只清 target：换工程后残留旧井 id（撞名时新工程剖面面板凭空高亮
    // 旧井、井对话框预选旧集）与旧层位名（保存剖面把旧工程层位写进新工
    // 程），且与 setSelection 等值短路复合后重选同集不再广播。
    m_selection->setActiveHorizon(QString());
    m_selection->clear("project");
  }
  emit detailsChanged({}, m_selection ? m_selection->targetGeneration() : 0);
}
void EvidenceInteraction::observe(const QString &id) {
  if (!m_layers) return;
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  if (!layer || layer->fields().indexOf("facies_code") < 0) return;
  // Connection ownership belongs to this controller, not a persistent layer
  // property: a second application/session may adopt the same QGIS layer.
  const auto key = QStringLiteral("evidenceObserver:%1").arg(quintptr(this));
  if (layer->property(key.toUtf8().constData()).toBool()) return;
  layer->setProperty(key.toUtf8().constData(), true);
  connect(layer, &QgsVectorLayer::selectionChanged, this, [this, id] { capture(id); });
  connect(layer, &QgsVectorLayer::destroyed, this, [this, id, layer] {
    // An old provider can finish teardown after its replacement is adopted.
    // Its terminal signal must not clear the replacement target.
    if (m_layers && m_layers->layer(id) && m_layers->layer(id) != layer) return;
    if (m_selection && m_selection->target().layerId == id) {
      m_primary->clear(); m_source->clear(); m_snapshot.clear();
      emit detailsChanged({{"layer_id", id}, {"message", tr("图层已卸载；恢复图层后将按稳定身份重新定位")}}, m_selection->targetGeneration());
    }
  });
}
void EvidenceInteraction::setLayer(const QString &id) {
  if (!m_workbench || !m_selection || !m_layers || id.isEmpty()) return;
  const auto d = m_workbench->declaration(id);
  if (!d.horizon.isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, d.horizon)) return;
  observe(id); capture(id);
}
void EvidenceInteraction::capture(const QString &id) {
  if (m_receiving || !m_workbench || !m_selection) return;
  QScopedValueRollback<bool> guard(m_receiving, true);
  const auto h = m_workbench->declaration(id).horizon;
  if (!h.isEmpty() && h != m_selection->activeHorizon()) return;
  const auto ids = m_workbench->selectedFeatures(id);
  m_snapshot = m_detailsEnabled ? m_workbench->featureEvidence(id, ids) : m_workbench->featureEvidenceSummary(id, ids);
  if (ids.size() > 1) m_snapshot.insert("records", m_workbench->evidence(id, ids));
  m_snapshot.insert("project_id", projectId());
  m_snapshot.insert("project_session", m_project ? QVariant(m_project->sessionId()) : QVariant());
  SelectionTarget target;
  target.projectId = projectId(); target.horizon = h; target.layerId = id;
  target.assetId = m_snapshot.value("asset_id").toString(); target.versionId = m_snapshot.value("version_id").toString();
  target.featureUid = m_snapshot.value("feature_uid").toString();
  const auto interpretation = m_snapshot.value("interpretation").toMap();
  target.interpretationId = interpretation.value("id").toString(); target.assertionId = interpretation.value("assertion_id").toString();
  target.revision = interpretation.value("revision").toULongLong();
  m_selection->setTarget(target, "map");
  // Native selection now owns the target. A graph/source rubber band must not
  // continue displaying a different feature on this same provider.
  m_primary->clear(); m_source->clear();
  if (m_layers) m_primary->bindLayer(qobject_cast<QgsVectorLayer *>(m_layers->layer(id)));
  emit detailsChanged(m_snapshot, m_selection->targetGeneration());
}
void EvidenceInteraction::receive(const SelectionTarget &target, const QString &origin, quint64 generation) {
  if (m_receiving || !m_workbench || !m_selection || !m_layers) return;
  if (target.isEmpty()) { m_snapshot.clear(); m_primary->clear(); m_source->clear(); emit detailsChanged({}, generation); return; }
  if (target.projectId != projectId() || (!target.horizon.isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, target.horizon))) return;
  QScopedValueRollback<bool> guard(m_receiving, true);
  if (target.layerId != m_snapshot.value("layer_id").toString() ||
      target.featureUid != m_snapshot.value("feature_uid").toString()) m_source->clear();
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(target.layerId));
  QList<qint64> ids;
  if (!layer || target.featureUid.isEmpty()) m_primary->clear();
  if (layer && !target.featureUid.isEmpty()) {
    m_primary->bindLayer(layer); QString error;
    const auto fid = m_primary->featureId(target.featureUid, &error);
    if (fid == FID_NULL) {
      m_primary->clear();
      m_snapshot = {{"project_id", target.projectId}, {"layer_id", target.layerId}, {"version_id", target.versionId},
        {"feature_uid", target.featureUid}, {"horizon", target.horizon}, {"error", error}, {"missing_feature", true}};
      emit detailsChanged(m_snapshot, generation); emit navigationFailed(error); return;
    }
    ids << fid;
    if (origin != "map" && !m_primary->showFeature(target.featureUid, false, &error)) emit navigationFailed(error);
  }
  m_snapshot = m_detailsEnabled ? m_workbench->featureEvidence(target.layerId, ids) : m_workbench->featureEvidenceSummary(target.layerId, ids);
  m_snapshot.insert("project_id", projectId()); m_snapshot.insert("project_session", m_project ? QVariant(m_project->sessionId()) : QVariant());
  if (!m_snapshot.contains("error") && layer) {
    auto current = target;
    current.assetId = m_snapshot.value("asset_id").toString(); current.versionId = m_snapshot.value("version_id").toString();
    const auto interpretation = m_snapshot.value("interpretation").toMap();
    current.interpretationId = interpretation.value("id").toString(); current.assertionId = interpretation.value("assertion_id").toString();
    current.revision = interpretation.value("revision", 0).toULongLong();
    m_selection->setTarget(current, origin == "restore" ? origin : QStringLiteral("refresh"));
    generation = m_selection->targetGeneration();
  }
  emit detailsChanged(m_snapshot, generation);
}
void EvidenceInteraction::refresh() {
  if (!m_selection || m_selection->target().isEmpty()) return;
  receive(m_selection->target(), "refresh", m_selection->targetGeneration());
}
void EvidenceInteraction::setDetailsEnabled(bool enabled) {
  if (m_detailsEnabled == enabled) return;
  m_detailsEnabled = enabled;
  refresh();
}
void EvidenceInteraction::navigate(const QVariantMap &evidence, bool locate) {
  if (!m_selection || !m_workbench || !m_layers) return;
  // A view captured in another project/session must not navigate reused IDs.
  if (evidence.contains("project_id") && evidence.value("project_id").toString() != projectId()) { emit navigationFailed(tr("证据属于其他工程")); return; }
  if (evidence.contains("project_session") && m_project && evidence.value("project_session").toULongLong() != m_project->sessionId()) { emit navigationFailed(tr("证据查询已过期，请刷新")); return; }
  const auto h = evidence.value("source_horizon", evidence.value("horizon")).toString();
  // 稳定 ID / 分析令牌：禁止仅按 horizon 显示名字符串相等。
  if (!h.isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, h)) { emit navigationFailed(tr("来源属于其他层位，请先切换层位")); return; }
  const auto entity = evidence.value("entity_id").toString();
  if (!entity.isEmpty() && evidence.value("has_coordinates").toBool()) {
    QString error;
    if (!m_source->showPoint(evidence.value("x").toDouble(), evidence.value("y").toDouble(), DataCatalog::localGridCrsWkt(), locate, &error)) { emit navigationFailed(error); return; }
    m_selection->setSelection({entity}, "evidence_well");
    auto selected = m_selection->target(); selected.evidenceId = evidence.value("evidence_id", evidence.value("id")).toString();
    m_selection->setTarget(selected, "evidence"); return;
  }
  const auto sourceVersion = evidence.value("source_version", evidence.value("version_id")).toString();
  QString error;
  QString layerId = evidence.value("source_layer").toString();
  if (!sourceVersion.isEmpty()) layerId = m_workbench->layerForVersion(sourceVersion, &error);
  if (layerId.isEmpty()) { emit navigationFailed(error.isEmpty() ? tr("该证据没有可定位的已保存空间来源；可从数据管理查看原始资料") : error); return; }
  auto *layer = m_layers->instantiate(layerId, &error);
  if (!layer) { emit navigationFailed(error); return; }
  if (m_project && m_project->project())
    if (auto *node = m_project->project()->layerTreeRoot()->findLayer(layer->id())) node->setItemVisibilityChecked(true);
  const auto uid = evidence.value("source_feature_uid").toString();
  const bool ok = !uid.isEmpty() && qobject_cast<QgsVectorLayer *>(layer)
      ? (m_source->bindLayer(qobject_cast<QgsVectorLayer *>(layer)), m_source->showFeature(uid, locate, &error))
      : m_source->showLayer(layer, locate, &error);
  if (!ok) { emit navigationFailed(error); return; }
  auto selected = m_selection->target();
  selected.evidenceId = evidence.value("evidence_id", evidence.value("id")).toString();
  m_selection->setTarget(selected, "evidence");
}
void EvidenceInteraction::navigateFeature(const QVariantMap &hit) {
  if (!m_selection || !m_workbench || !m_layers) return;
  if (hit.contains("project_id") && hit.value("project_id").toString() != projectId()) { emit navigationFailed(tr("命中图斑属于其他工程")); return; }
  if (hit.contains("project_session") && m_project && hit.value("project_session").toULongLong() != m_project->sessionId()) { emit navigationFailed(tr("反查结果已过期，请刷新")); return; }
  const auto h = hit.value("horizon").toString();
  if (!h.isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, h)) { emit navigationFailed(tr("命中图斑属于层位 %1，请先切换层位").arg(h)); return; }
  const auto version = hit.value("version_id").toString();
  QString error;
  // 命中行定位其自身版本（可能是旧成果版本），不走来源解析。
  const QString layerId = m_workbench->layerForVersion(version, &error);
  if (layerId.isEmpty()) { emit navigationFailed(error.isEmpty() ? tr("命中图斑所在版本已不可定位") : error); return; }
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(layerId, &error));
  if (!layer) { emit navigationFailed(error); return; }
  if (m_project && m_project->project())
    if (auto *node = m_project->project()->layerTreeRoot()->findLayer(layer->id())) node->setItemVisibilityChecked(true);
  const auto uid = hit.value("feature_uid").toString();
  m_primary->bindLayer(layer);
  if (!m_primary->showFeature(uid, true, &error)) { emit navigationFailed(error); return; }
  SelectionTarget target;
  target.projectId = projectId();
  target.horizon = h.isEmpty() ? m_workbench->declaration(layerId).horizon : h;
  target.layerId = layerId; target.versionId = version;
  target.assetId = hit.value("asset_id").toString(); target.featureUid = uid;
  target.evidenceId = hit.value("evidence_id", hit.value("id")).toString();
  m_selection->setTarget(target, QStringLiteral("reverse_index"));
}
bool EvidenceInteraction::canGoBack() const {
  if (!m_selection) return false;
  for (const auto &target : m_back)
    if (target.projectId == projectId() &&
        (target.horizon.isEmpty() || paleo::strat::matchesActiveAnalysis(m_selection, target.horizon)))
      return true;
  return false;
}
void EvidenceInteraction::back() {
  if (!m_selection) return;
  const bool was = canGoBack();
  while (!m_back.isEmpty()) {
    const auto target = m_back.takeLast();
    if (target.projectId != projectId() ||
        (!target.horizon.isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, target.horizon)))
      continue; // 其他工程/层位的历史项不可定位，丢弃继续找
    m_selection->setTarget(target, QStringLiteral("back"));
    break;
  }
  if (canGoBack() != was) emit historyChanged(canGoBack());
}
void EvidenceInteraction::navigateGraphNode(const QVariantMap &node) {
  if (!node.value("source_version").toString().isEmpty()) { navigate(node); return; }
  if (!m_workbench || !m_selection || !m_layers) return;
  if (node.contains("project_id") && node.value("project_id").toString() != projectId()) { emit navigationFailed(tr("图谱节点属于其他工程")); return; }
  if (node.contains("project_session") && m_project && node.value("project_session").toULongLong() != m_project->sessionId()) { emit navigationFailed(tr("图谱查询已过期，请刷新")); return; }
  if (!node.value("horizon").toString().isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, node.value("horizon").toString())) { emit navigationFailed(tr("图谱节点属于其他层位，请先切换层位")); return; }
  const auto version = node.value("version_id").toString(); QString error;
  QString layerId = node.value("layer_id").toString();
  if (layerId.isEmpty() || (!version.isEmpty() && m_workbench->versionForLayer(layerId).id != version)) layerId = m_workbench->layerForVersion(version, &error);
  if (layerId.isEmpty()) { emit navigationFailed(error); return; }
  const auto declaration = m_workbench->declaration(layerId);
  if (!declaration.horizon.isEmpty() && !paleo::strat::matchesActiveAnalysis(m_selection, declaration.horizon)) { emit navigationFailed(tr("图谱节点属于其他层位，请先切换层位")); return; }
  auto *layer = m_layers->instantiate(layerId, &error);
  if (!layer) { emit navigationFailed(error); return; }
  const auto uid = node.value("feature_uid").toString();
  if (uid.isEmpty()) { navigate({{"source_version", version}}); return; }
  auto *vector = qobject_cast<QgsVectorLayer *>(layer);
  if (!vector) { emit navigationFailed(tr("该节点没有矢量图斑")); return; }
  m_primary->bindLayer(vector);
  if (!m_primary->showFeature(uid, true, &error)) { emit navigationFailed(error); return; }
  SelectionTarget t;
  t.projectId = projectId(); t.horizon = declaration.horizon; t.layerId = layerId; t.featureUid = uid; t.versionId = version;
  t.assetId = node.value("asset_id").toString(); t.evidenceId = node.value("evidence_id").toString();
  t.interpretationId = node.value("interpretation_id").toString(); t.assertionId = node.value("assertion_id").toString();
  m_selection->setTarget(t, "graph");
}
