#include <QtTest>
#include <QLabel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <cmath>
#include <memory>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/previewdoc.h"
#include "../src/services/welllogset.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"
#include "importuxfixtures.h"

namespace
{
struct PreviewStack
{
  QTemporaryDir temporary;
  QString project = temporary.filePath(QStringLiteral("project"));
  PaleoProjectStore store;
  std::unique_ptr<DataImportService> imports;
  std::unique_ptr<DataPreviewTabs> preview;

  PreviewStack()
  {
    QDir().mkpath(project);
    store.setProjectPaths(project + QStringLiteral("/project.qgz"),
                          project + QStringLiteral("/project.gpkg"),
                          project + QStringLiteral("/metadata/project.sqlite"));
    reopen();
  }
  void reopen()
  {
    preview.reset();
    imports.reset();
    imports = std::make_unique<DataImportService>(&store);
    imports->setProjectDir(project);
    preview = std::make_unique<DataPreviewTabs>();
    // No task service: the production fallback completes before openAsset
    // returns. These assertions inspect the assembled widget, not a DTO helper.
    preview->setImportService(imports.get());
  }
  QString source(const QString &name, const QByteArray &bytes) const
  {
    const QString path = temporary.filePath(QStringLiteral("source/") + name);
    return ImportUxFixtures::write(path, bytes) ? path : QString();
  }
  QWidget *currentPage() const
  {
    auto *tabs = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
    return tabs ? tabs->currentWidget() : nullptr;
  }
  WellComposite::WellCompositePanel *currentPanel() const
  {
    auto *page = currentPage();
    return page ? page->findChild<WellComposite::WellCompositePanel *>(
        QStringLiteral("wellCompositePanel")) : nullptr;
  }
};

WellComposite::CurveData curveNamed(const WellComposite::ComprehensiveWellData &data,
                                    const QString &name)
{
  for (const auto &curve : data.continuousCurves)
    if (curve.name == name) return curve;
  return {};
}

QString onlyWellId(const DataCatalog &catalog)
{
  const auto wells = catalog.entities(QStringLiteral("well"));
  return wells.size() == 1 ? wells.first().id : QString();
}
}

class TestImportPreview : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  void realMixedImportOpensBothAssetsWithPerSourceMetricDepths()
  {
    PreviewStack stack;
    QVERIFY2(stack.imports->catalogWritable(), qPrintable(stack.imports->catalogOpenError()));
    const QByteArray feetRows("3280.839895013123,10\n3284.120734908136,20\n");
    const QString xml = stack.source(QStringLiteral("complete.xml"), ImportUxFixtures::xml());
    const QString csv = stack.source(QStringLiteral("RES.csv"),
        ImportUxFixtures::csv("A1", "RES[ohm.m]", feetRows, "MD[ft]"));
    QVERIFY(!xml.isEmpty());
    QVERIFY(!csv.isEmpty());
    QString error;
    const QString xmlAsset = stack.imports->importProjectFile(xml, &error);
    QVERIFY2(!xmlAsset.isEmpty(), qPrintable(error));
    const QString csvAsset = stack.imports->importProjectFile(csv, &error);
    QVERIFY2(!csvAsset.isEmpty(), qPrintable(error));
    QCOMPARE(stack.imports->catalog()->entities(QStringLiteral("well")).size(), 1);
    const auto refs = WellLogSet::wellCurveIndex(stack.imports->catalog(), stack.project,
                                               onlyWellId(*stack.imports->catalog()));
    QCOMPARE(refs.size(), 4);
    for (const auto &ref : refs)
    {
      QCOMPARE(ref.indexBasis, QStringLiteral("MD"));
      QVERIFY(!ref.format.isEmpty());
    }

    // Open each through the public UI entry point: the ft file is first a
    // sibling, then the current document. Both paths must convert its own axis.
    for (const QString &asset : {xmlAsset, csvAsset})
    {
      stack.preview->openAsset(asset);
      auto *panel = stack.currentPanel();
      QVERIFY(panel);
      QCOMPARE(panel->wellName(), QStringLiteral("A1"));
      const auto data = panel->currentData();
      QCOMPARE(data.continuousCurves.size(), 4);
      const auto gr = curveNamed(data, QStringLiteral("GR"));
      const auto res = curveNamed(data, QStringLiteral("RES"));
      QCOMPARE(gr.depths, QVector<float>({1000, 1000.5f, 1001}));
      QCOMPARE(gr.values, QVector<float>({40, 45, 50}));
      QCOMPARE(res.values, QVector<float>({10, 20}));
      QCOMPARE(res.depths.size(), 2);
      QVERIFY2(std::abs(res.depths.first() - 1000.0f) < 0.001f,
               qPrintable(QStringLiteral("ft source displayed at %1 m").arg(res.depths.first())));
      QVERIFY(std::abs(res.depths.last() - 1001.0f) < 0.001f);
      QCOMPARE(res.unit, QStringLiteral("ohm.m"));
      auto *hint = stack.currentPage()->findChild<QLabel *>(QStringLiteral("lasPendingHint"));
      QVERIFY(hint);
      QVERIFY(hint->isHidden());
    }
    QCOMPARE(stack.preview->tabCount(), 2);
  }

  void unsafeSourceAxisIsExcludedButValidSiblingRemainsVisible_data()
  {
    QTest::addColumn<bool>("xml");
    QTest::addColumn<QByteArray>("axis");
    QTest::addColumn<QString>("reasonDetail");
    QTest::newRow("csv-time") << false << QByteArray("TIME[s]") << QStringLiteral("TIME");
    QTest::newRow("csv-tvd") << false << QByteArray("TVD[m]") << QStringLiteral("TVD");
    QTest::newRow("csv-tvdss") << false << QByteArray("TVDSS[m]") << QStringLiteral("TVDSS");
    QTest::newRow("csv-unknown-basis") << false << QByteArray("DEPTH[m]") << QStringLiteral("[m]");
    QTest::newRow("csv-unknown-unit") << false << QByteArray("MD[furlong]") << QStringLiteral("furlong");
    QTest::newRow("csv-missing-unit") << false << QByteArray("MD") << QStringLiteral("[]");
    QTest::newRow("xml-time") << true << QByteArray("TIME[ms]") << QStringLiteral("TIME");
    QTest::newRow("xml-tvd") << true << QByteArray("TVD[m]") << QStringLiteral("TVD");
    QTest::newRow("xml-unknown-basis") << true << QByteArray("DEPTH[m]") << QStringLiteral("[m]");
    QTest::newRow("xml-unknown-unit") << true << QByteArray("MD[furlong]") << QStringLiteral("furlong");
  }

  void unsafeSourceAxisIsExcludedButValidSiblingRemainsVisible()
  {
    QFETCH(bool, xml);
    QFETCH(QByteArray, axis);
    QFETCH(QString, reasonDetail);
    PreviewStack stack;
    QString error;
    const QString valid = stack.source(QStringLiteral("valid.csv"),
        ImportUxFixtures::csv("A1", "SAFE[mV]", "1000,3\n1001,4\n"));
    const QString validAsset = stack.imports->importProjectFile(valid, &error);
    QVERIFY2(!validAsset.isEmpty(), qPrintable(error));
    QByteArray unsafeBytes;
    if (xml)
    {
      unsafeBytes = ImportUxFixtures::xml();
      unsafeBytes.replace("MD[m]", axis);
    }
    else unsafeBytes = ImportUxFixtures::csv("A1", "UNSAFE[API]", "1000,70\n1001,80\n", axis);
    const QString unsafeName = xml ? QStringLiteral("unsafe.xml") : QStringLiteral("unsafe.csv");
    const QString unsafe = stack.source(unsafeName, unsafeBytes);
    const QString unsafeAsset = stack.imports->importProjectFile(unsafe, &error);
    // Import preserves scientifically valid raw samples. The MD-only chart
    // must reject their incompatible basis without discarding the valid file.
    QVERIFY2(!unsafeAsset.isEmpty(), qPrintable(error));
    QCOMPARE(stack.imports->catalog()->entities(QStringLiteral("well")).size(), 1);
    QCOMPARE(stack.imports->catalog()->assets().size(), 2);
    for (const QString &asset : {validAsset, unsafeAsset})
    {
      stack.preview->openAsset(asset);
      auto *panel = stack.currentPanel();
      QVERIFY(panel);
      const auto data = panel->currentData();
      QCOMPARE(data.continuousCurves.size(), 1);
      const auto safe = curveNamed(data, QStringLiteral("SAFE"));
      QCOMPARE(safe.depths, QVector<float>({1000, 1001}));
      QCOMPARE(safe.values, QVector<float>({3, 4}));
      QVERIFY(curveNamed(data, QStringLiteral("UNSAFE")).isEmpty());
      QVERIFY(curveNamed(data, QStringLiteral("GR")).isEmpty());
      auto *hint = stack.currentPage()->findChild<QLabel *>(QStringLiteral("lasPendingHint"));
      QVERIFY(hint);
      QVERIFY(!hint->isHidden());
      QVERIFY2(hint->text().contains(unsafeName), qPrintable(hint->text()));
      QVERIFY2(hint->text().contains(QStringLiteral("MD")), qPrintable(hint->text()));
      QVERIFY2(hint->text().contains(reasonDetail), qPrintable(hint->text()));
      QVERIFY2(hint->text().contains(QStringLiteral("原始数据保留")), qPrintable(hint->text()));
    }
    QCOMPARE(stack.imports->catalog()->assets().size(), 2);
  }

  void managedHeaderMetadataAndRealUiKeepCurveSourcesConsistent()
  {
    PreviewStack stack;
    const QString path = stack.source(QStringLiteral("large.xml"), ImportUxFixtures::paddedXml());
    QString error;
    const QString asset = stack.imports->importProjectFile(path, &error);
    QVERIFY2(!asset.isEmpty(), qPrintable(error));
    const auto version = stack.imports->catalog()->currentVersion(asset);
    LasHeaderInfo stored, presented;
    QVERIFY(WellLogSet::storedHeader(version, &stored));
    PreviewDocService document(stack.imports.get());
    const QString managed = stack.imports->absolutePath(asset);
    QVERIFY2(document.wellLogHeaderAt(managed, &presented, &error), qPrintable(error));
    QCOMPARE(presented.curveNames, stored.curveNames);
    QCOMPARE(presented.wellName, QStringLiteral("A1"));
    QCOMPARE(presented.indexBasis, QStringLiteral("MD"));
    QCOMPARE(version.extra.value(QStringLiteral("logCurves")).toStringList(), stored.curveNames.mid(1));
    stack.preview->openAsset(asset);
    auto *panel = stack.currentPanel();
    QVERIFY(panel);
    const auto data = panel->currentData();
    QCOMPARE(data.continuousCurves.size(), 3);
    const auto refs = WellLogSet::wellCurveIndex(stack.imports->catalog(), stack.project,
                                               onlyWellId(*stack.imports->catalog()));
    QCOMPARE(refs.size(), 3);
    for (const auto &ref : refs)
    {
      QCOMPARE(ref.sourceVersionId, version.id);
      QCOMPARE(ref.path, managed);
      QCOMPARE(stored.curveNames.value(ref.column), ref.mnemonic);
      QVERIFY(!curveNamed(data, ref.mnemonic).isEmpty());
    }
  }

  void oldLasWithoutStoredMetadataReopensAndDrawsOriginalGr()
  {
    PreviewStack stack;
    const QByteArray source("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\n"
        "STRT.M 1000:\nSTOP.M 1001:\nSTEP.M 1:\nNULL. -999.25:\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~ASCII\n1000 30\n1001 40\n");
    const QString path = stack.source(QStringLiteral("legacy.las"), source);
    QVERIFY(!path.isEmpty());
    CatalogEntity well;
    well.id = QStringLiteral("legacy-well");
    well.name = QStringLiteral("A1");
    well.entityType = QStringLiteral("well");
    CatalogAsset asset;
    asset.id = QStringLiteral("ast-40");
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    asset.displayName = QStringLiteral("legacy.las");
    CatalogVersion version;
    version.id = QStringLiteral("ver-40");
    version.assetId = asset.id;
    version.stage = QStringLiteral("RAW");
    version.managed = false;
    version.path = path;
    version.fileName = QStringLiteral("legacy.las");
    EntityAssetLink link;
    link.assetId = asset.id;
    link.entityId = well.id;
    link.entityType = QStringLiteral("well");
    link.role = QStringLiteral("well_log");
    QString error;
    auto *catalog = stack.imports->catalog();
    QVERIFY2(catalog->addEntity(well, &error), qPrintable(error));
    QVERIFY2(catalog->addAsset(asset, &error), qPrintable(error));
    QVERIFY2(catalog->addVersion(version, &error), qPrintable(error));
    QVERIFY2(catalog->addLink(link, &error), qPrintable(error));
    stack.reopen();
    const auto reopened = stack.imports->catalog()->currentVersion(asset.id);
    QVERIFY(reopened.extra.isEmpty());
    LasHeaderInfo stored;
    QVERIFY(!WellLogSet::storedHeader(reopened, &stored));
    stack.preview->openAsset(asset.id);
    auto *panel = stack.currentPanel();
    QVERIFY(panel);
    QCOMPARE(panel->wellName(), QStringLiteral("A1"));
    const auto data = panel->currentData();
    QCOMPARE(data.continuousCurves.size(), 1);
    const auto gr = curveNamed(data, QStringLiteral("GR"));
    QCOMPARE(gr.depths, QVector<float>({1000, 1001}));
    QCOMPARE(gr.values, QVector<float>({30, 40}));
    auto *hint = stack.currentPage()->findChild<QLabel *>(QStringLiteral("lasPendingHint"));
    QVERIFY(hint);
    QVERIFY(hint->isHidden());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize())
    return 1;
  TestImportPreview test;
  const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown();
  return result;
}

#include "tst_import_preview.moc"
