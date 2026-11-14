// 层：数据
// catalog/catalogstore_publications.cpp — 借用既有连接，与成员版本共用提交事务。
#include "catalogstore.h"
#include "catalogstore_internal.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QUuid>

namespace paleo::catalog_detail {
bool ensurePublicationSchema(QSqlDatabase &db, QString *error) {
  if (!execSql(db, QStringLiteral(
      "CREATE TABLE IF NOT EXISTS paleo_operation_publications ("
      "document_id TEXT NOT NULL,operation_id TEXT NOT NULL,request_hash TEXT NOT NULL,"
      "output_set_id TEXT NOT NULL UNIQUE,manifest_hash TEXT NOT NULL,receipt_json TEXT NOT NULL,"
      "PRIMARY KEY(document_id,operation_id))"), error) ||
      !execSql(db, QStringLiteral("CREATE TABLE IF NOT EXISTS paleo_publication_meta "
                                 "(key TEXT PRIMARY KEY,value TEXT NOT NULL)"), error)) return false;
  QSqlQuery query(db);
  query.prepare(QStringLiteral("INSERT OR IGNORE INTO paleo_publication_meta(key,value) VALUES('generation',?)"));
  query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!query.exec()) { setError(error, query.lastError().text()); return false; }
  return true;
}

bool writePublicationGeneration(QSqlDatabase &db, const QString &generation, QString *error) {
  if (generation.isEmpty()) return true;
  if (QUuid(generation).isNull() || QUuid(generation).toString(QUuid::WithoutBraces) != generation) {
    setError(error, QStringLiteral("invalid_publication_generation")); return false;
  }
  QSqlQuery query(db);
  query.prepare(QStringLiteral("UPDATE paleo_publication_meta SET value=? WHERE key='generation'"));
  query.addBindValue(generation);
  if (!query.exec()) { setError(error, query.lastError().text()); return false; }
  return true;
}

bool loadPublicationRecords(QSqlDatabase &db, QVector<PublicationReceipt> *out, QString *generation, QString *error) {
  out->clear();
  generation->clear();
  QSqlQuery exists(db);
  if (!exists.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='paleo_operation_publications'"))) {
    setError(error, exists.lastError().text()); return false;
  }
  if (!exists.next()) return true; // Legacy databases carry no v2 proof.
  QSqlQuery meta(db);
  if (!meta.exec(QStringLiteral("SELECT value FROM paleo_publication_meta WHERE key='generation'")) ||
      !meta.next() || QUuid(meta.value(0).toString()).isNull()) {
    setError(error, QStringLiteral("publication_generation_missing")); return false;
  }
  *generation = meta.value(0).toString();
  QSqlQuery query(db);
  if (!query.exec(QStringLiteral("SELECT receipt_json FROM paleo_operation_publications ORDER BY document_id,operation_id"))) {
    setError(error, query.lastError().text()); return false;
  }
  while (query.next()) {
    PublicationReceipt receipt{QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object()};
    if (!receipt.valid(error)) return false;
    out->append(std::move(receipt));
  }
  return true;
}
} // namespace paleo::catalog_detail

PublicationLookup CatalogStore::publication(const QString &documentId, const QString &operationId,
                                             const QString &requestHash) const {
  PublicationLookup out;
  if (!m_open || m_connectionName.isEmpty()) return out;
  auto db = QSqlDatabase::database(m_connectionName, false);
  if (!db.isValid() || !db.isOpen()) return out;
  QSqlQuery exists(db);
  if (!exists.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='paleo_operation_publications'")))
    return out;
  if (!exists.next()) { out.code = QStringLiteral("legacy_unverifiable"); return out; }
  QSqlQuery meta(db);
  if (!meta.exec(QStringLiteral("SELECT value FROM paleo_publication_meta WHERE key='generation'")) ||
      !meta.next() || QUuid(meta.value(0).toString()).isNull()) return out;
  out.storeGeneration = meta.value(0).toString();
  QSqlQuery query(db);
  query.prepare(QStringLiteral("SELECT request_hash,output_set_id,manifest_hash,"
      "CASE WHEN length(receipt_json)<=65536 THEN receipt_json ELSE '' END "
      "FROM paleo_operation_publications WHERE document_id=? AND operation_id=?"));
  query.addBindValue(documentId); query.addBindValue(operationId);
  if (!query.exec()) return out;
  if (!query.next()) { out.code = QStringLiteral("not_committed"); return out; }
  out.receipt.value = QJsonDocument::fromJson(query.value(3).toString().toUtf8()).object();
  if (!out.receipt.valid() || out.receipt.documentId() != documentId || out.receipt.operationId() != operationId ||
      out.receipt.requestHash() != query.value(0).toString() || out.receipt.outputSetId() != query.value(1).toString() ||
      out.receipt.manifestHash() != query.value(2).toString()) {
    out.code = QStringLiteral("receipt_corrupt"); return out;
  }
  if (out.receipt.requestHash() != requestHash) { out.code = QStringLiteral("conflict"); return out; }
  for (const auto &entry : out.receipt.value.value("members").toArray()) {
    const auto member = entry.toObject();
    QSqlQuery row(db);
    row.prepare(QStringLiteral("SELECT asset_id,path,sha256,json_extract(extra_json,'$.outputSet.manifestHash') "
                               "FROM versions WHERE id=?"));
    row.addBindValue(member.value("versionId").toString());
    if (!row.exec()) return out;
    if (!row.next() || row.value(0).toString() != member.value("assetId").toString() ||
        row.value(1).toString() != member.value("path").toString() ||
        row.value(2).toString() != member.value("sha256").toString() ||
        row.value(3).toString() != out.receipt.manifestHash()) {
      out.code = QStringLiteral("receipt_corrupt"); return out;
    }
  }
  out.code = QStringLiteral("committed");
  return out;
}

bool CatalogStore::insertPublication(const PublicationReceipt &receipt, QString *error) {
  using paleo::catalog_detail::setError;
  if (!m_inTxn || !m_open || !m_writable || !receipt.valid(error)) {
    setError(error, QStringLiteral("publication_requires_member_transaction")); return false;
  }
  const auto before = publication(receipt.documentId(), receipt.operationId(), receipt.requestHash());
  if (before.code == QLatin1String("committed") && before.receipt == receipt) return true;
  if (before.code != QLatin1String("not_committed")) {
    setError(error, QStringLiteral("publication_conflict")); return false;
  }
  QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
  query.prepare(QStringLiteral("INSERT INTO paleo_operation_publications "
      "(document_id,operation_id,request_hash,output_set_id,manifest_hash,receipt_json) VALUES(?,?,?,?,?,?)"));
  query.addBindValue(receipt.documentId()); query.addBindValue(receipt.operationId());
  query.addBindValue(receipt.requestHash()); query.addBindValue(receipt.outputSetId());
  query.addBindValue(receipt.manifestHash());
  query.addBindValue(QString::fromUtf8(QJsonDocument(receipt.value).toJson(QJsonDocument::Compact)));
  if (!query.exec()) { setError(error, QStringLiteral("publication_conflict: ") + query.lastError().text()); return false; }
  return true;
}
