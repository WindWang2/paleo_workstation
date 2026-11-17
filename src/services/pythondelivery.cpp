// 层：数据
#include "pythondelivery_internal.h"
#include "pythondelivery_files.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QUuid>
#include <QtEndian>

namespace paleo::python {
namespace {
using namespace delivery;
bool readExact(QFile &source, QByteArray &bytes, qint64 size, QCryptographicHash &hash) {
  bytes = source.read(size);
  if (bytes.size() != size) return false;
  hash.addData(bytes); return true;
}
bool readHeader(QFile &source, Header &header, QCryptographicHash &hash) {
  QByteArray magic, length, raw;
  if (!readExact(source, magic, 9, hash) || magic != "PALEOPY1\n" ||
      !readExact(source, length, 4, hash)) return false;
  const auto size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(length.constData()));
  return size > 0 && size <= MaxHeader && readExact(source, raw, size, hash) && parseHeader(raw, header) &&
      source.size() == 13 + qint64(size) + header.treeSize;
}
bool writeEntry(QFile &source, const Entry &entry, const QString &stage,
                QCryptographicHash &packageHash, const std::function<bool()> &cancelled) {
  const auto target = QDir(stage).filePath(entry.path);
  if (!directory(QFileInfo(target).absolutePath(), true)) return false;
  QFile output(target);
  if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !regularHandle(output)) return false;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 remaining = entry.size;
  while (remaining > 0) {
    if (cancelled && cancelled()) return false;
    const auto bytes = source.read(qMin<qint64>(remaining, 64 * 1024));
    if (bytes.isEmpty() || output.write(bytes) != bytes.size()) return false;
    hash.addData(bytes); packageHash.addData(bytes); remaining -= bytes.size();
  }
  if (QString::fromLatin1(hash.result().toHex()) != entry.sha256) return false;
  auto mode = QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
  if (entry.executable) mode |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
  return output.setPermissions(mode) && syncFile(output);
}
void removeStage(const QString &stage, const QString &generations) {
  const QFileInfo info(stage);
  if (!info.exists() || info.isSymLink() || info.isJunction() ||
      info.absolutePath() != generations ||
      !(info.fileName().startsWith(".stage-") || info.fileName().startsWith(".scripts-stage-"))) return;
  QFile::setPermissions(stage, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QDirIterator items(stage, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
  while (items.hasNext()) {
    const auto path = items.next(); const auto child = items.fileInfo();
    if (!child.isSymLink() && !child.isJunction() && (child.isDir() || regular(path)))
      QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    (child.isDir() ? QFileDevice::ExeOwner : QFileDevice::Permissions{}));
  }
  QDir(stage).removeRecursively();
}
bool sealDirectories(const QString &root) {
#ifndef Q_OS_WIN
  const auto mode = QFileDevice::ReadOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
      QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
  QDirIterator directories(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                           QDirIterator::Subdirectories);
  while (directories.hasNext())
    if (!QFile::setPermissions(directories.next(), mode)) return false;
  return QFile::setPermissions(root, mode);
#else
  Q_UNUSED(root)
  return true;
#endif
}
bool syncTree(const QString &root) {
  QStringList paths{root};
  QDirIterator directories(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                           QDirIterator::Subdirectories);
  while (directories.hasNext()) paths.append(directories.next());
  for (auto it = paths.crbegin(); it != paths.crend(); ++it)
    if (!syncDirectory(*it)) return false;
  return true;
}
QString selectionPath(const Header &header, const QString &store) {
  return QDir(store).filePath(header.kind == "runtime" ? "current-" + header.platform + ".json" : "current.json");
}
bool selectionSafe(const Header &header, const QString &path) {
  if (!QFileInfo::exists(path)) return !QFileInfo(path).isSymLink() && !QFileInfo(path).isJunction();
  QFile input(path);
  if (!openRegular(input, path) || input.size() > 64 * 1024) return false;
  const auto bytes = input.read(64 * 1024 + 1);
  const auto parsed = QJsonDocument::fromJson(bytes);
  if (!parsed.isObject() || parsed.toJson(QJsonDocument::Compact) + '\n' != bytes) return false;
  const auto object = parsed.object();
  const auto anchor = object.value("manifestSha256").toString();
  if (!digest(anchor) || object.value("generation").toString() != "generations/" + anchor) return false;
  if (header.kind == "scripts")
    return object.keys() == QStringList{"generation", "manifestSha256", "schema", "version"} &&
        object.value("schema") == "paleo.python-scripts.selection.v1" && object.value("version") == 1;
  return object.keys() == QStringList{"generation", "manifestSha256", "platform", "runtimeVersion", "schema"} &&
      object.value("schema") == "paleo.python-runtime.selection.v1" && object.value("platform") == header.platform &&
      object.value("runtimeVersion") == "cpython-3.14.10-numpy-2.5.4-1";
}
bool updateSelection(const Header &header, const QString &store) {
  const auto path = selectionPath(header, store);
  if (!selectionSafe(header, path)) return false;
  QJsonObject selection{{"generation", "generations/" + header.manifestSha256},
                        {"manifestSha256", header.manifestSha256}};
  if (header.kind == "scripts") {
    selection.insert("schema", "paleo.python-scripts.selection.v1"); selection.insert("version", 1);
  } else {
    selection.insert("schema", "paleo.python-runtime.selection.v1");
    selection.insert("platform", header.platform);
    selection.insert("runtimeVersion", "cpython-3.14.10-numpy-2.5.4-1");
  }
  const auto bytes = QJsonDocument(selection).toJson(QJsonDocument::Compact) + '\n';
  QSaveFile output(path); output.setDirectWriteFallback(false);
  return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
QString installPayload(QFile &source, const Header &header, const DeliveryRequest &request,
                       QCryptographicHash &hash, DeliveryResult &result,
                       const std::function<bool()> &cancelled) {
  // Validate caller spelling before Qt normalization can discard a Windows
  // device/UNC prefix. Only an already checked local path may be normalized.
  if (!directory(request.storeRoot, true)) return "python_delivery_store_invalid";
  const auto store = QDir::fromNativeSeparators(request.storeRoot);
  if (!storeEntries(header, store)) return "python_delivery_store_invalid";
  StoreLock lock(QDir(store).filePath(header.kind == "runtime" ? ".prepare.lock" : ".scripts.lock"));
  if (!lock.locked()) return "python_delivery_store_busy";
  if (!storeEntries(header, store)) return "python_delivery_store_invalid";
  const auto generations = QDir(store).filePath("generations");
  if (!directory(generations, true) || !selectionSafe(header, selectionPath(header, store)))
    return "python_delivery_store_invalid";
  const QStorageInfo storage(store);
  if (!storage.isValid() || !storage.isReady() || storage.isReadOnly() ||
      storage.bytesAvailable() < header.treeSize + MaxHeader) return "python_delivery_space_insufficient";
  const auto prefix = header.kind == "runtime" ? ".stage-" : ".scripts-stage-";
  const auto suffix = QUuid::createUuid().toString(QUuid::Id128);
  const auto stage = QDir(generations).filePath(QString(prefix) + suffix);
  if (!QDir().mkdir(stage, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
    return "python_delivery_write_failed";
  const auto cleanup = qScopeGuard([&] { removeStage(stage, generations); });
  for (const auto &file : header.entries)
    if (!writeEntry(source, file, stage, hash, cancelled))
      return cancelled && cancelled() ? "cancelled" : "python_delivery_changed";
  if (!source.atEnd() || QString::fromLatin1(hash.result().toHex()) != request.packageSha256)
    return "python_delivery_anchor_mismatch";
  auto code = verifyInstalled(header, stage, cancelled);
  if (!code.isEmpty()) return code;
  const auto generation = QDir(generations).filePath(header.manifestSha256);
  result.alreadyInstalled = QFileInfo::exists(generation);
  if (result.alreadyInstalled) {
    code = verifyInstalled(header, generation, cancelled);
    if (!code.isEmpty()) return code;
  } else if (!sealDirectories(stage) || !syncTree(stage) || !QDir().rename(stage, generation))
    return "python_delivery_write_failed";
  if (cancelled && cancelled()) return "cancelled";
  if (!syncDirectory(generation) || !syncDirectory(generations) || !syncDirectory(store))
    return "python_delivery_write_failed";
  result.root = generation;
  if (!updateSelection(header, store)) return "python_delivery_selection_failed";
  if (!syncDirectory(store)) return "python_delivery_commit_uncertain";
  return "ok";
}
} // namespace

DeliveryResult installDelivery(const DeliveryRequest &request, const std::function<bool()> &cancelled) {
  DeliveryResult result;
  if (!delivery::digest(request.packageSha256)) { result.code = "python_delivery_anchor_required"; return result; }
  if (request.storeRoot.isEmpty() || !QDir::isAbsolutePath(request.storeRoot)) {
    result.code = "python_delivery_store_invalid"; return result;
  }
  if (cancelled && cancelled()) { result.code = "cancelled"; return result; }
  QFile input(request.packagePath);
  if (!delivery::openRegular(input, request.packagePath) ||
      input.size() > delivery::MaxTree + delivery::MaxHeader + 13) return result;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  delivery::Header header;
  if (!readHeader(input, header, hash)) return result;
  result.kind = header.kind; result.manifestSha256 = header.manifestSha256; result.licenseStatus = header.licenseStatus;
  result.code = installPayload(input, header, request, hash, result, cancelled);
  return result;
}
} // namespace paleo::python
