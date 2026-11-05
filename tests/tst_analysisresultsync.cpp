// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <cmath>
#include "catalog/datacatalog.h"
#include "services/stratigraphicsubintervals.h"
#include "services/subintervalfactors.h"
#include "workflow/analysisresultsync.h"
#include "workflow/seismicresultpublisher.h"
#include "paleo_data/service.h"

class AnalysisResultSyncTest : public QObject {
  Q_OBJECT
private slots:
  void consumerInvalidationProjectsAtomicallyAndPreservesFrozenInputs() {
    QTemporaryDir dir;paleo::data::DataService ds;std::string error;QString detail;
    QVERIFY(ds.createProject((dir.path()+"/artifacts/metadata/area-model").toStdString(),"scoped results",error));
    auto draft=paleo::strat::draftScheme("scoped","scoped",{"A","B"},
      {{17,{{"A",1000.},{"B",1080.}}},{18,{{"A",2000.},{"B",2080.}}}});
    draft.boundaries[0].seismicHorizonResourceId=10;draft.boundaries[1].seismicHorizonResourceId=20;
    paleo::data::StratigraphicScheme head,other;std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(ds.commitStratigraphicScheme(draft,{},head,issues,error));draft.id="unrelated";
    QVERIFY(ds.commitStratigraphicScheme(draft,{},other,issues,error));
    const auto interval=QString::fromStdString(head.intervals.front().id.value);
    const auto s2=paleo::strat::makeSelection(head,interval,2),s4=paleo::strat::makeSelection(head,interval,4);
    const auto otherUnit=paleo::strat::makeSelection(other,QString::fromStdString(other.intervals.front().id.value),2);
    DataCatalog catalog;QVERIFY(catalog.open(dir.path(),&detail));
    const QByteArray rawBytes="synthetic immutable SEG-Y bytes",cacheBytes="synthetic immutable SF3P bytes";
    {QFile file(dir.filePath("volume.sgy"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(rawBytes),qint64(rawBytes.size()));}
    {QFile file(dir.filePath("volume.sf3p"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(cacheBytes),qint64(cacheBytes.size()));}
    CatalogAsset raw;raw.id="raw-volume";raw.type="seismic";QVERIFY(catalog.addAsset(raw,&detail));
    CatalogVersion rawVersion;rawVersion.id="raw-v1";rawVersion.assetId=raw.id;rawVersion.stage="RAW";rawVersion.path="volume.sgy";
    rawVersion.sha256=QString::fromLatin1(QCryptographicHash::hash(rawBytes,QCryptographicHash::Sha256).toHex());QVERIFY(catalog.addVersion(rawVersion,&detail));
    const auto add=[&](const QString &id,const QString &type,const paleo::strat::UnitSelection &unit,qlonglong well,const QStringList &parents) {
      CatalogAsset asset;asset.id=id;asset.type=type;if(!catalog.addAsset(asset,&detail))return false;
      CatalogVersion version;version.id=id+"-v1";version.assetId=id;version.stage="DERIVED";version.path=id+".json";
      version.sha256="frozen-result-"+id;version.parentVersionIds=parents;
      if(unit.isValid())version.extra.insert("unitSelection",paleo::strat::toMap(unit));
      if(well>0)version.extra.insert("analysisWellId",well);
      return catalog.addVersion(version,&detail);
    };
    QVERIFY(add("well-1","well_facies_prediction",s2,17,{rawVersion.id}));
    QVERIFY(add("well-2","well_facies_prediction",s2,18,{rawVersion.id}));
    QVERIFY(add("seismic-s2","prediction_raster",s2,0,{rawVersion.id}));
    QVERIFY(add("seismic-s4","prediction_raster",s4,0,{rawVersion.id}));
    QVERIFY(add("other-result","prediction_raster",otherUnit,0,{rawVersion.id}));
    QVERIFY(add("map-s2","edited_facies",{},0,{"seismic-s2-v1"}));
    QVERIFY(add("map-child","composed_facies",{},0,{"map-s2-v1"}));
    const auto originals=catalog.versions();
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    auto changed=head;for(auto &well:changed.wellOccurrences)if(well.wellId==17)well.internal[1]=1048.;
    paleo::data::StratigraphicScheme wellHead;QVERIFY(ds.commitStratigraphicScheme(changed,head.revision.value,wellHead,issues,error));
    auto staged=catalog.createStagingCopy(QString());QCOMPARE(syncAnalysisResults(staged.get(),dir.path()),QString());
    QVERIFY(!staged->journal().isEmpty());QVERIFY(!catalog.versionById("well-1-v1").extra.value("stale").toBool());
    catalog.debugAbortJournalAfter(1);
    QVERIFY(!syncAnalysisResults(&catalog,dir.path()).isEmpty());
    for(const auto &version:catalog.versions())QVERIFY(!version.extra.value("stale").toBool());
    {DataCatalog disk;QVERIFY(disk.open(dir.path(),&detail));for(const auto &version:disk.versions())QVERIFY(!version.extra.value("stale").toBool());}
    catalog.debugAbortJournalAfter(0);
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    QVERIFY(catalog.versionById("well-1-v1").extra.value("stale").toBool());
    QVERIFY(!catalog.versionById("well-2-v1").extra.value("stale").toBool());
    QVERIFY(!catalog.versionById("seismic-s2-v1").extra.value("stale").toBool());
    QVERIFY(!catalog.versionById("seismic-s4-v1").extra.value("stale").toBool());
    changed=wellHead;changed.divisions.front().fractions={.25,.6,.75};paleo::data::StratigraphicScheme divided;
    QVERIFY(ds.commitStratigraphicScheme(changed,wellHead.revision.value,divided,issues,error));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    for(const auto &id:{"seismic-s2-v1","map-s2-v1","map-child-v1"})QVERIFY(catalog.versionById(id).extra.value("stale").toBool());
    for(const auto &id:{"seismic-s4-v1","other-result-v1","raw-v1"})QVERIFY(!catalog.versionById(id).extra.value("stale").toBool());
    const auto currentUnit=paleo::strat::makeSelection(divided,interval,2);
    QVERIFY(add("current-well-2","well_facies_prediction",currentUnit,18,{rawVersion.id}));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    auto rebound=divided;rebound.boundaries.front().seismicHorizonResourceId=99;paleo::data::StratigraphicScheme surface;
    QVERIFY(ds.commitStratigraphicScheme(rebound,divided.revision.value,surface,issues,error));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    QVERIFY(catalog.versionById("seismic-s4-v1").extra.value("stale").toBool());
    QVERIFY(!catalog.versionById("current-well-2-v1").extra.value("stale").toBool());
    QCOMPARE(catalog.versionById("current-well-2-v1").extra.value("unitSelection").toMap(),paleo::strat::toMap(currentUnit));
    const auto revision=catalog.catalogRevision();QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());QCOMPARE(catalog.catalogRevision(),revision);
    for(const auto &old:originals) {
      const auto version=catalog.versionById(old.id);QCOMPARE(version.path,old.path);QCOMPARE(version.sha256,old.sha256);
      QCOMPARE(version.parentVersionIds,old.parentVersionIds);QCOMPARE(version.extra.value("unitSelection"),old.extra.value("unitSelection"));
    }
    paleo::data::StratigraphicScheme historical;QVERIFY(ds.readStratigraphicScheme(head.id,head.revision.value,historical,error));
    QCOMPARE(historical.boundaries.front().seismicHorizonResourceId,10LL);QVERIFY(std::isnan(historical.wellOccurrences.front().internal[1]));
    {QFile file(dir.filePath("volume.sgy"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),rawBytes);}
    {QFile file(dir.filePath("volume.sf3p"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),cacheBytes);}
    QVERIFY(catalog.open(dir.path(),&detail));QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    QVERIFY(catalog.versionById("seismic-s4-v1").extra.value("stale").toBool());
    QVERIFY(!catalog.versionById("current-well-2-v1").extra.value("stale").toBool());
  }
  void batchInvalidationKeepsUnaffectedNativeWellChildrenCurrent() {
    QTemporaryDir dir;paleo::data::DataService ds;std::string error;QString detail;
    QVERIFY(ds.createProject((dir.path()+"/artifacts/metadata/area-model").toStdString(),"batch isolation",error));
    auto draft=paleo::strat::draftScheme("batch","batch",{"A","B"},
      {{17,{{"A",1000.},{"B",1080.}}},{18,{{"A",2000.},{"B",2080.}}}});
    paleo::data::StratigraphicScheme head;std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(ds.commitStratigraphicScheme(draft,{},head,issues,error));
    const auto unit=paleo::strat::makeSelection(head,QString::fromStdString(head.intervals.front().id.value),2);
    DataCatalog catalog;QVERIFY(catalog.open(dir.path(),&detail));
    QVariantList supporting;
    for(int id:{17,18}) {
      CatalogEntity entity;entity.id=QString::number(id);entity.entityType="well";entity.name=entity.id;entity.extra.insert("areaModelWellId",id);
      QVERIFY(catalog.addEntity(entity,&detail));supporting.append(QVariantMap{{"entity_id",entity.id}});
    }
    CatalogAsset asset;asset.id="batch";asset.type="wells_prediction";QVERIFY(catalog.addAsset(asset,&detail));
    CatalogVersion version;version.id="batch-v1";version.assetId=asset.id;version.stage="DERIVED";
    version.extra={{"unitSelection",paleo::strat::toMap(unit)},{"supporting_wells",supporting}};QVERIFY(catalog.addVersion(version,&detail));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    auto changed=head;for(auto &well:changed.wellOccurrences)if(well.wellId==17)well.internal[1]=1048.;
    paleo::data::StratigraphicScheme next;QVERIFY(ds.commitStratigraphicScheme(changed,head.revision.value,next,issues,error));
    for(int repeat=0;repeat<2;++repeat) {
      QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
      std::vector<paleo::data::SubIntervalResultBinding> bindings;QVERIFY(ds.listSubIntervalResults(head.id,{},bindings,error));QCOMPARE(bindings.size(),size_t(3));
      for(const auto &binding:bindings)QCOMPARE(binding.status,binding.wellId==18?std::string("current"):std::string("stale"));
    }
    QVERIFY(catalog.versionById(version.id).extra.value("stale").toBool());
  }
  void batchWellResultsKeepNativeUnitBindingsAcrossReopenAndInvalidation() {
    QTemporaryDir dir;paleo::data::DataService ds;std::string error;
    QVERIFY(ds.createProject((dir.path()+"/artifacts/metadata/area-model").toStdString(),"test",error));
    auto draft=paleo::strat::draftScheme("test","test",{"A","B"},{{17,{{"A",1000.},{"B",1080.}}}});
    paleo::data::StratigraphicScheme head;std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(ds.commitStratigraphicScheme(draft,{},head,issues,error));
    const auto unit=paleo::strat::makeSelection(head,paleo::strat::selectorOptions(head).first().intervalId,1);
    DataCatalog catalog;QString detail;QVERIFY(catalog.open(dir.path(),&detail));
    CatalogEntity well;well.id="native-well";well.entityType="well";well.name="W";well.extra.insert("areaModelWellId",17);
    QVERIFY(catalog.addEntity(well,&detail));CatalogAsset asset;asset.id="batch";asset.type="wells_prediction";QVERIFY(catalog.addAsset(asset,&detail));
    CatalogVersion version;version.id="prediction-v1";version.assetId=asset.id;version.stage="DERIVED";
    version.extra={{"unitSelection",paleo::strat::toMap(unit)},{"supporting_wells",QVariantList{QVariantMap{{"entity_id",well.id},{"facies_code",1}}}}};
    QVERIFY(catalog.addVersion(version,&detail));QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    std::vector<paleo::data::SubIntervalResultBinding> bindings;QVERIFY(ds.listSubIntervalResults(head.id,{},bindings,error));QCOMPARE(bindings.size(),size_t(2));
    int native=0;for(const auto &binding:bindings){QCOMPARE(binding.status,std::string("current"));if(binding.wellId==17){++native;QCOMPARE(binding.subInterval.value,unit.subIntervalId.toStdString());}}
    QCOMPARE(native,1);
    auto changed=head;changed.divisions.front().fractions={.2,.5,.75};changed.divisions.front().mode="interpreted";
    paleo::data::StratigraphicScheme next;QVERIFY(ds.commitStratigraphicScheme(changed,head.revision.value,next,issues,error));
    QCOMPARE(syncAnalysisResults(&catalog,dir.path()),QString());
    ds.closeProject();QVERIFY(ds.openProject((dir.path()+"/artifacts/metadata/area-model").toStdString(),error));
    QVERIFY(ds.listSubIntervalResults(head.id,{},bindings,error));QCOMPARE(bindings.size(),size_t(2));
    for(const auto &binding:bindings)QCOMPARE(binding.status,std::string("stale"));
  }
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
