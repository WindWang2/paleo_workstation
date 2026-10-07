// 层：数据
#include "paleoprojectfile.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QUuid>

#include <cmath>

namespace
{
  void setErr(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  QJsonObject membersToJson(const PaleoProjectFile &f)
  {
    QJsonObject m;
    m.insert(QStringLiteral("qgz"), f.qgz);
    m.insert(QStringLiteral("catalog"), f.catalog);
    m.insert(QStringLiteral("manifest"), f.manifest);
    m.insert(QStringLiteral("gpkg"), f.gpkg);
    m.insert(QStringLiteral("areaRules"), f.areaRules);
    return m;
  }

  QJsonObject georeferenceToJsonImpl(const PaleoGeoreference &g)
  {
    QJsonObject anchor;
    anchor.insert(QStringLiteral("lonDeg"), g.anchorLonDeg);
    anchor.insert(QStringLiteral("latDeg"), g.anchorLatDeg);
    anchor.insert(QStringLiteral("metersPerDegLon"), g.metersPerDegLon);
    anchor.insert(QStringLiteral("metersPerDegLat"), g.metersPerDegLat);
    QJsonObject params;
    params.insert(QStringLiteral("a"), g.a);
    params.insert(QStringLiteral("b"), g.b);
    params.insert(QStringLiteral("tE"), g.tE);
    params.insert(QStringLiteral("tN"), g.tN);
    QJsonArray cps;
    for (const PaleoGeoreference::ControlPoint &c : g.controlPoints)
    {
      QJsonObject o;
      o.insert(QStringLiteral("well"), c.well);
      o.insert(QStringLiteral("x"), c.x);
      o.insert(QStringLiteral("y"), c.y);
      o.insert(QStringLiteral("lon"), c.lon);
      o.insert(QStringLiteral("lat"), c.lat);
      o.insert(QStringLiteral("residualM"), c.residualM);
      cps.append(o);
    }
    QJsonObject o;
    o.insert(QStringLiteral("kind"), g.kind);
    o.insert(QStringLiteral("targetCrs"), g.targetCrs);
    o.insert(QStringLiteral("anchor"), anchor);
    o.insert(QStringLiteral("params"), params);
    o.insert(QStringLiteral("formula"), g.formula);
    o.insert(QStringLiteral("controlPoints"), cps);
    o.insert(QStringLiteral("maxResidualM"), g.maxResidualM);
    o.insert(QStringLiteral("provenance"), g.provenance);
    return o;
  }

  // 数值键缺失/非有限 → nullptr 返回；kind 缺省 similarity2d（唯一实现）。
  bool num(const QJsonObject &o, const QString &key, double *out)
  {
    const QJsonValue v = o.value(key);
    if (!v.isDouble())
      return false;
    const double d = v.toDouble();
    if (!std::isfinite(d))
      return false;
    *out = d;
    return true;
  }
} // namespace

QJsonObject paleoGeoreferenceToJson(const PaleoGeoreference &g)
{
  QJsonObject anchor;
  anchor.insert(QStringLiteral("lonDeg"), g.anchorLonDeg);
  anchor.insert(QStringLiteral("latDeg"), g.anchorLatDeg);
  anchor.insert(QStringLiteral("metersPerDegLon"), g.metersPerDegLon);
  anchor.insert(QStringLiteral("metersPerDegLat"), g.metersPerDegLat);
  QJsonObject params;
  params.insert(QStringLiteral("a"), g.a);
  params.insert(QStringLiteral("b"), g.b);
  params.insert(QStringLiteral("tE"), g.tE);
  params.insert(QStringLiteral("tN"), g.tN);
  QJsonArray cps;
  for (const PaleoGeoreference::ControlPoint &c : g.controlPoints)
  {
    QJsonObject o;
    o.insert(QStringLiteral("well"), c.well);
    o.insert(QStringLiteral("x"), c.x);
    o.insert(QStringLiteral("y"), c.y);
    o.insert(QStringLiteral("lon"), c.lon);
    o.insert(QStringLiteral("lat"), c.lat);
    o.insert(QStringLiteral("residualM"), c.residualM);
    cps.append(o);
  }
  QJsonObject o;
  o.insert(QStringLiteral("kind"), g.kind);
  o.insert(QStringLiteral("targetCrs"), g.targetCrs);
  o.insert(QStringLiteral("anchor"), anchor);
  o.insert(QStringLiteral("params"), params);
  o.insert(QStringLiteral("formula"), g.formula);
  o.insert(QStringLiteral("controlPoints"), cps);
  o.insert(QStringLiteral("maxResidualM"), g.maxResidualM);
  o.insert(QStringLiteral("provenance"), g.provenance);
  return o;
}

bool paleoGeoreferenceFromJson(const QJsonObject &o, PaleoGeoreference *g,
                               QString *error)
{
  g->kind = o.value(QStringLiteral("kind")).toString(
      QStringLiteral("similarity2d"));
  if (g->kind != QLatin1String("similarity2d"))
  {
    setErr(error, QStringLiteral("georeference.kind '%1' 不支持").arg(g->kind));
    return false;
  }
  g->targetCrs = o.value(QStringLiteral("targetCrs")).toString();
  const QJsonObject anchor = o.value(QStringLiteral("anchor")).toObject();
  const QJsonObject params = o.value(QStringLiteral("params")).toObject();
  if (!num(anchor, QStringLiteral("lonDeg"), &g->anchorLonDeg) ||
      !num(anchor, QStringLiteral("latDeg"), &g->anchorLatDeg) ||
      !num(anchor, QStringLiteral("metersPerDegLon"), &g->metersPerDegLon) ||
      !num(anchor, QStringLiteral("metersPerDegLat"), &g->metersPerDegLat) ||
      !num(params, QStringLiteral("a"), &g->a) ||
      !num(params, QStringLiteral("b"), &g->b) ||
      !num(params, QStringLiteral("tE"), &g->tE) ||
      !num(params, QStringLiteral("tN"), &g->tN))
  {
    setErr(error,
           QStringLiteral("georeference 缺 anchor/params 数值字段或含非有限值"));
    return false;
  }
  g->formula = o.value(QStringLiteral("formula")).toString();
  g->provenance = o.value(QStringLiteral("provenance")).toString();
  g->controlPoints.clear();
  g->maxResidualM = o.value(QStringLiteral("maxResidualM")).toDouble(0.0);
  const QJsonArray cps = o.value(QStringLiteral("controlPoints")).toArray();
  for (const QJsonValue &v : cps)
  {
    const QJsonObject c = v.toObject();
    PaleoGeoreference::ControlPoint cp;
    cp.well = c.value(QStringLiteral("well")).toString();
    num(c, QStringLiteral("x"), &cp.x);
    num(c, QStringLiteral("y"), &cp.y);
    num(c, QStringLiteral("lon"), &cp.lon);
    num(c, QStringLiteral("lat"), &cp.lat);
    num(c, QStringLiteral("residualM"), &cp.residualM);
    g->controlPoints.append(cp);
  }
  if (!g->isComplete())
  {
    setErr(error, QStringLiteral("georeference 参数无效（须使用 EPSG:4326、有效锚点、正度米系数和非零缩放）"));
    return false;
  }
  return true;
}

bool PaleoGeoreference::isComplete() const
{
  return std::isfinite(anchorLonDeg) && std::isfinite(anchorLatDeg) &&
         std::abs(anchorLonDeg) <= 180.0 && std::abs(anchorLatDeg) < 90.0 &&
         std::isfinite(metersPerDegLon) && metersPerDegLon > 0.0 &&
         std::isfinite(metersPerDegLat) && metersPerDegLat > 0.0 &&
         std::isfinite(a) && std::isfinite(b) && std::isfinite(tE) &&
         std::isfinite(tN) && std::hypot(a, b) > 1e-12 &&
         kind == QLatin1String("similarity2d") &&
         targetCrs.compare(QLatin1String("EPSG:4326"), Qt::CaseInsensitive) == 0;
}

bool applyGeoreference(const PaleoGeoreference &g, double x, double y,
                       double *lonDeg, double *latDeg)
{
  if (!g.isComplete() || !std::isfinite(x) || !std::isfinite(y))
    return false;
  const double e = g.a * x - g.b * y + g.tE;
  const double n = g.b * x + g.a * y + g.tN;
  const double lon = g.anchorLonDeg + e / g.metersPerDegLon;
  const double lat = g.anchorLatDeg + n / g.metersPerDegLat;
  if (!std::isfinite(lon) || !std::isfinite(lat) || std::abs(lon) > 180.0 || std::abs(lat) >= 90.0)
    return false;
  if (lonDeg) *lonDeg = lon;
  if (latDeg) *latDeg = lat;
  return true;
}

QString paleoProjectFilePath(const QString &projectDir)
{
  return QDir(projectDir).filePath(
      QString::fromLatin1(PaleoProjectFile::kFileName));
}

PaleoProjectFile projectFileForQgz(const QString &qgzPath)
{
  PaleoProjectFile f;
  const QFileInfo qi(qgzPath);
  const QString base = qi.completeBaseName();
  f.name = base;
  f.projectId = QStringLiteral("proj-%1").arg(
      QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));
  f.createdUtc =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  f.qgz = qi.fileName();
  f.catalog = QStringLiteral("artifacts/metadata/catalog.json");
  f.manifest = qi.fileName() + QStringLiteral(".project.sqlite");
  f.gpkg = base + QStringLiteral(".gpkg");
  f.areaRules = QStringLiteral("project_area.json");
  return f;
}

bool writeProjectFile(const QString &projectDir, const PaleoProjectFile &file,
                      QString *error)
{
  if (projectDir.isEmpty())
  {
    setErr(error, QStringLiteral("project dir is empty"));
    return false;
  }
  if (!QDir().mkpath(projectDir))
  {
    setErr(error, QStringLiteral("cannot create project dir: %1").arg(projectDir));
    return false;
  }

  QJsonObject o;
  o.insert(QStringLiteral("format"), QStringLiteral("paleo-project"));
  o.insert(QStringLiteral("formatVersion"), file.formatVersion);
  o.insert(QStringLiteral("name"), file.name);
  o.insert(QStringLiteral("projectId"), file.projectId);
  o.insert(QStringLiteral("createdUtc"), file.createdUtc);
  o.insert(QStringLiteral("members"), membersToJson(file));
  o.insert(QStringLiteral("map"), QJsonObject{
      {QStringLiteral("crs"), file.mapCrs},
      {QStringLiteral("basemap"), QJsonObject{
          {QStringLiteral("enabled"), file.basemapEnabled},
          {QStringLiteral("topo"), file.basemapTopo},
          {QStringLiteral("hillshade"), file.basemapHillshade}}}});
  if (!file.sourceAreaRoot.isEmpty())
  {
    QJsonObject s;
    s.insert(QStringLiteral("root"), file.sourceAreaRoot);
    s.insert(QStringLiteral("importedUtc"), file.sourceAreaImportedUtc);
    s.insert(QStringLiteral("stats"),
             QJsonObject::fromVariantMap(file.sourceStats));
    o.insert(QStringLiteral("sourceArea"), s);
  }
  if (file.georeference)
  {
    if (!file.georeference->isComplete())
    {
      setErr(error,
             QStringLiteral("georeference 参数不完整，拒写 project.paleo"));
      return false;
    }
    o.insert(QStringLiteral("georeference"),
             paleoGeoreferenceToJson(*file.georeference));
  }

  const QString path = paleoProjectFilePath(projectDir);
  QSaveFile out(path);
  out.setDirectWriteFallback(false);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setErr(error,
           QStringLiteral("cannot write %1: %2").arg(path, out.errorString()));
    return false;
  }
  const QByteArray bytes =
      QJsonDocument(o).toJson(QJsonDocument::Indented);
  if (out.write(bytes) != bytes.size())
  {
    const QString detail = out.errorString();
    out.cancelWriting();
    setErr(error, QStringLiteral("short write to %1: %2").arg(path, detail));
    return false;
  }
  if (!out.commit())
  {
    setErr(error, QStringLiteral("cannot replace %1: %2").arg(path, out.errorString()));
    return false;
  }
  return true;
}

PaleoProjectFile readProjectFile(const QString &path, bool *ok, QString *error)
{
  PaleoProjectFile f;
  QFile in(path);
  if (!in.open(QIODevice::ReadOnly))
  {
    setErr(error, QStringLiteral("cannot open %1: %2").arg(path, in.errorString()));
    if (ok)
      *ok = false;
    return f;
  }
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject())
  {
    setErr(error, QStringLiteral("project file %1 is not valid JSON").arg(path));
    if (ok)
      *ok = false;
    return f;
  }

  const QJsonObject o = doc.object();
  if (o.value(QStringLiteral("format")).toString() !=
      QLatin1String("paleo-project"))
  {
    setErr(error, QStringLiteral("%1 is not a paleo-project file").arg(path));
    if (ok)
      *ok = false;
    return f;
  }
  const int ver = o.value(QStringLiteral("formatVersion")).toInt(-1);
  if (ver < 1 || ver > 1)
  {
    setErr(error,
           QStringLiteral("project file formatVersion %1 is newer than this "
                          "build understands").arg(ver));
    if (ok)
      *ok = false;
    return f;
  }

  f.formatVersion = ver;
  f.name = o.value(QStringLiteral("name")).toString();
  f.projectId = o.value(QStringLiteral("projectId")).toString();
  f.createdUtc = o.value(QStringLiteral("createdUtc")).toString();
  const QJsonObject m = o.value(QStringLiteral("members")).toObject();
  f.qgz = m.value(QStringLiteral("qgz")).toString();
  f.catalog = m.value(QStringLiteral("catalog")).toString();
  f.manifest = m.value(QStringLiteral("manifest")).toString();
  f.gpkg = m.value(QStringLiteral("gpkg")).toString();
  f.areaRules = m.value(QStringLiteral("areaRules")).toString();
  const QJsonObject s = o.value(QStringLiteral("sourceArea")).toObject();
  f.sourceAreaRoot = s.value(QStringLiteral("root")).toString();
  f.sourceAreaImportedUtc = s.value(QStringLiteral("importedUtc")).toString();
  f.sourceStats = s.value(QStringLiteral("stats")).toObject().toVariantMap();
  const auto map = o.value(QStringLiteral("map")).toObject();
  f.mapCrs = map.value(QStringLiteral("crs")).toString(QStringLiteral("EPSG:3857"));
  const auto basemap = map.value(QStringLiteral("basemap")).toObject();
  f.basemapEnabled = basemap.value(QStringLiteral("enabled")).toBool(true);
  f.basemapTopo = basemap.value(QStringLiteral("topo")).toString();
  f.basemapHillshade = basemap.value(QStringLiteral("hillshade")).toString();
  if (o.contains(QStringLiteral("georeference")))
  {
    PaleoGeoreference g;
    QString gerr;
    if (paleoGeoreferenceFromJson(o.value(QStringLiteral("georeference")).toObject(),
                                  &g, &gerr))
      f.georeference = g;
    else
      f.georeferenceError = gerr; // 节在但坏：如实报，不拦打开
  }

  if (ok)
    *ok = true;
  return f;
}

QStringList missingMembers(const QString &projectDir,
                           const PaleoProjectFile &file)
{
  QStringList missing;
  const QDir dir(projectDir);
  const QMap<QString, QString> members = {
      {QStringLiteral("qgz"), file.qgz},
      {QStringLiteral("catalog"), file.catalog},
      {QStringLiteral("manifest"), file.manifest},
      {QStringLiteral("gpkg"), file.gpkg},
      {QStringLiteral("areaRules"), file.areaRules},
  };
  for (auto it = members.constBegin(); it != members.constEnd(); ++it)
  {
    if (it.value().isEmpty())
      continue; // 未声明的成员不算缺失（工程早期没建属正常）
    if (it.key() == QLatin1String("catalog"))
    {
      const bool present =
          QFile::exists(dir.filePath(it.value())) ||
          QFile::exists(dir.filePath(
              QStringLiteral("artifacts/metadata/catalog.sqlite"))) ||
          QFile::exists(dir.filePath(
              QStringLiteral("artifacts/metadata/catalog.json"))) ||
          QFile::exists(dir.filePath(
              QStringLiteral("artifacts/metadata/catalog.json.migrated")));
      if (!present)
        missing.append(QStringLiteral("%1: %2").arg(it.key(), it.value()));
      continue;
    }
    if (!QFile::exists(dir.filePath(it.value())))
      missing.append(QStringLiteral("%1: %2").arg(it.key(), it.value()));
  }
  return missing;
}
