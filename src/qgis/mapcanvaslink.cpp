// 层：QGIS 封装
#include "mapcanvaslink.h"
#include <QScopedValueRollback>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsmapcanvas.h>
#include <qgsrubberband.h>
MapCanvasLink::MapCanvasLink(QgsMapCanvas *main, QgsMapCanvas *reference,
                             QObject *parent)
    : QObject(parent), m_main(main), m_reference(reference) {
  auto connectCanvas = [this](QgsMapCanvas *source, QgsMapCanvas *target) {
    connect(source, &QgsMapCanvas::extentsChanged, this,
            [this, source, target] { sync(source, target); });
    connect(source, &QgsMapCanvas::destinationCrsChanged, this,
            [this, source, target] { sync(source, target); });
    connect(source, &QgsMapCanvas::xyCoordinates, this,
            [this, source, target](const QgsPointXY &p) {
              cursor(source, target, p);
            });
  };
  connectCanvas(main, reference);
  connectCanvas(reference, main);
  // Markers must be released while each scene still exists.
  connect(main, &QObject::destroyed, this, [this] {
    delete m_mainMarker.data();
    m_mainMarker = nullptr;
    m_enabled = false;
  });
  connect(reference, &QObject::destroyed, this, [this] {
    delete m_referenceMarker.data();
    m_referenceMarker = nullptr;
    m_enabled = false;
  });
  sync(main, reference);
}
MapCanvasLink::~MapCanvasLink() { setEnabled(false); }
void MapCanvasLink::setEnabled(bool enabled) {
  m_enabled = enabled;
  if (!enabled) {
    delete m_mainMarker.data();
    m_mainMarker = nullptr;
    delete m_referenceMarker.data();
    m_referenceMarker = nullptr;
  } else if (m_main && m_reference)
    sync(m_main, m_reference);
}
void MapCanvasLink::sync(QgsMapCanvas *source, QgsMapCanvas *target) {
  if (!m_enabled || m_syncing || !m_main || !m_reference)
    return;
  QScopedValueRollback<bool> guard(m_syncing, true);
  auto extent = source->extent();
  try {
    if (source->mapSettings().destinationCrs() !=
        target->mapSettings().destinationCrs()) {
      QgsCoordinateTransform transform(
          source->mapSettings().destinationCrs(),
          target->mapSettings().destinationCrs(),
          source->mapSettings().transformContext());
      extent = transform.transformBoundingBox(extent);
    }
  } catch (const QgsCsException &) {
    return;
  }
  if (!extent.isEmpty()) {
    target->setExtent(extent);
    target->refresh();
  }
}
void MapCanvasLink::cursor(QgsMapCanvas *source, QgsMapCanvas *target,
                           const QgsPointXY &point) {
  if (!m_enabled || !m_main || !m_reference)
    return;
  auto p = point;
  try {
    QgsCoordinateTransform transform(source->mapSettings().destinationCrs(),
                                     target->mapSettings().destinationCrs(),
                                     source->mapSettings().transformContext());
    if (transform.isValid())
      p = transform.transform(point);
  } catch (const QgsCsException &) {
    return;
  }
  auto &marker = target == m_main ? m_mainMarker : m_referenceMarker;
  if (!marker) {
    marker = new QgsRubberBand(target, Qgis::GeometryType::Point);
    marker->setIcon(QgsRubberBand::ICON_CROSS);
    marker->setColor(QColor("#E53935"));
    marker->setIconSize(12);
    marker->setWidth(2);
  }
  marker->reset(Qgis::GeometryType::Point);
  marker->addPoint(p);
}
