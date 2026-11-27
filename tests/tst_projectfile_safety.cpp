// PW-075 / PW-126：project.paleo 成员路径越界与 formatVersion 缺失的判定。
#include "metadata/metastore.h"
#include "metadata/layermanifest.h"
#include "catalog/datacatalog.h"
#include "catalog/importledger.h"
#include "io/ziparchive.h"
#include "io/projectcopy.h"
#include "metadata/managedpath.h"
#include "metadata/paleoprojectfile.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

class TestProjectFileSafety : public QObject
{
  Q_OBJECT

private:
  static QString write(const QTemporaryDir &dir, const QByteArray &json)
  {
    const QString path = dir.filePath(QStringLiteral("project.paleo"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return QString();
    f.write(json);
    return path;
  }

private slots:
  void temporaryDataUsesProjectVolume()
  {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    auto scratch = paleo::managedpath::temporaryDirectory(project.path(), QStringLiteral("well-preview"));
    QVERIFY(scratch);
    QCOMPARE(QFileInfo(scratch->path()).absolutePath(), project.filePath(QStringLiteral("artifacts/staging")));
    const QString scratchPath = scratch->path();
    scratch.reset();
    QVERIFY(!QFileInfo::exists(scratchPath));
    QVERIFY(!paleo::managedpath::temporaryDirectory(project.filePath(QStringLiteral("missing")), QStringLiteral("import")));
    QVERIFY(!paleo::managedpath::temporaryDirectory(project.path(), QStringLiteral("../source")));
  }
  void verifiedCopyPublishesOnlyProjectMembers()
  {
    QTemporaryDir old, data, other;
    QVERIFY(old.isValid() && data.isValid() && other.isValid());
    const auto writeBytes = [](const QString &path, const QByteArray &bytes) {
      if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
      QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QString qgz = old.filePath("old.qgz"), member = "artifacts/raw/file.sgy";
    QVERIFY(writeBytes(old.filePath(member), "managed"));
    const QString points = QStringLiteral("artifacts/raw/well points.csv");
    QVERIFY(writeBytes(old.filePath(points), "X,Y\n1,2\n"));
    const auto pointsUri = [&](const QDir &dir) {
      QUrl url = QUrl::fromLocalFile(dir.filePath(points));
      url.setQuery(QStringLiteral("delimiter=%2C&xField=X&yField=Y"), QUrl::StrictMode);
      url.setFragment(QStringLiteral("points"));
      return url.toString(QUrl::FullyEncoded);
    };
    QVERIFY(writeBytes(old.filePath("unrelated.sgy"), "outside"));
    const QString text = QStringLiteral("<qgis><maplayer><datasource>%1</datasource></maplayer>"
      "<maplayer><datasource>./unrelated.sgy</datasource></maplayer>"
      "<maplayer><provider>delimitedtext</provider><datasource>%2</datasource></maplayer></qgis>")
      .arg(old.filePath(member), pointsUri(QDir(old.path())).toHtmlEscaped());
    QString error;
    QVERIFY(paleo::io::zipWriteArchive(qgz, {{"old.qgs", text.toUtf8()}}, {}, &error));
    const QString entry = paleoProjectFilePath(old.path());
    QVERIFY(paleo::projectcopy::copyVerified(qgz, data.path(), data.path(), error).isEmpty());
    auto manifest = projectFileForQgz(qgz);
    manifest.manifest.clear(); // The legacy store is still owned through the QGIS filename.
    QVERIFY(writeProjectFile(old.path(), manifest, &error));
    {
      DataCatalog catalog;
      QVERIFY2(catalog.open(old.path(), &error), qPrintable(error));
      paleo::imports::ImportLedger ledger;
      ledger.load(&catalog);
      CatalogAsset asset; asset.id = "old-asset"; asset.type = "seismic"; asset.format = "sgy"; asset.displayName = "linked";
      QVERIFY2(catalog.addAsset(asset, &error), qPrintable(error));
      CatalogVersion version; version.id = "old-version"; version.assetId = asset.id; version.stage = "RAW"; version.versionNumber = 1;
      version.path = old.filePath(member); version.sourceUri = version.path; version.fileName = "file.sgy"; version.managed = false;
      QVERIFY2(catalog.addVersion(version, &error), qPrintable(error));
      paleo::imports::LedgerBatch batch; batch.id = "batch-original"; batch.imported = 2;
      QVERIFY2(ledger.record(batch, &error), qPrintable(error));
    }
    const QString oldDb = qgz + ".project.sqlite";
    {
      LayerManifest layers(oldDb);
      LayerDeclaration layer; layer.layerId = "test-layer"; layer.source = old.filePath(member); layer.type = "raster";
      QVERIFY2(layers.upsert(layer, &error), qPrintable(error));
    }
    MetaStore::closeConnectionsFor(oldDb);
    QVERIFY(paleo::projectcopy::copyVerified(entry, old.path(), data.path(), error).isEmpty());
    QVERIFY(paleo::projectcopy::copyVerified(entry, other.path(), data.path(), error).isEmpty());
    QVERIFY(paleo::projectcopy::copyVerified(entry, data.path(), data.path(), error, []{return true;}).isEmpty());
    QVERIFY(QDir(data.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    QStringList review;
    const QString copy = paleo::projectcopy::copyVerified(entry, data.path(), data.path(), error, {}, {}, &review);
    QVERIFY2(!copy.isEmpty(), qPrintable(error));
    QCOMPARE(review.size(), 1);
    QVERIFY(review.first().contains("old-version"));
    QDir root(QFileInfo(copy).absolutePath());
    QVERIFY(QFileInfo::exists(root.filePath(member)));
    {
      DataCatalog catalog;
      catalog.setLockedReadOnly(true);
      QVERIFY2(catalog.open(root.path(), &error), qPrintable(error));
      paleo::imports::ImportLedger ledger;
      ledger.load(&catalog);
      QCOMPARE(ledger.count(), 1);
      QCOMPARE(ledger.batches().first().id, QStringLiteral("batch-original"));
      QCOMPARE(ledger.batches().first().imported, 2);
    }
    const QString copiedDb = root.filePath("old.qgz.project.sqlite");
    {
      LayerManifest layers(copiedDb); layers.setReadOnly(true);
      LayerDeclaration layer;
      QVERIFY2(layers.find("test-layer", &layer, &error), qPrintable(error));
      QCOMPARE(layer.source, root.filePath(member));
    }
    MetaStore::closeConnectionsFor(copiedDb);
    QVERIFY(!QFileInfo::exists(root.filePath("unrelated.sgy")));
    QVERIFY(QFileInfo::exists(old.filePath("unrelated.sgy")));
    QByteArray xml;
    QVERIFY(paleo::io::zipExtractBytes(paleo::io::zipReadArchive(root.filePath("old.qgz"), &error), "old.qgs", &xml, &error));
    QVERIFY(xml.contains(root.filePath(member).toUtf8()));
    QVERIFY(xml.contains(old.filePath("unrelated.sgy").toUtf8()));
    QVERIFY(xml.contains(pointsUri(root).toHtmlEscaped().toUtf8()));
    QVERIFY(paleo::projectcopy::copyVerified(entry, data.path(), data.path(), error).isEmpty());
  }
  void nativeBackendRoundTripAndUnknownBackendRejected()
  {
    QTemporaryDir dir;
    auto file=projectFileForQgz(dir.filePath("project.qgz"));
    file.dataBackend="data_service_versioned";
    file.catalog="artifacts/metadata/catalog.sqlite";
    QString error; QVERIFY2(writeProjectFile(dir.path(),file,&error),qPrintable(error));
    bool ok=false; const auto read=readProjectFile(dir.filePath("project.paleo"),&ok,&error);
    QVERIFY2(ok,qPrintable(error)); QCOMPARE(read.dataBackend,file.dataBackend);
    QCOMPARE(read.catalog,file.catalog); QCOMPARE(read.projectId,file.projectId);
    const auto path=write(dir,R"({"format":"paleo-project","formatVersion":1,"dataBackend":"future-incompatible","members":{}})");
    readProjectFile(path,&ok,&error); QVERIFY(!ok); QVERIFY(error.contains("backend"));
  }
  void relativeMembersAccepted()
  {
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","formatVersion":1,
      "members":{"qgz":"a.qgz","catalog":"artifacts/metadata/catalog.json","gpkg":"a.gpkg"}})");
    bool ok = false;
    QString err;
    const PaleoProjectFile f = readProjectFile(p, &ok, &err);
    QVERIFY2(ok, qPrintable(err));
    QCOMPARE(f.gpkg, QStringLiteral("a.gpkg"));
  }

  void boundedReadRejectsOversizedManifest()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeProjectFile(dir.path(), projectFileForQgz(dir.filePath(QStringLiteral("test.qgz")))));
    const QString path = paleoProjectFilePath(dir.path());
    bool ok = false;
    QString error;
    readProjectFile(path, &ok, &error, 16);
    QVERIFY(!ok);
    QVERIFY(error.contains(QStringLiteral("16-byte")));
    readProjectFile(path, &ok, &error, QFileInfo(path).size());
    QVERIFY2(ok, qPrintable(error));
  }

  void escapingMembersRejected_data()
  {
    QTest::addColumn<QByteArray>("member");
    QTest::newRow("dotdot") << QByteArray("../../etc/x.gpkg");
    QTest::newRow("absolute") << QByteArray("/etc/x.gpkg");
    QTest::newRow("drive") << QByteArray("C:/x.gpkg");
    QTest::newRow("backslash-dotdot") << QByteArray("sub\\\\..\\\\..\\\\x.gpkg");
  }
  void escapingMembersRejected()
  {
    QFETCH(QByteArray, member);
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","formatVersion":1,"members":{"gpkg":")" +
                                     member + R"("}})");
    bool ok = true;
    QString err;
    readProjectFile(p, &ok, &err);
    QVERIFY(!ok);
    QVERIFY2(err.contains(QStringLiteral("escapes")), qPrintable(err));
  }

  void escapingBasemapMembersRejected_data()
  {
    escapingMembersRejected_data();
  }
  void escapingBasemapMembersRejected()
  {
    QFETCH(QByteArray, member);
    QTemporaryDir dir;
    for (const QByteArray &role : {QByteArray("topo"), QByteArray("hillshade")}) {
      const QString path = write(dir, R"({"format":"paleo-project","formatVersion":1,"members":{},)"
                                      R"("map":{"basemap":{")" + role + R"(":")" + member + R"("}}})");
      bool ok = true;
      QString error;
      readProjectFile(path, &ok, &error);
      QVERIFY(!ok);
      QVERIFY2(error.contains(QStringLiteral("escapes")), qPrintable(error));
    }
  }

  void missingVersionIsNotNewer()
  {
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","members":{}})");
    bool ok = true;
    QString err;
    readProjectFile(p, &ok, &err);
    QVERIFY(!ok);
    QVERIFY2(!err.contains(QStringLiteral("newer")), qPrintable(err));
    QVERIFY(err.contains(QStringLiteral("missing or invalid")));
  }

  void newerVersionStillReportedAsNewer()
  {
    QTemporaryDir dir;
    const QString p = write(dir, R"({"format":"paleo-project","formatVersion":7,"members":{}})");
    bool ok = true;
    QString err;
    readProjectFile(p, &ok, &err);
    QVERIFY(!ok);
    QVERIFY(err.contains(QStringLiteral("newer")));
  }
};

QTEST_GUILESS_MAIN(TestProjectFileSafety)
#include "tst_projectfile_safety.moc"
