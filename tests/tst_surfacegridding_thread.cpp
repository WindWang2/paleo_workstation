// 层：测试
// #122：网格化 worker 不得跨线程写活 catalog——stage/commit 回到 catalog 所属线程。
#include "catalog/datacatalog.h"
#include "workflow/surfacegridding.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

namespace
{
QByteArray planeScatter()
{
  QByteArray t;
  for (int j = 0; j <= 4; ++j)
    for (int i = 0; i <= 4; ++i)
      t += QByteArray::number(i * 100.0) + ' ' + QByteArray::number(j * 100.0) + ' ' +
           QByteArray::number(1000.0 + i * 2.0 + j) + '\n';
  return t;
}

int griddedVersions(const DataCatalog &cat, QString *path)
{
  int n = 0;
  for (const CatalogVersion &v : cat.versions())
    if (v.assetId.contains(QLatin1String("gridded_surface")) ||
        v.path.contains(QLatin1String("GRID_")))
    {
      ++n;
      if (path)
        *path = cat.versionFilePath(v);
    }
  return n;
}

int leftoverGridStaging(const QString &projectDir)
{
  return QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging")))
      .entryList({QStringLiteral("grid-*")}, QDir::Dirs | QDir::NoDotAndDotDot)
      .size();
}
} // namespace

class TestSurfaceGriddingThread : public QObject
{
  Q_OBJECT
  private slots:
  void workerThreadRegistersOnOwnerThread();
  void sameThreadRegistersInline();
};

void TestSurfaceGriddingThread::workerThreadRegistersOnOwnerThread()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  QString err;
  DataCatalog cat;
  QVERIFY2(cat.open(tmp.path(), &err), qPrintable(err));
  SurfaceGriddingWorkflow wf(nullptr);
  wf.setCatalog(&cat, tmp.path());
  QSignalSpy ready(&wf, &SurfaceGriddingWorkflow::rasterReady);
  QSignalSpy failed(&wf, &SurfaceGriddingWorkflow::griddingFailed);
  DataCatalog::resetThreadViolationCount();

  QString workerErr = QStringLiteral("not run");
  SurfaceGriddingWorkflow::Options opt;
  opt.cellSize = 50.0;
  opt.useBarriers = false;
  const QByteArray text = planeScatter();
  QThread *worker = QThread::create(
      [&]
      {
        SurfaceGriddingWorkflow::Outcome o;
        workerErr = wf.gridHorizonText(QStringLiteral("H1"), text, opt, nullptr, nullptr, &o);
      });
  worker->start();
  QVERIFY(worker->wait(60000));
  delete worker;
  QCOMPARE(workerErr, QString());
  // worker 期间 catalog 未被跨线程读写。
  QCOMPARE(DataCatalog::threadViolationCount(), 0);
  // 登记排队在 owner 线程执行。
  QTRY_COMPARE(ready.count(), 1);
  QCOMPARE(failed.count(), 0);
  QCOMPARE(DataCatalog::threadViolationCount(), 0);
  QString managed;
  QCOMPARE(griddedVersions(cat, &managed), 1);
  QVERIFY(QFileInfo::exists(managed));
  QCOMPARE(QFileInfo(ready.at(0).at(1).toString()).canonicalFilePath(),
           QFileInfo(managed).canonicalFilePath());
  QCOMPARE(leftoverGridStaging(tmp.path()), 0);

  // 落盘：重开后版本仍在。
  DataCatalog re;
  QVERIFY2(re.open(tmp.path(), &err), qPrintable(err));
  QCOMPARE(griddedVersions(re, nullptr), 1);
}

void TestSurfaceGriddingThread::sameThreadRegistersInline()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  QString err;
  DataCatalog cat;
  QVERIFY2(cat.open(tmp.path(), &err), qPrintable(err));
  SurfaceGriddingWorkflow wf(nullptr);
  wf.setCatalog(&cat, tmp.path());
  QSignalSpy ready(&wf, &SurfaceGriddingWorkflow::rasterReady);
  SurfaceGriddingWorkflow::Options opt;
  opt.cellSize = 50.0;
  opt.useBarriers = false;
  SurfaceGriddingWorkflow::Outcome o;
  QCOMPARE(wf.gridHorizonText(QStringLiteral("H1"), planeScatter(), opt, nullptr, nullptr, &o),
           QString());
  QCOMPARE(ready.count(), 1); // 同线程同步登记
  QVERIFY(!o.assetId.isEmpty());
  QVERIFY(!o.versionId.isEmpty());
  QVERIFY(QFileInfo::exists(o.tifPath));
  QCOMPARE(griddedVersions(cat, nullptr), 1);
  QCOMPARE(leftoverGridStaging(tmp.path()), 0);
}

QTEST_GUILESS_MAIN(TestSurfaceGriddingThread)
#include "tst_surfacegridding_thread.moc"
