#include <QtTest>
#include "catalog/areasourceprojection.h"
#include "catalog/areawellprojection.h"
#include "catalog/documentsourceprojection.h"
#include <paleo_data/service.h>
#include "../vendor/Data_service/src/store/project_store.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

namespace {
using namespace paleo::data;
struct Fixture {
  QTemporaryDir temp;
  QString project=temp.path()+"/project",source=temp.path()+"/SYNTH.las",error;
  std::string detail;
  DataService service;
  DataCatalog catalog;
  long long wellId=0,sourceId=0;
  QString root() const {return QDir(project).filePath("artifacts/metadata/area-model");}
  bool init() {
    if(!service.createProject(root().toStdString(),"delta",detail))return false;
    QFile file(source);if(!file.open(QIODevice::WriteOnly))return false;
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH\nNULL. -999.25\n~Curve\nDEPT.M\nGR.API\n~ASCII\n100 12\n101 13\n");file.close();
    ImportOptions options;options.copySources=false;ImportReport report;
    if(!service.importFile(source.toStdString(),options,report,detail))return false;
    std::vector<WellInfoRecord> wells;std::vector<SourceIndexEntry> sources;
    if(!service.listWellInfos(wells,detail)||wells.size()!=1||!service.listSourceIndexEntries(sources,detail)||sources.size()!=1)return false;
    wellId=wells.front().id;sourceId=sources.front().id;
    return catalog.open(project,&error)&&registerAreaSources(catalog,&error);
  }
  ModelChangeHead head() {ModelChangeHead value;service.readModelChangeHead(value,detail);return value;}
  bool rename(const std::string &name) {WellInfo well;return service.readWellInfo(wellId,well,detail)&&(well.wellName=name,service.saveWellInfo(wellId,well,detail));}
  std::vector<ModelChangeItem> batch(const ModelChangeHead &head) {
    ModelChangeQuery query;query.expectedGeneration=head.generation;query.sequence=head.sequence;query.maxBytes=16*1024*1024;
    std::vector<ModelChangeItem> items;
    for(;;) {ModelChangePage page;if(!service.readModelChanges(query,page,detail))return {};
      for(auto &item:page.items)items.push_back(std::move(item));if(page.complete)return items;query.afterOrdinal=page.nextOrdinal;
    }
  }
  CatalogEntity storedWell() const {
    for(const auto &well:catalog.storedEntities("well"))if(well.extra.value("areaModelWellId").toLongLong()==wellId)return well;
    return {};
  }
};
bool catalogSql(const QString &file,const QString &command) {
  const auto connection="delta-test-"+QUuid::createUuid().toString();bool ok=false;
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(file);
    if(db.open()){QSqlQuery query(db);ok=query.exec(command);}db.close();}
  QSqlDatabase::removeDatabase(connection);return ok;
}
}
class AreaSourceDeltaTest : public QObject {
  Q_OBJECT
private slots:
  void unchangedIsConstantWork() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto before=f.catalog.mutationSeq();AreaSourceProjectionStats stats;
    QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
    QVERIFY(stats.unchanged);QCOMPARE(stats.bootstrapScans,0);QCOMPARE(stats.pages,0);
    QCOMPARE(stats.cacheBuilds,0);QCOMPARE(f.catalog.mutationSeq(),before);
  }
  void exactAfterImageThenLatestDelta() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    QVERIFY(f.rename("B2"));const auto b2=f.head();const auto items=f.batch(b2);QVERIFY(!items.empty());
    QVERIFY(f.rename("B3"));const auto b3=f.head();
    QVERIFY(QFile::remove(f.source));
    QVERIFY2(applyAreaSourceChangeBatch(f.catalog,b3,b2.sequence,items,&f.error),qPrintable(f.error));
    QCOMPARE(f.storedWell().name,QStringLiteral("B2"));QCOMPARE(f.catalog.projectionCheckpoint("area-sources").sequence,b2.sequence);
    AreaSourceProjectionStats stats;QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
    QCOMPARE(f.storedWell().name,QStringLiteral("B3"));QCOMPARE(stats.bootstrapScans,0);QCOMPARE(stats.batches,1);
    const auto mutation=f.catalog.mutationSeq();
    QVERIFY(applyAreaSourceChangeBatch(f.catalog,b3,b2.sequence,items,&f.error));QCOMPARE(f.catalog.mutationSeq(),mutation);
    DataCatalog reopened;QVERIFY(reopened.open(f.project,&f.error));QCOMPARE(reopened.projectionCheckpoint("area-sources").sequence,b3.sequence);
  }
  void replayReusesIndependentlyRegisteredWell() {
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error + QString::fromStdString(f.detail)));
    WellInfo model;
    model.wellName = "NEW-WELL";
    long long wellId = 0;
    QVERIFY2(f.service.createWell(model, wellId, f.detail), f.detail.c_str());
    QVERIFY2(registerAreaWells(f.catalog, &f.error), qPrintable(f.error));
    const auto initialIds = f.catalog.wellsMatchingName("NEW-WELL");
    QCOMPARE(initialIds.size(), 1);

    QVERIFY2(registerAreaSources(f.catalog, &f.error), qPrintable(f.error));
    QCOMPARE(f.catalog.wellsMatchingName("NEW-WELL"), initialIds);
    QCOMPARE(f.catalog.storedEntities("well").size(), 2);
    const auto well = f.catalog.storedEntityById(initialIds.front());
    QCOMPARE(well.extra.value("areaModelWellId").toLongLong(), wellId);
    QCOMPARE(well.extra.value("areaModelGeneration").toString(),
             QString::fromStdString(f.head().generation));
  }
  void replayKeepsProjectionUntilPositionChanges_data() {
    QTest::addColumn<bool>("changeUnit");
    QTest::newRow("coordinate") << false;
    QTest::newRow("unit") << true;
  }
  void replayKeepsProjectionUntilPositionChanges() {
    QFETCH(bool, changeUnit);
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error + QString::fromStdString(f.detail)));
    WellInfo model;
    QVERIFY(f.service.readWellInfo(f.wellId, model, f.detail));
    model.wellheadX = 500000;
    model.wellheadY = 3200000;
    model.coordinateSystem = "EPSG:32650";
    model.coordinateUnit = "m";
    QVERIFY2(f.service.saveWellInfo(f.wellId, model, f.detail), f.detail.c_str());
    QVERIFY2(registerAreaSources(f.catalog, &f.error), qPrintable(f.error));
    auto well = f.storedWell();
    well.coordinateStatus = "ok";
    well.extra.insert("projectLon", 117.0);
    well.extra.insert("projectLat", 28.9);
    QVERIFY2(f.catalog.updateEntity(well, &f.error), qPrintable(f.error));

    QVERIFY(f.rename("RENAMED-WELL"));
    QVERIFY2(registerAreaSources(f.catalog, &f.error), qPrintable(f.error));
    well = f.storedWell();
    QCOMPARE(well.name, QStringLiteral("RENAMED-WELL"));
    QCOMPARE(well.coordinateStatus, QStringLiteral("ok"));
    QCOMPARE(well.extra.value("projectLon").toDouble(), 117.0);
    QCOMPARE(well.extra.value("projectLat").toDouble(), 28.9);

    QVERIFY(f.service.readWellInfo(f.wellId, model, f.detail));
    if (changeUnit)
      model.coordinateUnit = "ft";
    else
      model.wellheadX += 25;
    QVERIFY2(f.service.saveWellInfo(f.wellId, model, f.detail), f.detail.c_str());
    QVERIFY2(registerAreaSources(f.catalog, &f.error), qPrintable(f.error));
    well = f.storedWell();
    QCOMPARE(well.coordinateStatus, QStringLiteral("untransformed"));
    QVERIFY(!well.extra.contains("projectLon"));
    QVERIFY(!well.extra.contains("projectLat"));
  }
  void checkpointFailureRollsBackAllFacts() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto old=f.catalog.projectionCheckpoint("area-sources");const auto oldName=f.storedWell().name;
    QVERIFY(f.rename("AFTER-FAILURE"));
    QVERIFY(catalogSql(f.catalog.sqliteCatalogPath(),"CREATE TRIGGER delta_failure BEFORE INSERT ON catalog_meta WHEN NEW.key='projection_checkpoints_v1' BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END"));
    QVERIFY(!registerAreaSources(f.catalog,&f.error));QCOMPARE(f.catalog.projectionCheckpoint("area-sources"),old);QCOMPARE(f.storedWell().name,oldName);
    {DataCatalog reopened;QVERIFY(reopened.open(f.project,&f.error));QCOMPARE(reopened.projectionCheckpoint("area-sources"),old);}
    QVERIFY(catalogSql(f.catalog.sqliteCatalogPath(),"DROP TRIGGER delta_failure"));
    QVERIFY2(registerAreaSources(f.catalog,&f.error),qPrintable(f.error));QCOMPARE(f.storedWell().name,QStringLiteral("AFTER-FAILURE"));
  }
  void sameBytesNewImportKeepsOldMapping() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto original=f.catalog.versions().front();ImportOptions options;options.copySources=false;options.forceNewBatch=true;
    ImportReport report;QVERIFY2(f.service.importFile(f.source.toStdString(),options,report,f.detail),f.detail.c_str());
    QVERIFY2(registerAreaSources(f.catalog,&f.error),qPrintable(f.error));
    QCOMPARE(f.catalog.versionById(original.id).extra.value("areaModelSourceId").toLongLong(),f.sourceId);
    QSet<qlonglong> sources;for(const auto &version:f.catalog.versions())sources.insert(version.extra.value("areaModelSourceId").toLongLong());
    QCOMPARE(sources.size(),2);QCOMPARE(f.catalog.versions().size(),2);
  }
  void retiredSourceKeepsRawHistory() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto original=f.catalog.versions().front();
    {paleo::data::detail::ProjectStore writer;QVERIFY(writer.open(f.root(),false,{},f.detail));
      QVERIFY(writer.exec("UPDATE sources SET active=0 WHERE id=?",{f.sourceId},f.detail));}
    QVERIFY2(registerAreaSources(f.catalog,&f.error),qPrintable(f.error));
    const auto version=f.catalog.versionById(original.id);QVERIFY(!version.id.isEmpty());
    QVERIFY(!version.extra.value("areaModelObservation").toMap().value("active").toBool());QVERIFY(QFileInfo::exists(f.source));
    const auto projection=paleo::document::projectDocumentSources(f.catalog,"fixture");
    const auto source=projection.records.value(projection.versionKeys.value(original.id));
    QCOMPARE(source.state.model,paleo::document::ModelAvailability::Failed);
    QCOMPARE(source.state.code,QStringLiteral("model_source_inactive"));
    for(const auto &link:f.catalog.linksForAsset(original.assetId))QVERIFY(link.unresolved && !link.isPrimary);
  }
  void removedRoleIsReviewedWithoutRemovingHistory() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    std::vector<DataEntry> curves;QVERIFY(f.service.listData("curves",f.wellId,curves,f.detail));QVERIFY(!curves.empty());
    const auto version=f.catalog.versions().front();QVERIFY(!f.catalog.linksForAsset(version.assetId).isEmpty());
    QVERIFY(f.service.deleteData("curves",curves.front().id,f.detail));
    QVERIFY2(registerAreaSources(f.catalog,&f.error),qPrintable(f.error));
    for(const auto &link:f.catalog.linksForAsset(version.assetId))if(link.role=="well_log")QVERIFY(link.unresolved && !link.isPrimary);
    QVERIFY(!f.catalog.versionById(version.id).id.isEmpty());QVERIFY(QFileInfo::exists(f.source));
  }
  void generationChangeBootstrapsWithoutRetargetingSource() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto before=f.catalog.versions().front();const auto head=f.head();ModelChangeHead forked;
    QVERIFY2(f.service.forkModelGeneration(head,forked,f.detail),f.detail.c_str());
    AreaSourceProjectionStats stats;QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
    QCOMPARE(stats.bootstrapScans,1);QCOMPARE(f.catalog.versions().size(),1);
    QCOMPARE(f.catalog.versionById(before.id).extra.value("areaModelSourceId").toLongLong(),f.sourceId);
    QCOMPARE(f.catalog.projectionCheckpoint("area-sources").generation,QString::fromStdString(forked.generation));
  }
  void bootstrapRetiresUnavailableSources_data() {
    QTest::addColumn<QString>("mode");
    for(const auto &mode:{"inactive","failed","ignored","deleted"})QTest::newRow(mode)<<QString::fromLatin1(mode);
  }
  void bootstrapRetiresUnavailableSources() {
    QFETCH(QString,mode);
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto original=f.catalog.versions().front();QVERIFY(!f.catalog.linksForAsset(original.assetId).isEmpty());
    {
      paleo::data::detail::ProjectStore writer;QVERIFY(writer.open(f.root(),false,{},f.detail));
      paleo::data::detail::Transaction transaction(writer,f.detail);QVERIFY(transaction.active);
      if(mode=="deleted") {
        QVERIFY(writer.exec("DELETE FROM issues WHERE source_id=?",{f.sourceId},f.detail));
        QVERIFY(writer.exec("DELETE FROM curves WHERE set_id IN (SELECT id FROM log_sets WHERE source_id=?)",{f.sourceId},f.detail));
        QVERIFY(writer.exec("DELETE FROM log_sets WHERE source_id=?",{f.sourceId},f.detail));
        QVERIFY2(writer.exec("DELETE FROM sources WHERE id=?",{f.sourceId},f.detail),f.detail.c_str());
      } else if(mode=="inactive") {
        QVERIFY(writer.exec("UPDATE sources SET active=0 WHERE id=?",{f.sourceId},f.detail));
      } else {
        QVERIFY(writer.exec("UPDATE sources SET status=? WHERE id=?",{mode,f.sourceId},f.detail));
      }
      QVERIFY2(transaction.commit(f.detail),f.detail.c_str());
    }
    const auto changed=f.head();ModelChangeHead forked;
    QVERIFY2(f.service.forkModelGeneration(changed,forked,f.detail),f.detail.c_str());
    AreaSourceProjectionStats stats;QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
    QCOMPARE(stats.bootstrapScans,1);QCOMPARE(f.catalog.versions().size(),1);
    const auto retired=f.catalog.versionById(original.id);
    QVERIFY(!retired.extra.value("areaModelObservation").toMap().value("active").toBool());
    QCOMPARE(retired.extra.value("areaModelSourceId").toLongLong(),f.sourceId);
    for(const auto &link:f.catalog.linksForAsset(original.assetId))QVERIFY(link.unresolved && !link.isPrimary);
    QVERIFY(QFileInfo::exists(f.source));
    DataCatalog reopened;QVERIFY(reopened.open(f.project,&f.error));
    QVERIFY(!reopened.versionById(original.id).extra.value("areaModelObservation").toMap().value("active").toBool());
    QCOMPARE(reopened.projectionCheckpoint("area-sources").generation,QString::fromStdString(forked.generation));
    QVERIFY(registerAreaSourcesWithStats(f.catalog,&f.error,&stats));QVERIFY(stats.unchanged);
  }
  void sameContentRelocationBeforeBootstrapUsesNewLocator() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto original=f.catalog.versions().front();const auto moved=f.temp.path()+"/RELOCATED.las";
    QVERIFY(QFile::copy(f.source,moved));
    std::shared_ptr<const PreparedSourceRelocation> prepared;
    QVERIFY2(f.service.prepareSourceRelocation(f.sourceId,moved.toStdString(),prepared,f.detail),f.detail.c_str());
    QVERIFY2(f.service.commitSourceRelocation(*prepared,f.detail),f.detail.c_str());
    const auto changed=f.head();ModelChangeHead forked;
    QVERIFY2(f.service.forkModelGeneration(changed,forked,f.detail),f.detail.c_str());QVERIFY(QFile::remove(f.source));
    AreaSourceProjectionStats stats;QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
    QCOMPARE(stats.bootstrapScans,1);const auto relocated=f.catalog.currentVersion(original.assetId);
    QVERIFY(relocated.id!=original.id);QCOMPARE(f.catalog.versionFilePath(relocated),QDir::cleanPath(moved));
    QCOMPARE(relocated.sha256,original.sha256);QCOMPARE(relocated.extra.value("areaModelSourceId").toLongLong(),f.sourceId);
    QCOMPARE(f.catalog.versionById(original.id).path,original.path);QCOMPARE(f.catalog.versions().size(),2);
    QVERIFY(registerAreaSourcesWithStats(f.catalog,&f.error,&stats));QVERIFY(stats.unchanged);
    QCOMPARE(f.catalog.versionFilePath(f.catalog.currentVersion(original.assetId)),QDir::cleanPath(moved));
  }
  void bootstrapDoesNotUseDerivedVersionAsSourceRepresentative() {
    Fixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto original=f.catalog.versions().front();const auto oldGeneration=original.extra.value("areaModelGeneration").toString();
    CatalogVersion derived=original;derived.id=f.catalog.nextVersionId();derived.stage="DERIVED";
    derived.versionNumber=original.versionNumber+1;derived.parentVersionIds={original.id};
    QVERIFY(f.catalog.addVersion(derived,&f.error));
    const auto changed=f.head();ModelChangeHead forked;QVERIFY2(f.service.forkModelGeneration(changed,forked,f.detail),f.detail.c_str());
    AreaSourceProjectionStats stats;QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
    QCOMPARE(stats.bootstrapScans,1);QCOMPARE(f.catalog.versions().size(),2);
    QCOMPARE(f.catalog.versionById(original.id).extra.value("areaModelGeneration").toString(),QString::fromStdString(forked.generation));
    QCOMPARE(f.catalog.versionById(derived.id).extra.value("areaModelGeneration").toString(),oldGeneration);
    QCOMPARE(f.catalog.versionById(derived.id).stage,QString("DERIVED"));
  }
  void checkpointParticipatesInStagingAndBatchRollback() {
    QTemporaryDir temp;DataCatalog catalog;QString error;QVERIFY(catalog.open(temp.path(),&error));
    CatalogProjectionCheckpoint first{"store","generation",1};QVERIFY(catalog.setProjectionCheckpoint("test",first,&error));
    {DataCatalog::BatchSave batch(&catalog);QVERIFY(catalog.setProjectionCheckpoint("test",{"store","generation",2},&error));batch.abort();}
    QCOMPARE(catalog.projectionCheckpoint("test"),first);
    auto staging=catalog.createStagingCopy(temp.path()+"/overlay");QVERIFY(staging);
    QVERIFY(staging->setProjectionCheckpoint("test",{"store","generation",3},&error));
    QVERIFY(catalog.applyJournal(staging->journal(),&error));QCOMPARE(catalog.projectionCheckpoint("test").sequence,3);
    DataCatalog reopened;QVERIFY(reopened.open(temp.path(),&error));QCOMPARE(reopened.projectionCheckpoint("test").sequence,3);
  }
};
QTEST_GUILESS_MAIN(AreaSourceDeltaTest)
#include "tst_area_source_delta.moc"
