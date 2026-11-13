// 层：测试壳
#include <QtTest>
#include "catalog/datacatalog.h"
#include "services/projectquerycapability.h"
#include "services/artifactversioncapability.h"
#include "domain/wellref.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <thread>

using namespace paleo::query;
int runWellQueryMetadataContract();
namespace {
struct Fixture {
  QTemporaryDir temp; DataCatalog catalog; QString error;
  bool open() { return temp.isValid() && catalog.open(temp.path(), &error); }
  bool asset(const QString &id, const QString &type = "document", const QString &name = {}) {
    CatalogAsset row; row.id = id; row.type = type; row.displayName = name.isEmpty() ? id : name;
    return catalog.addAsset(row, &error);
  }
  bool version(const QString &id, const QString &assetId, const QStringList &parents = {}) {
    CatalogVersion row; row.id = id; row.assetId = assetId; row.stage = "DERIVED";
    row.fileName = id + ".txt"; row.path = "DERIVED/" + assetId + "/" + id + "/" + row.fileName;
    row.parentVersionIds = parents; row.managed = true; return catalog.addVersion(row, &error);
  }
};
QString cursor(const QJsonObject &page) { return page.value("nextCursor").toString(); }
QSet<QString> nodeIds(const QJsonObject &page) {
  QSet<QString> ids; for (const auto &value : page.value("nodes").toArray()) ids.insert(value.toObject().value("version_id").toString()); return ids;
}
}
class TestProjectQuery : public QObject {
  Q_OBJECT
private slots:
  void sdkMetadataContract() { QCOMPARE(runWellQueryMetadataContract(),0); }
  void tenThousandRowsUseKeysetAndCompleteAcrossManySmallPages() {
    Fixture f; QVERIFY(f.open());
    {
      DataCatalog::BatchSave batch(&f.catalog);
      for (int i = 0; i < 10000; ++i) QVERIFY2(f.asset(QString("asset-%1").arg(i,5,10,QChar('0')), i%2 ? "horizon" : "document", QStringLiteral("重名中文")), qPrintable(f.error));
      QVERIFY2(batch.flush(&f.error), qPrintable(f.error));
    }
    QHash<QString,int> reads; f.catalog.setReadObserver([&](const char *name){++reads[QString::fromLatin1(name)];});
    const auto native = f.catalog.assetPage({}, {}, 3);
    QCOMPARE(native.rowsVisited, 3); QCOMPARE(native.totalCount, 10000); QVERIFY(native.hasMore);
    ProjectQueryCapability service; service.bind(&f.catalog, {"doc",1,1});
    QJsonObject args{{"topic","assets"},{"page_size",7},{"max_bytes",8192}};
    QSet<QString> ids; int pages = 0;
    do {
      const auto page = service.query(args); QVERIFY2(page.value("ok").toBool(), qPrintable(QJsonDocument(page).toJson()));
      QCOMPARE(page.value("count").toInt(), 10000); QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 8192);
      for (const auto &value : page.value("assets").toArray()) {
        const auto id = value.toObject().value("id").toString(); QVERIFY(!ids.contains(id)); ids.insert(id);
      }
      if (!page.value("hasMore").toBool()) break;
      args.insert("cursor", cursor(page)); QVERIFY(++pages < 2000);
    } while (true);
    QCOMPARE(ids.size(), 10000); QVERIFY(pages > 128);
    QCOMPARE(reads.value("assets"), 0); QCOMPARE(reads.value("entities"), 0); QCOMPARE(reads.value("versions"), 0);
    QVERIFY(reads.value("assetPage") > 1000);
    QString mismatch; QVERIFY2(f.catalog.indexHealthy(&mismatch), qPrintable(mismatch));
  }

  void cursorBindsQueryRevisionEpochAndReplayWindow() {
    Fixture f; QVERIFY(f.open()); for (int i = 0; i < 6; ++i) QVERIFY(f.asset(QString::number(i)));
    ProjectQueryCapability service; service.bind(&f.catalog, {"doc",9,1});
    QJsonObject args{{"topic","assets"},{"page_size",1}};
    const auto first = service.query(args); QVERIFY(first.value("ok").toBool()); args.insert("cursor", cursor(first));
    const auto second = service.query(args); QCOMPARE(service.query(args), second);
    auto changed = args; changed.insert("asset_type", "document");
    QCOMPARE(service.query(changed).value("code").toString(), QStringLiteral("cursor_query_mismatch"));
    auto tampered = args; auto token = cursor(first); token[token.size()-1] = token.back() == QChar('a') ? QChar('b') : QChar('a'); tampered["cursor"] = token;
    QCOMPARE(service.query(tampered).value("code").toString(), QStringLiteral("cursor_invalid"));
    auto thirdArgs = args; thirdArgs["cursor"] = cursor(second); const auto third = service.query(thirdArgs); QVERIFY(third.value("ok").toBool());
    QCOMPARE(service.query(args).value("code").toString(), QStringLiteral("cursor_replay_unavailable"));
    QVERIFY(f.catalog.renameAsset("2", "renamed", &f.error));
    thirdArgs["cursor"] = cursor(third);
    QCOMPARE(service.query(thirdArgs).value("code").toString(), QStringLiteral("stale_cursor"));
    service.bind(&f.catalog, {"doc",10,1});
    QCOMPARE(service.query(thirdArgs).value("code").toString(), QStringLiteral("cursor_scope_mismatch"));
  }

  void legacyReadOnlyQueriesUseEphemeralDocumentAndChatScopes() {
    Fixture f; QVERIFY(f.open()); for(int i=0;i<3;++i) QVERIFY(f.asset(QString::number(i)));
    CatalogEntity well;well.id="legacy-well";well.entityType="well";well.name="Legacy";
    QVERIFY(f.catalog.addEntity(well,&f.error));
    ProjectQueryCapability service; service.bind(&f.catalog,{{},0,1,"chat-a"});
    QJsonObject args{{"topic","assets"},{"page_size",1}};
    const auto page=service.query(args);QVERIFY(page.value("ok").toBool());
    const auto identity=page.value("coverage").toObject().value("documentId").toString();
    QVERIFY(identity.startsWith(QStringLiteral("session:")));QVERIFY(!identity.contains(f.temp.path()));
    const auto wells=service.query({{"topic","wells"}});QVERIFY(wells.value("ok").toBool());
    paleo::document::WellRef ref;
    QVERIFY(paleo::document::wellRefFromJson(wells.value("wells").toArray().first().toObject().value("well_ref").toObject(),ref));
    QCOMPARE(ref.documentId,identity);QVERIFY(!ref.modelAvailable);
    args["cursor"]=cursor(page);service.bind(&f.catalog,{{},0,1,"chat-b"});
    QCOMPARE(service.query(args).value("code").toString(),QStringLiteral("cursor_scope_mismatch"));
  }

  void foreignThreadQueriesRefuseBeforeReadingCatalogFacts() {
    Fixture f;QVERIFY(f.open());QVERIFY(f.asset("a"));int reads=0;
    f.catalog.setReadObserver([&](const char*){++reads;});
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});QJsonObject result;
    std::thread worker([&]{result=service.query({{"topic","assets"}});});worker.join();
    QCOMPARE(result.value("code").toString(),QStringLiteral("document_unavailable"));QCOMPARE(reads,0);
    QVERIFY(service.query({{"topic","assets"}}).value("ok").toBool());QVERIFY(reads>0);
  }

  void pendingBatchRollbackReopenAndStagingKeepIndexesAccurate() {
    Fixture f; QVERIFY(f.open()); for (int i=0;i<4;++i) QVERIFY(f.asset(QString::number(i)));
    ProjectQueryCapability service; service.bind(&f.catalog, {"doc",1,1});
    QJsonObject args{{"topic","assets"},{"page_size",1}}; auto page = service.query(args);
    {
      DataCatalog::BatchSave batch(&f.catalog); QVERIFY(f.asset("in-batch"));
      args["cursor"] = cursor(page); QCOMPARE(service.query(args).value("code").toString(), QStringLiteral("stale_cursor"));
      batch.abort();
    }
    QVERIFY(f.catalog.assetById("in-batch").id.isEmpty()); QString mismatch;
    QVERIFY2(f.catalog.indexHealthy(&mismatch), qPrintable(mismatch));
    args.remove("cursor"); page = service.query(args); args["cursor"] = cursor(page);
    QVERIFY(f.catalog.open(f.temp.path(), &f.error));
    QCOMPARE(service.query(args).value("code").toString(), QStringLiteral("stale_cursor"));
    QVERIFY2(f.catalog.indexHealthy(&mismatch), qPrintable(mismatch));
  }

  void parentsBeyondDescribeV1LimitAreReadable() {
    Fixture f; QVERIFY(f.open()); QVERIFY(f.asset("a")); QStringList parents;
    for(int i=0;i<400;++i) parents.append(QString("source-%1").arg(i));
    QVERIFY2(f.version("v","a",parents), qPrintable(f.error));
    paleo::capability::ArtifactVersionRequest request; request.documentId="doc"; request.versionId="v";
    request.expectedCatalogRevision=f.catalog.catalogRevision();
    const auto descriptor=paleo::capability::ArtifactVersionCapability::describe(&f.catalog,"doc",request);
    QVERIFY(descriptor.ok); QCOMPARE(descriptor.version.value("parentVersionIds").toArray().size(),128);
    ProjectQueryCapability service; service.bind(&f.catalog,{"doc",1,1});
    QJsonObject args{{"topic","version_parents"},{"version_id","v"},{"page_size",19}}; QStringList actual;
    do {
      const auto page=service.query(args); QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));
      QCOMPARE(page.value("contract").toString(),QStringLiteral("catalog.version.parents.page.v1"));
      for(const auto &value:page.value("parents").toArray())actual.append(value.toObject().value("version_id").toString());
      if(!page.value("hasMore").toBool())break;
      args["cursor"]=cursor(page);
    }while(true);
    QCOMPARE(actual,parents); QVERIFY(!descriptor.version.contains("nextCursor"));
  }

  void metadataRenameRollbackAndPreparedAdoptionMaintainIndexes() {
    Fixture f; QVERIFY(f.open()); QVERIFY(f.asset("a","document","same")); QVERIFY(f.asset("b","document","same"));
    QCOMPARE(f.catalog.assetNamePage("same",{},1).totalCount,2);
    QVERIFY(f.catalog.renameAsset("b","other",&f.error)); QCOMPARE(f.catalog.assetNamePage("same",{},2).totalCount,1);
    f.catalog.setLockedReadOnly(true); QVERIFY(!f.catalog.renameAsset("a","failed",&f.error)); f.catalog.setLockedReadOnly(false);
    QCOMPARE(f.catalog.assetNamePage("same",{},2).items.first().id,QStringLiteral("a"));
    CatalogEntity well; well.id="well";well.entityType="well";well.name="A-1";well.extra["areaModelWellId"]=qlonglong(1);
    QVERIFY(f.catalog.addEntity(well,&f.error)); well.name="B-2";well.extra["areaModelWellId"]=qlonglong(2);
    QVERIFY(f.catalog.updateEntity(well,&f.error)); QCOMPARE(f.catalog.wellNamePage("a1",{},2).totalCount,0);
    QCOMPARE(f.catalog.modelWellPage(1,{},2).totalCount,0); QCOMPARE(f.catalog.modelWellPage(2,{},2).totalCount,1);
    QVERIFY(f.version("v1","a")); QVERIFY(f.version("v2","a")); QCOMPARE(f.catalog.currentVersion("a").id,QStringLiteral("v2"));
    auto stage=f.catalog.createStagingCopy(f.temp.filePath("overlay")); QVERIFY(stage);
    const auto checkpoint=stage->checkpointStaging(); CatalogAsset staged;staged.id="staged";staged.type="document";
    QVERIFY(stage->addAsset(staged,&f.error)); QCOMPARE(stage->assetPage({}, {}, 10).totalCount,3);
    stage->rollbackStaging(checkpoint); QCOMPARE(stage->assetPage({}, {}, 10).totalCount,2);
    QVERIFY(stage->addAsset(staged,&f.error)); QVERIFY(f.catalog.applyJournal(stage->journal(),&f.error));
    auto prepared=DataCatalog::prepareOpen(f.temp.path(),true); QVERIFY(prepared&&prepared->isOpen()); DataCatalog adopted;
    adopted.setLockedReadOnly(true);
    QVERIFY2(adopted.adoptPrepared(prepared,&f.error),qPrintable(f.error)); QCOMPARE(adopted.assetPage({}, {}, 10).totalCount,3);
    QCOMPARE(adopted.currentVersion("a").id,QStringLiteral("v2")); QString mismatch;QVERIFY2(adopted.indexHealthy(&mismatch),qPrintable(mismatch));
  }

  void wellLinkRolePagesDoNotSkipEntriesAfterByteBoundaries() {
    Fixture f;QVERIFY(f.open());CatalogEntity well;well.id="well";well.name="W-1";well.entityType="well";QVERIFY(f.catalog.addEntity(well,&f.error));
    {
      DataCatalog::BatchSave batch(&f.catalog);
      for(int i=0;i<90;++i){const auto id=QString("a-%1").arg(i);QVERIFY(f.asset(id,"document",QString(60,QChar(0x4e2d))));
        EntityAssetLink link;link.entityType="well";link.entityId="well";link.assetId=id;link.role=i%2?"well_log":"time_depth";link.isPrimary=false;
        QVERIFY(f.catalog.addLink(link,&f.error));}
      QVERIFY(batch.flush(&f.error));
    }
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});
    QJsonObject args{{"topic","well_details"},{"well_id","well"},{"role","well_log"},{"page_size",50},{"max_bytes",4096}};QSet<QString> ids;
    do{const auto page=service.query(args);QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));QCOMPARE(page.value("totalCount").toInt(),45);
      for(const auto &value:page.value("links").toArray()){const auto id=value.toObject().value("asset_id").toString();QVERIFY(!ids.contains(id));ids.insert(id);}
      if(!page.value("hasMore").toBool())break;
      args["cursor"]=cursor(page);
    }while(true);QCOMPARE(ids.size(),45);
  }

  void directionalTraversalDoesNotVisitSiblingsOrLoseCycleDirection() {
    Fixture f; QVERIFY(f.open()); for(const auto &id:QStringList{"p","a","v","c","b"})QVERIFY(f.asset(id));
    QVERIFY(f.version("P","p"));QVERIFY(f.version("A","a",{"P"}));QVERIFY(f.version("V","v",{"A"}));
    QVERIFY(f.version("C","c",{"V"}));QVERIFY(f.version("B","b",{"P"}));
    QHash<QString,int> reads;f.catalog.setReadObserver([&](const char *name){++reads[QString::fromLatin1(name)];});
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});
    QJsonObject args{{"asset","V"},{"upstream_depth",2},{"downstream_depth",1},{"page_size",1}};QSet<QString> ids;
    do {const auto page=service.lineage(args);QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));ids|=nodeIds(page);
      QCOMPARE(page.value("selection").toObject().value("count").toInt(),page.value("nodes").toArray().size());
      if(!page.value("hasMore").toBool())break;
      args["cursor"]=cursor(page);
    }while(true);
    QCOMPARE(ids,QSet<QString>({"P","A","V","C"}));QCOMPARE(reads.value("versions"),0);QCOMPARE(reads.value("assets"),0);
    Fixture cycle;QVERIFY(cycle.open());for(const auto &id:QStringList{"a","p","v","c"})QVERIFY(cycle.asset(id));
    QVERIFY(cycle.version("A","a",{"P"}));QVERIFY(cycle.version("P","p",{"V"}));
    QVERIFY(cycle.version("V","v",{"A"}));QVERIFY(cycle.version("C","c",{"A"}));
    service.bind(&cycle.catalog,{"cycle",1,1});args={{"asset","V"},{"upstream_depth",1},{"downstream_depth",3},{"page_size",1}};ids.clear();
    do {const auto page=service.lineage(args);QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));ids|=nodeIds(page);
      if(!page.value("hasMore").toBool())break;
      args["cursor"]=cursor(page);
    }while(true);
    QCOMPARE(ids,QSet<QString>({"A","P","V","C"}));
  }

  void modelValuesAndHeadAreBoundWithoutWholeWellReads() {
    Fixture f;QVERIFY(f.open());
    const auto root=QDir(f.temp.path()).filePath("artifacts/metadata/area-model");
    paleo::data::DataService model;std::string error;QVERIFY2(model.createProject(root.toStdString(),"model",error),error.c_str());
    paleo::data::WellInfo info;info.wellName="A-1";info.totalDrilledDepth=123.0;long long first=0,second=0;
    QVERIFY2(model.createWell(info,first,error),error.c_str());info.wellName="中文井";info.totalDrilledDepth=456.0;
    QVERIFY2(model.createWell(info,second,error),error.c_str());
    for(const auto &pair:QVector<QPair<QString,qlonglong>>{{"well-1",first},{"well-2",second}}) {
      CatalogEntity row;row.id=pair.first;row.entityType="well";row.name="stale";row.td=9;
      row.extra["areaModelWellId"]=pair.second;QVERIFY(f.catalog.addEntity(row,&f.error));
    }
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});
    QJsonObject args{{"topic","wells"},{"page_size",1}};const auto page=service.query(args);
    QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));
    QCOMPARE(page.value("wells").toArray().first().toObject().value("name").toString(),QStringLiteral("A-1"));
    QCOMPARE(page.value("wells").toArray().first().toObject().value("td").toDouble(),123.0);
    paleo::document::WellRef selected;
    QVERIFY(paleo::document::wellRefFromJson(page.value("wells").toArray().first().toObject().value("well_ref").toObject(),selected));
    QCOMPARE(selected.modelWellId,first);QVERIFY(selected.modelAvailable);QCOMPARE(selected.modelStoreId,QStringLiteral("area-model"));
    auto details=service.query({{"topic","well_details"},{"well_name"," a_ 1 "}});
    QVERIFY2(details.value("ok").toBool(),qPrintable(QJsonDocument(details).toJson()));QCOMPARE(details.value("well_id").toString(),QStringLiteral("well-1"));
    info.totalDrilledDepth=777.0;QVERIFY2(model.saveWellInfo(second,info,error),error.c_str());args["cursor"]=cursor(page);
    QCOMPARE(service.query(args).value("code").toString(),QStringLiteral("model_revision_changed"));
    args.remove("cursor");args["page_size"]=2;const auto renewed=service.query(args);QVERIFY(renewed.value("ok").toBool());
    QCOMPARE(renewed.value("wells").toArray().at(1).toObject().value("td").toDouble(),777.0);
  }

  void highBranchingLineageAdvancesOnceAndRespectsZeroDepth() {
    Fixture f;QVERIFY(f.open());QVERIFY(f.asset("a"));QStringList parents;
    for(int i=0;i<300;++i)parents.append(QString("missing-%1").arg(i));
    QVERIFY(f.version("seed","a",parents));QHash<QString,int> reads;
    f.catalog.setReadObserver([&](const char* name){++reads[QString::fromLatin1(name)];});
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});
    const auto zero=service.lineage({{"asset","seed"},{"upstream_depth",0},{"downstream_depth",0}});
    QVERIFY(zero.value("ok").toBool());QCOMPARE(nodeIds(zero),QSet<QString>{"seed"});
    QVERIFY(zero.value("edges").toArray().isEmpty());QVERIFY(!zero.value("hasMore").toBool());
    QCOMPARE(reads.value("versionParentsPage"),0);reads.clear();
    QJsonObject args{{"asset","seed"},{"upstream_depth",1},{"downstream_depth",0},{"page_size",3},{"max_bytes",4096}};
    QSet<QString> nodes,edges;int missing=0;
    do {
      const auto page=service.lineage(args);QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));
      if(args.contains("cursor")){const auto before=reads;QCOMPARE(service.lineage(args),page);QCOMPARE(reads,before);}
      for(const auto& value:page.value("nodes").toArray()){
        const auto entry=value.toObject();const auto id=entry.value("version_id").toString();QVERIFY(!nodes.contains(id));nodes.insert(id);
        if(!entry.value("available").toBool())++missing;
      }
      for(const auto& value:page.value("edges").toArray()){
        const auto id=value.toObject().value("parent").toString();QVERIFY(!edges.contains(id));edges.insert(id);
      }
      if(!page.value("hasMore").toBool()){QCOMPARE(page.value("totalCount").toInt(),301);break;}
      QVERIFY(page.value("totalCount").isNull());args["cursor"]=cursor(page);
    }while(true);
    QCOMPARE(nodes.size(),301);QCOMPARE(edges.size(),300);QCOMPARE(missing,300);
    QCOMPARE(reads.value("versionParentsPage"),300);QCOMPARE(reads.value("versionChildrenPage"),0);
    QCOMPARE(reads.value("versions"),0);QCOMPARE(reads.value("assets"),0);
  }

  void effectiveWellNameResolutionContinuesWithoutWholeModelReads() {
    Fixture f;QVERIFY(f.open());paleo::data::DataService model;std::string error;
    const auto root=QDir(f.temp.path()).filePath("artifacts/metadata/area-model");
    QVERIFY2(model.createProject(root.toStdString(),"model",error),error.c_str());
    paleo::data::WellInfo info;qlonglong first=0,last=0;
    {
      DataCatalog::BatchSave batch(&f.catalog);
      for(int i=0;i<260;++i){info.wellName="actual-"+std::to_string(i);long long id=0;
        QVERIFY2(model.createWell(info,id,error),error.c_str());if(!first)first=id;last=id;
        CatalogEntity well;well.id=QString("well-%1").arg(i,3,10,QChar('0'));well.entityType="well";well.name="target";
        well.extra["areaModelWellId"]=qlonglong(id);QVERIFY(f.catalog.addEntity(well,&f.error));}
      QVERIFY(batch.flush(&f.error));
    }
    QHash<QString,int> reads;f.catalog.setReadObserver([&](const char* name){++reads[QString::fromLatin1(name)];});
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});
    QJsonObject args{{"topic","well_details"},{"well_name","target"},{"page_size",1},{"max_bytes",2048}};
    const auto searching=service.query(args);QVERIFY(searching.value("ok").toBool());
    QCOMPARE(searching.value("resolution").toString(),QStringLiteral("searching"));QVERIFY(searching.value("hasMore").toBool());
    QVERIFY(searching.value("totalCount").isNull());QCOMPARE(searching.value("returnedCount").toInt(),0);
    args["cursor"]=cursor(searching);QCOMPARE(service.query(args).value("code").toString(),QStringLiteral("well_not_found"));
    info.wellName="target";QVERIFY2(model.saveWellInfo(last,info,error),error.c_str());
    QCOMPARE(service.query(args).value("code").toString(),QStringLiteral("model_revision_changed"));
    args.remove("cursor");auto page=service.query(args);QVERIFY(page.value("ok").toBool());args["cursor"]=cursor(page);
    page=service.query(args);QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));
    QCOMPARE(page.value("well_id").toString(),QStringLiteral("well-259"));QVERIFY(!page.value("hasMore").toBool());
    info.wellName="Target";QVERIFY2(model.saveWellInfo(first,info,error),error.c_str());
    args.remove("cursor");page=service.query(args);QVERIFY(page.value("ok").toBool());args["cursor"]=cursor(page);
    QCOMPARE(service.query(args).value("code").toString(),QStringLiteral("well_ambiguous"));
    QCOMPARE(reads.value("entities"),0);QCOMPARE(reads.value("storedEntities"),0);QCOMPARE(reads.value("wellsMatchingName"),0);
  }

  void expiryAndBudgetsAreStructured() {
    Fixture f;QVERIFY(f.open());for(int i=0;i<3;++i)QVERIFY(f.asset(QString::number(i)));
    QueryPolicy policy;policy.ttlMs=1;ProjectQueryCapability service(policy);service.bind(&f.catalog,{"doc",1,1});
    QJsonObject args{{"topic","assets"},{"page_size",1}};const auto page=service.query(args);QVERIFY(page.value("ok").toBool());
    args["cursor"]=cursor(page);QTest::qWait(5);QCOMPARE(service.query(args).value("code").toString(),QStringLiteral("cursor_expired"));
    QCOMPARE(service.query({{"topic","assets"},{"page_size",1.5}}).value("code").toString(),QStringLiteral("query_budget_invalid"));
    QCOMPARE(service.query({{"topic","assets"},{"max_bytes",1024}}).value("code").toString(),QStringLiteral("query_budget_invalid"));
    CatalogEntity large;large.id="huge-name";large.entityType="well";large.name=QString(3000,QChar(0x4e2d));QVERIFY(f.catalog.addEntity(large,&f.error));
    const auto absent=service.query({{"topic","well_details"},{"well_name","absent"},{"max_bytes",2048}});
    QCOMPARE(absent.value("code").toString(),QStringLiteral("well_not_found"));QVERIFY(!absent.value("known_wells_complete").toBool());
    QVERIFY(QJsonDocument(absent).toJson(QJsonDocument::Compact).size()<=2048);
    QCOMPARE(service.query({{"topic","well_details"},{"well_name",QString(1000,QChar('x'))},{"max_bytes",2048}}).value("code").toString(),QStringLiteral("query_entry_too_large"));
  }

  void queryCitationsHidePortableExternalAbsolutePaths() {
    Fixture f;QVERIFY(f.open());QVERIFY(f.asset("a"));
    CatalogVersion version;version.id="v";version.assetId="a";version.stage="RAW";version.fileName="external.txt";
    version.path=QStringLiteral("C:\\Users\\private\\external.txt");version.managed=false;
    QVERIFY2(f.catalog.addVersion(version,&f.error),qPrintable(f.error));
    ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1});
    const auto page=service.query({{"topic","assets"}});QVERIFY(page.value("ok").toBool());
    QCOMPARE(page.value("assets").toArray().first().toObject().value("path").toString(),QStringLiteral("<external>/external.txt"));
  }
};
QTEST_GUILESS_MAIN(TestProjectQuery)
#include "tst_project_query.moc"
