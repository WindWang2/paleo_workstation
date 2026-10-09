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
  for (const auto &survey : entities(QStringLiteral("seismic_survey"))) {
    if (survey.corners.size() < 3) continue;
    double xmin = std::numeric_limits<double>::infinity(), ymin = xmin;
    double xmax = -xmin, ymax = -xmin;
    int finiteCorners = 0;
    for (const auto &corner : survey.corners) {
      if (!std::isfinite(corner.first) || !std::isfinite(corner.second)) continue;
      ++finiteCorners;
      xmin = std::min(xmin, corner.first); xmax = std::max(xmax, corner.first);
      ymin = std::min(ymin, corner.second); ymax = std::max(ymax, corner.second);
    }
    if (finiteCorners < 3 || !(xmin < xmax && ymin < ymax)) continue;
    QJsonArray ring{QJsonArray{xmin, ymin}, QJsonArray{xmax, ymin}, QJsonArray{xmax, ymax},
                    QJsonArray{xmin, ymax}, QJsonArray{xmin, ymin}};
    features.append(QJsonObject{{"type", "Feature"},
        {"properties", QJsonObject{{"id", survey.id}, {"name", survey.name}}},
        {"geometry", QJsonObject{{"type", "Polygon"}, {"coordinates", QJsonArray{ring}}}}});
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
  if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 ||
      !file.commit())
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1").arg(path));
    return false;
  }
  return true;
}
