// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "catalog/datacatalog.h"
#include "services/stratigraphicsubintervals.h"
#include "services/subintervalfactors.h"
#include "workflow/analysisresultsync.h"
#include "workflow/seismicresultpublisher.h"
#include "paleo_data/service.h"

class AnalysisResultSyncTest : public QObject {
  Q_OBJECT
private slots:
  void editedMapLineageRemainsCurrentUntilExternalInputChanges() {
    QTemporaryDir dir; paleo::data::DataService ds; std::string error;
    QVERIFY(ds.createProject((dir.path()+"/artifacts/metadata/area-model").toStdString(),"test",error));
    auto draft=paleo::strat::draftScheme("test","test",{"A","B"},{});
    paleo::data::StratigraphicScheme head; std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(ds.commitStratigraphicScheme(draft,{},head,issues,error));
    const auto unit=paleo::strat::makeSelection(head,paleo::strat::selectorOptions(head).first().intervalId,1);
    DataCatalog catalog; QString detail; QVERIFY(catalog.open(dir.path(),&detail));
    CatalogAsset input; input.id="input"; input.type="single_factor"; QVERIFY(catalog.addAsset(input,&detail));
    CatalogVersion source; source.id="input-v1"; source.assetId=input.id; source.stage="DERIVED";
    source.extra.insert("unitSelection",paleo::strat::toMap(unit)); QVERIFY(catalog.addVersion(source,&detail));
    CatalogAsset map; map.id="map"; map.type="edited_facies"; QVERIFY(catalog.addAsset(map,&detail));
    CatalogVersion first; first.id="map-v1"; first.assetId=map.id; first.stage="DERIVED";
    first.parentVersionIds={source.id}; QVERIFY(catalog.addVersion(first,&detail));
    CatalogVersion edited=first; edited.id="map-v2"; edited.versionNumber=2; edited.parentVersionIds={first.id};
    QVERIFY(catalog.addVersion(edited,&detail));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    const auto status=[&](const std::string &id) {
      std::vector<paleo::data::SubIntervalResultBinding> results;
      if(!ds.listSubIntervalResults(head.id,{},results,error))return std::string("read-error");
      for(const auto &b:results)if(b.assetRef==id)return b.status;
      return std::string("missing");
    };
    QCOMPARE(status("map-v2"),std::string("current"));
    // The ignored same-asset edge must still walk the predecessor's inputs.
    source.id="input-v2"; source.versionNumber=2; QVERIFY(catalog.addVersion(source,&detail));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    QCOMPARE(status("map-v2"),std::string("stale"));
  }
  void frameworkInitializesFromActualTopsAndTracksUpdates() {
    QTemporaryDir dir;
    DataCatalog catalog;
    QString error;
    QVERIFY(catalog.open(dir.path(), &error));
    paleo::data::DataService ds;
    std::string detail;
    QVERIFY(ds.createProject((dir.path()+"/artifacts/metadata/area-model").toStdString(), "test", detail));
    CatalogEntity well; well.id="well"; well.entityType="well"; well.name="W";
    QVERIFY(catalog.addEntity(well, &error));
    CatalogAsset tops; tops.id="tops"; tops.type="well_stratification"; tops.format="dat";
    QVERIFY(catalog.addAsset(tops, &error));
    CatalogVersion source; source.id="tops-v1"; source.assetId=tops.id; source.stage="RAW"; source.path="tops.dat";
    QFile file(dir.filePath(source.path));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nW D61 1000 0 0 -1000 1000 -99999\nW D62 1100 0 0 -1100 1100 -99999\n");
    file.close();
    QVERIFY(catalog.addVersion(source, &error));
    EntityAssetLink link; link.entityType="well"; link.entityId=well.id; link.assetId=tops.id; link.role="tops";
    QVERIFY(catalog.addLink(link, &error));
    QCOMPARE(ensureProjectAnalysisScheme(&catalog, dir.path(), {"D61","D62"}), QString());
    paleo::data::StratigraphicScheme first, again;
    QVERIFY(ds.readStratigraphicScheme("project-default", {}, first, detail));
    QVERIFY(!first.revision.empty());
    const auto options=paleo::strat::selectorOptions(first);
    const auto unit=paleo::strat::makeSelection(first,options.first().intervalId,1);
    const auto range=paleo::data::framework::wellRange(first,paleo::strat::stableWellKey(well.id),{unit.subIntervalId.toStdString()});
    QVERIFY(range.resolved);
    QCOMPARE(range.top,1000.0); QCOMPARE(range.bottom,1025.0);
    QCOMPARE(ensureProjectAnalysisScheme(&catalog, dir.path(), {"D61","D62"}), QString());
    QVERIFY(ds.readStratigraphicScheme("project-default", {}, again, detail));
    QCOMPARE(again.revision.value,first.revision.value);
    // A new raw revision changes geometry and produces a new framework revision.
    source.id="tops-v2"; source.versionNumber=2; source.path="tops2.dat";
    QFile second(dir.filePath(source.path)); QVERIFY(second.open(QIODevice::WriteOnly));
    second.write("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nW D61 1000 0 0 -1000 1000 -99999\nW D62 1200 0 0 -1200 1200 -99999\n"); second.close();
    QVERIFY(catalog.addVersion(source, &error));
    QCOMPARE(ensureProjectAnalysisScheme(&catalog, dir.path(), {"D61","D62"}), QString());
    QVERIFY(ds.readStratigraphicScheme("project-default", {}, again, detail));
    QVERIFY(again.revision.value!=first.revision.value);
    const auto updated=paleo::data::framework::wellRange(again,paleo::strat::stableWellKey(well.id),{unit.subIntervalId.toStdString()});
    QCOMPARE(updated.bottom,1050.0);
  }
  void immutableVersionsSurviveRetryAndInvalidation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    paleo::data::DataService ds;
    std::string error;
    QVERIFY2(ds.createProject((dir.path()+"/artifacts/metadata/area-model").toStdString(), "test", error), error.c_str());
    auto draft = paleo::strat::draftScheme("test", "test", {"A", "B"}, {});
    paleo::data::StratigraphicScheme head;
    std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY2(ds.commitStratigraphicScheme(draft, {}, head, issues, error), error.c_str());
    const auto options = paleo::strat::selectorOptions(head);
    auto unit = paleo::strat::makeSelection(head, options.first().intervalId, 2);
    DataCatalog catalog;
    QString detail;
    QVERIFY2(catalog.open(dir.path(), &detail), qPrintable(detail));
    CatalogAsset asset; asset.id="prediction"; asset.type="prediction_raster";
    QVERIFY(catalog.addAsset(asset, &detail));
    CatalogVersion version; version.id="prediction-v1"; version.assetId=asset.id;
    version.stage="DERIVED"; version.path="prediction.tif";
    version.extra.insert("unitSelection", paleo::strat::toMap(unit));
    QVERIFY(catalog.addVersion(version, &detail));
    QCOMPARE(syncAnalysisResults(&catalog, dir.path(), [] { return true; }), QString());
    std::vector<paleo::data::SubIntervalResultBinding> before;
    QVERIFY(ds.listSubIntervalResults(head.id, {}, before, error));
    QVERIFY(before.empty());
    QCOMPARE(syncAnalysisResults(&catalog, dir.path()), QString());
    QCOMPARE(syncAnalysisResults(&catalog, dir.path()), QString());
    std::vector<paleo::data::SubIntervalResultBinding> results;
    QVERIFY(ds.listSubIntervalResults(head.id, {}, results, error));
    QCOMPARE(results.size(), size_t(1));
    QCOMPARE(results.front().assetRef, std::string("prediction-v1"));
    QCOMPARE(results.front().subInterval.value, unit.subIntervalId.toStdString());
    QVERIFY(catalog.updateVersionExtra(version.id, {{"stale", true}}, &detail));
    QCOMPARE(syncAnalysisResults(&catalog, dir.path()), QString());
    QVERIFY(ds.listSubIntervalResults(head.id, {}, results, error));
    QCOMPARE(results.size(), size_t(1));
    QCOMPARE(results.front().status, std::string("stale"));
    // A downstream map inherits the frozen unit from its source version.
    CatalogAsset map; map.id="map"; map.type="facies_polygons";
    QVERIFY(catalog.addAsset(map, &detail));
    CatalogVersion output; output.id="map-v1"; output.assetId=map.id;
    output.stage="DERIVED"; output.path="map.gpkg"; output.parentVersionIds={version.id};
    QVERIFY(catalog.addVersion(output, &detail));
    QCOMPARE(syncAnalysisResults(&catalog, dir.path()), QString());
    QVERIFY(ds.listSubIntervalResults(head.id, {}, results, error));
    QCOMPARE(results.size(), size_t(2));
    bool mapStale = false;
    for (const auto &binding : results)
      if (binding.kind == "map") mapStale = binding.status == "stale";
    QVERIFY(mapStale);
    CatalogAsset contour;contour.id="contour";contour.type="contour_lines";QVERIFY(catalog.addAsset(contour,&detail));
    CatalogVersion line;line.id="contour-v1";line.assetId=contour.id;line.stage="DERIVED";
    line.path="contour.gpkg";line.parentVersionIds={version.id};QVERIFY(catalog.addVersion(line,&detail));
    QCOMPARE(analysisResultUnitSelection(&catalog,line),unit);
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    QVERIFY(ds.listSubIntervalResults(head.id,{},results,error));QCOMPARE(results.size(),size_t(3));
    bool contourStale=false;
    for(const auto &binding:results)if(binding.assetRef=="contour-v1")contourStale=binding.kind=="single_factor"&&binding.status=="stale";
    QVERIFY(contourStale);
    paleo::data::DataService reader;
    QVERIFY(reader.openProjectReadOnly((dir.path()+"/artifacts/metadata/area-model").toStdString(), error));
    QVERIFY(!reader.invalidateSubIntervalResult("catalog.prediction-v1", error));
    QVERIFY(!error.empty());
    const QVariantMap votes{{"seismicSliceVotes", QVariantList{QVariantMap{{"classCode", 3}, {"weight", 10}}}}};
    QVERIFY(!publishSeismicAggregate(dir.path(), {}, votes).ok);
    QVERIFY(publishSeismicAggregate(dir.path(), unit, votes).ok);
  }
};
QTEST_GUILESS_MAIN(AnalysisResultSyncTest)
#include "tst_analysisresultsync.moc"
