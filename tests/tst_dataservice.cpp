#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QtEndian>
#include <paleo_data/service.h>
#include <paleo_data/catalog.h>
#include "../src/catalog/datacatalog.h"
#include "../src/io/ziparchive.h"

namespace {
std::string u(const QString &value) { return value.toUtf8().toStdString(); }
}

class TestDataService : public QObject {
  Q_OBJECT
private slots:
  void catalogRoundTripsThroughSubmodule() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QString error;
    {
      DataCatalog catalog; QVERIFY2(catalog.open(directory.path(), &error), qPrintable(error));
      CatalogEntity entity; entity.id = "well-1"; entity.entityType = "well";
      entity.name = QString::fromUtf8("测试井"); entity.kb = qQNaN();
      entity.extra.insert("future_extension", QVariantMap{{"kept", true}});
      QVERIFY2(catalog.addEntity(entity, &error), qPrintable(error));
      CatalogAsset asset{"ast-1", "well_log", "las", "input.las"};
      QVERIFY(catalog.addAsset(asset, &error));
      CatalogVersion version; version.id = "ver-1"; version.assetId = asset.id;
      version.managed = false; version.path = directory.filePath("external.las");
      version.extra.insert("nested", QVariantMap{{"number", 42}});
      QVERIFY(catalog.addVersion(version, &error));
    }
    const auto name = QStringLiteral("tst_dataservice_catalog");
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", name);
      db.setDatabaseName(directory.filePath("artifacts/metadata/catalog.sqlite"));
      db.setConnectOptions("QSQLITE_OPEN_READONLY");
      QVERIFY2(db.open(), qPrintable(db.lastError().text()));
      paleo::data::CatalogStorage store(u(name));
      paleo::data::CatalogSnapshot snapshot; std::string detail;
      QVERIFY2(store.load(snapshot, detail), detail.c_str());
      QCOMPARE(snapshot.entities.size(), size_t(1));
      QCOMPARE(QString::fromUtf8(snapshot.entities[0].name), QString::fromUtf8("测试井"));
      QVERIFY(QString::fromUtf8(snapshot.entities[0].extraJson).contains("future_extension"));
      QCOMPARE(snapshot.versions[0].id, std::string("ver-1"));
    }
    QSqlDatabase::removeDatabase(name);
    DataCatalog reopened; QVERIFY2(reopened.open(directory.path(), &error), qPrintable(error));
    QVERIFY(std::isnan(reopened.entities().first().kb));
    QCOMPARE(reopened.entities().first().extra.value("future_extension").toMap().value("kept").toBool(), true);
    QCOMPARE(reopened.versionById("ver-1").extra.value("nested").toMap().value("number").toInt(), 42);
  }

  void nativeSurveyReadsUpdatesAndReopens() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto project = directory.filePath("survey");
    const auto path = directory.filePath("input.las");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nSP.MV : potential\n~Ascii\n100 -20\n101 -999.25\n"); file.close();
    long long curveId = 0; std::string error;
    {
      paleo::data::DataService service;
      QVERIFY2(service.createProject(u(project), "survey", error), error.c_str());
      paleo::data::ImportOptions options; options.copySources = true;
      paleo::data::ImportReport report;
      QVERIFY2(service.importFile(u(path), options, report, error), error.c_str());
      std::vector<paleo::data::DataEntry> curves;
      QVERIFY(service.listData("curves", 0, curves, error)); QCOMPARE(curves.size(), size_t(1));
      curveId = curves[0].id;
      QVERIFY2(service.updateCurveValue(curveId, 0, -12, error), error.c_str());
    }
    paleo::data::DataService reader;
    QVERIFY2(reader.openProjectReadOnly(u(project), error), error.c_str());
    paleo::data::CurveSlice values;
    QVERIFY(reader.readCurveSlice(curveId, paleo::data::Missing, paleo::data::Missing, true, values, error));
    QCOMPARE(values.values[0], -12.0); QCOMPARE(values.rawValues[0], -20.0);
    QCOMPARE(values.quality[0], static_cast<unsigned char>(4));
    QVERIFY(std::isnan(values.values[1]));
    QVERIFY(!reader.updateCurveValue(curveId, 0, 999, error));
  }

  void hostZipAndServiceParserLinkTogether() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto path = directory.filePath("report.docx"); QString error;
    const QByteArray xml = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>service bridge</w:t></w:r></w:p></w:body></w:document>";
    QVERIFY2(paleo::io::zipWriteArchive(path, {{"word/document.xml", xml}}, {}, &error), qPrintable(error));
    const auto parsed = paleo::data::parseFile(u(path));
    QVERIFY2(parsed.status != "failed", parsed.message.c_str());
    QVERIFY(QString::fromUtf8(parsed.documentText).contains("service bridge"));
  }

};
QTEST_GUILESS_MAIN(TestDataService)
#include "tst_dataservice.moc"
