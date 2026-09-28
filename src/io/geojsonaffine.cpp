// 层：数据
#include "geojsonaffine.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>
#include <functional>
#include <limits>

void geoAffineApply(const GeoAffineParams &p, double inX, double inY,
                    double *outX, double *outY)
{
  const double r = qDegreesToRadians(p.rotDeg);
  const double c = qCos(r), s = qSin(r);
  const double x = inX * p.sx, y = inY * p.sy;
  *outX = x * c - y * s + p.tx;
  *outY = x * s + y * c + p.ty;
}

void geoAffineTransformCoords(const GeoAffineParams &p, QJsonValue *coords)
{
  if (!coords || !coords->isArray())
    return;
  QJsonArray arr = coords->toArray();
  // 顶层自身就是 position（Point/MultiPoint 的 [x,y(,z…)]）：直接变换前
  // 两分量，不再递归——子项是数字，进不了分支。
  if (arr.size() >= 2 && arr.at(0).isDouble() && arr.at(1).isDouble())
  {
    double x, y;
    geoAffineApply(p, arr.at(0).toDouble(), arr.at(1).toDouble(), &x, &y);
    arr[0] = x;
    arr[1] = y;
    *coords = arr;
    return;
  }
  for (int i = 0; i < arr.size(); ++i)
  {
    QJsonValue v = arr.at(i);
    if (!v.isArray())
      continue;
    geoAffineTransformCoords(p, &v);
    arr[i] = v;
  }
  *coords = arr;
}

bool geoAffineTransformFile(const QString &inPath, const QString &outPath,
                            const GeoAffineParams &p, QString *error,
                            int *outFeatures, double outBounds[4])
{
  QFile in(inPath);
  if (!in.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = in.errorString();
    return false;
  }
  QJsonParseError perr{};
  const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &perr);
  if (perr.error != QJsonParseError::NoError || !doc.isObject())
  {
    if (error)
      *error = QStringLiteral("GeoJSON 解析失败：%1").arg(perr.errorString());
    return false;
  }

  QJsonObject root = doc.object();
  const QJsonArray features = root.value(QStringLiteral("features")).toArray();
  if (features.isEmpty())
  {
    if (error)
      *error = QStringLiteral("GeoJSON 没有要素");
    return false;
  }

  double minX = std::numeric_limits<double>::max(),
         minY = std::numeric_limits<double>::max(),
         maxX = std::numeric_limits<double>::lowest(),
         maxY = std::numeric_limits<double>::lowest();
  QJsonArray out;
  for (const QJsonValue &fv : features)
  {
    QJsonObject feat = fv.toObject();
    QJsonValue coords = feat.value(QStringLiteral("geometry"))
                            .toObject()
                            .value(QStringLiteral("coordinates"));
    QJsonValue tc = coords;
    geoAffineTransformCoords(p, &tc);
    QJsonObject geom = feat.value(QStringLiteral("geometry")).toObject();
    geom.insert(QStringLiteral("coordinates"), tc);
    feat.insert(QStringLiteral("geometry"), geom);
    out.append(feat);

    // bounds：复用变换后数组（position 形状 [x,y,...] 递归收集）。
    std::function<void(const QJsonValue &)> collect = [&](const QJsonValue &v) {
      if (!v.isArray())
        return;
      const QJsonArray a = v.toArray();
      if (a.size() >= 2 && a.at(0).isDouble() && a.at(1).isDouble())
      {
        minX = qMin(minX, a.at(0).toDouble());
        maxX = qMax(maxX, a.at(0).toDouble());
        minY = qMin(minY, a.at(1).toDouble());
        maxY = qMax(maxY, a.at(1).toDouble());
        return;
      }
      for (const QJsonValue &e : a)
        collect(e);
    };
    collect(tc);
  }
  root.insert(QStringLiteral("features"), out);
  root.insert(QStringLiteral("paleo_provisional_affine"),
              QJsonObject::fromVariantMap(geoAffineToMap(p)));

  QFile of(outPath);
  if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = of.errorString();
    return false;
  }
  if (of.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0)
  {
    if (error)
      *error = of.errorString();
    return false;
  }
  of.close();

  if (outFeatures)
    *outFeatures = out.size();
  if (outBounds)
  {
    outBounds[0] = minX;
    outBounds[1] = minY;
    outBounds[2] = maxX;
    outBounds[3] = maxY;
  }
  return true;
}

bool geoJsonBounds(const QString &inPath, double outBounds[4], QString *error)
{
  QFile in(inPath);
  if (!in.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = in.errorString();
    return false;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
  if (!doc.isObject())
  {
    if (error)
      *error = QStringLiteral("GeoJSON 解析失败");
    return false;
  }
  double minX = std::numeric_limits<double>::max(),
         minY = std::numeric_limits<double>::max(),
         maxX = std::numeric_limits<double>::lowest(),
         maxY = std::numeric_limits<double>::lowest();
  std::function<void(const QJsonValue &)> collect = [&](const QJsonValue &v) {
    if (!v.isArray())
      return;
    const QJsonArray a = v.toArray();
    if (a.size() >= 2 && a.at(0).isDouble() && a.at(1).isDouble())
    {
      minX = qMin(minX, a.at(0).toDouble());
      maxX = qMax(maxX, a.at(0).toDouble());
      minY = qMin(minY, a.at(1).toDouble());
      maxY = qMax(maxY, a.at(1).toDouble());
      return;
    }
    for (const QJsonValue &e : a)
      collect(e);
  };
  for (const QJsonValue &f : doc.object().value(QStringLiteral("features")).toArray())
    collect(f.toObject().value(QStringLiteral("geometry")).toObject()
                .value(QStringLiteral("coordinates")));
  if (minX > maxX || minY > maxY)
  {
    if (error)
      *error = QStringLiteral("GeoJSON 没有坐标");
    return false;
  }
  outBounds[0] = minX;
  outBounds[1] = minY;
  outBounds[2] = maxX;
  outBounds[3] = maxY;
  return true;
}

QVariantMap geoAffineToMap(const GeoAffineParams &p)
{
  return {{QStringLiteral("tx"), p.tx}, {QStringLiteral("ty"), p.ty},
          {QStringLiteral("sx"), p.sx}, {QStringLiteral("sy"), p.sy},
          {QStringLiteral("rotDeg"), p.rotDeg}};
}
