#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TestAreaImportFinalization : public QObject {
  Q_OBJECT
private slots:
  void batchBuildsOneRevisionAndCancellationKeepsTheHead() {
    QTemporaryDir temp; const auto project = temp.filePath("project");
    const auto seed = temp.filePath("seed.txt");
    QFile file(seed); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("synthetic"); file.close();
    paleo::data::DataService service; std::string detail;
    QVERIFY2(service.createProject(paleo::area_model::directory(project).toStdString(), "test", detail), detail.c_str());
    paleo::data::ParsedFile parsed; parsed.kind = "synthetic"; parsed.status = "parsed";
    for (int i = 0; i < 2; ++i) {
      paleo::data::SeismicHorizon horizon; horizon.horizonName = i ? "T2" : "T1";
      horizon.coordinateSystem = "EPSG:32650"; horizon.coordinateUnit = "m";
      horizon.timeUnit = "ms"; horizon.timeType = "TWT"; horizon.timeReference = "fixture";
      horizon.points.push_back({100, 200, 500000, 4000000, 1000. + 100. * i});
      parsed.horizons.push_back(horizon);
    }
    long long sourceId = 0;
    const auto source = seed.toStdString();
    const auto sha = QCryptographicHash::hash("synthetic", QCryptographicHash::Sha256).toHex().toStdString();
    QVERIFY2(service.importPreparedFile(source, source, source, sha, parsed, sourceId, detail), detail.c_str());
    service.closeProject();
    QString error;
    for (int i = 0; i < 2; ++i) {
      const auto path = temp.filePath(QString("W%1.las").arg(i)); QFile log(path);
      QVERIFY(log.open(QIODevice::WriteOnly));
      log.write(QString("~Version\nVERS. 2.0 :\n~Well\nWELL. W%1 :\n~Curve\nDEPT.M :\nGR.API :\n~A\n1000 10\n1001 20\n").arg(i).toUtf8()); log.close();
      paleo::data::ImportReport report;
      QVERIFY2(paleo::area_model::importFileWithCrs(project, path, report, &error, {}, {}, {}, {},
          paleo::area_model::ImportFinalization::Deferred), qPrintable(error));
      QCOMPARE(report.imported, 1);
    }
    QVERIFY(service.openProject(paleo::area_model::directory(project).toStdString(), detail));
    std::vector<paleo::data::StratigraphicSchemeSummary> schemes;
    QVERIFY(service.listStratigraphicSchemes(schemes, detail)); QVERIFY(schemes.empty()); service.closeProject();
    QVERIFY2(paleo::area_model::finalizeImport(project, &error), qPrintable(error));
    QVERIFY(service.openProject(paleo::area_model::directory(project).toStdString(), detail));
    QVERIFY(service.listStratigraphicSchemes(schemes, detail)); QCOMPARE(schemes.size(), size_t(1));
    const auto id = schemes.front().id;
    std::vector<paleo::data::StratigraphicSchemeRevisionSummary> revisions;
    QVERIFY(service.listStratigraphicSchemeRevisions(id, revisions, detail)); QCOMPARE(revisions.size(), size_t(1));
    paleo::data::ModelChangeHead before; QVERIFY(service.readModelChangeHead(before, detail)); service.closeProject();
    QVERIFY(!paleo::area_model::finalizeImport(project, &error, [] { return true; }));
    QVERIFY(!error.isEmpty());
    QVERIFY(service.openProject(paleo::area_model::directory(project).toStdString(), detail));
    paleo::data::ModelChangeHead after; QVERIFY(service.readModelChangeHead(after, detail));
    QCOMPARE(after.sequence, before.sequence);
    QVERIFY(service.listStratigraphicSchemeRevisions(id, revisions, detail)); QCOMPARE(revisions.size(), size_t(1));
    service.closeProject();
    // The ordinary single-input API still completes derived work immediately.
    paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importFileWithCrs(project, temp.filePath("W0.las"), report, &error, {}, {}, {}, {}), qPrintable(error));
    QCOMPARE(report.cached, 1);
    QVERIFY(service.openProjectReadOnly(paleo::area_model::directory(project).toStdString(), detail));
    QVERIFY(service.listStratigraphicSchemeRevisions(id, revisions, detail)); QCOMPARE(revisions.size(), size_t(2));
  }
};
QTEST_GUILESS_MAIN(TestAreaImportFinalization)
#include "tst_areaimport_finalization.moc"
