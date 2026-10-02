#include <QtTest>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>

#include <algorithm>

#include "catalog/datacatalog.h"
#include "io/lasparser.h"
#include "services/welllogset.h"

namespace
{
  QByteArray tinyLas()
  {
    return QByteArray(
        "~Version Information\n"
        " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n"
        " WRAP.                   NO:   One line per depth step\n"
        "~Well Information Block\n"
        " STRT.M        1000.0000:\n"
        " STOP.M        1001.0000:\n"
        " STEP.M          1.0000:\n"
        " NULL.        -999.2500:\n"
        " WELL.         PERF :\n"
        "~Curve Information Block\n"
        " DEPT.M                  :   DEPT\n"
        " GR.GAPI                 :   GR\n"
        " RHOB.G/CM3              :   RHOB\n"
        "~A\n"
        "1000.00 80.00 2.30\n"
        "1001.00 81.00 2.31\n");
  }

  bool writeFile(const QString &path, const QByteArray &bytes)
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    return file.write(bytes) == bytes.size();
  }
} // namespace

class TestWellLogSetPerf : public QObject
{
  Q_OBJECT

private slots:
  void wellCurveIndex_100x3_under500ms_and_linearRatio();
  void realArea_headerIndexOrSkip();
};

void TestWellLogSetPerf::wellCurveIndex_100x3_under500ms_and_linearRatio()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QByteArray las = tinyLas();
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  QStringList wellIds;
  wellIds.reserve(100);
  {
    DataCatalog::BatchSave batch(&cat);
    for (int w = 0; w < 100; ++w)
    {
      const QString wellId = QStringLiteral("well-%1").arg(w, 3, 10, QLatin1Char('0'));
      wellIds.append(wellId);
      CatalogEntity well;
      well.id = wellId;
      well.entityType = QStringLiteral("well");
      well.name = wellId;
      QVERIFY2(cat.addEntity(well, &err), qPrintable(err));
      for (int f = 0; f < 3; ++f)
      {
        const QString fileName = QStringLiteral("w%1_%2.las").arg(w, 3, 10, QLatin1Char('0')).arg(f);
        const QString path = dir.filePath(fileName);
        QVERIFY(writeFile(path, las));
        const QString assetId = QStringLiteral("ast-%1-%2").arg(w).arg(f);
        const QString versionId = QStringLiteral("ver-%1-%2").arg(w).arg(f);
        CatalogAsset asset;
        asset.id = assetId;
        asset.type = QStringLiteral("well_log");
        asset.format = QStringLiteral("las");
        asset.displayName = fileName;
        QVERIFY2(cat.addAsset(asset, &err), qPrintable(err));
        CatalogVersion version;
        version.id = versionId;
        version.assetId = assetId;
        version.stage = QStringLiteral("RAW");
        version.versionNumber = 1;
        version.managed = false;
        version.path = path;
        version.fileName = fileName;
        QVERIFY2(cat.addVersion(version, &err), qPrintable(err));
        EntityAssetLink link;
        link.entityType = QStringLiteral("well");
        link.entityId = wellId;
        link.assetId = assetId;
        link.role = QStringLiteral("well_log");
        link.isPrimary = f == 0;
        link.unresolved = false;
        link.ordinal = f;
        QVERIFY2(cat.addLink(link, &err), qPrintable(err));
      }
    }
    QVERIFY2(batch.flush(&err), qPrintable(err));
  }

  QElapsedTimer clock;
  clock.start();
  int curves = 0;
  for (const QString &wellId : wellIds)
    curves += WellLogSet::wellCurveIndex(&cat, dir.path(), wellId, nullptr).size();
  const double ms100 = clock.nsecsElapsed() / 1.0e6;
  qInfo("BASELINE wellCurveIndex_100x3_ms = %.3f", ms100);
  QCOMPARE(curves, 100 * 6);
  const QString slow =
      QStringLiteral("wellCurveIndex 100x3 took %1 ms").arg(ms100, 0, 'f', 3);
  QVERIFY2(ms100 < 500.0, qPrintable(slow));

  clock.restart();
  int curves10 = 0;
  for (int i = 0; i < 10; ++i)
    curves10 += WellLogSet::wellCurveIndex(&cat, dir.path(), wellIds.at(i), nullptr).size();
  const double ms10 = clock.nsecsElapsed() / 1.0e6;
  const double denom = ms10 < 1.0 ? 1.0 : ms10;
  const double ratio = ms100 / denom;
  qInfo("BASELINE wellCurveIndex_ratio_100_over_10 = %.3f", ratio);
  QCOMPARE(curves10, 10 * 6);
  const QString ratioMsg =
      QStringLiteral("ratio %1 (100 wells %2 ms, 10 wells %3 ms)")
          .arg(ratio, 0, 'f', 3)
          .arg(ms100, 0, 'f', 3)
          .arg(ms10, 0, 'f', 3);
  QVERIFY2(ratio < 20.0, qPrintable(ratioMsg));
}

// 真工区只读：未设 PALEO_REAL_PROJECT_AREA 时 QSKIP。只登记外部 LAS 路径，
// 计时的是 wellCurveIndex（parseHeader），不把曲线体写入 catalog。
void TestWellLogSetPerf::realArea_headerIndexOrSkip()
{
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty() || !QDir(area).exists())
    QSKIP("PALEO_REAL_PROJECT_AREA not set — real-area well logset skipped");

  const QString curveRoot = QDir(area).filePath(QString::fromUtf8("井曲线"));
  if (!QDir(curveRoot).exists())
    QSKIP("PALEO_REAL_PROJECT_AREA has no 井曲线 directory — real-area well logset skipped");

  QStringList paths;
  QDirIterator scan(curveRoot,
                    {QStringLiteral("*.las"), QStringLiteral("*.LAS")},
                    QDir::Files, QDirIterator::Subdirectories);
  while (scan.hasNext())
    paths.append(scan.next());
  if (paths.isEmpty())
    QSKIP("井曲线 has no LAS — real-area well logset skipped");

  struct Rec
  {
    QString path;
  };
  QHash<QString, QVector<Rec>> byWell;
  int rejected = 0;
  for (const QString &path : paths)
  {
    LasHeaderInfo header;
    QString error;
    if (!LasParser::parseHeader(path, header, &error) || header.wellName.trimmed().isEmpty())
    {
      ++rejected;
      continue;
    }
    byWell[header.wellName.trimmed()].append(Rec{path});
  }
  QVERIFY2(!byWell.isEmpty(), "real-area LAS headers produced no well name");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  QStringList wellIds;
  {
    DataCatalog::BatchSave batch(&cat);
    int w = 0;
    for (auto git = byWell.cbegin(); git != byWell.cend(); ++git, ++w)
    {
      const QString wellId = QStringLiteral("real-%1").arg(w, 3, 10, QLatin1Char('0'));
      wellIds.append(wellId);
      CatalogEntity well;
      well.id = wellId;
      well.entityType = QStringLiteral("well");
      well.name = git.key();
      QVERIFY2(cat.addEntity(well, &err), qPrintable(err));
      QVector<Rec> files = git.value();
      std::sort(files.begin(), files.end(), [](const Rec &a, const Rec &b) {
        return a.path < b.path;
      });
      for (int f = 0; f < files.size(); ++f)
      {
        const QString assetId = QStringLiteral("ast-real-%1-%2").arg(w).arg(f);
        const QString versionId = QStringLiteral("ver-real-%1-%2").arg(w).arg(f);
        const QString fileName = QFileInfo(files.at(f).path).fileName();
        CatalogAsset asset;
        asset.id = assetId;
        asset.type = QStringLiteral("well_log");
        asset.format = QStringLiteral("las");
        asset.displayName = fileName;
        QVERIFY2(cat.addAsset(asset, &err), qPrintable(err));
        CatalogVersion version;
        version.id = versionId;
        version.assetId = assetId;
        version.stage = QStringLiteral("RAW");
        version.versionNumber = 1;
        version.managed = false;
        version.path = files.at(f).path;
        version.fileName = fileName;
        QVERIFY2(cat.addVersion(version, &err), qPrintable(err));
        EntityAssetLink link;
        link.entityType = QStringLiteral("well");
        link.entityId = wellId;
        link.assetId = assetId;
        link.role = QStringLiteral("well_log");
        link.isPrimary = f == 0;
        link.unresolved = false;
        link.ordinal = f;
        QVERIFY2(cat.addLink(link, &err), qPrintable(err));
      }
    }
    QVERIFY2(batch.flush(&err), qPrintable(err));
  }

  QElapsedTimer clock;
  clock.start();
  int curves = 0;
  int warnings = 0;
  for (const QString &wellId : wellIds)
  {
    WellLogWarnings warn;
    const QVector<WellCurveRef> index =
        WellLogSet::wellCurveIndex(&cat, dir.path(), wellId, &warn);
    curves += index.size();
    warnings += warn.count;
    for (const WellCurveRef &ref : index)
      QVERIFY(ref.column >= 1);
  }
  const double ms = clock.nsecsElapsed() / 1.0e6;
  int files = 0;
  for (const QString &wellId : wellIds)
    files += WellLogSet::wellLogFiles(&cat, dir.path(), wellId, nullptr).size();

  qInfo("BASELINE real_area_wells = %d", int(wellIds.size()));
  qInfo("BASELINE real_area_files = %d", files);
  qInfo("BASELINE real_area_curves = %d", curves);
  qInfo("BASELINE real_area_header_rejects = %d", rejected);
  qInfo("BASELINE real_area_index_warnings = %d", warnings);
  qInfo("BASELINE real_area_wellCurveIndex_ms = %.3f", ms);
  QVERIFY(files > 0);
  QVERIFY(curves > 0);
}

QTEST_MAIN(TestWellLogSetPerf)
#include "tst_welllogset_perf.moc"
