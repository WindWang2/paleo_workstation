// 层：数据
#include "datacatalog.h"
#include "catalogstore.h"
#include "../metadata/storeerrors_internal.h"
#include <QJsonArray>

PublicationLookup DataCatalog::publication(const QString &documentId, const QString &operationId,
                                            const QString &requestHash) const {
  if (!noteRead("publication") || !m_store || m_staging || m_primaryCorruptOnDisk)
    return {};
  return m_store->publication(documentId, operationId, requestHash);
}

bool DataCatalog::stagePublication(const PublicationReceipt &receipt, QString *error) {
  using paleo::store_detail::setError;
  if (!checkWriteThread("stagePublication", error) || !receipt.valid(error)) return false;
  if (m_staging || !m_store || m_lockedReadOnly || m_batchDepth != 1) {
    setError(error, QStringLiteral("publication_requires_output_batch")); return false;
  }
  const auto key = publicationKey(receipt.documentId(), receipt.operationId());
  const auto prior = m_publications.constFind(key);
  if (prior != m_publications.cend()) {
    if (*prior == receipt) return true;
    setError(error, QStringLiteral("publication_conflict")); return false;
  }
  for (const auto &entry : receipt.value.value("members").toArray()) {
    const auto member = entry.toObject();
    const auto id = member.value("versionId").toString();
    const auto version = versionById(id);
    if (!m_dirtyVersions.contains(id) || version.id != id ||
        version.assetId != member.value("assetId").toString() ||
        version.path != member.value("path").toString() || version.sha256 != member.value("sha256").toString() ||
        QJsonObject::fromVariantMap(version.extra.value("outputSet").toMap()) != receipt.value) {
      setError(error, QStringLiteral("publication_member_mismatch")); return false;
    }
  }
  m_publications.insert(key, receipt); m_dirtyPublications.insert(key);
  ++m_mutationSeq;
  return save(error);
}
