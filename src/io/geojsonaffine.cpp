// 层：数据
#include "geojsonaffine.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QJsonObject>
#include <QStringList>
#include <QtMath>
#include <cmath>
#include <limits>

namespace
{
struct CoordinateBounds
{
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();

  // GeoJSON positions are [x,y,...]; nested arrays cover lines and rings.
  void collect(const QJsonValue &value)
  {
    if (!value.isArray())
      return;
    const QJsonArray array = value.toArray();
    if (array.size() >= 2 && array.at(0).isDouble() && array.at(1).isDouble())
    {
      minX = qMin(minX, array.at(0).toDouble());
      maxX = qMax(maxX, array.at(0).toDouble());
      minY = qMin(minY, array.at(1).toDouble());
      maxY = qMax(maxY, array.at(1).toDouble());
      return;
    }
    for (const QJsonValue &element : array)
      collect(element);
  }

  bool empty() const { return minX > maxX || minY > maxY; }
  void copyTo(double out[4]) const
  {
    out[0] = minX;
    out[1] = minY;
    out[2] = maxX;
    out[3] = maxY;
  }
};
} // namespace

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

bool geoAffineParamsValid(const GeoAffineParams &p, QString *why)
{
  constexpr double kMinScale = 1e-9;
  constexpr double kMaxScale = 1e9;
  constexpr double kMaxShift = 1e10;
  QStringList bad;
  const auto shift = [&bad](const char *name, double v) {
    if (!std::isfinite(v))
      bad << QStringLiteral("%1 非有限值").arg(QLatin1String(name));
    else if (std::fabs(v) > kMaxShift)
      bad << QStringLiteral("%1=%2 超出平移量程 ±%3")
                 .arg(QLatin1String(name)).arg(v).arg(kMaxShift);
  };
  const auto scale = [&bad](const char *name, double v) {
    if (!std::isfinite(v))
      bad << QStringLiteral("%1 非有限值").arg(QLatin1String(name));
    else if (std::fabs(v) < kMinScale)
      bad << QStringLiteral("%1=%2 缩放为零或近零（几何塌缩）").arg(QLatin1String(name)).arg(v);
    else if (std::fabs(v) > kMaxScale)
      bad << QStringLiteral("%1=%2 缩放超出量程").arg(QLatin1String(name)).arg(v);
  };
  shift("tx", p.tx);
  shift("ty", p.ty);
  scale("sx", p.sx);
  scale("sy", p.sy);
  if (!std::isfinite(p.rotDeg))
    bad << QStringLiteral("rotDeg 非有限值");
  if (!bad.isEmpty() && why)
    *why = QStringLiteral("仿射参数不合法：%1").arg(bad.join(QStringLiteral("；")));
  return bad.isEmpty();
}

bool geoAffineTransformFile(const QString &inPath, const QString &outPath,
                            const GeoAffineParams &p, QString *error,
                            int *outFeatures, double outBounds[4])
{
  if (!geoAffineParamsValid(p, error))
    return false;
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

  CoordinateBounds bounds;
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

    bounds.collect(tc);
  }
  root.insert(QStringLiteral("features"), out);
  root.insert(QStringLiteral("paleo_provisional_affine"),
              QJsonObject::fromVariantMap(geoAffineToMap(p)));

  // 原子写审计（T6）：配准结果 GeoJSON 裸 QFile 截断写在中途崩溃/短写时
  // 留半截文件——OGR 之后读到残缺要素。QSaveFile 写全量成功才替换。
  QSaveFile of(outPath);
  of.setDirectWriteFallback(false);
  if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = of.errorString();
    return false;
  }
  if (of.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 || !of.commit())
  {
    if (error)
      *error = QStringLiteral("write failed: %1").arg(outPath);
    return false;
  }

  if (outFeatures)
    *outFeatures = out.size();
  if (outBounds)
    bounds.copyTo(outBounds);
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
  CoordinateBounds bounds;
  for (const QJsonValue &f : doc.object().value(QStringLiteral("features")).toArray())
    bounds.collect(f.toObject().value(QStringLiteral("geometry")).toObject()
                .value(QStringLiteral("coordinates")));
  if (bounds.empty())
  {
    if (error)
      *error = QStringLiteral("GeoJSON 没有坐标");
    return false;
  }
  bounds.copyTo(outBounds);
  return true;
}

QVariantMap geoAffineToMap(const GeoAffineParams &p)
{
  return {{QStringLiteral("tx"), p.tx}, {QStringLiteral("ty"), p.ty},
          {QStringLiteral("sx"), p.sx}, {QStringLiteral("sy"), p.sy},
          {QStringLiteral("rotDeg"), p.rotDeg}};
}
