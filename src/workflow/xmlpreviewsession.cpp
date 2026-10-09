// 层：功能
#include "xmlpreviewsession.h"
#include "../catalog/datacatalog.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QtConcurrent>

void XmlPreviewSession::open(const QString &path, const QString &expectedSha)
{
  if (m_verificationCancelled) m_verificationCancelled->store(true);
  m_verificationCancelled = std::make_shared<std::atomic_bool>(false);
  const auto cancelled = m_verificationCancelled;
  const auto generation = ++m_generation;
  // 会话独占副本：摘要和解析都只看这份，不回读调用方路径。
  m_directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/paleo-xml-XXXXXX"));
  const auto directory = m_directory;
  auto *watcher = new QFutureWatcher<XmlPreviewData>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
    const auto data = watcher->result();
    watcher->deleteLater();
    if (generation == m_generation) emit ready(data);
  });
  watcher->setFuture(QtConcurrent::run([path, expectedSha, cancelled, directory] {
    XmlPreviewData data;
    const auto refuse = [&data](const QString &reason) {
      data.chartError = reason;
      data.table.error = reason;
      return data;
    };
    const QFileInfo info(path);
    if (!directory || !directory->isValid())
      return refuse(QStringLiteral("无法创建预览临时目录"));
    if (!info.isFile() || info.isSymLink())
      return refuse(QStringLiteral("不是普通文件：%1").arg(path));
    const QString suffix = info.suffix();
    const QString copyName = suffix.isEmpty() ? QStringLiteral("source")
                                               : QStringLiteral("source.") + suffix;
    const QString copyPath = directory->filePath(copyName);
    if (!QFile::copy(info.absoluteFilePath(), copyPath))
      return refuse(QStringLiteral("无法读取源文件：%1").arg(path));
    if (!expectedSha.isEmpty()) {
      QString hashError;
      const QString actual = DataCatalog::sha256FileHex(copyPath, &hashError, [cancelled] {
        return cancelled->load();
      });
      if (cancelled->load()) return XmlPreviewData{};
      if (actual.isEmpty())
        return refuse(hashError.isEmpty() ? QStringLiteral("无法读取源文件：%1").arg(path) : hashError);
      if (actual.compare(expectedSha, Qt::CaseInsensitive) != 0)
        return refuse(QStringLiteral("源文件与入库时的 SHA-256 不一致，请重新导入"));
    }
    data.chartOk = PreviewDocService::wellCompositeAt(copyPath, &data.chart, &data.chartError);
    data.table = PreviewDocService::xmlDataListAt(copyPath);
    return data;
  }));
}
