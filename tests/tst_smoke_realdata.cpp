#include <QtTest>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QFile>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/segyreader.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

#include <gdal.h>
#include <gdal_priv.h>

#include <QDir>
#include <QElapsedTimer>
#include <algorithm>

// 真数据导入冒烟（验收：打开 project_area 目录跑一次导入，数据不提交）。
// 环境变量 PALEO_REAL_PROJECT_AREA 指向真目录时执行；未设置时跳过。
class TestSmokeRealdata : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    if (!QgisRuntime::isInitialized())
      QSKIP("QGIS runtime unavailable");
  }

  void importsWholeWorkarea();

private:
  static QString realDir()
  {
    return qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  }
};

void TestSmokeRealdata::importsWholeWorkarea()
{
  const QString src = realDir();
  if (src.isEmpty() || !QDir(src).exists())
    QSKIP("PALEO_REAL_PROJECT_AREA not set — real-data smoke skipped");

  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));

  QgisProjectService projectSvc;
  QVERIFY(projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))));
  LayerManifest manifest(QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layerSvc(&projectSvc, &manifest);
  PaleoProjectStore store;
  DataImportService svc(&layerSvc, &store);
  svc.setProjectDir(projectDir);

  // 按目录序导入全部文件（井位→井曲线→井分层→时深→层位→地震→参考）。
  const QStringList subdirs{QString::fromUtf8("井位"), QString::fromUtf8("井曲线"),
                            QString::fromUtf8("井分层"), QString::fromUtf8("时深"),
                            QString::fromUtf8("层位"),  QString::fromUtf8("地震体"),
                            QString::fromUtf8("参考相图"), QString::fromUtf8("参考资料")};
  int files = 0, failed = 0;
  QElapsedTimer timer;
  timer.start();
  for (const QString &sub : subdirs)
  {
    QDir dir(QDir(src).filePath(sub));
    if (!dir.exists())
      continue;
    const auto entries = dir.entryInfoList(QDir::Files, QDir::Name);
    // 时深在 TD 子目录
    if (sub == QString::fromUtf8("时深"))
    {
      QDir tdDir(dir.filePath(QStringLiteral("TD")));
      for (const QFileInfo &fi : tdDir.entryInfoList(QDir::Files, QDir::Name))
      {
        QString err;
        if (svc.importProjectFile(fi.absoluteFilePath(), &err).isEmpty())
        {
          ++failed;
          qWarning("SMOKE FAIL %s: %s", qPrintable(fi.fileName()), qPrintable(err));
        }
        ++files;
      }
      continue;
    }
    for (const QFileInfo &fi : entries)
    {
      QString err;
      if (svc.importProjectFile(fi.absoluteFilePath(), &err).isEmpty())
      {
        ++failed;
        qWarning("SMOKE FAIL %s: %s", qPrintable(fi.fileName()), qPrintable(err));
      }
      ++files;
    }
  }
  qWarning("SMOKE imported %d files (%d failed) in %lld ms", files, failed, timer.elapsed());
  QCOMPARE(failed, 0);
  QCOMPARE(files, 1 + 20 + 1 + 20 + 8 + 1 + 3 + 6); // 60 个真实文件

  DataCatalog *cat = svc.catalog();
  QCOMPARE(cat->entities(QStringLiteral("well")).size(), 20);
  QCOMPARE(cat->entities(QStringLiteral("sequence_boundary")).size(), 8);
  QCOMPARE(cat->entities(QStringLiteral("seismic_survey")).size(), 1);
  QCOMPARE(cat->entities(QStringLiteral("auxiliary")).size(), 3 + 6); // 3 GeoJSON + 6 参考资料

  // A1 四条主关联：井口、LAS、分层、时深（plan §5A）。
  const auto a1Links = cat->linksForEntity(QStringLiteral("well-A1"));
  QStringList roles;
  for (const EntityAssetLink &l : a1Links)
    roles.append(l.role);
  std::sort(roles.begin(), roles.end());
  QCOMPARE(roles, QStringList({QStringLiteral("time_depth"), QStringLiteral("tops"),
                               QStringLiteral("well_head"), QStringLiteral("well_log")}));

  // A1 坐标与状态
  const CatalogEntity a1 = cat->entityById(QStringLiteral("well-A1"));
  QCOMPARE(a1.coordinateStatus, QStringLiteral("untransformed"));
  QVERIFY(qAbs(a1.surfaceX - 5288.67) < 0.01 && qAbs(a1.surfaceY - 8219.94) < 0.01);

  // D61：RAW + DERIVED + 图层清单
  bool sawD61Raster = false;
  for (const LayerDeclaration &d : layerSvc.declared())
    if (d.layerId == QLatin1String("horizon.D61") && d.type == QLatin1String("raster"))
      sawD61Raster = true;
  QVERIFY(sawD61Raster);
  // 八个层位全部入清单（都在 8 界面集合内）
  QCOMPARE(layerSvc.declared().size(), 8);

  // 手工核对（plan §5A）：A1 (5288.67, 8219.94) 压在 D61 派生栅格的非空像元上
  //（容差半像元 → 相邻格都查）。
  {
    QString d61Decl;
    for (const LayerDeclaration &d : layerSvc.declared())
      if (d.layerId == QLatin1String("horizon.D61"))
        d61Decl = d.source;
    QVERIFY(!d61Decl.isEmpty());
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen(d61Decl.toUtf8().constData(), GA_ReadOnly);
    QVERIFY2(ds, "open D61 derived raster");
    double gt[6] = {0, 0, 0, 0, 0, 0};
    GDALGetGeoTransform(ds, gt);
    const int px = qBound(0, int((5288.67 - gt[0]) / gt[1]), GDALGetRasterXSize(ds) - 1);
    const int py = qBound(0, int((8219.94 - gt[3]) / gt[5]), GDALGetRasterYSize(ds) - 1);
    int hasNd = 0;
    const float nd = static_cast<float>(
        GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNd));
    float v = 0;
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, px, py, 1, 1, &v, 1, 1, GDT_Float32, 0, 0) == CE_None);
    qWarning("SMOKE D61 at A1 pixel(%d,%d) = %f (nodata %f)", px, py, v, nd);
    QVERIFY2(hasNd && v != nd, "A1 must sit on filled D61 cell");
    GDALClose(ds);
  }

  // 地震体：survey 几何冻结（inline 1315–1725、crossline 4165–4805、2ms）
  const CatalogEntity survey = cat->entities(QStringLiteral("seismic_survey")).front();
  QCOMPARE(survey.inlineMin, 1315.0);
  QCOMPARE(survey.inlineMax, 1725.0);
  QCOMPARE(survey.xlineMin, 4165.0);
  QCOMPARE(survey.xlineMax, 4805.0);
  QCOMPARE(survey.sampleIntervalUs, 2000.0);

  // SEG-Y 测线级：解码一条 inline（约 641 道 × 901 样点）
  for (const CatalogAsset &a : cat->assets())
  {
    if (a.type != QLatin1String("seismic"))
      continue;
    const QString path = svc.absolutePath(a.id);
    SegyReader r;
    QString err;
    QVERIFY2(r.open(path, &err), qPrintable(err));
    QCOMPARE(r.samplesPerTrace(), 901);
    QCOMPARE(r.traceCount(), 263451);
    QVector<SegyTrace> line;
    QVERIFY2(r.readInline(1515, &line, &err), qPrintable(err));
    QCOMPARE(line.size(), 641);
    qWarning("SMOKE inline 1515: %d traces x %d samples", line.size(),
             line.front().samples.size());
    QCOMPARE(line.front().samples.size(), 901);
  }
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestSmokeRealdata tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_smoke_realdata.moc"
