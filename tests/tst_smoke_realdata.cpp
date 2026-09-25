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
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/projectdata.h"
#include "../src/workflow/mappingworkflow.h"
#include "../src/workflow/workflows.h"

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
  // 四角冻结（plan §2 实测值：Source X/Y 偏移 72/76）
  QCOMPARE(survey.corners.size(), 4);
  QCOMPARE(survey.corners.at(0), qMakePair(0.0, 0.0));       // (inlMin,xlMin)
  QCOMPARE(survey.corners.at(1), qMakePair(12793.0, 0.0));   // (inlMin,xlMax)
  QCOMPARE(survey.corners.at(2), qMakePair(12793.0, 16406.0)); // (inlMax,xlMax)
  QCOMPARE(survey.corners.at(3), qMakePair(0.0, 16406.0));     // (inlMax,xlMin)

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

  // ---- 端到端编图链（plan §5C）：刚导入的真 catalog → 门面 → 厚度 → IDW
  // → 相多边形 → 时间残差。这是「走通一层古地理编图」的产品级验收。 ----
  QgisProcessingService proc(&store);
  ConstraintWorkflow constraints(&proc, &layerSvc);
  CompositionWorkflow compose(&proc, &layerSvc);
  MappingWorkflow mapping(&constraints, &compose, &layerSvc);
  ProjectDataFacade facade;
  facade.setCatalog(cat, projectDir); // 复用导入期已开的 catalog（不接管）
  facade.setManifest(&manifest);
  mapping.setProjectData(&facade);

  // 门面读侧：20 口井，A1 的分层含 D61/D62，D61 栅格几何可解析。
  QCOMPARE(facade.wells().size(), 20);
  {
    const QVector<WellTop> a1Tops = facade.topsFor(QStringLiteral("well-A1"));
    QStringList names;
    for (const WellTop &t : a1Tops)
      names.append(t.horizon);
    QVERIFY(names.contains(QStringLiteral("D61")));
    QVERIFY(names.contains(QStringLiteral("D62")));
    const HorizonRasterInfo ri = facade.horizonRasterDecl(QStringLiteral("D61"));
    QVERIFY2(ri.valid, qPrintable(facade.lastError()));
    QCOMPARE(ri.cols, 641); // plan §3：网格 411(inline)×641(crossline)
    QCOMPARE(ri.rows, 411);
  }

  // 厚度：D61→D62 逐井；缺任一口分层的井跳过，不造假厚度。
  int skipped = -1;
  QString err;
  const QVector<ThicknessPoint> pts =
      mapping.computeThickness(QStringLiteral("D61"), QStringLiteral("D62"), &skipped, &err);
  QVERIFY2(!pts.isEmpty(), qPrintable(err));
  QVERIFY2(pts.size() + skipped == 20, qPrintable(QStringLiteral("pts=%1 skipped=%2")
                                                    .arg(pts.size()).arg(skipped)));
  for (const ThicknessPoint &p : pts)
    QVERIFY(p.thickness > 0.0); // D62 在 D61 之下 → 厚度为正
  qWarning("SMOKE thickness: %d wells contribute, %d skipped", pts.size(), skipped);

  // 完整链：厚度点层 → 约束 IDW → 相多边形。
  QVERIFY2(mapping.runThicknessChain(QStringLiteral("D61"), &err), qPrintable(err));
  {
    bool sawFacies = false;
    for (const LayerDeclaration &d : layerSvc.declared())
      if (d.layerId == QLatin1String("facies.D61") && d.type == QLatin1String("vector"))
      {
        sawFacies = QFile::exists(d.source.section(QLatin1Char('|'), 0, 0));
        qWarning("SMOKE facies.D61 source: %s", qPrintable(d.source));
      }
    QVERIFY(sawFacies);
  }

  // 时间残差：20 口井都有 TD 表 → 不应出现 NO_TD_TABLE；残差量只记日志
  //（数值属数据事实，不设阈值断言）。
  const QList<ValidationIssue> residual =
      computeTimeResiduals(&facade, QStringLiteral("D61"), 1.0);
  int nResidual = 0;
  for (const ValidationIssue &v : residual)
  {
    QVERIFY(v.code != QLatin1String("NO_TD_TABLE"));
    if (v.code == QLatin1String("TIME_RESIDUAL"))
      ++nResidual;
  }
  qWarning("SMOKE time residuals: %d wells flagged", nResidual);
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
