// 层：QGIS 封装
#include "mapfeaturepicker.h"

#include <QMenu>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsfeature.h>
#include <qgshighlight.h>
#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsproject.h>
#include <qgstolerance.h>
#include <qgsvectorlayer.h>
#include <algorithm>
#include <cmath>

namespace {
struct Hit {
  QgsFeatureId id;
  double distance;
};

QList<Hit> candidates(QgsMapCanvas *canvas, QgsVectorLayer *layer, QPoint pos) {
  const auto &settings = canvas->mapSettings();
  const QgsPointXY point = settings.mapToPixel().toMapCoordinates(pos.x(), pos.y());
  const double radius = std::max(4.0 * settings.mapUnitsPerPixel(), QgsTolerance::vertexSearchRadius(settings));
  const QgsGeometry center = QgsGeometry::fromPointXY(point);
  const QgsRectangle bounds(point.x() - radius, point.y() - radius, point.x() + radius, point.y() + radius);
  const bool transform = layer->crs().isValid() && settings.destinationCrs().isValid() &&
                         layer->crs() != settings.destinationCrs();
  QgsCoordinateTransform toMap(layer->crs(), settings.destinationCrs(), settings.transformContext());
  const auto queryBounds = transform ? toMap.transformBoundingBox(bounds, Qgis::TransformDirection::Reverse) : bounds;
  auto rows = layer->getFeatures(QgsFeatureRequest().setFilterRect(queryBounds).setNoAttributes());
  QList<Hit> hits;
  QgsFeature feature;
  while (rows.nextFeature(feature)) {
    auto geometry = feature.geometry();
    if (geometry.isEmpty()) continue;
    if (transform) geometry.transform(toMap);
    const double distance = geometry.distance(center);
    if (distance >= 0 && distance <= radius) hits.append({feature.id(), distance});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
    return a.distance == b.distance ? a.id < b.id : a.distance < b.distance;
  });
  return hits;
}
}

MapFeaturePicker::MapFeaturePicker(QgsMapCanvas *canvas, QgsMapTool *owner)
    : m_canvas(canvas), m_owner(owner) {
  connect(canvas, &QgsMapCanvas::currentLayerChanged, this, [this] { clear(); });
  connect(canvas, &QgsMapCanvas::mapToolSet, this, [this] { clear(); });
  // Canvas graphics items belong to the scene when its view is destroyed first.
  connect(canvas, &QObject::destroyed, this, [this] { m_highlight.release(); });
}

MapFeaturePicker::~MapFeaturePicker() { clear(); }

void MapFeaturePicker::clear() {
  if (m_menu) {
    m_menu->close();
    m_menu.clear();
  }
  m_highlight.reset();
}

void MapFeaturePicker::highlight(QgsVectorLayer *layer, qint64 id) {
  m_highlight.reset();
  if (!m_canvas || m_canvas->currentLayer() != layer) return;
  const auto feature = layer->getFeature(id);
  if (!feature.hasGeometry()) return;
  m_highlight = std::make_unique<QgsHighlight>(m_canvas, feature.geometry(), layer);
  m_highlight->applyDefaultStyle();
  m_highlight->show();
}

void MapFeaturePicker::hover(QgsVectorLayer *layer, const QPoint &position) {
  if (m_menu) return;
  m_highlight.reset();
  if (!layer || !m_canvas || layer != m_canvas->currentLayer()) return;
  try {
    const auto hits = candidates(m_canvas, layer, position);
    if (!hits.isEmpty()) highlight(layer, hits.first().id);
  } catch (const QgsCsException &) {
    // No spatial candidate is valid until the user picks in a transformable area.
  }
}

MapFeaturePicker::Result MapFeaturePicker::choose(QgsVectorLayer *layer, const QPoint &position,
    bool modifying, const std::function<void(qint64)> &apply) {
  clear();
  if (!layer || !m_canvas || !m_owner || m_canvas->currentLayer() != layer ||
      (modifying && !layer->isEditable())) return Result::Failed;
  QList<Hit> hits;
  try {
    hits = candidates(m_canvas, layer, position);
  } catch (const QgsCsException &error) {
    emit m_owner->messageEmitted(error.what(), Qgis::MessageLevel::Warning);
    return Result::Failed;
  }
  if (hits.isEmpty()) return Result::Empty;
  if (hits.size() == 1) {
    apply(hits.first().id);
    return Result::Handled;
  }

  auto *menu = new QMenu(m_canvas->viewport());
  m_menu = menu;
  menu->setObjectName(QStringLiteral("mapFeatureCandidates"));
  menu->setAccessibleName(tr("选择目标要素"));
  menu->addSection(modifying ? tr("选择要删除的要素") : tr("选择要素"));
  const QPointer<QgsVectorLayer> target(layer);
  const QPointer<QgsProject> project(m_canvas->project());
  for (const auto &hit : hits) {
    auto *action = menu->addAction(tr("要素 %1").arg(hit.id));
    action->setData(hit.id);
    connect(action, &QAction::triggered, this, [this, target, project, modifying, apply, id = hit.id, position, menu] {
      if (m_menu != menu || !m_canvas || !target || m_canvas->mapTool() != m_owner ||
          m_canvas->project() != project || m_canvas->currentLayer() != target ||
          (modifying && !target->isEditable())) return;
      try {
        const auto hitsNow = candidates(m_canvas, target, position);
        if (std::any_of(hitsNow.begin(), hitsNow.end(), [id](const Hit &hit) { return hit.id == id; }))
          apply(id);
      } catch (const QgsCsException &error) {
        emit m_owner->messageEmitted(error.what(), Qgis::MessageLevel::Warning);
      }
    });
  }
  const auto invalidate = [this, menu] { if (m_menu == menu) clear(); };
  auto *session = project ? project.data() : QgsProject::instance();
  connect(session, &QgsProject::aboutToBeCleared, menu, invalidate);
  connect(session, &QObject::destroyed, menu, invalidate);
  connect(session, qOverload<const QStringList &>(&QgsProject::layersWillBeRemoved), menu,
          [id = layer->id(), invalidate](const QStringList &ids) { if (ids.contains(id)) invalidate(); });
  connect(layer, &QObject::destroyed, menu, invalidate);
  connect(layer, &QgsVectorLayer::editingStopped, menu, invalidate);
  connect(menu, &QMenu::hovered, this, [this, target](QAction *action) {
    if (target && action->data().isValid()) highlight(target, action->data().toLongLong());
  });
  connect(menu, &QMenu::aboutToHide, this, [this, menu] {
    m_highlight.reset();
    menu->deleteLater();
  });
  menu->popup(m_canvas->viewport()->mapToGlobal(position));
  menu->setActiveAction(menu->actions().at(1));
  return Result::Handled;
}
