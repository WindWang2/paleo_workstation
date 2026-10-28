// 层：数据
#include "nativemodel.h"
#include "nativemodel_internal.h"
#include "dataimport_log.h"
#include "../catalog/datacatalog.h"
#include <paleo_data/service.h>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <optional>

namespace paleo::native_model {
namespace {
struct Reference {
  QString database, digest, sourceDigest, error;
  qlonglong sourceId = 0, logSetId = 0;
  QVariantMap header;
  QVariantMap binding;
};
QMutex mutex;
QHash<const void *, QHash<QString, Reference>> projects;
QString key(const QString &path) {
  QString value = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
  value = value.toCaseFolded();
#endif
  return value;
}
std::optional<Reference> lookup(const QString &path) {
  const auto name = key(path);
  QMutexLocker lock(&mutex);
  std::optional<Reference> selected;
  for (const auto &project : projects) {
    const auto it = project.constFind(name);
    if (it == project.cend()) continue;
    // A path shared by different external versions cannot identify a version.
    if (it->error == QLatin1String("ambiguous")) return {};
    if (selected && (selected->digest != it->digest || selected->sourceDigest != it->sourceDigest)) return {};
    selected = *it;
  }
  return selected;
}
Read fail(QString *error, const QString &detail) {
  if (error) *error = detail;
  return Read::Failed;
}
void headerFrom(const Reference &ref, LasHeaderInfo &header) {
  header = {};
  header.curveNames = ref.header.value("names").toStringList();
  header.wellName = ref.header.value("well").toString();
  header.indexBasis = ref.header.value("basis").toString();
  header.nullValue = ref.header.value("null").toDouble();
  header.wrap = ref.header.value("wrap").toBool();
  header.delimiter = char(ref.header.value("delimiter").toInt());
  header.sawAscii = ref.header.value("sawAscii").toBool();
}
}

void registerCatalog(const DataCatalog &catalog, const void *owner) {
  QHash<QString, Reference> entries;
  for (const auto &version : catalog.versions()) {
    const auto binding = version.extra.value("nativeModel").toMap();
    if (binding.isEmpty()) continue;
    const QString source = catalog.versionFilePath(version);
    if (source.isEmpty()) continue;
    Reference ref;
    ref.binding = binding;
    CatalogVersion database; database.managed = true;
    database.path = binding.value("path").toString();
    ref.database = DataCatalog::resolvedVersionPath(catalog.projectDir(), database);
    ref.digest = binding.value("sha256").toString();
    ref.sourceDigest = binding.value("sourceSha256").toString();
    ref.sourceId = binding.value("sourceId").toLongLong();
    ref.logSetId = binding.value("logSetId").toLongLong();
    ref.header = binding.value("logHeader").toMap();
    if (binding.value("schema").toInt() != 1 || ref.database.isEmpty() ||
        ref.digest.size() != 64 || ref.sourceDigest != version.sha256 || ref.sourceId <= 0)
      ref.error = QStringLiteral("invalid native model binding");
    const QString name = key(source);
    if (entries.contains(name) && (entries.value(name).error == QLatin1String("ambiguous") || entries.value(name).digest != ref.digest ||
                                   entries.value(name).sourceDigest != ref.sourceDigest))
      ref.error = QStringLiteral("ambiguous");
    entries.insert(name, ref);
  }
  QMutexLocker lock(&mutex);
  projects.insert(owner, entries);
}
void unregisterCatalog(const void *owner) {
  QMutexLocker lock(&mutex);
  projects.remove(owner);
}

Read logHeader(const QString &path, LasHeaderInfo &header, QString *error) {
  const auto ref = lookup(path);
  if (!ref || ref->binding.value("kind") != QLatin1String("well_log")) return Read::Unbound;
  if (!ref->error.isEmpty()) return fail(error, ref->error);
  if (!QFileInfo::exists(path) || !QFileInfo::exists(ref->database))
    return fail(error, QStringLiteral("native model or source is missing"));
  // Header queries use the committed metadata snapshot, never unpack sample BLOBs.
  headerFrom(*ref, header);
  return Read::Ready;
}

Read log(const QString &path, LasHeaderInfo &header, QList<LasCurve> &curves,
         QString *error) {
  const auto ref = lookup(path);
  if (!ref || ref->binding.value("kind") != QLatin1String("well_log")) return Read::Unbound;
  if (!ref->error.isEmpty()) return fail(error, ref->error);
  if (dataimport_detail::logFileDigest(ref->database) != ref->digest)
    return fail(error, QStringLiteral("native model SHA-256 mismatch"));
  paleo::data::DataService service;
  std::string detail;
  const auto root = QFileInfo(ref->database).absolutePath().toUtf8().toStdString();
  if (!service.openProjectReadOnly(root, detail) || !service.verifySource(ref->sourceId, detail))
    return fail(error, QString::fromUtf8(detail.data(), qsizetype(detail.size())));
  paleo::data::LogSet logs;
  if (!service.readLogSet(ref->logSetId, true, logs, detail))
    return fail(error, QString::fromUtf8(detail.data(), qsizetype(detail.size())));
  LasHeaderInfo next; headerFrom(*ref, next);
  if (next.curveNames.size() != qsizetype(logs.logCurves.size()) + 1)
    return fail(error, QStringLiteral("native model curve metadata is inconsistent"));
  QList<LasCurve> values;
  LasCurve axis;
  axis.name = ref->header.value("axisName").toString();
  axis.unit = ref->header.value("axisUnit").toString();
  axis.descr = ref->header.value("axisDescription").toString();
  axis.values.assign(logs.samplingAxis.depthValues.begin(), logs.samplingAxis.depthValues.end());
  values.append(std::move(axis));
  for (const auto &curve : logs.logCurves) {
    LasCurve value;
    value.name = QString::fromUtf8(curve.curveName);
    value.unit = QString::fromUtf8(curve.curveUnit);
    value.descr = QString::fromUtf8(curve.description);
    value.values.assign(curve.sampleValues.begin(), curve.sampleValues.end());
    values.append(std::move(value));
  }
  header = std::move(next); curves = std::move(values);
  return Read::Ready;
}

bool hasLog(const QString &path) {
  const auto ref = lookup(path);
  return ref && ref->binding.value("kind") == QLatin1String("well_log");
}
Read open(const QString &path, const QString &kind, paleo::data::DataService &service,
          QVariantMap &binding, QString *error) {
  const auto ref = lookup(path);
  if (!ref || ref->binding.value("kind") != kind) return Read::Unbound;
  if (!ref->error.isEmpty()) return fail(error, ref->error);
  if (dataimport_detail::logFileDigest(ref->database) != ref->digest)
    return fail(error, QStringLiteral("native model SHA-256 mismatch"));
  std::string detail;
  if (!service.openProjectReadOnly(QFileInfo(ref->database).absolutePath().toUtf8().toStdString(), detail) ||
      !service.verifySource(ref->sourceId, detail))
    return fail(error, QString::fromUtf8(detail));
  binding = ref->binding;
  return Read::Ready;
}
QString cacheToken(const QString &path) {
  const auto ref = lookup(path);
  if (!ref) return {};
  const QFileInfo file(ref->database);
  return QStringLiteral("|native-v1:%1:%2:%3:%4:%5")
      .arg(ref->digest, ref->error).arg(file.size())
      .arg(file.lastModified().toMSecsSinceEpoch()).arg(file.metadataChangeTime().toMSecsSinceEpoch());
}
}
