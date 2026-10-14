// 层：QGIS 封装
#include "evidencemapadapter.h"
#include "mappingfeatureidentity.h"
#include <QThread>
#include <QTimer>
#include <limits>
#include <cmath>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsfeaturerequest.h>
#include <qgsmapcanvas.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>

EvidenceMapAdapter::EvidenceMapAdapter(QgsMapCanvas *canvas, QObject *parent)
    : QObject(parent), m_canvas(canvas) {
  if (canvas) {
    connect(canvas, &QObject::destroyed, this, [this] { clearBand(); ++m_generation; });
    connect(canvas, &QgsMapCanvas::destinationCrsChanged, this, &EvidenceMapAdapter::queueRestore);
  }
}
EvidenceMapAdapter::~EvidenceMapAdapter() { clearBand(); }
QgsVectorLayer *EvidenceMapAdapter::layer() const { return m_layer.data(); }
void EvidenceMapAdapter::clearBand() { delete m_band.data(); m_band = nullptr; }
void EvidenceMapAdapter::clear() { ++m_generation; m_uid.clear(); clearBand(); }
bool EvidenceMapAdapter::fail(QString *error, const QString &reason) {
  clearBand();
  if (error) *error = reason;
  emit unavailable(reason);
  return false;
}
void EvidenceMapAdapter::bindLayer(QgsVectorLayer *layer) {
  if (m_layer == layer) {
    if (!layer) return;
    const auto key = layer->customProperty(QStringLiteral("paleoLayerId")).toString();
    const auto version = layer->customProperty(QStringLiteral("paleoVersionId")).toString();
    if (key == m_layerId && version == m_versionId) return;
    m_layerId = key; m_versionId = version;
    invalidate(); // same working provider, new immutable saved version
    return;
  }
  ++m_generation;
  if (m_layer) disconnect(m_layer, nullptr, this, nullptr);
  clearBand(); m_layer = layer; m_index.clear(); m_ambiguous.clear(); m_dirty = true;
  if (!layer) return; // remember the stable target for a later provider reload
  const auto key = layer->customProperty(QStringLiteral("paleoLayerId")).toString();
  const auto version = layer->customProperty(QStringLiteral("paleoVersionId")).toString();
  if (key.isEmpty() || key != m_layerId || version != m_versionId) m_uid.clear();
  m_layerId = key; m_versionId = version;
  connect(layer, &QgsVectorLayer::featureAdded, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QgsVectorLayer::featureDeleted, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QgsVectorLayer::updatedFields, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QgsVectorLayer::attributeValueChanged, this,
          [this](QgsFeatureId, int index, const QVariant &) {
    if (m_layer && index == m_layer->fields().indexOf(MappingFeatureIdentity::UidField)) invalidate();
  });
  connect(layer, &QgsVectorLayer::geometryChanged, this, &EvidenceMapAdapter::queueRestore);
  connect(layer, &QgsVectorLayer::editingStopped, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QgsVectorLayer::afterRollBack, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QgsMapLayer::dataChanged, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QgsMapLayer::dataSourceChanged, this, &EvidenceMapAdapter::invalidate);
  connect(layer, &QObject::destroyed, this, [this] {
    ++m_generation; clearBand(); m_index.clear(); m_ambiguous.clear(); m_dirty = true;
  });
  connect(layer, &QgsVectorLayer::selectionChanged, this, [this] {
    if (!m_layer) return;
    const auto identities = m_layer->selectedFeatureIds();
    QStringList uids;
    if (!identities.isEmpty()) {
      if (m_layer->fields().indexOf(MappingFeatureIdentity::UidField) < 0) {
        emit unavailable(tr("历史图元无稳定身份；可按保存版本查看来源"));
        return;
      }
      rebuildIndex();
      QgsFeatureRequest request;
      request.setFilterFids(identities);
      request.setSubsetOfAttributes({m_layer->fields().indexOf(MappingFeatureIdentity::UidField)});
      request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
      auto features = m_layer->getFeatures(request);
      QgsFeature feature;
      while (features.nextFeature(feature)) {
        const auto identity = MappingFeatureIdentity::uid(feature);
        if (identity.isEmpty() || m_ambiguous.contains(identity)) {
          emit unavailable(tr("历史图元没有可唯一解析的稳定身份；可按保存版本查看来源"));
          return;
        }
        uids << identity;
      }
    }
    uids.sort();
    emit featuresSelected(m_layerId, m_versionId, uids);
  });
  queueRestore();
}
void EvidenceMapAdapter::invalidate() { m_dirty = true; queueRestore(); }
void EvidenceMapAdapter::queueRestore() {
  const quint64 generation = ++m_generation;
  if (m_uid.isEmpty()) return;
  QTimer::singleShot(0, this, [this, generation] {
    if (generation != m_generation || !m_layer || m_uid.isEmpty()) return;
    QString ignored;
    showFeature(m_uid, false, &ignored);
  });
}
void EvidenceMapAdapter::rebuildIndex() {
  if (!m_dirty || !m_layer) return;
  m_index.clear(); m_ambiguous.clear(); ++m_indexBuilds;
  const int field = m_layer->fields().indexOf(MappingFeatureIdentity::UidField);
  if (field >= 0) {
    QgsFeatureRequest request;
    request.setSubsetOfAttributes({field});
    request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
    auto features = m_layer->getFeatures(request);
    QgsFeature feature;
    while (features.nextFeature(feature)) {
      const auto identity = MappingFeatureIdentity::uid(feature);
      if (identity.isEmpty()) continue;
      if (m_index.contains(identity)) m_ambiguous.insert(identity);
      else m_index.insert(identity, feature.id());
    }
  }
  m_dirty = false;
}
QgsFeatureId EvidenceMapAdapter::featureId(const QString &featureUid, QString *error) {
  if (QThread::currentThread() != thread()) {
    if (error) *error = tr("地图身份查询需要在地图所属线程执行");
    return FID_NULL;
  }
  if (!m_layer || !m_layer->isValid()) {
    fail(error, tr("图层暂不可用；保存的证据关联仍保留")); return FID_NULL;
  }
  if (featureUid.isEmpty()) {
    fail(error, tr("缺少稳定图元身份，请按版本查看来源")); return FID_NULL;
  }
  rebuildIndex();
  if (m_ambiguous.contains(featureUid)) {
    fail(error, tr("图元身份重复，不能确定证据的空间对象")); return FID_NULL;
  }
  if (!m_index.contains(featureUid)) {
    fail(error, tr("关联图元已删除或当前图层不含该身份；历史来源仍保留")); return FID_NULL;
  }
  return m_index.value(featureUid);
}
bool EvidenceMapAdapter::showFeature(const QString &featureUid, bool locate, QString *error) {
  if (QThread::currentThread() != thread()) return false;
  ++m_generation; m_uid = featureUid;
  if (!m_canvas) return fail(error, tr("地图暂不可用；保存的证据关联仍保留"));
  const auto fid = featureId(featureUid, error);
  if (FID_IS_NULL(fid)) return false;
  auto feature = m_layer->getFeature(fid);
  if (!feature.isValid() || MappingFeatureIdentity::uid(feature) != featureUid) {
    m_dirty = true;
    return fail(error, tr("图层已更新，请重新定位关联图元"));
  }
  auto geometry = feature.geometry();
  if (geometry.isEmpty() || !m_layer->crs().isValid() ||
      !m_canvas->mapSettings().destinationCrs().isValid())
    return fail(error, tr("关联图元的几何或坐标参考无效"));
  try {
    if (m_layer->crs() != m_canvas->mapSettings().destinationCrs()) {
      QgsCoordinateTransform transform(m_layer->crs(), m_canvas->mapSettings().destinationCrs(),
                                       m_canvas->mapSettings().transformContext());
      if (geometry.transform(transform) != Qgis::GeometryOperationResult::Success)
        return fail(error, tr("关联图元坐标转换失败"));
    }
  } catch (const QgsCsException &) { return fail(error, tr("关联图元坐标转换失败")); }
  clearBand();
  m_band = new QgsRubberBand(m_canvas, geometry.type());
  m_band->setColor(m_canvas->selectionColor());
  m_band->setFillColor(Qt::transparent);
  m_band->setWidth(3);
  if (geometry.type() == Qgis::GeometryType::Point) {
    m_band->setIcon(QgsRubberBand::ICON_CIRCLE); m_band->setIconSize(12);
  }
  m_band->setToGeometry(geometry); m_band->show();
  if (locate) {
    auto bounds = geometry.boundingBox();
    if (bounds.width() == 0 || bounds.height() == 0) {
      const auto extent = m_canvas->extent();
      const double margin = qMax(1.0, qMax(extent.width(), extent.height()) * 0.05);
      bounds.grow(margin);
    } else bounds.scale(1.2);
    m_canvas->setExtent(bounds); m_canvas->refresh();
  }
  return true;
}
bool EvidenceMapAdapter::showExtent(bool locate, QString *error) {
  if (QThread::currentThread() != thread()) return false;
  clear();
  if (!m_canvas || !m_layer || !m_layer->isValid() || m_layer->extent().isEmpty())
    return fail(error, tr("当前版本没有可定位的地图范围"));
  if (!m_layer->crs().isValid() || !m_canvas->mapSettings().destinationCrs().isValid())
    return fail(error, tr("当前版本的坐标参考无效"));
  if (locate) {
    auto extent = m_layer->extent();
    try {
      if (m_layer->crs() != m_canvas->mapSettings().destinationCrs()) {
        QgsCoordinateTransform transform(m_layer->crs(), m_canvas->mapSettings().destinationCrs(),
                                         m_canvas->mapSettings().transformContext());
        extent = transform.transformBoundingBox(extent);
      }
    } catch (const QgsCsException &) { return fail(error, tr("版本地图范围坐标转换失败")); }
    m_canvas->setExtent(extent); m_canvas->refresh();
  }
  return true;
}
bool EvidenceMapAdapter::showLayer(QgsMapLayer *layer, bool locate, QString *error) {
  if (QThread::currentThread() != thread()) return false;
  if (layer && layer->thread() != QThread::currentThread())
    return fail(error, tr("来源图层必须在所属线程定位"));
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    bindLayer(vector);
    return showExtent(locate, error);
  }
  // Raster/source navigation is explicitly extent-level. No pixel/feature
  // identity or scientifically determined area of influence is fabricated.
  bindLayer(nullptr); clear();
  if (!m_canvas || !layer || !layer->isValid() || layer->extent().isEmpty())
    return fail(error, tr("来源图层暂不可用或没有有效空间范围"));
  if (!layer->crs().isValid() || !m_canvas->mapSettings().destinationCrs().isValid())
    return fail(error, tr("来源图层的坐标参考无效"));
  if (locate) {
    auto extent = layer->extent();
    try {
      if (layer->crs() != m_canvas->mapSettings().destinationCrs()) {
        QgsCoordinateTransform transform(layer->crs(), m_canvas->mapSettings().destinationCrs(),
                                         m_canvas->mapSettings().transformContext());
        extent = transform.transformBoundingBox(extent);
      }
    } catch (const QgsCsException &) { return fail(error, tr("来源图层空间范围坐标转换失败")); }
    m_canvas->setExtent(extent); m_canvas->refresh();
  }
  return true;
}
bool EvidenceMapAdapter::showPoint(double x, double y, const QString &sourceCrsWkt,
                                   bool locate, QString *error) {
  if (QThread::currentThread() != thread()) return false;
  bindLayer(nullptr); clear();
  if (!m_canvas) return fail(error, tr("地图暂不可用，无法定位来源井点"));
  if (!std::isfinite(x) || !std::isfinite(y))
    return fail(error, tr("来源井点坐标无效"));
  const auto source = QgsCoordinateReferenceSystem::fromWkt(sourceCrsWkt);
  const auto destination = m_canvas->mapSettings().destinationCrs();
  if (!source.isValid() || !destination.isValid())
    return fail(error, tr("来源井点的坐标参考无效"));
  auto geometry = QgsGeometry::fromPointXY(QgsPointXY(x, y));
  try {
    if (source != destination) {
      QgsCoordinateTransform transform(source, destination, m_canvas->mapSettings().transformContext());
      if (geometry.transform(transform) != Qgis::GeometryOperationResult::Success)
        return fail(error, tr("来源井点坐标转换失败"));
    }
  } catch (const QgsCsException &) { return fail(error, tr("来源井点坐标转换失败")); }
  m_band = new QgsRubberBand(m_canvas, Qgis::GeometryType::Point);
  m_band->setColor(m_canvas->selectionColor()); m_band->setFillColor(Qt::transparent);
  m_band->setIcon(QgsRubberBand::ICON_CIRCLE); m_band->setIconSize(12); m_band->setWidth(3);
  m_band->setToGeometry(geometry); m_band->show();
  if (locate) {
    auto extent = m_canvas->extent();
    const auto point = geometry.asPoint();
    if (extent.isEmpty()) {
      const double radius = destination.isGeographic() ? 0.01 : 500.0;
      extent = QgsRectangle(point.x() - radius, point.y() - radius,
                            point.x() + radius, point.y() + radius);
    }
    else {
      const auto halfWidth = extent.width() / 2.0, halfHeight = extent.height() / 2.0;
      extent = QgsRectangle(point.x() - halfWidth, point.y() - halfHeight,
                            point.x() + halfWidth, point.y() + halfHeight);
    }
    m_canvas->setExtent(extent); m_canvas->refresh();
  }
  return true;
}
