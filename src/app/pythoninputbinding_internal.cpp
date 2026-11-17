// 层：组装根
#include "pythoninputbinding_internal.h"
#include "workflow/documentsession.h"
#include "workflow/capabilityhost.h"
#include "catalog/datacatalog.h"
#include <QDir>
#include <QFileInfo>

namespace paleo::python::inputbinding {
bool live(const Scope &scope) {
  return scope.catalog && scope.document && scope.host && scope.catalog->isOpen() && scope.document->isOpen();
}
bool samePath(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
  return left.compare(right, Qt::CaseInsensitive) == 0;
#else
  return left == right;
#endif
}
QString checkScope(const Scope &scope, const host::CallContext &expected) {
  if (!live(scope)) return "document_unavailable";
  const auto current = scope.host->context();
  if (!live(scope)) return "document_unavailable";
  const auto code = host::checkContext(current, expected, host::Read | host::PrivateStage | host::LocalExecute);
  if (!code.isEmpty()) return code;
  if (scope.document->documentId() != expected.documentId) return "source_document_mismatch";
  return {};
}
QString modelRoot(const Scope &scope) {
  const auto project = QFileInfo(scope.catalog->projectDir()).canonicalFilePath();
  if (project.isEmpty()) return {};
  const auto model = QDir(project).filePath("artifacts/metadata/area-model");
  const QFileInfo directory(model), database(model + "/data.sqlite");
  if (!directory.isDir() || directory.isSymLink() || directory.isJunction() || !samePath(directory.canonicalFilePath(), model) ||
      !database.isFile() || database.isSymLink() || database.isJunction() || !samePath(database.canonicalFilePath(), model + "/data.sqlite")) return {};
  return directory.canonicalFilePath();
}
QString mappedSource(const Scope &scope, const document::SourceRef &reference,
                     const QString &model, document::SourceRecord &out, const QString &mappingError) {
  if (!live(scope)) return "document_unavailable";
  out = scope.document->source(reference);
  if (!live(scope)) return "document_unavailable";
  const auto version = scope.catalog->versionById(out.versionId);
  const auto asset = scope.catalog->assetById(out.assetId);
  if (out.ref != reference || out.versionId.isEmpty() || out.assetId.isEmpty() || version.id != out.versionId ||
      version.assetId != out.assetId || asset.id != out.assetId ||
      version.extra.value("areaModelSourceId").toLongLong() != reference.sourceId ||
      document::SourceFingerprint::fromStored(version.sha256).stored() != reference.fileRevisionId ||
      out.fingerprint.stored() != reference.fileRevisionId ||
      !samePath(QFileInfo(out.modelDirectory).canonicalFilePath(), model)) return mappingError;
  return {};
}
} // namespace paleo::python::inputbinding
