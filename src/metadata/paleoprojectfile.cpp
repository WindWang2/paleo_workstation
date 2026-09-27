#include "paleoprojectfile.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

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
  if (!file.sourceAreaRoot.isEmpty())
  {
    QJsonObject s;
    s.insert(QStringLiteral("root"), file.sourceAreaRoot);
    s.insert(QStringLiteral("importedUtc"), file.sourceAreaImportedUtc);
    s.insert(QStringLiteral("stats"),
             QJsonObject::fromVariantMap(file.sourceStats));
    o.insert(QStringLiteral("sourceArea"), s);
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
    if (!QFile::exists(dir.filePath(it.value())))
      missing.append(QStringLiteral("%1: %2").arg(it.key(), it.value()));
  }
  return missing;
}
