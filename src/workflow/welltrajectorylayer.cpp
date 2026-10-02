// 层：功能
#include "welltrajectorylayer.h"

#include "../catalog/datacatalog.h"
#include "../services/projectdata.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace paleo
{

QByteArray wellTrajectoriesGeoJson(ProjectDataFacade *data, DataCatalog *catalog,
                                   QString *error)
{
  if (error)
    error->clear();
  if (!data || !catalog)
  {
    if (error)
      *error = QStringLiteral("数据门面/catalog 缺失");
    return QByteArray();
  }

  QJsonArray feats;
  for (const ProjectWell &w : data->wells())
  {
    if (w.coordinateStatus != QLatin1String("ok") &&
        w.coordinateStatus != QLatin1String("untransformed"))
      continue;
    if (!std::isfinite(w.surfaceX) || !std::isfinite(w.surfaceY))
      continue;
    const auto survey = data->trajectoryFor(w.id);
    if (!survey)
    {
      // 无链接（直井回退，lastError 空）只跳过；链接在但站表坏 → 记因，
      // 不把坏测斜静默降级成「没有测斜」。
      if (error && !data->lastError().isEmpty())
        *error += (error->isEmpty() ? QString() : QStringLiteral("；")) +
                  QStringLiteral("井 %1：%2").arg(w.name, data->lastError());
      continue;
    }

    QJsonArray origin;
    origin.append(w.surfaceX);
    origin.append(w.surfaceY);
    QJsonArray coords;
    coords.append(origin);
    for (const TrajectoryPoint &p : survey->points())
      if (p.md > 0.0 || p.tvd > 0.0 || p.north != 0.0 || p.east != 0.0)
        coords.append(QJsonArray{w.surfaceX + p.east, w.surfaceY + p.north});
    const double td = catalog->entityById(w.id).td;
    if (td > survey->points().constLast().md)
    {
      // 井深超出末站：沿末站姿态延伸到 TD（与剖面井底线同源）。
      const TrajectoryPoint tip = survey->pointAt(td);
      coords.append(QJsonArray{w.surfaceX + tip.east, w.surfaceY + tip.north});
    }
    if (coords.size() < 2)
      continue; // 全直井测斜（零位移）——线退化成点，不画

    QJsonObject props;
    props.insert(QStringLiteral("id"), w.id);
    props.insert(QStringLiteral("name"), w.name);
    QJsonObject geom;
    geom.insert(QStringLiteral("type"), QStringLiteral("LineString"));
    geom.insert(QStringLiteral("coordinates"), coords);
    QJsonObject f;
    f.insert(QStringLiteral("type"), QStringLiteral("Feature"));
    f.insert(QStringLiteral("properties"), props);
    f.insert(QStringLiteral("geometry"), geom);
    feats.append(f);
  }

  QJsonObject root;
  root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
  QJsonObject crsProps;
  crsProps.insert(QStringLiteral("name"), DataCatalog::localGridCrsWkt());
  QJsonObject crs;
  crs.insert(QStringLiteral("type"), QStringLiteral("name"));
  crs.insert(QStringLiteral("properties"), crsProps);
  root.insert(QStringLiteral("crs"), crs);
  root.insert(QStringLiteral("features"), feats);
  return feats.isEmpty() ? QByteArray() : QJsonDocument(root).toJson();
}

} // namespace paleo
