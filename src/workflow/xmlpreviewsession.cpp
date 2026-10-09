// 层：功能
#include "xmlpreviewsession.h"
#include <QFutureWatcher>
#include <QFile>
#include <QCryptographicHash>
#include <QtConcurrent>

void XmlPreviewSession::open(const QString &path, const QString &expectedSha)
{
  const auto generation = ++m_generation;
  auto *watcher = new QFutureWatcher<XmlPreviewData>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
    const auto data = watcher->result(); watcher->deleteLater();
    if (generation == m_generation) emit ready(data);
  });
  watcher->setFuture(QtConcurrent::run([path, expectedSha] {
    XmlPreviewData data;
    if (!expectedSha.isEmpty()) {
      QFile file(path);
      QCryptographicHash hash(QCryptographicHash::Sha256);
      if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) ||
          QString::fromLatin1(hash.result().toHex()).compare(expectedSha, Qt::CaseInsensitive)) {
        data.chartError = QStringLiteral("源文件与入库时的 SHA-256 不一致，请重新导入");
        data.table.error = data.chartError;
        return data;
      }
    }
    data.chartOk = PreviewDocService::wellCompositeAt(path, &data.chart, &data.chartError);
    data.table = PreviewDocService::xmlDataListAt(path);
    return data;
  }));
}
