// 层：QGIS 封装
#include "clusterpatchpicker.h"
#include <QApplication>
#include <QMouseEvent>
#include <utility>
#include <qgsfeaturerequest.h>
#include <qgsmapcanvas.h>
#include <qgsmaptoolidentify.h>
#include <qgsmaptoolpan.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>

ClusterPatchPicker::ClusterPatchPicker(QgsMapCanvas *canvas) : QObject(canvas), m_canvas(canvas) {
  m_outline = new QgsRubberBand(canvas, Qgis::GeometryType::Polygon);
  canvas->viewport()->installEventFilter(this);
  auto clear = [this] {
    m_press.reset();
    if (m_outline) m_outline->reset(Qgis::GeometryType::Polygon);
    emit cleared();
  };
  connect(canvas, &QgsMapCanvas::currentLayerChanged, this, clear);
  connect(canvas, &QgsMapCanvas::layersChanged, this, [this, clear] {
    if (!visibleTarget()) clear();
    else if (!m_highlightIds.isEmpty()) highlight(m_highlightIds, m_highlightColor);
  });
}

QgsVectorLayer *ClusterPatchPicker::visibleTarget() const {
  auto *layer = qobject_cast<QgsVectorLayer *>(m_canvas->currentLayer());
  if (!layer || layer->isEditable() || !m_canvas->layers().contains(layer) ||
      !layer->isInScaleRange(m_canvas->scale()) || layer->geometryType() != Qgis::GeometryType::Polygon ||
      layer->fields().indexOf("cluster_id") < 0 || layer->fields().indexOf("patch_id") < 0 ||
      !layer->customProperty("paleoLayerId").toString().startsWith(QLatin1String("product."))) return nullptr;
  return layer;
}

void ClusterPatchPicker::forwardPress() {
  if (!m_press) return;
  auto press = std::move(m_press);
  m_forwarding = true;
  QApplication::sendEvent(m_canvas->viewport(), press.get());
  m_forwarding = false;
}

bool ClusterPatchPicker::eventFilter(QObject *, QEvent *event) {
  if (m_forwarding || (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease &&
      event->type() != QEvent::MouseMove)) return false;
  auto *mouse = static_cast<QMouseEvent *>(event);
  const auto pos = mouse->position().toPoint();
  if (event->type() == QEvent::MouseButtonPress) {
    forwardPress();
    if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier ||
        !qobject_cast<QgsMapToolPan *>(m_canvas->mapTool()) || !visibleTarget()) return false;
    // Defer both halves of a potential click. A native pan must never receive a press
    // without its release, nor may a simple inspection recenter the map.
    m_press.reset(mouse->clone());
    return true;
  }
  if (!m_press) return false;
  if ((pos - m_press->position().toPoint()).manhattanLength() >= QApplication::startDragDistance()) {
    forwardPress();
    return false;
  }
  if (event->type() == QEvent::MouseMove) return true;
  auto *layer = visibleTarget();
  if (!layer || !qobject_cast<QgsMapToolPan *>(m_canvas->mapTool()) || mouse->button() != Qt::LeftButton) {
    forwardPress();
    return false;
  }
  QgsMapToolIdentify identify(m_canvas);
  const auto hits = identify.identify(pos.x(), pos.y(), {layer}, QgsMapToolIdentify::TopDownAll);
  if (hits.isEmpty()) { forwardPress(); emit cleared(); return false; }
  m_press.reset();
  m_canvas->setFocus(Qt::MouseFocusReason);
  emit picked(layer->customProperty("paleoLayerId").toString(), hits.first().mFeature.id());
  return true;
}

void ClusterPatchPicker::highlight(const QList<qint64> &ids, const QColor &color) {
  m_highlightIds = ids;
  m_highlightColor = color;
  if (!m_outline) return;
  m_outline->reset(Qgis::GeometryType::Polygon);
  auto *layer = visibleTarget();
  if (!layer || ids.isEmpty()) return;
  m_outline->setStrokeColor(color);
  m_outline->setFillColor(Qt::transparent);
  m_outline->setWidth(2);
  QgsFeatureRequest request;
  request.setFilterFids(QgsFeatureIds(ids.begin(), ids.end())).setNoAttributes();
  auto features = layer->getFeatures(request);
  QgsFeature feature;
  while (features.nextFeature(feature)) m_outline->addGeometry(feature.geometry(), layer, false);
  m_outline->updatePosition();
  m_outline->show();
}

void ClusterPatchPicker::previewDraft(const QString &layerId, const QVariantList &edits) {
  if (m_draftLayer == layerId && m_draftEdits == edits) return;
  m_draftLayer = layerId;
  m_draftEdits = edits;
  for (auto band : std::as_const(m_previewBands)) delete band.data();
  m_previewBands.clear();
  auto *layer = visibleTarget();
  if (!layer || edits.isEmpty() || layer->customProperty("paleoLayerId").toString() != layerId) return;
  // Reuse the previewed scope samples. Panning never scans the layer or recreates draft geometry.
  QgsFeatureIds ids;
  QHash<int, QPair<qsizetype, QColor>> clusters;
  QHash<qint64, QPair<qsizetype, QColor>> patches;
  for (qsizetype i = edits.size(); i > 0; --i) {
    const auto edit = edits.at(i - 1).toMap();
    const auto color = QColor(edit.value("preview_color").toString());
    const auto fid = edit.value("feature_id").toLongLong();
    const auto cluster = edit.value("cluster_id").toInt();
    if (edit.value("whole_cluster").toBool()) {
      if (!clusters.contains(cluster)) clusters.insert(cluster, {i, color});
    } else if (!patches.contains(fid)) patches.insert(fid, {i, color});
    const auto sample = edit.value("preview_ids").value<QList<qint64>>();
    for (const auto id : sample) {
      if (ids.size() >= 256) break;
      ids.insert(id);
    }
  }
  QgsFeatureRequest request;
  request.setFilterFids(ids).setSubsetOfAttributes(QStringList{QStringLiteral("cluster_id")}, layer->fields());
  auto features = layer->getFeatures(request);
  QHash<QRgb, QgsRubberBand *> colors;
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    const auto patch = patches.value(feature.id());
    const auto cluster = clusters.value(feature.attribute(QStringLiteral("cluster_id")).toInt());
    const auto color = (patch.first > cluster.first ? patch : cluster).second;
    if (!color.isValid()) continue;
    auto *&band = colors[color.rgb()];
    if (!band) {
      band = new QgsRubberBand(m_canvas, Qgis::GeometryType::Polygon);
      band->setZValue(m_outline->zValue() - 1);
      band->setFillColor(color);
      band->setStrokeColor(color.darker());
      m_previewBands << band;
    }
    band->addGeometry(feature.geometry(), layer, false);
  }
  for (auto *band : std::as_const(colors)) { band->updatePosition(); band->show(); }
}
