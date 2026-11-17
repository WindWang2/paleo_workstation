// 层：测试壳
#pragma once
#include "services/pythonenvironmentregistry.h"
#include "services/pythondelivery_internal.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace python_environment_fixture {
inline QString hash(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
inline QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'; }
inline bool write(const QString &path, const QByteArray &bytes, bool executable = false) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path); if (file.exists()) file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
  auto permissions = QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
  if (executable) permissions |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
  return file.setPermissions(permissions);
}
inline QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
struct Fixture {
  QTemporaryDir temporary;
  QString scriptsRoot, scriptsSha, error;
  explicit Fixture(const QString &templates) {
    using namespace paleo::python;
    scriptsRoot = temporary.filePath("scripts");
    const auto hashes = delivery::compiledScriptHashes(); QJsonArray rows;
    for (const auto &id : {QString("attribute_correlation"), QString("summary"), QString("well_quality")}) {
      const auto bytes = read(templates + '/' + id + ".py");
      if (bytes.isEmpty() || hash(bytes) != hashes.value(id) || !write(scriptsRoot + '/' + id + ".py", bytes)) { error = "compiled template source unavailable"; return; }
      rows.append(QJsonObject{{"id", id}, {"path", id + ".py"}, {"source", "vendor/agent-python/templates/" + id + ".py"},
                              {"size", bytes.size()}, {"sha256", hash(bytes)}});
    }
    const QByteArray notice("Internal fixture notice only. No redistribution grant.\n");
    if (!write(scriptsRoot + "/NOTICE.md", notice)) { error = "notice write failed"; return; }
    qint64 total = notice.size(); for (const auto &row : rows) total += row.toObject().value("size").toInt();
    const auto manifest = json({{"schema", "paleo.python-scripts.v1"}, {"version", 1}, {"profile", QLatin1String(ProfileId)},
        {"runtimeVersion", "cpython-3.14.10-numpy-2.5.4-1"}, {"pythonVersion", QLatin1String(PythonVersion)},
        {"numpyVersion", QLatin1String(NumpyVersion)}, {"sourceRuntimeLockSha256", runtimeSourceLockSha256()},
        {"scripts", rows}, {"licenses", QJsonArray{QJsonObject{{"path", "NOTICE.md"}, {"source", "NOTICE.md"}, {"kind", "notice"},
            {"size", notice.size()}, {"sha256", hash(notice)}}}}, {"licenseStatus", "notice-only"}, {"treeSize", double(total)}});
    scriptsSha = hash(manifest);
    if (!write(scriptsRoot + "/scripts-manifest.json", manifest)) error = "scripts manifest write failed";
  }
  paleo::python::EnvironmentCandidate candidate(const QString &name, const QByteArray &bytes = "fixture", bool trusted = true, bool generated = true) {
    using namespace paleo::python;
    const auto root = temporary.filePath(name);
#ifdef Q_OS_WIN
    const QString executable = "python.exe";
#else
    const QString executable = "bin/python3.14";
#endif
    if (!write(root + '/' + executable, bytes, true)) { error = "runtime fixture write failed"; return {}; }
    const auto manifest = json({{"schema", QLatin1String(RuntimeSchema)}, {"profile", QLatin1String(ProfileId)},
        {"runtimeVersion", "cpython-3.14.10-numpy-2.5.4-1"}, {"pythonVersion", QLatin1String(PythonVersion)},
        {"numpyVersion", QLatin1String(NumpyVersion)}, {"sourceLockSha256", runtimeSourceLockSha256()},
        {"platform", runtimePlatform()}, {"interpreter", executable}, {"treeSize", bytes.size()},
        {"files", QJsonArray{QJsonObject{{"path", executable}, {"size", bytes.size()}, {"sha256", hash(bytes)}, {"executable", true}}}}});
    if (!write(root + "/runtime-manifest.json", manifest)) { error = "runtime manifest write failed"; return {}; }
    ExecutionPolicy policy; policy.allowTrusted = trusted; policy.allowGenerated = generated;
    return {root, hash(manifest), scriptsRoot, scriptsSha, policy};
  }
  QString configurationPath() const { return temporary.filePath("configuration/agent-python.json"); }
};
} // namespace python_environment_fixture
