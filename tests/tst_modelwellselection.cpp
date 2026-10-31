#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "metadata/paleoprojectstore.h"
#include "services/previewdoc.h"
#include "ui/pages/datapage.h"
#include "ui/datapreview/datapreviewtabs.h"
#include "ui/pages/constraintpage.h"
#include "linkage/selectioncontext.h"
#include "workflow/workflows.h"
#include <QTabWidget>
#include <QTableWidget>

class TestModelWellSelection : public QObject {
  Q_OBJECT
private slots:
  void factorResultsUseSelectedRevisionForPointsAndMaps() {
    QTemporaryDir dir;DataCatalog catalog;QString error;QVERIFY(catalog.open(dir.path(),&error));
    paleo::strat::UnitSelection unit;unit.schemeId="scheme";unit.revision="r1";unit.intervalId="parent";
    unit.subIntervalId="S1";unit.subIndex=1;unit.topBoundary="TOP";
    const auto add=[&](QString id,QString kind,QString horizon,paleo::strat::UnitSelection selection) {
      CatalogAsset asset;asset.id="ast-"+id;asset.type=kind;QVERIFY(catalog.addAsset(asset,&error));
      CatalogVersion version;version.id=id;version.assetId=asset.id;version.path=id+".gpkg";version.versionNumber=1;
      version.extra={{"kind",kind},{"factor_id","sandthick"},{"horizon",horizon},{"unitSelection",paleo::strat::toMap(selection)}};
      QVERIFY(catalog.addVersion(version,&error));
    };
    add("points","well_factor_points","TOP",unit);add("raster","single_factor_raster","S1",unit);
    CatalogAsset contour;contour.id="contour";contour.type="contour_lines";QVERIFY(catalog.addAsset(contour,&error));
    CatalogVersion line;line.id="line";line.assetId=contour.id;line.path="line.gpkg";line.parentVersionIds={"raster"};
    line.extra={{"kind","contour_lines"},{"horizon","S1"},{"factor_layer_id","factor.S1.sandthick"}};QVERIFY(catalog.addVersion(line,&error));
    auto other=unit;other.revision="r2";add("other","single_factor_raster","S1",other);
    ConstraintWorkflow wf(nullptr,nullptr);wf.setCatalog(&catalog,dir.path());ConstraintPage page(&wf);
    SelectionContext selection;selection.setActiveUnit(unit);page.setProperty("paleo.selection",QVariant::fromValue<QObject *>(&selection));
    page.setHorizons({"TOP"});page.refreshAnalysisUnit();page.refreshFactorResults();
    auto *table=page.findChild<QTableWidget *>("factorResultsTable");QVERIFY(table);QCOMPARE(table->rowCount(),3);
    QCOMPARE(wf.savedFactorResults("TOP","sandthick",paleo::strat::toMap(other)).size(),1);
  }
  void analysisSelectionChangesIdentityWithoutMarkingSavedMapsStale() {
    ConstraintPage page(nullptr);page.setHorizons({"TOP"});SelectionContext selection;
    page.setProperty("paleo.selection",QVariant::fromValue<QObject *>(&selection));
    paleo::strat::UnitSelection unit;unit.schemeId="scheme";unit.revision="revision";
    unit.intervalId="parent";unit.subIntervalId="sub-A";unit.subIndex=1;unit.topBoundary="TOP";
    selection.setActiveUnit(unit);page.noteFactorLayer("sandthick","factor.sub-A.sandthick");
    page.refreshAnalysisUnit();
    QVERIFY(page.property("paleo.page.factorstale.sub-A").toStringList().isEmpty());
    auto *factors=page.findChild<QTableWidget *>("factorTable");QVERIFY(factors);
    page.noteFactorLayer("sandratio","factor.sub-A.sandratio");
    for(int row=0;row<factors->rowCount();++row)
      factors->item(row,0)->setCheckState(factors->item(row,0)->data(Qt::UserRole).toString()=="sandratio"?Qt::Checked:Qt::Unchecked);
    QVERIFY(page.property("paleo.page.factorstale.sub-A").toStringList().isEmpty());
    const auto first=page.wellFactorParams().value("unitSelection").toMap();
    unit.subIntervalId="sub-B";unit.subIndex=2;selection.setActiveUnit(unit);page.refreshAnalysisUnit();
    const auto second=page.wellFactorParams().value("unitSelection").toMap();
    QVERIFY(first!=second);QCOMPARE(second.value("subIntervalId").toString(),QString("sub-B"));
    QVERIFY(page.property("paleo.page.factorstale.sub-B").toStringList().isEmpty());
  }
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
