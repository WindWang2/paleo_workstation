// 层：数据
// catalog/publicationreceipt.h — 单 Catalog 同事务提交的不可变业务凭证。
#pragma once
#include <QJsonObject>
#include <QString>

struct PublicationReceipt {
  QJsonObject value;
  QString documentId() const { return value.value("documentId").toString(); }
  QString operationId() const { return value.value("operationId").toString(); }
  QString requestHash() const { return value.value("requestHash").toString(); }
  QString outputSetId() const { return value.value("outputSetId").toString(); }
  QString manifestHash() const { return value.value("manifestHash").toString(); }
  bool valid(QString *error = nullptr) const;
  static QString hash(const QJsonObject &value);
  bool operator==(const PublicationReceipt &) const = default;
};

struct PublicationLookup {
  // committed / not_committed / conflict / receipt_corrupt / unavailable /
  // legacy_unverifiable. Absence is only authoritative after an actual SQL read.
  QString code = QStringLiteral("unavailable");
  PublicationReceipt receipt;
  QString storeGeneration;
};

QString publicationKey(const QString &documentId, const QString &operationId);
