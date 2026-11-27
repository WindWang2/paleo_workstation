// 层：功能
#include "folderimport.h"
#include "io/dataimportservice.h"

#include <QElapsedTimer>
#include <QFileInfo>

namespace {
// Directory plans omit hashes for very large files to keep discovery cheap.
// Explicit attachments bind the detected type and well to the selected bytes.
void bindAttachmentHashes(QVector<FolderPreviewRow> &rows,
    const std::function<bool(int, const QString &)> &progress = {})
{
  QElapsedTimer elapsed;
  elapsed.start();
  for (auto &row : rows) {
    if (row.skipped || row.importBlocked || !row.sourceSha256.isEmpty()) continue;
    QString error;
    bool stopped = false;
    const auto cancelled = [&] {
      if (!progress || !elapsed.hasExpired(150)) return false;
      elapsed.restart();
      stopped = !progress(0, row.path);
      return stopped;
    };
    row.sourceSha256 = DataCatalog::sha256FileHex(row.path, &error, cancelled);
    if (stopped) return;
    if (row.sourceSha256.isEmpty()) {
      row.importBlocked = true;
      row.issues.append(error);
    }
  }
}
}

void FolderImportWorkflow::importAgentAttachments(const QStringList &paths,
    const ProjectGuard &projectStillCurrent, const ImportDone &done,
    const AttachmentReview &needsReview, const OriginalsReceived &received,
    const std::function<QString()> &identifySender)
{
  if (!projectStillCurrent || !projectStillCurrent()) {
    if (done) done({}, tr("工程已变化，请重新选择资料。"), false);
    return;
  }
  if (paths.size() == 1 && QFileInfo(paths.first()).isDir()) {
    receiveOriginalPackage(paths.first(), projectStillCurrent,
      [this, received, done, needsReview, projectStillCurrent](const QStringList &files,
          qsizetype reused, const QString &error, bool cancelled) {
        const QPointer<FolderImportWorkflow> alive(this);
        if (received) received(files, reused, error, cancelled);
        if (!alive) return;
        if (files.isEmpty() || cancelled || !projectStillCurrent()) {
          if (done) done({}, error, cancelled);
        } else {
          importAgentAttachments(files, projectStillCurrent, done, needsReview);
        }
      }, identifySender);
    return;
  }
  QStringList files;
  QVector<FolderRowResult> declined;
  for (const auto &path : paths) {
    const QFileInfo file(path);
    if (file.isAbsolute() && file.isFile() && file.isReadable() && file.size() > 0) {
      files.append(file.absoluteFilePath());
    } else {
      FolderRowResult row;
      row.path = path;
      row.outcome = FolderRowResult::Outcome::Failed;
      row.message = tr("请选择可读取的非空本地文件。");
      declined.append(row);
    }
  }
  if (files.isEmpty()) {
    if (done) done(declined, {}, false);
    return;
  }
  previewAsync(tr("扫描所选文件"),
    [files](ImportSession &session, QString *error, const auto &progress) {
      auto rows = DataImportService::producePreviewForFiles(session, files, error, progress);
      bindAttachmentHashes(rows, progress);
      return rows;
    },
    [this, files](QString *error) {
      auto rows = m_svc->previewFiles(files, error);
      bindAttachmentHashes(rows);
      return rows;
    },
    [this, projectStillCurrent, done, needsReview, declined](
        const QVector<FolderPreviewRow> &preview, const QString &error) mutable {
      if (!error.isEmpty() || !projectStillCurrent()) {
        if (done) done(declined, error.isEmpty() ? tr("工程已变化，请重新选择资料。") : error, false);
        return;
      }
      QStringList ready;
      QMap<QString, QString> types, wells, hashes;
      QVector<FolderPreviewRow> review;
      for (const auto &item : preview) {
        FolderRowResult row;
        row.path = item.path;
        row.classifiedType = item.classifiedType;
        if (item.importBlocked || item.skipped) {
          row.outcome = item.importBlocked ? FolderRowResult::Outcome::Failed : FolderRowResult::Outcome::Skipped;
          row.message = item.importBlocked ? item.issues.join(QStringLiteral("\n")) : item.skipReason;
        } else if (item.decision != QLatin1String("skip") && (item.needsWellChoice ||
                   item.needsImportReview || (item.typeEditable && item.classifiedType == QLatin1String("unknown")))) {
          review.append(item);
          continue;
        } else {
          ready.append(item.path);
          types.insert(item.path, item.displayType);
          hashes.insert(item.path, item.sourceSha256);
          if (!item.suggestedWellId.isEmpty()) wells.insert(item.path, item.suggestedWellId);
          continue;
        }
        declined.append(row);
      }
      const auto finish = [declined, review, done, needsReview, projectStillCurrent](
          const QVector<FolderRowResult> &rows, const QString &err, bool cancelled) {
        if (done) done(rows + declined, err, cancelled);
        if (!cancelled && err.isEmpty() && !review.isEmpty() && needsReview && projectStillCurrent()) needsReview(review);
      };
      if (ready.isEmpty()) finish({}, {}, false);
      else importFiles(ready, types, {}, finish, wells, hashes, {}, ready, {}, {}, projectStillCurrent);
    }, [declined, done] {
      if (done) done(declined, {}, true);
    });
}
