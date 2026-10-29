#include <QtTest>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QTemporaryDir>
#include <paleo_data/service.h>
#include "io/dataimportservice.h"
#include "io/welllogread.h"
#include "io/lascache.h"
#include "io/wellfileparsers.h"
#include "io/nativemodel.h"
#include "io/areamodel.h"
#include "metadata/paleoprojectstore.h"
#include "services/storagegovernance.h"
#include "services/projectdata.h"

namespace {
QByteArray content() {
  return "~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 -999\n101 -999.25\n102 -20\n";
}
QString writeSource(const QTemporaryDir &temp) {
  const auto path = temp.filePath("source.las");
  QFile file(path); if (!file.open(QIODevice::WriteOnly)) return {};
  file.write(content()); return path;
}
QStringList files(const QString &path) {
  QStringList result; QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) result << it.next();
  return result;
}
}
class TestNativeImport : public QObject {
  Q_OBJECT
private slots:
  void areaBatchKeepsOneModel() {
    QTemporaryDir temp; const auto input = temp.filePath("input"), project = temp.filePath("project");
    QVERIFY(QDir().mkpath(input)); QStringList paths;
    for (int i = 0; i < 24; ++i) {
      const auto path = input + QString("/well-%1.las").arg(i); paths << path;
      QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly));
      QByteArray data = "~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-" + QByteArray::number(i)
          + "\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n";
      for (int row = 0; row < 2000; ++row) data += QByteArray::number(row) + ' ' + QByteArray::number(row % 100) + '\n';
      QCOMPARE(f.write(data), qint64(data.size()));
    }
    QString error; paleo::data::ImportReport report; QElapsedTimer timer; timer.start();
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    const auto areaMs = timer.restart();
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(project);
    auto session = imports.beginImport(); QVERIFY(session); session->linkExternal = true;
    DataImportService::produceFiles(*session, paths, {}, {});
    for (const auto &row : session->rows) QVERIFY2(row.outcome != FolderRowResult::Outcome::Failed, qPrintable(row.message));
    const auto projectionMs = timer.restart();
    QCOMPARE(imports.commitImport(*session, &error), DataImportService::CommitStatus::Committed);
    qInfo() << "24 LAS / 48000 samples: area_ms=" << areaMs << "catalog_ms=" << projectionMs << "commit_ms=" << timer.elapsed();
    QCOMPARE(imports.catalog()->versions().size(), 24);
    for (const auto &version : imports.catalog()->versions()) {
      QVERIFY(!version.managed); QVERIFY(version.extra.value("nativeModel").toMap().isEmpty());
    }
    for (const auto &path : files(project)) QVERIFY2(!path.endsWith(".las") && !path.contains("/native-model/"), qPrintable(path));
  }
  void externalRegistrationReusesAreaModel() {
    QTemporaryDir temp; const auto source = writeSource(temp);
    const auto project = temp.filePath("project"); QString error;
    paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, temp.path(), report, &error, {}, {project}), qPrintable(error));
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(project);
    paleo::data::DataService model; std::string detail;
    QVERIFY(model.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::DataEntry> entries; QVERIFY(model.listData("curves", 0, entries, detail));
    QVERIFY(!entries.empty()); QVERIFY(model.updateCurveValue(entries.front().id, 0, 88, detail)); model.closeProject();
    DataImportService::ImportOptions options; options.linkExternal = true; options.forceType = "well_log";
    const auto result = imports.importProjectFileEx(source, options, &error);
    QVERIFY2(result.outcome == DataImportService::ImportOutcome::Imported, qPrintable(error));
    const auto version = imports.catalog()->versions().first();
    QVERIFY(!version.managed); QCOMPARE(version.path, source);
    QVERIFY(version.extra.value("nativeModel").toMap().isEmpty());
    for (const auto &path : files(project)) {
      QVERIFY2(!path.endsWith(".las"), qPrintable(path));
      QVERIFY2(!path.contains("/native-model/"), qPrintable(path));
    }
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY2(WellLogRead::parse(source, header, curves, &error), qPrintable(error));
    QCOMPARE(curves[1].values[0], 88.0);
    QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), content());
  }
  void managedAndExternal_data() {
    QTest::addColumn<bool>("external");
    QTest::newRow("managed") << false; QTest::newRow("external") << true;
  }
  void managedAndExternal() {
    QFETCH(bool, external);
    QTemporaryDir temp; const auto source = writeSource(temp);
    QVERIFY(!source.isEmpty());
    const auto project = temp.filePath("project");
    PaleoProjectStore store;
    store.setProjectPaths(project+"/project.qgz",project+"/project.gpkg",project+"/metadata/project.sqlite");
    DataImportService imports(&store); imports.setProjectDir(project);
    DataImportService::ImportOptions options; options.linkExternal=external;
    options.forceType="well_log"; options.targetWellId="new:SYNTH-1";
    QString error;
    const auto result=imports.importProjectFileEx(source,options,&error);
    QVERIFY2(result.outcome==DataImportService::ImportOutcome::Imported,qPrintable(error));
    const auto versions=imports.catalog()->versions(); QCOMPARE(versions.size(),1);
    const auto version=versions.first();
    const auto binding=version.extra.value("nativeModel").toMap();
    QVERIFY2(!binding.isEmpty(),qPrintable(error));
    const auto database=QDir(project).filePath(binding.value("path").toString());
    QVERIFY(QFileInfo::exists(database));
    QVERIFY(!QFileInfo::exists(database+"-wal")); QVERIFY(!QFileInfo::exists(database+"-shm"));
    const auto path=imports.catalog()->versionFilePath(version);
    paleo::data::DataService reader; std::string detail;
    QVERIFY2(reader.openProjectReadOnly(QFileInfo(database).absolutePath().toUtf8().toStdString(),detail),detail.c_str());
    QVERIFY2(reader.verifySource(binding.value("sourceId").toLongLong(),detail),detail.c_str());
    paleo::data::LogSet logs;
    QVERIFY2(reader.readLogSet(binding.value("logSetId").toLongLong(),true,logs,detail),detail.c_str());
    QCOMPARE(logs.logCurves.front().sampleValues[0],-999.0);
    QVERIFY(std::isnan(logs.logCurves.front().sampleValues[1]));
    reader.closeProject();
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY2(WellLogRead::parse(path,header,curves,&error),qPrintable(error));
    QCOMPARE(curves.size(),2); QCOMPARE(curves[1].values[0],-999.0);
    QVERIFY(std::isnan(curves[1].values[1])); QCOMPARE(curves[1].values[2],-20.0);
    imports.setProjectDir(project); // Reopen and register bindings from the persisted catalog.
    QVERIFY2(WellLogRead::parse(path,header,curves,&error),qPrintable(error));
    QVERIFY(LasCache::shared().load(path).ok);
    const auto report=paleo::storage::scan(paleo::storage::snapshot(imports.catalog()));
    QVERIFY(report.complete);
    for (const auto &orphan:report.orphans) QVERIFY(orphan.canonicalPath!=QFileInfo(database).canonicalFilePath());
    QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),content()); original.close();
    // A damaged native model must be reported, even when RAW is still readable.
    QVERIFY(QFile::setPermissions(database,QFile::permissions(database)|QFileDevice::WriteOwner));
    QFile corrupt(database); QVERIFY(corrupt.open(QIODevice::WriteOnly|QIODevice::Append)); corrupt.write("damaged"); corrupt.close();
    QVERIFY(!WellLogRead::parse(path,header,curves,&error)); QVERIFY(!error.isEmpty());
    QVERIFY(!LasCache::shared().load(path).ok);
  }
  void wellTablesAndDerivedVersions() {
    QTemporaryDir temp; const auto project=temp.filePath("project");
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(project);
    const auto write=[&](const QString &name,const QByteArray &text) {
      const auto path=temp.filePath(name); QFile f(path);
      if(!f.open(QIODevice::WriteOnly))return QString(); f.write(text); return path;
    };
    const auto heads=write("heads.csv","WELL,X,Y,KB,TD\nA1,233000,3990000,25,2000\n");
    DataImportService::ImportOptions options; options.forceType="well_head";
    options.tableMapping={{"crs","EPSG:32649"},{"projectCrs","EPSG:32649"}};
    QString error;
    QVERIFY2(!imports.importProjectFile(heads,options,&error).isEmpty(),qPrintable(error));
    const auto tops=write("tops.dat","# WELL TOP MD X Y Z TVD TIME\nA1,D61,1000,233000,3990000,-25,990,1200\n");
    options={}; options.forceType="well_stratification";
    const auto topsAsset=imports.importProjectFile(tops,options,&error);
    QVERIFY2(!topsAsset.isEmpty(),qPrintable(error));
    const auto td=write("td.dat","#TimeDepth File From SMI\n# Well : A1\n0 0 0 0\n1200 990 990 1000\n2500 1990 1990 2000\n");
    options.forceType="time_depth";
    const auto tdAsset=imports.importProjectFile(td,options,&error);
    QVERIFY2(!tdAsset.isEmpty(),qPrintable(error));
    const auto dev=write("dev.dat","# Well : A1\n0 0 0\n1000 10 20\n2000 20 30\n");
    options.forceType="well_deviation";
    const auto devAsset=imports.importProjectFile(dev,options,&error);
    QVERIFY2(!devAsset.isEmpty(),qPrintable(error));
    for(const auto &version:imports.catalog()->versions())
      QVERIFY2(!version.extra.value("nativeModel").toMap().isEmpty(),qPrintable(version.path));
    QVector<WellTopRecord> rows;
    const auto topPath=imports.catalog()->versionFilePath(imports.catalog()->currentVersion(topsAsset));
    QVERIFY2(readWellTops(topPath,rows,&error),qPrintable(error));
    QCOMPARE(rows.size(),1); QCOMPARE(rows[0].md,1000.0); QCOMPARE(rows[0].timeMs,1200.0);
    QCOMPARE(rows[0].x,233000.0); QVERIFY(rows[0].hasMd&&rows[0].hasTime&&rows[0].hasX);
    TimeDepthTable time;
    QVERIFY2(readTimeDepth(imports.catalog()->versionFilePath(imports.catalog()->currentVersion(tdAsset)),time,&error),qPrintable(error));
    QCOMPARE(time.rows.size(),3); QCOMPARE(time.rows[1].md,1000.0); QCOMPARE(time.rows[1].tvd,990.0);
    DeviationTable survey;
    QCOMPARE(paleo::native_model::deviation(imports.catalog()->versionFilePath(imports.catalog()->currentVersion(devAsset)),survey,&error),paleo::native_model::Read::Ready);
    QCOMPARE(survey.stations.size(),3); QCOMPARE(survey.stations[1].inclinationDeg,10.0);
    ProjectDataFacade data; data.setCatalog(imports.catalog(),project);
    const auto well=imports.catalog()->entities("well").first();
    QCOMPARE(data.topsForAsset(well.id,topsAsset).size(),1);
    QCOMPARE(data.tdTableFor(well.id).size(),3);
    QVERIFY2(data.trajectoryFor(well.id,&error).has_value(),qPrintable(error));
  }
  void relocatedProjectUsesRelativeSourceReferences() {
    QTemporaryDir temp; const auto source=writeSource(temp);
    const auto before=temp.filePath("before"), after=temp.filePath("after"); QString relative,error;
    {
      PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(before);
      DataImportService::ImportOptions options; options.targetWellId="new:SYNTH-1";
      QVERIFY2(!imports.importProjectFile(source,options,&error).isEmpty(),qPrintable(error));
      relative=imports.catalog()->versions().first().path;
    }
    QVERIFY(QDir(temp.path()).rename("before","after"));
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(after);
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY2(WellLogRead::parse(QDir(after).filePath(relative),header,curves,&error),qPrintable(error));
    QCOMPARE(curves[1].values[0],-999.0);
  }
  void cancelledSessionPublishesNothing() {
    QTemporaryDir temp; const auto source=writeSource(temp), project=temp.filePath("project");
    PaleoProjectStore store; DataImportService imports(&store); imports.setProjectDir(project);
    auto session=imports.beginImport(); QVERIFY(session);
    DataImportService::ImportOptions options; options.targetWellId="new:SYNTH-1";
    DataImportService::produceFile(*session,source,options);
    QVERIFY2(session->error.isEmpty(),qPrintable(session->error));
    QVERIFY(!session->cat->versions().first().extra.value("nativeModel").toMap().isEmpty());
    session->cancelled=true; QString error;
    QVERIFY(imports.commitImport(*session,&error)!=DataImportService::CommitStatus::Committed);
    QVERIFY(imports.catalog()->versions().isEmpty()); QVERIFY(files(project+"/artifacts/raw").isEmpty());
    session.reset(); QVERIFY(QFileInfo::exists(source));
  }
  void legacyRawStillReads() {
    QTemporaryDir temp; const auto source=writeSource(temp);
    LasHeaderInfo header; QList<LasCurve> curves; QString error;
    QVERIFY2(WellLogRead::parse(source,header,curves,&error),qPrintable(error));
    QCOMPARE(curves[1].values[0],-999.0);
  }
  void closingAnotherProjectRemovesItsExternalBindings() {
    QTemporaryDir temp; const auto source=writeSource(temp);
    PaleoProjectStore store; DataImportService active(&store); active.setProjectDir(temp.filePath("active"));
    DataImportService::ImportOptions options; options.linkExternal=true; options.targetWellId="new:SYNTH-1";
    QString error; QVERIFY2(!active.importProjectFile(source,options,&error).isEmpty(),qPrintable(error));
    {
      PaleoProjectStore otherStore; DataImportService other(&otherStore);
      other.setProjectDir(temp.filePath("other")); options.targetWellId="new:CONFIRMED-OTHER";
      QVERIFY2(!other.importProjectFile(source,options,&error).isEmpty(),qPrintable(error));
    }
    // A closed project must not make this live version ambiguous and let its
    // broken database fall back to an otherwise readable external source.
    const auto binding=active.catalog()->versions().first().extra.value("nativeModel").toMap();
    const auto database=QDir(active.projectDir()).filePath(binding.value("path").toString());
    QVERIFY(QFile::setPermissions(database,QFile::permissions(database)|QFileDevice::WriteOwner));
    QFile corrupt(database); QVERIFY(corrupt.open(QIODevice::WriteOnly|QIODevice::Append)); corrupt.write("bad"); corrupt.close();
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY(!WellLogRead::parse(source,header,curves,&error));
    QVERIFY(error.contains("SHA-256"));
  }
};
QTEST_GUILESS_MAIN(TestNativeImport)
#include "tst_nativeimport.moc"
