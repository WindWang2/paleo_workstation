// 层：数据
#include "domain/shaequals.h"
#include "externalsourceverification.h"
#include "datacatalog.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCryptographicHash>

namespace {
QString pathKey(const QString &path) {
  const QFileInfo file(path);
  auto key = QDir::cleanPath(file.canonicalFilePath().isEmpty() ? file.absoluteFilePath() : file.canonicalFilePath());
#ifdef Q_OS_WIN
  key = key.toCaseFolded();
#endif
  return key;
}
}

bool verifyExternalSourceFile(const QString &project, const CatalogVersion &version,
                             QString *error, const std::function<bool()> &cancelled,
                             bool *mismatched,
                             const std::function<void(qint64, qint64)> &progress) {
  if (error) error->clear();
  if (mismatched) *mismatched = false;
  const auto fail = [error](const QString &why) { if (error) *error = why; return false; };
  if (cancelled && cancelled()) return fail(QStringLiteral("已取消"));
  if (version.managed || version.sha256.isEmpty()) return true;
  if (!version.sha256.startsWith(QLatin1String("quick:"))) {
    QFile file(version.path);
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray bytes(64 * 1024, Qt::Uninitialized);
    qint64 done = 0, reported = 0;
    if (progress) progress(0, file.size());
    for (;;) {
      if (cancelled && cancelled()) return fail(QStringLiteral("已取消"));
      const auto count = file.read(bytes.data(), bytes.size());
      if (count < 0) return fail(file.errorString());
      if (!count) break;
      hash.addData(QByteArrayView(bytes.constData(), count)); done += count;
      if (progress && done - reported >= 1024 * 1024) { progress(done, file.size()); reported = done; }
    }
    if (progress) progress(done, file.size());
    if (cancelled && cancelled()) return fail(QStringLiteral("已取消"));
    const auto actual = QString::fromLatin1(hash.result().toHex());
    if (actual.compare(version.sha256, Qt::CaseInsensitive) == 0) return true;
    if (mismatched) *mismatched = true;
    return fail(QStringLiteral("源文件与入库时的 SHA-256 不一致"));
  }

  // A quick fingerprint without its model identity must fail closed. Never fall
  // back to reading all amplitudes, nor accept an arbitrary quick: string.
  const auto sourceId = version.extra.value("areaModelSourceId").toLongLong();
  if (project.isEmpty() || sourceId <= 0)
    return fail(QStringLiteral("快速指纹缺少工区模型来源，无法校验"));
  paleo::data::DataService service;
  std::string detail;
  const auto root = QDir(project).filePath("artifacts/metadata/area-model");
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), detail))
    return fail(QString::fromUtf8(detail));
  std::vector<paleo::data::SourceIndexEntry> sources;
  if (!service.listSourceIndexEntries(sources, detail)) return fail(QString::fromUtf8(detail));
  for (const auto &source : sources) {
    if (source.id != sourceId) continue;
    std::string actualPath;
    if (!service.sourcePath(sourceId, actualPath, detail)) return fail(QString::fromUtf8(detail));
    if (!paleo::domain::shaEquals(QString::fromUtf8(source.sha256), version.sha256) ||
        pathKey(QString::fromUtf8(actualPath)) != pathKey(version.path)) {
      if (mismatched) *mismatched = true;
      return fail(QStringLiteral("资料版本与工区模型来源不一致，请刷新资料索引"));
    }
    if (cancelled && cancelled()) return fail(QStringLiteral("已取消"));
    const bool ok = service.verifySource(sourceId, detail);
    if (cancelled && cancelled()) return fail(QStringLiteral("已取消"));
    if (!ok) {
      const auto why = QString::fromUtf8(detail);
      if (mismatched) *mismatched = why.startsWith(QStringLiteral("来源文件已改变"));
      return fail(why);
    }
    return true;
  }
  return fail(QStringLiteral("工区模型中不存在该资料来源"));
}
