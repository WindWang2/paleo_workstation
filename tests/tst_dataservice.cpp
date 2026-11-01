#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <QtEndian>
#include <paleo_data/service.h>
#include <paleo_data/catalog.h>
#include "../src/catalog/datacatalog.h"
#include "../src/io/ziparchive.h"

namespace {
std::string u(const QString &value) { return value.toUtf8().toStdString(); }
#ifdef Q_OS_WIN
QByteArray syntheticBiffWorkbook() {
  const auto word = [](quint16 n) { QByteArray b(2, '\0'); qToLittleEndian(n, b.data()); return b; };
  const auto dword = [](quint32 n) { QByteArray b(4, '\0'); qToLittleEndian(n, b.data()); return b; };
  const auto record = [&](quint16 type, const QByteArray &body) { return word(type) + word(quint16(body.size())) + body; };
  const auto bof = [&](quint16 kind) { return record(0x809, word(0x600) + word(kind) + word(0) + word(1997) + dword(0) + dword(0)); };
  const auto bounds = [&](quint32 offset) { return record(0x85, dword(offset) + QByteArray::fromHex("00000500") + "Sheet"); };
  const auto head = bof(5) + record(0x42, word(1252));
  QByteArray stream = head + bounds(quint32(head.size() + bounds(0).size() + 4)) + record(10, {});
  stream += bof(16) + record(0x200, dword(0) + dword(2) + word(0) + word(1) + word(0));
  for (int row = 0; row < 2; ++row) {
    double value = row ? -31 : 12.5; quint64 bits; memcpy(&bits, &value, sizeof(bits));
    QByteArray bytes(8, '\0'); qToLittleEndian(bits, bytes.data());
    stream += record(0x203, word(quint16(row)) + word(0) + word(0) + bytes);
  }
  // A regular CFBF stream exceeds the 4096-byte mini-stream cutoff. Pad with
  // one opaque drawing record inside the sheet, before its closing EOF.
  stream += record(0xec, QByteArray(4608 - stream.size() - 8, '\0')) + record(10, {});
  QByteArray header(512, '\0'); header.replace(0, 8, QByteArray::fromHex("D0CF11E0A1B11AE1"));
  header.replace(24, 10, word(0x3e) + word(3) + word(0xfffe) + word(9) + word(6));
  for (const auto pair : {qMakePair(44, 1u), qMakePair(56, 4096u), qMakePair(60, 0xfffffffeu), qMakePair(68, 0xfffffffeu)})
    header.replace(pair.first, 4, dword(pair.second));
  for (int offset = 76; offset < 512; offset += 4) header.replace(offset, 4, dword(offset == 76 ? 10 : 0xffffffffu));
  const auto entry = [&](const QString &name, char type, quint32 start, quint32 size, quint32 child) {
    QByteArray e(128, '\0'); const auto chars = name + QChar(0);
    for (int i = 0; i < chars.size(); ++i) e.replace(i * 2, 2, word(chars[i].unicode()));
    e.replace(64, 2, word(quint16(chars.size() * 2))); e[66] = type; e[67] = 1;
    e.replace(68, 12, dword(0xffffffffu) + dword(0xffffffffu) + dword(child));
    e.replace(116, 8, dword(start) + dword(size)); return e;
  };
  QByteArray directory = entry("Root Entry", 5, 0xfffffffeu, 0, 1) + entry("Workbook", 2, 1, 4608, 0xffffffffu);
  directory.append(QByteArray(512 - directory.size(), '\0'));
  QByteArray fat(512, char(-1));
  for (int sector = 0; sector < 11; ++sector) fat.replace(sector * 4, 4, dword(sector == 10 ? 0xfffffffdu : sector == 0 || sector == 9 ? 0xfffffffeu : quint32(sector + 1)));
  return header + directory + stream + fat;
}
#endif
}

class TestDataService : public QObject {
  Q_OBJECT
private slots:
  void shippedBiffRuntimeReadsSignedCells() {
#ifndef Q_OS_WIN
    QSKIP("The private BIFF runtime is packaged by the Windows build.");
#else
    QVERIFY(QFileInfo::exists(QCoreApplication::applicationDirPath() + "/paleo_freexl.dll"));
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QFile file(directory.filePath("synthetic.xls")); QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes = syntheticBiffWorkbook(); QCOMPARE(file.write(bytes), bytes.size()); file.close();
    const auto parsed = paleo::data::parseFile(u(file.fileName()));
    QVERIFY2(parsed.status != "failed", parsed.message.c_str());
    QCOMPARE(parsed.sourceTables.size(), size_t(1));
    QCOMPARE(parsed.sourceTables[0].cells.size(), size_t(2));
    QCOMPARE(QString::fromUtf8(parsed.sourceTables[0].cells[0].text).toDouble(), 12.5);
    QCOMPARE(QString::fromUtf8(parsed.sourceTables[0].cells[1].text).toDouble(), -31.0);
#endif
  }
  void petrelSurfaceUsesContentAndSurvivesReopening_data() {
    QTest::addColumn<QString>("fileName");
    QTest::newRow("extensionless") << QString("SYNTH_SURFACE");
    QTest::newRow("text") << QString("surface.txt");
    QTest::newRow("legacy-dat") << QString("surface.dat");
  }
  void petrelSurfaceUsesContentAndSurvivesReopening() {
    QFETCH(QString, fileName);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto path = directory.filePath(fileName);
    const QByteArray original("  inline : 10 xline : 20 1000 2000 200\r\nINLINE : 11 XLINE : 21 1010 2010 220\r\n");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size()); file.close();
    const auto project = directory.filePath("model");
    paleo::data::DataService service; std::string error;
    QVERIFY2(service.createProject(u(project), "surface", error), error.c_str());
    paleo::data::ImportOptions options; options.copySources = false;
    paleo::data::ImportReport report;
    QVERIFY2(service.importFile(u(path), options, report, error), error.c_str());
    service.closeProject();
    QVERIFY2(service.openProjectReadOnly(u(project), error), error.c_str());
    std::vector<paleo::data::DataEntry> entries;
    QVERIFY(service.listData("horizons", 0, entries, error)); QCOMPARE(entries.size(), size_t(1));
    paleo::data::SeismicHorizon horizon;
    QVERIFY2(service.readHorizon(entries.front().id, 0, 10, horizon, error), error.c_str());
    QCOMPARE(horizon.points.size(), size_t(2));
    QCOMPARE(horizon.points.back().inlineNumber, 11);
    QCOMPARE(horizon.points.back().crosslineNumber, 21);
    QCOMPARE(horizon.points.back().x, 1010.0); QCOMPARE(horizon.points.back().zTime, 220.0);
    QVERIFY(horizon.coordinateSystem.empty()); QVERIFY(horizon.timeUnit.empty());
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
  }
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
