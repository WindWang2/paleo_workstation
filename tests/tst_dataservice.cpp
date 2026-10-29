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

  void planarRangeMergesWellsAndSeismicGrid() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto project = directory.filePath("area");
    std::string error;
    // 3×2 规则网格 SEG-Y（IEEE、4 样点、CDP 坐标；SgyCoordinateMapper 拟合
    // 需要 ≥6 个坐标采样，2×2 不够）。
    const auto sgy = directory.filePath("grid.sgy");
    {
      QByteArray bytes(3200, ' ');
      QByteArray bin(400, '\0');
      qToBigEndian<quint16>(2000, bin.data() + 16);
      qToBigEndian<quint16>(4, bin.data() + 20);
      qToBigEndian<quint16>(5, bin.data() + 24);
      bytes += bin;
      const struct { int il, xl; double x, y; } traces[6] = {
          {10, 20, 1000, 5000}, {10, 21, 1050, 5000}, {10, 22, 1100, 5000},
          {11, 20, 1000, 5100}, {11, 21, 1050, 5100}, {11, 22, 1100, 5100}};
      for (const auto &trace : traces) {
        QByteArray header(240, '\0');
        qToBigEndian<qint16>(1, header.data() + 70);
        qToBigEndian<qint32>(qint32(trace.x), header.data() + 72);
        qToBigEndian<qint32>(qint32(trace.y), header.data() + 76);
        qToBigEndian<qint32>(qint32(trace.x), header.data() + 180);
        qToBigEndian<qint32>(qint32(trace.y), header.data() + 184);
        qToBigEndian<qint32>(trace.il, header.data() + 188);
        qToBigEndian<qint32>(trace.xl, header.data() + 192);
        bytes += header + QByteArray(4 * 4, '\0');
      }
      QFile file(sgy); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(bytes); file.close();
    }
    {
      paleo::data::DataService service;
      QVERIFY2(service.createProject(u(project), "planar", error), error.c_str());
      paleo::data::WellInfo well;
      well.wellName = "RANGE-1"; well.wellheadX = 1250; well.wellheadY = 5250;
      well.coordinateSystem = "EPSG:32650"; well.coordinateUnit = "m";
      long long id = 0;
      QVERIFY2(service.createWell(well, id, error), error.c_str());
      service.closeProject();
    }
    {
      const auto name = QStringLiteral("tst_dataservice_planar");
      auto db = QSqlDatabase::addDatabase("QSQLITE", name);
      db.setDatabaseName(project + "/data.sqlite");
      QVERIFY2(db.open(), qPrintable(db.lastError().text()));
      QSqlQuery query(db);
      const auto geometry = QStringLiteral(
          R"({"traceCount":6,"inlineMin":10,"inlineMax":11,"crosslineMin":20,"crosslineMax":22,"crs":"","unit":""})");
      const auto sampling = QStringLiteral(
          R"({"sampleCount":4,"firstTime":0,"sampleInterval":2,"timeUnit":"ms","timeType":"","timeReference":""})");
      QVERIFY2(query.exec(QStringLiteral(
                    "INSERT INTO volumes(source_id,name,path,format,amplitude_unit,geometry,sampling) "
                    "VALUES(NULL,'grid','%1','SEG-Y','','%2','%3')")
                    .arg(QDir::toNativeSeparators(sgy), geometry, sampling)),
               qPrintable(query.lastError().text()));
      db.close(); QSqlDatabase::removeDatabase(name);
    }
    paleo::data::DataService reader;
    QVERIFY2(reader.openProjectReadOnly(u(project), error), error.c_str());
    paleo::data::ProjectPlanarRange range;
    QVERIFY2(reader.readProjectPlanarRange(range, error), error.c_str());
    QVERIFY(range.wells.valid);
    QCOMPARE(range.wells.minX, 1250.0); QCOMPARE(range.wells.maxX, 1250.0);
    QCOMPARE(range.wells.minY, 5250.0); QCOMPARE(range.wells.maxY, 5250.0);
    QVERIFY2(range.seismic.valid, "规则网格体应可换算出平面范围");
    QCOMPARE(range.seismic.minX, 1000.0); QCOMPARE(range.seismic.maxX, 1100.0);
    QCOMPARE(range.seismic.minY, 5000.0); QCOMPARE(range.seismic.maxY, 5100.0);
    QVERIFY(range.merged.valid);
    QCOMPARE(range.merged.minX, 1000.0); QCOMPARE(range.merged.maxX, 1250.0);
    QCOMPARE(range.merged.minY, 5000.0); QCOMPARE(range.merged.maxY, 5250.0);
  }
};
QTEST_GUILESS_MAIN(TestDataService)
#include "tst_dataservice.moc"
