// 层：数据
#include "operationretention.h"
#include "../io/pathcanon.h"
#include "../metadata/managedpath.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace paleo::storage {
namespace {
constexpr qint64 maxJournalBytes = 128LL * 1024 * 1024;
constexpr int maxReferences = 1000000;
QString identity(const QString &path) {
  auto value = PathCanon::canonicalize(path);
#ifdef Q_OS_WIN
  value = value.toCaseFolded();
#endif
  return value;
}
bool identifier(const QJsonValue &value, QString &id, bool emptyAllowed) {
  if (!value.isString()) return false;
  id = value.toString();
  if ((!emptyAllowed && id.isEmpty()) || id.toUtf8().size() > 4096) return false;
  for (const auto c : id) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
  return true;
}
struct Reader {
  QString root;
  OperationRetention result;
  QSet<QString> paths, versions, operations, keys;
  std::function<bool()> cancelled;
  int references = 0;
  bool check() {
    if (cancelled && cancelled()) {result.code = "operation_retention_cancelled"; return false;}
    if (++references > maxReferences) {result.code = "operation_retention_budget_exceeded"; return false;}
    return true;
  }
  bool version(const QJsonValue &value, bool emptyAllowed = false) {
    QString id;
    if (!check() || !identifier(value, id, emptyAllowed)) return false;
    if (!id.isEmpty()) versions.insert(id);
    return true;
  }
  bool path(const QJsonValue &value, bool privateAllowed = false) {
    QString relative;
    if (!check() || !identifier(value, relative, false) || QDir::cleanPath(relative) != relative) return false;
    const auto parts = relative.split(QLatin1Char('/'));
    if (!(relative.startsWith(QLatin1String("artifacts/derived/")) && parts.size() >= 5) &&
        !(privateAllowed && relative.startsWith(QLatin1String(".paleo/capability-ops/")) && parts.size() >= 4)) return false;
    const auto absolute = managedpath::resolveManagedPath(root, relative);
    if (absolute.isEmpty() || QFileInfo(absolute).isDir()) return false;
    const auto directory = identity(QFileInfo(absolute).absolutePath());
    // A file at the project root cannot be an operation-owned output directory.
    if (directory.isEmpty() || directory == identity(root)) return false;
    paths.insert(relative); result.canonicalRoots.insert(directory); return true;
  }
  bool staging(const QJsonValue &value) {
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    return path(object.value("path")) && version(object.value("expectedHead"), true);
  }
  bool stage(const QJsonObject &object) {
    if (object.isEmpty()) return false;
    if (!object.value("targets").isArray() || !object.value("parents").isArray()) return false;
    const auto targets = object.value("targets").toArray(); if (targets.size() > 64) return false;
    for (const auto &value : targets)
      if (!value.isObject() || !staging(value.toObject().value("staging"))) return false;
    for (const auto &value : object.value("parents").toArray()) if (!version(value)) return false;
    return true;
  }
  bool prepared(const QJsonObject &object) {
    if (object.isEmpty()) return true;
    if (object.value("schema") != QJsonValue(2) || !object.value("members").isArray()) return false;
    const auto members = object.value("members").toArray(); if (members.size() > 64) return false;
    for (const auto &value : members)
      if (!value.isObject() || !staging(value.toObject().value("staging"))) return false;
    return true;
  }
  bool artifacts(const QJsonObject &object) {
    if (object.isEmpty()) return true;
    if (object.value("schema") != QJsonValue(1) || !object.value("ports").isArray()) return false;
    const auto ports = object.value("ports").toArray(); if (ports.size() > 64) return false;
    for (const auto &value : ports)
      if (!value.isObject() || !path(value.toObject().value("path"), true)) return false;
    return true;
  }
  bool sourcePath(const QJsonObject &source) {
    if (source.contains("versionId") && !version(source.value("versionId"), true)) return false;
    QString path;
    if (!check() || !identifier(source.value("path"), path, false)) return false;
    const bool absolute = QDir::isAbsolutePath(path);
    const auto canonical = identity(absolute ? path : QDir(root).filePath(path));
    const auto canonicalRoot = identity(root);
    if (absolute && !canonical.startsWith(canonicalRoot + QLatin1Char('/'))) return true;
    const auto relative = absolute ? QDir(root).relativeFilePath(path) : path;
    const auto safe = managedpath::resolveManagedPath(root, relative);
    if (safe.isEmpty() || QDir::cleanPath(relative) != relative) return false;
    paths.insert(relative);
    const auto directory = identity(QFileInfo(safe).isDir() ? safe : QFileInfo(safe).absolutePath());
    result.canonicalRoots.insert(directory == canonicalRoot ? identity(safe) : directory);
    return true;
  }
  bool storedInput(const QJsonValue &value) {
    if(!value.isObject())return false;
    const auto ref=value.toObject();const auto digest=ref.value("manifestHash").toString();
    if(digest.size()!=64)return false;
    for(const auto c:digest)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    QString directory;if(!identifier(ref.value("directory"),directory,false) || QDir::cleanPath(directory)!=directory ||
        !directory.startsWith(QLatin1String(".paleo/capability-ops/")))return false;
    const auto relative=directory+"/manifest.json";
    if(!path(relative,true))return false;
    const QFileInfo manifest(managedpath::resolveManagedPath(root,relative));
    // Retain the whole private directory, including future content-addressed responses. No owner hashing.
    return manifest.isFile() && manifest.isReadable();
  }
  bool frozenSource(const QJsonValue &value) {
    if (!value.isObject()) return false;
    const auto request = value.toObject();
    if (!request.contains("plan")) return request.isEmpty();
    if (!request.value("plan").isObject()) return false;
    const auto plan = request.value("plan").toObject();
    if(plan.contains("storedInput") && !storedInput(plan.value("storedInput")))return false;
    for (const auto &field : {"sourceVerification","raster"}) {
      if (!plan.contains(field)) continue;
      if (!plan.value(field).isObject() || !sourcePath(plan.value(field).toObject())) return false;
    }
    return true;
  }
  bool record(const QJsonValue &value) {
    if (!check() || !value.isObject()) return false;
    const auto object = value.toObject(); QString id;
    if (object.value("schemaVersion") != QJsonValue(2)) {result.code = "operation_retention_unknown_schema"; return false;}
    for (const auto &field : {"documentId","callerScope","clientRequestId","key","stateRevision"})
      if (!object.value(field).isString() || object.value(field).toString().isEmpty()) return false;
    bool validRevision = false;
    if (!object.value("stateRevision").toString().toULongLong(&validRevision) || !validRevision ||
        !object.value("events").isArray() || object.value("events").toArray().size() > 128) return false;
    const auto key = QString::fromUtf8(QJsonDocument(QJsonArray{object.value("documentId"),object.value("callerScope"),
        object.value("clientRequestId")}).toJson(QJsonDocument::Compact));
    if (key != object.value("key").toString() || keys.contains(key)) return false;
    if (!identifier(object.value("operationId"), id, false) || operations.contains(id) ||
        !object.value("stagePlan").isObject() || !object.value("preparedManifest").isObject() ||
        !object.value("artifactManifest").isObject() || !object.value("frozenRequest").isObject() ||
        !object.value("presentationIntent").isObject() || !object.value("externalRequest").isObject() ||
        !object.value("planHash").isString() || object.value("planHash").toString().size() != 64) return false;
    if (object.value("planHash").toString() != operationPlanHash(object.value("frozenRequest").toObject(),
        object.value("stagePlan").toObject(), object.value("presentationIntent").toObject(),
        object.value("externalRequest").toObject())) {result.code = "operation_retention_plan_mismatch"; return false;}
    operations.insert(id); keys.insert(key);
    return stage(object.value("stagePlan").toObject()) && prepared(object.value("preparedManifest").toObject()) &&
      artifacts(object.value("artifactManifest").toObject()) && frozenSource(object.value("frozenRequest"));
  }
  OperationRetention finish() {
    result.paths = paths.values(); result.paths.sort();
    result.versionIds = versions.values(); result.versionIds.sort();
    if (!result.paths.isEmpty() || !result.versionIds.isEmpty()) {
      auto roots = result.canonicalRoots.values(); roots.sort();
      const QJsonObject snapshot{{"paths", QJsonArray::fromStringList(result.paths)},
        {"versions", QJsonArray::fromStringList(result.versionIds)}, {"roots", QJsonArray::fromStringList(roots)}};
      result.revision = QCryptographicHash::hash(QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    }
    result.complete = true; result.code = "ok"; return result;
  }
};
}
QString operationPlanHash(const QJsonObject &frozen, const QJsonObject &stage,
    const QJsonObject &presentation, const QJsonObject &external) {
  const QJsonObject value{{"frozen",frozen},{"stage",stage},{"presentation",presentation},{"external",external}};
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool OperationRetention::protectsPath(const QString &absolutePath) const {
  auto candidate = identity(absolutePath);
  while (!candidate.isEmpty()) {
    if (canonicalRoots.contains(candidate)) return true;
    const auto slash = candidate.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) break;
    candidate.truncate(slash);
  }
  return false;
}
OperationRetention operationRetention(const QString &projectRoot, const std::function<bool()> &cancelled) {
  Reader reader; reader.root = QFileInfo(projectRoot).canonicalFilePath(); reader.cancelled = cancelled;
  reader.result.code = "operation_retention_unavailable";
  if (!reader.check()) return reader.result;
  if (reader.root.isEmpty() || !QFileInfo(reader.root).isDir()) return reader.result;
  const auto file = managedpath::resolveManagedPath(reader.root, QStringLiteral(".paleo/capability-ops/operations.json"));
  if (file.isEmpty()) {reader.result.code = "operation_retention_unsafe_path"; return reader.result;}
  for (const auto &directory : {QStringLiteral(".paleo"), QStringLiteral(".paleo/capability-ops")}) {
    const QFileInfo info(QDir(reader.root).filePath(directory));
    if (info.exists() && (!info.isDir() || !info.isReadable())) return reader.result;
  }
  if (!QFileInfo::exists(file)) return reader.finish();
  QFile input(file);
  if (input.size() > maxJournalBytes) {reader.result.code = "operation_retention_budget_exceeded"; return reader.result;}
  if (!QFileInfo(file).isFile() || input.size() < 0 || !input.open(QIODevice::ReadOnly)) return reader.result;
  QByteArray bytes;
  while (!input.atEnd()) {
    if (!reader.check()) return reader.result;
    const auto chunk = input.read(qMin<qint64>(65536, maxJournalBytes - bytes.size() + 1));
    if (chunk.isEmpty() && input.error() != QFileDevice::NoError) return reader.result;
    bytes.append(chunk);
    if (bytes.size() > maxJournalBytes) {reader.result.code = "operation_retention_budget_exceeded"; return reader.result;}
  }
  const auto document = QJsonDocument::fromJson(bytes);
  if (!document.isArray()) return reader.result;
  for (const auto &value : document.array()) if (!reader.record(value)) return reader.result;
  return reader.finish();
}
}
