// 层：数据
#include "publicationreceipt.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

QString publicationKey(const QString &documentId, const QString &operationId) {
  return QString::fromUtf8(QJsonDocument(QJsonArray{documentId, operationId}).toJson(QJsonDocument::Compact));
}

QString PublicationReceipt::hash(const QJsonObject &input) {
  auto value = input;
  value.remove("manifestHash");
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                                                    QCryptographicHash::Sha256).toHex());
}

bool PublicationReceipt::valid(QString *error) const {
  const auto fail = [&](const QString &why) { if (error) *error = why; return false; };
  if (value.value("schema") != QJsonValue(2) || value.value("ready") != QJsonValue(true) ||
      value.value("code") != QJsonValue("committed") ||
      QJsonDocument(value).toJson(QJsonDocument::Compact).size() > 64 * 1024)
    return fail(QStringLiteral("invalid_receipt_schema"));
  for (const auto &key : {"documentId", "operationId", "outputSetId", "algorithmId"}) {
    if (!value.value(key).isString() || value.value(key).toString().isEmpty() ||
        value.value(key).toString().toUtf8().size() > 256)
      return fail(QStringLiteral("invalid_receipt_identity"));
  }
  static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
  for (const auto &key : {"requestHash", "planHash", "manifestHash"})
    if (!sha.match(value.value(key).toString()).hasMatch())
      return fail(QStringLiteral("invalid_receipt_hash"));
  if (hash(value) != manifestHash()) return fail(QStringLiteral("receipt_hash_mismatch"));
  const auto members = value.value("members").toArray();
  if (members.isEmpty() || members.size() > 64) return fail(QStringLiteral("invalid_receipt_members"));
  QSet<QString> ports, assets, versions, paths;
  for (const auto &entry : members) {
    const auto member = entry.toObject();
    const auto port = member.value("portId").toString();
    const auto asset = member.value("assetId").toString();
    const auto version = member.value("versionId").toString();
    const auto path = member.value("path").toString();
    if (port.isEmpty() || asset.isEmpty() || version.isEmpty() || port.size() > 256 || asset.size() > 256 ||
        version.size() > 256 || path.isEmpty() || path.size() > 2048 || QDir::isAbsolutePath(path) ||
        path.contains('\\') || path != QDir::cleanPath(path) || path == QLatin1String("..") ||
        path.startsWith(QLatin1String("../")) || !sha.match(member.value("sha256").toString()).hasMatch() ||
        ports.contains(port) || assets.contains(asset) || versions.contains(version) || paths.contains(path))
      return fail(QStringLiteral("invalid_receipt_member"));
    ports.insert(port); assets.insert(asset); versions.insert(version); paths.insert(path);
  }
  if (error) error->clear();
  return true;
}
