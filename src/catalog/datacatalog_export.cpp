// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

#include "catalogstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QSaveFile>

#include <cmath>
#include <limits>

namespace
{
using paleo::store_detail::setError;
} // namespace

// 导出族（方向 99 拆分）：catalog.json 快照导出（不涨 revision）与
// 井位/测区 GeoJSON 图层数据源（§4 地图高亮）。

bool DataCatalog::exportCatalogJson(const QString &path, QString *error) const
{
  // #476：快照导出读全部四张共享容器——与其它 const 读方法同款线程门。
  if (!noteRead("exportCatalogJson"))
  {
    setError(error, tr("catalog 读取被拒绝：exportCatalogJson 不在 catalog 所属线程调用"));
    return false;
  }
  CatalogStore::Tables tables;
  tables.entities = m_entities;
  tables.assets = m_assets;
  tables.versions = m_versions;
  tables.links = m_links;
  tables.meta.revision = m_revision;
  const QByteArray bytes =
      QJsonDocument(CatalogStore::toJson(tables)).toJson(QJsonDocument::Indented);
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    return false;
  }
  if (f.write(bytes) != bytes.size())
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    setError(error, QStringLiteral("short write to %1: %2").arg(path, detail));
    return false;
  }
  if (!f.commit())
  {
    setError(error, QStringLiteral("cannot replace %1: %2").arg(path, f.errorString()));
    return false;
  }
  return true;
}

bool DataCatalog::writeSurveyGeoJson(const QString &path, QString *error) const
{
  QJsonArray features;
  const auto areas = entities(QStringLiteral("seismic_survey")) + entities(QStringLiteral("study_area"));
  // #476：entities() 读被拒（跨线程）时 areas 同样为空——落盘会把上一份好
  // 文件换成空 FeatureCollection（图层要素全消失且无错误上报）。与
  // writeWellsGeoJson 同约定：没有可导出的测区就不写文件、也不算失败。
  if (areas.isEmpty())
    return true;
  for (const auto &survey : areas) {
    // 有 boundaryGeometry 时它是权威几何（面矢量导入的原始边界，见
    // io/projectboundary.cpp）：不拿角点门槛卡它——只导 boundary 的
    // study_area（ corners 为空或不足 3）此前被整条跳过，图层直接丢一个测区。
    const auto boundary = survey.extra.value(QStringLiteral("boundaryGeometry"));
    QJsonObject boundaryGeometry;
    if (boundary.typeId() == QMetaType::QVariantMap) {
      const auto candidate = QJsonObject::fromVariantMap(boundary.toMap());
      // OGR 只认 Polygon：非面/非对象/coordinates 不是环数组的坏值不能冒充
      // 几何——静默写进去会让图层读到空几何或解析失败。回落角点环。
      if (candidate.value(QStringLiteral("type")).toString() == QLatin1String("Polygon") &&
          candidate.value(QStringLiteral("coordinates")).isArray())
        boundaryGeometry = candidate;
    }
    if (boundaryGeometry.isEmpty() && survey.corners.size() < 3) continue;
    double xmin = std::numeric_limits<double>::infinity(), ymin = xmin;
    double xmax = -xmin, ymax = -xmin;
    int finiteCorners = 0;
    for (const auto &corner : survey.corners) {
      if (!std::isfinite(corner.first) || !std::isfinite(corner.second)) continue;
      ++finiteCorners;
      xmin = std::min(xmin, corner.first); xmax = std::max(xmax, corner.first);
      ymin = std::min(ymin, corner.second); ymax = std::max(ymax, corner.second);
    }
    if (boundaryGeometry.isEmpty() &&
        (finiteCorners < 3 || !(xmin < xmax && ymin < ymax))) continue;
    QJsonObject geometry = boundaryGeometry;
    if (geometry.isEmpty()) {
      QJsonArray ring;
      for (const auto &corner : survey.corners)
        if (std::isfinite(corner.first) && std::isfinite(corner.second)) ring.append(QJsonArray{corner.first, corner.second});
      if (ring.first() != ring.last()) ring.append(ring.first());
      // Polygon 的 coordinates 是环数组；QJsonArray{ring} 会调用拷贝构造，
      // 把必要的外层数组丢掉，导致 OGR 读到空几何。
      QJsonArray rings;
      rings.append(ring);
      geometry = QJsonObject{{"type", "Polygon"}, {"coordinates", rings}};
    }
    features.append(QJsonObject{{"type", "Feature"},
        {"properties", QJsonObject{{"id", survey.id}, {"name", survey.name}}},
        {"geometry", geometry}});
  }
  const QJsonObject root{{"type", "FeatureCollection"},
      {"crs", QJsonObject{{"type", "name"}, {"properties", QJsonObject{{"name", localGridCrsWkt()}}}}},
      {"features", features}};
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    setError(error, tr("无法创建测区图层目录")); return false;
  }
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  const auto bytes = QJsonDocument(root).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
    setError(error, tr("测区图层写入失败：%1").arg(file.errorString())); return false;
  }
  return true;
}

bool DataCatalog::writeWellsGeoJson(const QString &path, QString *error) const
{
  // §4 井位图层数据源：只写有 surface 坐标且坐标有限的井；坐标是原始
  // surface_x/y（局部测网米），真投影参数出现前地图一直读它。
  // #476：直接遍历共享容器 m_entities——与其它 const 读方法同款线程门
  // （空井集本来就「不写文件、返回 true」，读被拒不会留下坏文件）。
  if (!noteRead("writeWellsGeoJson"))
    return true;
  QJsonArray feats;
  for (const CatalogEntity &e : m_entities)
  {
    if (e.entityType != QLatin1String("well") || !e.hasSurface)
      continue;
    if (!std::isfinite(e.surfaceX) || !std::isfinite(e.surfaceY))
      continue;
    QJsonObject props;
    props.insert(QStringLiteral("id"), e.id);
    props.insert(QStringLiteral("name"), e.name);
    props.insert(QStringLiteral("coordinate_status"), e.coordinateStatus);
    QJsonObject geom;
    geom.insert(QStringLiteral("type"), QStringLiteral("Point"));
    geom.insert(QStringLiteral("coordinates"), QJsonArray{e.surfaceX, e.surfaceY});
    QJsonObject f;
    f.insert(QStringLiteral("type"), QStringLiteral("Feature"));
    f.insert(QStringLiteral("properties"), props);
    f.insert(QStringLiteral("geometry"), geom);
    feats.append(f);
  }
  if (feats.isEmpty())
    return true; // 没有可定位的井——不写空文件，也不算失败

  QJsonObject root;
  root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
  QJsonObject crsProps;
  crsProps.insert(QStringLiteral("name"), localGridCrsWkt());
  QJsonObject crs;
  crs.insert(QStringLiteral("type"), QStringLiteral("name"));
  crs.insert(QStringLiteral("properties"), crsProps);
  root.insert(QStringLiteral("crs"), crs);
  root.insert(QStringLiteral("features"), feats);

  QDir().mkpath(QFileInfo(path).absolutePath());
  // 原子写审计（T6）：井点 GeoJSON 是 wells 图层的数据源——裸 QFile 截断
  // 写中途崩溃会留下半截文件，OGR 照常打开但要素缺失（静默坏图层）。
  // QSaveFile 全量写成功才替换，坏盘/短写保住上一份好文件。
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1（%2）").arg(path, file.errorString()));
    return false;
  }
  // 短写（0 < n < size）同样是失败：只查 <0 会让 commit 发布半截 GeoJSON。
  const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (file.write(payload) != payload.size() || !file.commit())
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1").arg(path));
    return false;
  }
  return true;
}
