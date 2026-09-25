#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "../src/io/dataimportservice.h"
#include "../src/io/lasparser.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

// §41.2 spine acceptance: DataImportService copies into data/<kind>/ via the
// store write queue, declares a manifest layer, and registers the asset —
// verified against the REAL stack (QgisProjectService + LayerManifest +
// QgisLayerService + PaleoProjectStore over a temp project). Plus LasParser
// coverage for ~V/~W/~C/~A incl. NULL→NaN mapping.
class TestImport : public QObject
{
  Q_OBJECT

  // Builds the full service stack over `projectDir` (must exist).
  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    QString metaPath;
  };

  static bool writeFile(const QString &path, const QByteArray &content)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    f.write(content);
    return true;
  }

  static std::unique_ptr<Stack> makeStack(const QString &projectDir, QString *errOut = nullptr)
  {
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
    {
      if (errOut)
        *errOut = s->projectSvc.lastErrors().join(QLatin1Char(';'));
      return nullptr;
    }
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    if (!s->manifest->open(errOut))
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->store->setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                              QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                              s->metaPath);
    s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

private slots:
  void initTestCase()
  {
    QVERIFY(QgisRuntime::isInitialized());
  }

  // CSV import: file lands under data/wells/, manifest declares wells.1,
  // asset registered + resolvable, imported() emitted with the triple.
  void importWellsCsv()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString inbox = tmp.filePath(QStringLiteral("inbox"));
    QVERIFY(QDir().mkpath(inbox));
    const QString csvPath = QDir(inbox).filePath(QStringLiteral("wells.csv"));
    QVERIFY(writeFile(csvPath, QByteArrayLiteral("well,lat,lon\nW-01,63.1,-117.2\nW-02,63.2,-117.4\n")));

    QString stackErr;
    auto stack = makeStack(projectDir, &stackErr);
    QVERIFY2(stack != nullptr, qPrintable(stackErr));
    DataImportService &svc = *stack->importSvc;

    QSignalSpy importedSpy(&svc, &DataImportService::imported);
    QSignalSpy failedSpy(&svc, &DataImportService::importFailed);

    QString err;
    const QString assetId = svc.importFile(QStringLiteral("wells"), csvPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    QCOMPARE(assetId, QStringLiteral("wells-1"));

    // copy landed under data/wells/, contents identical
    const QString dst = QDir(projectDir).filePath(QStringLiteral("data/wells/wells.csv"));
    QVERIFY2(QFile::exists(dst), qPrintable(dst));
    QCOMPARE(QFileInfo(dst).size(), QFileInfo(csvPath).size());

    // manifest declaration on the same sqlite the service's LayerManifest uses
    const QVector<LayerDeclaration> decls = stack->manifest->all();
    QCOMPARE(decls.size(), 1);
    QCOMPARE(decls.at(0).layerId, QStringLiteral("wells.1"));
    QCOMPARE(decls.at(0).type, QStringLiteral("vector"));
    QCOMPARE(decls.at(0).source, dst); // project-absolute provider path
    QCOMPARE(decls.at(0).group, QStringLiteral("00_Data"));
    QCOMPARE(stack->layerSvc->declared().size(), 1);

    // asset registry
    QCOMPARE(svc.assets(QStringLiteral("wells")), QStringList({assetId}));
    QVERIFY(svc.assets(QStringLiteral("seismic")).isEmpty());
    QCOMPARE(svc.assetSource(assetId), QStringLiteral("data/wells/wells.csv"));

    // signals: one imported(kind, assetId, layerId), no failure
    QCOMPARE(importedSpy.count(), 1);
    QCOMPARE(importedSpy.at(0).at(0).toString(), QStringLiteral("wells"));
    QCOMPARE(importedSpy.at(0).at(1).toString(), assetId);
    QCOMPARE(importedSpy.at(0).at(2).toString(), QStringLiteral("wells.1"));
    QCOMPARE(failedSpy.count(), 0);
  }

  // Raster extension → "raster" decl type; second import consumes seq 2.
  void importRasterAndSequence()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString inbox = tmp.filePath(QStringLiteral("inbox"));
    QVERIFY(QDir().mkpath(inbox));
    const QString csvPath = QDir(inbox).filePath(QStringLiteral("wells.csv"));
    const QString tifPath = QDir(inbox).filePath(QStringLiteral("dem.tif"));
    QVERIFY(writeFile(csvPath, QByteArrayLiteral("w\n1\n")));
    QVERIFY(writeFile(tifPath, QByteArrayLiteral("II*\x00faketiff")));

    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString a1 = svc.importFile(QStringLiteral("wells"), csvPath);
    const QString a2 = svc.importFile(QStringLiteral("raster"), tifPath);
    QCOMPARE(a1, QStringLiteral("wells-1"));
    QCOMPARE(a2, QStringLiteral("raster-2"));

    const QVector<LayerDeclaration> decls = stack->manifest->all();
    QCOMPARE(decls.size(), 2);
    const auto rasterDecl = std::find_if(decls.begin(), decls.end(), [](const LayerDeclaration &d) {
      return d.layerId == QStringLiteral("raster.2");
    });
    QVERIFY(rasterDecl != decls.end());
    QCOMPARE(rasterDecl->type, QStringLiteral("raster"));
    QCOMPARE(rasterDecl->group, QStringLiteral("00_Data"));
    QVERIFY(QFile::exists(QDir(projectDir).filePath(QStringLiteral("data/raster/dem.tif"))));

    QCOMPARE(svc.assets().size(), 2);                     // unfiltered
    QCOMPARE(svc.assets(QStringLiteral("raster")), QStringList({a2}));
  }

  // Missing source → "" + error + importFailed, nothing registered/copied.
  void importMissingSourceFails()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));

    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QSignalSpy failedSpy(&svc, &DataImportService::importFailed);
    QSignalSpy importedSpy(&svc, &DataImportService::imported);

    QString err;
    const QString assetId = svc.importFile(QStringLiteral("wells"),
                                           QStringLiteral("/nonexistent/ghost.las"), &err);
    QVERIFY(assetId.isEmpty());
    QVERIFY(!err.isEmpty());
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("wells"));
    QCOMPARE(failedSpy.at(0).at(1).toString(), QStringLiteral("/nonexistent/ghost.las"));
    QCOMPARE(importedSpy.count(), 0);
    QVERIFY(svc.assets().isEmpty());
    QVERIFY(stack->manifest->all().isEmpty());
    QVERIFY(svc.assetSource(QStringLiteral("wells-1")).isEmpty());
  }

  // LAS: ~V/~W/~C/~A parsed; 3 curves × 4 rows; NULL token → NaN.
  void lasParsesCurvesAndNulls()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString lasPath = tmp.filePath(QStringLiteral("w01.las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\n"
        " VERS.                  2.0 :   CWLS LOG ASCII STANDARD -VERSION 2.0\n"
        " WRAP.                  NO  :   ONE LINE PER DEPTH STEP\n"
        "~Well Information Block\n"
        "#MNEM.UNIT       DATA            INFORMATION\n"
        " STRT.M              1670.0000 :   START DEPTH\n"
        " STOP.M              1669.5000 :   STOP DEPTH\n"
        " STEP.M               -0.1250 :   STEP\n"
        " NULL.               -999.2500 :   NULL VALUE\n"
        " WELL.                  W-01   :   WELL NAME\n"
        "~Curve Information Block\n"
        " DEPT.M                    :   1  DEPTH\n"
        " GR  .API                  :   2  GAMMA RAY\n"
        " DT  .US/F                 :   3  SONIC DELTA-T\n"
        "~A  DEPT       GR       DT\n"
        "1670.000  82.10  460.00\n"
        "1669.875  81.50  455.20\n"
        "1669.750  -999.25 450.90\n"
        "1669.625  79.80  446.10\n")));

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY2(LasParser::parse(lasPath, names, curves, &err), qPrintable(err));

    QCOMPARE(names, QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("DT")}));
    QCOMPARE(curves.size(), 3);
    QCOMPARE(curves.at(0).name, QStringLiteral("DEPT"));
    QCOMPARE(curves.at(0).unit, QStringLiteral("M"));
    QCOMPARE(curves.at(1).name, QStringLiteral("GR"));
    QCOMPARE(curves.at(1).unit, QStringLiteral("API"));
    QCOMPARE(curves.at(2).descr, QStringLiteral("3  SONIC DELTA-T"));

    for (const LasCurve &c : curves)
      QCOMPARE(c.values.size(), 4);

    QCOMPARE(curves.at(0).values.at(0), 1670.0);
    QCOMPARE(curves.at(0).values.at(3), 1669.625);
    QCOMPARE(curves.at(1).values.at(0), 82.10);
    QVERIFY(qIsNaN(curves.at(1).values.at(2)));  // -999.25 NULL → NaN
    QCOMPARE(curves.at(1).values.at(3), 79.80);
    QCOMPARE(curves.at(2).values.at(2), 450.90);
    QVERIFY(!qIsNaN(curves.at(2).values.at(2)));
  }

  // WRAP YES → hard error; missing file → error.
  void lasFailureModes()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString wrapPath = tmp.filePath(QStringLiteral("wrap.las"));
    QVERIFY(writeFile(wrapPath, QByteArrayLiteral(
        "~Version Information\n"
        " VERS.                  2.0 :   CWLS LOG ASCII STANDARD -VERSION 2.0\n"
        " WRAP.                  YES :   MULTIPLE LINES PER DEPTH STEP\n"
        "~Curve Information\n"
        " DEPT.M : DEPTH\n"
        "~A DEPT\n"
        "1670.0\n")));

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY(!LasParser::parse(wrapPath, names, curves, &err));
    QVERIFY2(err.contains(QStringLiteral("WRAP"), Qt::CaseInsensitive), qPrintable(err));

    QVERIFY(!LasParser::parse(QStringLiteral("/nonexistent/ghost.las"), names, curves, &err));
    QVERIFY(!err.isEmpty());
  }
};

int main(int argc, char *argv[])
{
  // Offscreen QGIS bootstrap through the runtime that owns init order.
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestImport tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_import.moc"
