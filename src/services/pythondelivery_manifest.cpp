// 层：数据
#include "pythondelivery_internal.h"
#include "pythondelivery_files.h"
#include "pythonruntime.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>

namespace paleo::python::delivery {
namespace {
QString sha(const QByteArray &raw) {
  return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
}
bool entry(const QJsonObject &object, Entry &result) {
  const auto size = object.value("size").toDouble(-1);
  if (object.keys() != QStringList{"executable", "path", "sha256", "size"} ||
      !object.value("executable").isBool() || !std::isfinite(size) || size < 0 ||
      size > 1024LL * 1024 * 1024 || size != std::floor(size)) return false;
  result = {object.value("path").toString(), object.value("sha256").toString(),
            qint64(size), object.value("executable").toBool()};
  return safeRelative(result.path) && digest(result.sha256);
}
QString manifestName(const Header &header) {
  return header.kind == "runtime" ? "runtime-manifest.json" : "scripts-manifest.json";
}
bool compatible(const QJsonObject &object, const Header &header) {
  const auto sourceKey = header.kind == "runtime" ? "sourceLockSha256" : "sourceRuntimeLockSha256";
  return object.value("profile") == QLatin1String(ProfileId) &&
      object.value("pythonVersion") == QLatin1String(PythonVersion) &&
      object.value("numpyVersion") == QLatin1String(NumpyVersion) &&
      object.value("runtimeVersion") == "cpython-3.14.10-numpy-2.5.4-1" &&
      object.value(sourceKey).toString() == runtimeSourceLockSha256();
}
bool listedFiles(const QJsonObject &manifest, const Header &header) {
  auto rows = manifest.value(header.kind == "runtime" ? "files" : "scripts").toArray();
  if (header.kind == "scripts")
    for (const auto &row : manifest.value("licenses").toArray()) rows.append(row);
  if (rows.size() + 1 != header.entries.size()) return false;
  QHash<QString, Entry> entries;
  for (const auto &file : header.entries) entries.insert(file.path, file);
  QSet<QString> names;
  for (const auto &value : rows) {
    if (!value.isObject()) return false;
    const auto row = value.toObject();
    const auto name = row.value("path").toString();
    if (name == manifestName(header) || names.contains(name)) return false;
    names.insert(name);
    if (!entries.contains(name)) return false;
    const auto file = entries.value(name);
    if (row.value("sha256").toString() != file.sha256 ||
        row.value("size").toDouble(-1) != double(file.size) ||
        (header.kind == "runtime" ? row.value("executable").toBool() : false) != file.executable) return false;
  }
  return true;
}
bool verifyFile(const QString &path, const Entry &entry, const std::function<bool()> &cancelled) {
  QFile file(path);
  if (!openRegular(file, path) || file.size() != entry.size) return false;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 remaining = entry.size;
  while (remaining > 0) {
    if (cancelled && cancelled()) return false;
    const auto bytes = file.read(qMin<qint64>(remaining, 64 * 1024));
    if (bytes.isEmpty()) return false;
    hash.addData(bytes); remaining -= bytes.size();
  }
#ifndef Q_OS_WIN
  const auto executable = file.permissions().testAnyFlags(
      QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
  if (executable != entry.executable) return false;
#endif
  return file.atEnd() && QString::fromLatin1(hash.result().toHex()) == entry.sha256;
}
bool scriptRows(const QJsonArray &rows) {
  const QStringList ids{"attribute_correlation", "summary", "well_quality"};
  const QStringList hashes{QString::fromLatin1(PALEO_DELIVERY_CORRELATION_SHA),
                          QString::fromLatin1(PALEO_DELIVERY_SUMMARY_SHA), QString::fromLatin1(PALEO_DELIVERY_QUALITY_SHA)};
  if (rows.size() != ids.size()) return false;
  for (qsizetype index = 0; index < ids.size(); ++index) {
    const auto row = rows[index].toObject(); const auto &id = ids[index];
    const auto size = row.value("size").toDouble(-1);
    if (row.keys() != QStringList{"id", "path", "sha256", "size", "source"} ||
        row.value("id") != id || row.value("path") != id + ".py" || row.value("sha256") != hashes[index] ||
        row.value("source") != "vendor/agent-python/templates/" + id + ".py" ||
        !std::isfinite(size) || size <= 0 || size > 256 * 1024 || size != std::floor(size)) return false;
  }
  return true;
}
bool licenseRows(const QJsonObject &manifest) {
  const QSet<QString> allowed{"NOTICE.md", "COPYING", "COPYING.md", "COPYING.txt", "LICENSE", "LICENSE.md", "LICENSE.txt"};
  const auto rows = manifest.value("licenses").toArray();
  if (rows.isEmpty() || rows.size() > allowed.size()) return false;
  QSet<QString> seen; QString previous;
  for (const auto &value : rows) {
    const auto row = value.toObject(); const auto name = row.value("path").toString();
    const auto size = row.value("size").toDouble(-1);
    if (row.keys() != QStringList{"kind", "path", "sha256", "size", "source"} || !allowed.contains(name) ||
        seen.contains(name) || (!previous.isEmpty() && name <= previous) || row.value("source") != name ||
        row.value("kind") != (name == "NOTICE.md" ? "notice" : "license-text") || !digest(row.value("sha256").toString()) ||
        !std::isfinite(size) || size <= 0 || size > 256 * 1024 || size != std::floor(size)) return false;
    previous = name; seen.insert(name);
  }
  return seen.contains("NOTICE.md") && manifest.value("licenseStatus") ==
      (rows.size() > 1 ? "repository-texts-included" : "notice-only");
}
bool licenseText(const Header &header, const QString &root) {
  for (const auto &entry : header.entries) {
    if (entry.path.endsWith(".py") || entry.path == "scripts-manifest.json") continue;
    QFile input(QDir(root).filePath(entry.path));
    if (entry.size <= 0 || entry.size > 256 * 1024 || !openRegular(input, QDir(root).filePath(entry.path))) return false;
    const auto raw = input.read(256 * 1024 + 1);
    if (raw.size() != entry.size || raw.contains('\r') || raw.contains('\0')) return false;
    // A non-BOM prefix also preserves a legitimate leading U+FEFF when checking
    // the exact UTF-8 roundtrip; malformed sequences cannot roundtrip unchanged.
    const auto prefixed = QByteArray("x") + raw;
    if (QString::fromUtf8(prefixed).toUtf8() != prefixed) return false;
  }
  return true;
}
} // namespace

bool digest(const QString &value) {
  if (value.size() != 64 || value == QString(64, '0')) return false;
  for (const auto c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
bool parseHeader(const QByteArray &raw, Header &header) {
  const auto parsed = QJsonDocument::fromJson(raw);
  if (!parsed.isObject() || parsed.toJson(QJsonDocument::Compact) + '\n' != raw) return false;
  const auto object = parsed.object();
  if (object.keys() != QStringList{"entries", "kind", "licenseStatus", "manifestSha256", "platform", "schema"} ||
      object.value("schema") != "paleo.python-delivery.v1" || !object.value("entries").isArray()) return false;
  header.kind = object.value("kind").toString(); header.platform = object.value("platform").toString();
  header.manifestSha256 = object.value("manifestSha256").toString();
  header.licenseStatus = object.value("licenseStatus").toString();
  if (!digest(header.manifestSha256) ||
      !((header.kind == "runtime" && header.platform == runtimePlatform() && !header.platform.isEmpty() &&
         header.licenseStatus == "runtime-licenses-included") ||
        (header.kind == "scripts" && header.platform == "all" &&
         (header.licenseStatus == "notice-only" || header.licenseStatus == "repository-texts-included")))) return false;
  const auto rows = object.value("entries").toArray();
  if (rows.isEmpty() || rows.size() > 16384) return false;
  QSet<QString> folded, names; QByteArray previous;
  QHash<QString, QString> tree;
  bool manifestFound = false;
  for (const auto &value : rows) {
    Entry file;
    if (!value.isObject() || !entry(value.toObject(), file) ||
        (!previous.isEmpty() && file.path.toUtf8() <= previous) || folded.contains(file.path.toCaseFolded()) ||
        header.treeSize > MaxTree - file.size) return false;
    if (header.kind == "scripts" && (file.executable || file.path.contains('/'))) return false;
    if (file.path == manifestName(header)) {
      if (file.executable || file.size <= 0 || file.size > (header.kind == "scripts" ? 64 * 1024 : MaxHeader) ||
          file.sha256 != header.manifestSha256) return false;
      manifestFound = true;
    }
    previous = file.path.toUtf8(); folded.insert(file.path.toCaseFolded());
    names.insert(file.path); header.treeSize += file.size; header.entries.append(file);
  }
  for (const auto &name : names) {
    auto parent = name;
    tree.insert(name.toCaseFolded(), name);
    while (parent.contains('/')) {
      parent.truncate(parent.lastIndexOf('/'));
      if (folded.contains(parent.toCaseFolded())) return false;
      const auto key = parent.toCaseFolded();
      if (tree.contains(key) && tree.value(key) != parent) return false;
      tree.insert(key, parent);
      if (tree.size() > 49152) return false;
    }
  }
  return manifestFound && (header.kind != "scripts" || header.treeSize <= 4 * 1024 * 1024);
}
QString validateManifest(const Header &header, const QByteArray &raw) {
  if (sha(raw) != header.manifestSha256) return "python_manifest_changed";
  const auto parsed = QJsonDocument::fromJson(raw);
  if (!parsed.isObject() || !compatible(parsed.object(), header) || !listedFiles(parsed.object(), header))
    return "python_manifest_invalid";
  const auto object = parsed.object();
  if (header.kind == "runtime") {
    if (object.value("schema") != QLatin1String(RuntimeSchema) || object.value("platform") != header.platform)
      return "python_manifest_invalid";
  } else {
    const QStringList keys{"licenseStatus", "licenses", "numpyVersion", "profile", "pythonVersion", "runtimeVersion",
                           "schema", "scripts", "sourceRuntimeLockSha256", "treeSize", "version"};
    if (object.keys() != keys || parsed.toJson(QJsonDocument::Compact) + '\n' != raw ||
        object.value("schema") != "paleo.python-scripts.v1" || object.value("version") != 1 ||
        object.value("licenseStatus") != header.licenseStatus || !scriptRows(object.value("scripts").toArray()) ||
        !licenseRows(object) || object.value("treeSize").toDouble(-1) != double(header.treeSize - raw.size()))
      return "python_manifest_invalid";
  }
  return {};
}
QString verifyInstalled(const Header &header, const QString &root, const std::function<bool()> &cancelled) {
  if (!directory(root, false)) return "python_delivery_store_invalid";
  QSet<QString> expected;
  QByteArray manifest;
  for (const auto &file : header.entries) {
    if (!verifyFile(QDir(root).filePath(file.path), file, cancelled))
      return cancelled && cancelled() ? "cancelled" : "python_delivery_changed";
    expected.insert(file.path);
    if (file.path == manifestName(header)) {
      QFile input(QDir(root).filePath(file.path));
      if (file.size > MaxHeader || !openRegular(input, QDir(root).filePath(file.path))) return "python_manifest_invalid";
      manifest = input.read(MaxHeader + 1);
    }
  }
  QDirIterator files(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
  int visited = 0;
  while (files.hasNext()) {
    const auto path = files.next(); const auto info = files.fileInfo();
    if (++visited > 16384 * 3 || info.isSymLink() || info.isJunction() ||
        (!info.isDir() && !info.isFile())) return "python_delivery_changed";
    if (info.isFile() && !expected.remove(QDir(root).relativeFilePath(path))) return "python_delivery_changed";
  }
  if (!expected.isEmpty() || manifest.isEmpty()) return "python_manifest_invalid";
  const auto code = validateManifest(header, manifest);
  if (!code.isEmpty()) return code;
  if (header.kind == "scripts") return licenseText(header, root) ? QString() : QStringLiteral("python_scripts_notice_invalid");
  RuntimeProvider provider({root, header.manifestSha256, true});
  const auto result = provider.inspectIntegrity(cancelled);
  return result.ready() ? QString() : result.code;
}
} // namespace paleo::python::delivery
