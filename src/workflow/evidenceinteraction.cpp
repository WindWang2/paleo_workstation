// 层：功能
#include "evidenceinteraction.h"
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
    connect(selection, &SelectionContext::activeHorizonChanged, this, [this] { m_source->clear(); m_primary->clear(); refresh(); });
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
        const auto json = m_project->project()->readEntry("paleo/evidenceTarget", "selection");
        const auto saved = QJsonDocument::fromJson(json.toUtf8()).toVariant().toMap();
        if (saved.value("project_id").toString() != projectId() ||
            (!saved.value("horizon").toString().isEmpty() && saved.value("horizon").toString() != m_selection->activeHorizon())) return;
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
      if (m_selection && m_selection->target().layerId == id) capture(id);
    });
    connect(workbench, &MappingWorkbench::displayChanged, this, [this](const QString &id) {
      if (m_selection && m_selection->target().layerId == id && !m_receiving) capture(id);
    });
  }
}
QString EvidenceInteraction::projectId() const {
  return m_project ? m_project->mapConfiguration().projectId : QString();
}
void EvidenceInteraction::reset() {
  m_primary->clear(); m_source->clear(); m_snapshot.clear();
  if (m_selection) m_selection->clearTarget("project");
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
  connect(layer, &QgsVectorLayer::destroyed, this, [this, id] {
    if (m_selection && m_selection->target().layerId == id) {
      m_source->clear(); m_snapshot.clear();
      emit detailsChanged({{"layer_id", id}, {"message", tr("图层已卸载；恢复图层后将按稳定身份重新定位")}}, m_selection->targetGeneration());
    }
  });
}
void EvidenceInteraction::setLayer(const QString &id) {
  if (!m_workbench || !m_selection || !m_layers || id.isEmpty()) return;
  const auto d = m_workbench->declaration(id);
  if (!d.horizon.isEmpty() && d.horizon != m_selection->activeHorizon()) return;
  observe(id); capture(id);
}
void EvidenceInteraction::capture(const QString &id) {
  if (m_receiving || !m_workbench || !m_selection) return;
  QScopedValueRollback<bool> guard(m_receiving, true);
  const auto h = m_workbench->declaration(id).horizon;
  if (!h.isEmpty() && h != m_selection->activeHorizon()) return;
  const auto ids = m_workbench->selectedFeatures(id);
  m_snapshot = m_workbench->featureEvidence(id, ids);
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
  if (m_layers) m_primary->bindLayer(qobject_cast<QgsVectorLayer *>(m_layers->layer(id)));
  emit detailsChanged(m_snapshot, m_selection->targetGeneration());
}
void EvidenceInteraction::receive(const SelectionTarget &target, const QString &origin, quint64 generation) {
  if (m_receiving || !m_workbench || !m_selection || !m_layers) return;
  if (target.isEmpty()) { m_snapshot.clear(); m_primary->clear(); emit detailsChanged({}, generation); return; }
  if (target.projectId != projectId() || (!target.horizon.isEmpty() && target.horizon != m_selection->activeHorizon())) return;
  QScopedValueRollback<bool> guard(m_receiving, true);
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(target.layerId));
  QList<qint64> ids;
  if (layer && !target.featureUid.isEmpty()) {
    m_primary->bindLayer(layer); QString error;
    const auto fid = m_primary->featureId(target.featureUid, &error);
    if (fid == FID_NULL) { emit navigationFailed(error); return; }
    ids << fid;
    if (origin != "map" && !m_primary->showFeature(target.featureUid, false, &error)) emit navigationFailed(error);
  }
  m_snapshot = m_workbench->featureEvidence(target.layerId, ids);
  m_snapshot.insert("project_id", projectId()); m_snapshot.insert("project_session", m_project ? QVariant(m_project->sessionId()) : QVariant());
  emit detailsChanged(m_snapshot, generation);
}
void EvidenceInteraction::refresh() {
  if (!m_selection || m_selection->target().isEmpty()) return;
  receive(m_selection->target(), "refresh", m_selection->targetGeneration());
}
void EvidenceInteraction::navigate(const QVariantMap &evidence, bool locate) {
  if (!m_selection || !m_workbench || !m_layers) return;
  // A view captured in another project/session must not navigate reused IDs.
  if (evidence.contains("project_id") && evidence.value("project_id").toString() != projectId()) { emit navigationFailed(tr("证据属于其他工程")); return; }
  if (evidence.contains("project_session") && m_project && evidence.value("project_session").toULongLong() != m_project->sessionId()) { emit navigationFailed(tr("证据查询已过期，请刷新")); return; }
  const auto h = evidence.value("source_horizon", evidence.value("horizon")).toString();
  if (!h.isEmpty() && h != m_selection->activeHorizon()) { emit navigationFailed(tr("来源属于其他层位，请先切换层位")); return; }
  const auto entity = evidence.value("entity_id").toString();
  if (!entity.isEmpty() && evidence.value("has_coordinates").toBool()) {
    QString error;
    if (!m_source->showPoint(evidence.value("x").toDouble(), evidence.value("y").toDouble(), DataCatalog::localGridCrsWkt(), locate, &error)) { emit navigationFailed(error); return; }
    m_selection->setSelection({entity}, "evidence_well");
    auto selected = m_selection->target(); selected.evidenceId = evidence.value("id", evidence.value("evidence_id")).toString();
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
  selected.evidenceId = evidence.value("id", evidence.value("evidence_id")).toString();
  m_selection->setTarget(selected, "evidence");
}
void EvidenceInteraction::navigateGraphNode(const QVariantMap &node) {
  if (!node.value("source_version").toString().isEmpty()) { navigate(node); return; }
  if (!m_workbench || !m_selection || !m_layers) return;
  const auto version = node.value("version_id").toString(); QString error;
  QString layerId = node.value("layer_id").toString();
  if (layerId.isEmpty() || (!version.isEmpty() && m_workbench->versionForLayer(layerId).id != version)) layerId = m_workbench->layerForVersion(version, &error);
  if (layerId.isEmpty()) { emit navigationFailed(error); return; }
  const auto declaration = m_workbench->declaration(layerId);
  if (!declaration.horizon.isEmpty() && declaration.horizon != m_selection->activeHorizon()) { emit navigationFailed(tr("图谱节点属于其他层位，请先切换层位")); return; }
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
