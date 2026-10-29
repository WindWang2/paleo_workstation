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
#include <qgsexpression.h>
#include <qgsrasterlayer.h>

namespace {
QgsCoordinateReferenceSystem confirmedCrs()
{
  return QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32650"));
}
void ensureLayerCrs(QgsMapLayer *layer)
{
  if (layer && !layer->crs().isValid()) layer->setCrs(confirmedCrs());
}
QVariantMap spatialFeature(const QgsFeature &feature, const QgsVectorLayer *layer) {
  QVariantMap attributes;
  for (const auto &name : {QStringLiteral("id"), QStringLiteral("well_id"), QStringLiteral("horizon"),
                         QStringLiteral("facies_code"), QStringLiteral("type"), QStringLiteral("semantic")}) {
    const int index = feature.fields().indexOf(name);
    if (index >= 0) attributes.insert(name, feature.attribute(index));
  }
  const auto bounds = feature.geometry().boundingBox();
  return {{"feature_uid", MappingFeatureIdentity::uid(feature)},
      {"wkb", feature.geometry().asWkb()}, {"crs_wkt", layer->crs().toWkt()},
      {"geometry_hash", MappingFeatureIdentity::geometryFingerprint(feature)},
      {"bounds", QVariantList{bounds.xMinimum(), bounds.yMinimum(), bounds.xMaximum(), bounds.yMaximum()}},
      {"attributes", attributes}, {"facies_code", attributes.value("facies_code")},
      {"horizon", attributes.value("horizon")},
      {"layer_id", layer->customProperty("paleoLayerId")}, {"version_id", layer->customProperty("paleoVersionId")}};
}
}

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
  ensureLayerCrs(m_layer);
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
  ensureLayerCrs(layer);
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
  auto source = QgsCoordinateReferenceSystem::fromWkt(sourceCrsWkt);
  if (!source.isValid()) source = QgsCoordinateReferenceSystem(sourceCrsWkt);
  if (!source.isValid()) source = confirmedCrs();
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
QVariantMap EvidenceMapAdapter::featureSnapshot(const QString &featureUid, QString *error) {
  const auto invalid = [error](const QString &state, const QString &message) {
    if (error) *error = message;
    return QVariantMap{{"state", state}, {"message", message}};
  };
  if (QThread::currentThread() != thread() || !m_layer || m_layer->thread() != QThread::currentThread())
    return invalid("unknown", tr("空间快照需要在图层所属线程读取"));
  if (!m_layer->isValid() || !m_layer->crs().isValid())
    return invalid("unknown", tr("图层或坐标参考无效"));
  rebuildIndex();
  if (featureUid.isEmpty() || !m_index.contains(featureUid)) return invalid("missing", tr("稳定图元身份不存在"));
  if (m_ambiguous.contains(featureUid)) return invalid("ambiguous", tr("稳定图元身份重复"));
  const auto feature = m_layer->getFeature(m_index.value(featureUid));
  if (!feature.isValid() || MappingFeatureIdentity::uid(feature) != featureUid)
    return invalid("missing", tr("图层已更新，图元身份无法恢复"));
  if (feature.geometry().isEmpty() || !feature.geometry().isGeosValid())
    return invalid("unknown", tr("图元几何无效，不能确定空间关联"));
  auto result = spatialFeature(feature, m_layer);
  result.insert("state", "available"); result.insert("scope", "feature_geometry");
  return result;
}
QVariantMap EvidenceMapAdapter::spatialCandidates(const QVariantMap &target, const QString &pinnedUid,
                                                  int limit, QString *error) {
  return spatialCandidates(m_layer.data(), target, pinnedUid, limit, error);
}
QVariantMap EvidenceMapAdapter::spatialCandidates(QgsMapLayer *source, const QVariantMap &target,
                                                  const QString &pinnedUid, int limit, QString *error) {
  QVariantMap result{{"state", "unknown"}, {"scope", "spatial_candidates"},
      {"features", QVariantList{}}, {"unresolved", QVariantList{}}, {"truncated", false}};
  const auto invalid = [&result, error](const QString &state, const QString &message) {
    result.insert("state", state); result.insert("message", message);
    if (error) *error = message;
    return result;
  };
  if (QThread::currentThread() != thread() || !source || source->thread() != QThread::currentThread())
    return invalid("unknown", tr("空间查询需要有效的所属线程图层"));
  ensureLayerCrs(source);
  if (!source->isValid() || !source->crs().isValid()) return invalid("unknown", tr("来源图层或坐标参考无效"));
  const auto targetCrs = QgsCoordinateReferenceSystem::fromWkt(target.value("crs_wkt").toString());
  QgsGeometry geometry; geometry.fromWkb(target.value("wkb").toByteArray());
  if (!targetCrs.isValid() || geometry.isEmpty() || !geometry.isGeosValid())
    return invalid("unknown", tr("目标空间快照缺少有效的几何或坐标参考"));
  try {
    if (targetCrs != source->crs()) {
      QgsCoordinateTransform transform(targetCrs, source->crs(), source->transformContext());
      if (geometry.transform(transform) != Qgis::GeometryOperationResult::Success)
        return invalid("unknown", tr("目标与来源坐标转换失败"));
    }
  } catch (const QgsCsException &) { return invalid("unknown", tr("目标与来源坐标转换失败")); }
  result.insert("source_layer_id", source->customProperty("paleoLayerId"));
  result.insert("source_version_id", source->customProperty("paleoVersionId"));
  auto *vector = qobject_cast<QgsVectorLayer *>(source);
  if (!vector) {
    if (!qobject_cast<QgsRasterLayer *>(source)) return invalid("unknown", tr("该来源类型没有可核查的矢量或栅格空间范围"));
    if (!pinnedUid.isEmpty()) return invalid("missing", tr("栅格来源没有可解析的矢量图元身份"));
    result.insert("scope", "raster_extent");
    result.insert("state", source->extent().isEmpty() ? "unknown"
        : geometry.intersects(QgsGeometry::fromRect(source->extent())) ? "intersects" : "disjoint");
    result.insert("message", tr("仅核查来源空间范围；不证明有效像元或地质支撑关系"));
    return result;
  }
  const int budget = qBound(1, limit, 200), scanBudget = qMin(2000, budget * 5);
  const int uidField = vector->fields().indexOf(MappingFeatureIdentity::UidField);
  if (vector == m_layer && uidField >= 0) rebuildIndex();
  QgsFeatureRequest request;
  QgsAttributeList attributes;
  for (const auto &name : {MappingFeatureIdentity::UidField, QStringLiteral("id"), QStringLiteral("well_id"),
                         QStringLiteral("horizon"), QStringLiteral("facies_code"), QStringLiteral("type"), QStringLiteral("semantic")}) {
    const int index = vector->fields().indexOf(name);
    if (index >= 0) attributes << index;
  }
  request.setSubsetOfAttributes(attributes);
  if (!pinnedUid.isEmpty()) {
    if (uidField < 0) return invalid("missing", tr("来源图层没有稳定图元身份"));
    if (vector == m_layer) {
      rebuildIndex();
      if (m_ambiguous.contains(pinnedUid)) return invalid("ambiguous", tr("来源图元身份重复"));
      if (!m_index.contains(pinnedUid)) return invalid("missing", tr("引用的来源图元已不存在"));
      request.setFilterFid(m_index.value(pinnedUid));
    } else {
      // Same identity rule as MappingFeatureIdentity::uid(): QString::trimmed().
      // A provider trim() only strips ASCII spaces, so a tab or NBSP duplicate
      // would otherwise look unique and pass confirmation.
      QgsFeatureRequest identityRequest;
      identityRequest.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
      identityRequest.setSubsetOfAttributes(QgsAttributeList{uidField});
      QgsFeatureId matchedId = FID_NULL;
      bool matched = false;
      auto identities = vector->getFeatures(identityRequest);
      QgsFeature candidate;
      while (identities.nextFeature(candidate)) {
        if (MappingFeatureIdentity::uid(candidate) != pinnedUid) continue;
        if (matched) return invalid("ambiguous", tr("来源图元身份重复"));
        matched = true;
        matchedId = candidate.id();
      }
      if (!matched) return invalid("missing", tr("引用的来源图元已不存在"));
      request.setFilterFid(matchedId);
    }
  } else {
    request.setFilterRect(geometry.boundingBox()); request.setLimit(scanBudget + 1);
  }
  auto iterator = vector->getFeatures(request); QgsFeature feature;
  QVariantList matches, unresolved; QHash<QString, int> seen;
  int scanned = 0;
  while (iterator.nextFeature(feature)) {
    if (!pinnedUid.isEmpty() && ++scanned > 1) return invalid("ambiguous", tr("来源图元身份重复"));
    if (pinnedUid.isEmpty() && ++scanned > scanBudget) { result.insert("truncated", true); break; }
    const auto identity = MappingFeatureIdentity::uid(feature);
    if (!pinnedUid.isEmpty() && identity != pinnedUid) return invalid("missing", tr("来源图元身份已变化"));
    if (feature.geometry().isEmpty() || !feature.geometry().isGeosValid()) {
      unresolved << QVariantMap{{"feature_uid", identity}, {"state", "invalid_geometry"}}; continue;
    }
    if (!pinnedUid.isEmpty()) {
      auto pinned = spatialFeature(feature, vector); pinned.remove("wkb");
      result.insert("pinned_feature", pinned);
    }
    if (!geometry.intersects(feature.geometry())) continue;
    if (matches.size() >= budget) { result.insert("truncated", true); break; }
    auto candidate = spatialFeature(feature, vector);
    candidate.remove("wkb"); // at most bounded facts cross the query boundary
    candidate.insert("spatial_intersection", true); candidate.insert("association_basis", "exact_geometry_intersection");
    if (identity.isEmpty()) candidate.insert("identity_state", "legacy_missing");
    else if (m_layer == vector && m_ambiguous.contains(identity)) candidate.insert("identity_state", "ambiguous");
    else if (seen.contains(identity)) {
      candidate.insert("identity_state", "ambiguous");
      auto previous = matches[seen.value(identity)].toMap(); previous.insert("identity_state", "ambiguous");
      matches[seen.value(identity)] = previous;
    } else {
      seen.insert(identity, matches.size());
      candidate.insert("identity_state", m_layer == vector ? "stable" : "unvalidated");
    }
    matches << candidate;
  }
  if (!pinnedUid.isEmpty() && scanned == 0) return invalid("missing", tr("引用的来源图元已不存在"));
  result.insert("features", matches); result.insert("unresolved", unresolved); result.insert("scanned", scanned);
  result.insert("state", !matches.isEmpty() ? "intersects"
      : !unresolved.isEmpty() || result.value("truncated").toBool() ? "unknown" : "disjoint");
  result.insert("message", tr("空间相交仅提供局部候选，不等同地质支持或确定影响范围"));
  return result;
}
