// 层：数据
#include "mapsnapshot.h"
#include "managedpath.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QScopeGuard>

QString mapSnapshotHash(const QVariantMap &provenance) {
  auto data = provenance; data.remove("snapshot_hash");
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(data).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
void discardPreparedMapSnapshot(const PreparedMapSnapshot &snapshot) {
  if (!snapshot.committed && !snapshot.directory.isEmpty()) QDir(snapshot.directory).removeRecursively();
}
PreparedMapSnapshot prepareMapSnapshot(const MapSnapshotInput &input,
    const std::function<bool()> &cancelled, const std::function<void(double)> &progress) {
  PreparedMapSnapshot out; out.input = input;
  const auto stop = [&] {
    if (cancelled && cancelled()) { out.cancelled = true; out.error = QStringLiteral("已取消"); return true; }
    return false;
  };
  if (!input.valid || stop()) { if (out.error.isEmpty()) out.error = QStringLiteral("地图快照输入无效"); return out; }
  const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (input.operation == "publish") {
    out.finalDirectory = QDir(input.projectDirectory).filePath(QStringLiteral("result/%1/v%2").arg(input.horizon).arg(input.versionNumber));
    out.directory = out.finalDirectory + ".staging-" + token;
  } else out.directory = QDir(input.projectDirectory).filePath("artifacts/map-versions/" + token);
  if (!QDir().mkpath(out.directory)) { out.error = QStringLiteral("无法创建地图快照目录"); return out; }
  bool completed = false;
  const QString cleanupPath = out.directory;
  const auto cleanup = qScopeGuard([&] { if (!completed) QDir(cleanupPath).removeRecursively(); });
  out.provenance = QJsonDocument::fromJson(input.provenanceJson.toUtf8()).toVariant().toMap();
  QVariantList savedLayers; QVariantMap savedPdf;
  QHash<QString, QVariantMap> copied;
  for (int n = 0; n < input.files.size(); ++n) {
    if (stop()) return out;
    auto record = input.files[n].toMap(); const auto source = record.value("source").toString();
    if (!QFileInfo(source).isFile()) {
      if (input.operation == "save") {
        if (!record.value("pdf").toBool()) {
          record.insert("missing", true); record.insert("original_source", source);
          record.remove("source"); savedLayers << record;
        }
        continue;
      }
      out.error = QStringLiteral("地图来源文件缺失：%1").arg(source); return out;
    }
    QString expected = record.value("sha256").toString();
    if (input.operation == "publish" && !record.value("legacy").toBool() && expected.size() != 64) {
      out.error = QStringLiteral("保存快照的来源指纹缺失：%1").arg(source); return out;
    }
    if (expected.size() != 64) expected = paleo::managedpath::sha256FileHex(source, &out.error, cancelled);
    if (expected.isEmpty() || stop()) {
      if (out.error.isEmpty()) out.error = QStringLiteral("来源指纹读取失败：%1").arg(source);
      return out;
    }
    const QString key = QFileInfo(source).canonicalFilePath() + QChar(0x1f) + expected;
    QVariantMap frozen = copied.value(key);
    if (frozen.isEmpty()) {
      const QString name = record.value("pdf").toBool() ? QStringLiteral("layout.pdf") :
          QString::number(n) + "_" + QString::fromLatin1(QCryptographicHash::hash(record.value("layer_id").toString().toUtf8(), QCryptographicHash::Sha256).toHex().left(16)) + "." + QFileInfo(source).suffix();
      const auto destination = QDir(out.directory).filePath(name);
      QFile reader(source); QSaveFile writer(destination);
      if (!reader.open(QIODevice::ReadOnly) || !writer.open(QIODevice::WriteOnly)) { out.error = QStringLiteral("无法打开地图快照文件：%1").arg(source); return out; }
      QCryptographicHash digest(QCryptographicHash::Sha256);
      while (!reader.atEnd()) {
        if (stop()) { writer.cancelWriting(); return out; }
        const auto bytes = reader.read(4 * 1024 * 1024);
        if (bytes.isEmpty() && reader.error() != QFileDevice::NoError) { out.error = reader.errorString(); return out; }
        if (writer.write(bytes) != bytes.size()) { out.error = writer.errorString(); return out; }
        digest.addData(bytes);
      }
      if (QString::fromLatin1(digest.result().toHex()) != expected) { out.error = QStringLiteral("快照复制期间来源内容变化：%1").arg(source); return out; }
      if (!writer.commit()) { out.error = writer.errorString(); return out; }
      const auto verified = paleo::managedpath::sha256FileHex(destination, &out.error, cancelled);
      if (verified != expected || stop()) { if (out.error.isEmpty()) out.error = QStringLiteral("目标快照指纹不一致"); return out; }
      QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::ReadUser | QFileDevice::ReadGroup | QFileDevice::ReadOther);
      const QFileInfo info(destination);
      out.attestations << QVariantMap{{"path", destination}, {"sha256", expected}, {"size", info.size()}, {"modified", info.lastModified().toMSecsSinceEpoch()}};
      frozen = {{"path", QDir(input.projectDirectory).relativeFilePath(destination)}, {"sha256", expected}};
      copied.insert(key, frozen);
    }
    record.remove("source"); record.remove("pdf");
    record.insert("path", frozen.value("path")); record.insert("sha256", expected);
    if (input.files[n].toMap().value("pdf").toBool()) savedPdf = record; else savedLayers << record;
    if (progress) progress(95.0 * (n + 1) / qMax<qsizetype>(1, input.files.size()));
  }
  if (input.operation == "save") {
    // #439：与 saveVersion 对齐——仅当调用方传入图层清单时标记不可变快照。
    if (input.freezeLayers) {
      out.provenance.insert("snapshot_schema", "paleo_map_snapshot_v1");
      out.provenance.insert("saved_layers", savedLayers); out.provenance.insert("saved_pdf", savedPdf);
      out.provenance.insert("snapshot_hash", mapSnapshotHash(out.provenance));
    }
  } else {
    QSaveFile manifest(QDir(out.directory).filePath("evidence-snapshot.json"));
    const auto bytes = QJsonDocument::fromVariant(out.provenance).toJson(QJsonDocument::Indented);
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(bytes) != bytes.size() || !manifest.commit()) { out.error = manifest.errorString(); return out; }
    QFile::setPermissions(manifest.fileName(), QFileDevice::ReadOwner | QFileDevice::ReadUser | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    const auto manifestHash = paleo::managedpath::sha256FileHex(manifest.fileName(), &out.error, cancelled);
    if (manifestHash.isEmpty() || stop()) return out;
    const QFileInfo info(manifest.fileName());
    out.attestations << QVariantMap{{"path", info.absoluteFilePath()}, {"sha256", manifestHash},
        {"size", info.size()}, {"modified", info.lastModified().toMSecsSinceEpoch()}};
  }
  if (progress) progress(100);
  completed = true;
  return out;
}
bool verifyPreparedMapSnapshot(const PreparedMapSnapshot &snapshot, QString *error) {
  const auto reject = [&](const QString &message) { if (error) *error = message; return false; };
  if (!snapshot.isValid()) return reject(snapshot.error.isEmpty() ? QStringLiteral("地图快照未完成") : snapshot.error);
  for (const auto &entry : snapshot.attestations) {
    const auto record = entry.toMap(); const QFileInfo file(record.value("path").toString());
    if (!file.isFile() || file.isSymLink())
      return reject(QStringLiteral("登记前地图快照已经变化：%1").arg(file.filePath()));
    // sha256 是内容判据且必须最先执行：size/mtime 门的精度随文件系统而异
    // （mtime 秒级/纳秒级差一点就抢先报「已经变化」），而同大小、保留 mtime
    // 的就地篡改（字节翻转、同位宽改写）只有摘要能发现——prepare 已把 sha256
    // 记进 attestation，登记前必须真比（commit 之间才是快照的可信边界）。
    const QString expected = record.value("sha256").toString();
    if (expected.size() != 64)
      return reject(QStringLiteral("地图快照登记项缺少 SHA-256：%1").arg(file.filePath()));
    QString hashErr;
    const QString actual = paleo::managedpath::sha256FileHex(file.filePath(), &hashErr);
    if (actual.isEmpty())
      return reject(hashErr.isEmpty()
                      ? QStringLiteral("地图快照指纹读取失败：%1").arg(file.filePath())
                      : hashErr);
    if (actual.compare(expected, Qt::CaseInsensitive) != 0)
      return reject(QStringLiteral("登记前地图快照内容已变化（SHA-256 不一致）：%1")
                      .arg(file.filePath()));
    // 摘要一致后，size/mtime 作为廉价补充门（挡 attestation 自身被改）。
    if (file.size() != record.value("size").toLongLong() ||
        file.lastModified().toMSecsSinceEpoch() != record.value("modified").toLongLong())
      return reject(QStringLiteral("登记前地图快照已经变化：%1").arg(file.filePath()));
  }
  return true;
}
