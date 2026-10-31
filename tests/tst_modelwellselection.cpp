#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "metadata/paleoprojectstore.h"
#include "services/previewdoc.h"
#include "ui/pages/datapage.h"
#include "ui/datapreview/datapreviewtabs.h"
#include <QTabWidget>

class TestModelWellSelection : public QObject {
  Q_OBJECT
private slots:
  void modelWellTabSelectsEntityInsteadOfSyntheticAsset() {
    QTemporaryDir tmp; PaleoProjectStore store; DataImportService service(&store);
    service.setProjectDir(tmp.path()); auto *catalog = service.catalog(); QString error;
    CatalogEntity well; well.id = "well-projected"; well.name = "SYNTH-1"; well.entityType = "well";
    well.extra.insert("areaModelWellId", 42);
    QVERIFY(catalog->addEntity(well, &error));
    PreviewDocService doc(&service); DataPage page;
    page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&doc));
    QSignalSpy selected(&page, &DataPage::wellSelected);
    page.selectAsset("model-well:42");
    QCOMPARE(page.property("paleo.page.entityId").toString(), well.id);
    QVERIFY(page.property("paleo.page.assetId").toString().isEmpty());
    QCOMPARE(selected.count(), 1); QCOMPARE(selected.first().first().toString(), well.id);
    page.refreshAssetTable();
    QCOMPARE(page.property("paleo.page.entityId").toString(), well.id);
    QVERIFY(page.property("paleo.page.assetId").toString().isEmpty());
    selected.clear();
    page.selectAsset("model-well:999");
    QVERIFY(page.property("paleo.page.entityId").toString().isEmpty());
    QVERIFY(page.property("paleo.page.assetId").toString().isEmpty());
    QCOMPARE(selected.count(), 0);
  }
  void modelWellTitleSurvivesCatalogCommits() {
    QTemporaryDir tmp; PaleoProjectStore store; DataImportService service(&store);
    service.setProjectDir(tmp.path()); DataPreviewTabs preview; preview.setImportService(&service);
    preview.openModelWell(42, "SYNTH-1");
    auto *tabs = preview.findChild<QTabWidget *>("dataPreviewTabs"); QVERIFY(tabs);
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("SYNTH-1"));
    QString error; CatalogAsset asset; asset.id = "new-result"; asset.type = "well_facies_prediction";
    QVERIFY(service.catalog()->addAsset(asset, &error));
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("SYNTH-1"));
  }
};
QTEST_MAIN(TestModelWellSelection)
#include "tst_modelwellselection.moc"
