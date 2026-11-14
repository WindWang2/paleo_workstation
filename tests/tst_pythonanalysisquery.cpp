// 层：测试壳
#include "services/projectquerycapability.h"
#include "catalog/datacatalog.h"
#include "catalog/documentsourceprojection.h"
#include "domain/wellref.h"
#include <paleo_data/service.h>
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

using namespace paleo;
namespace {
struct Fixture {
  QTemporaryDir temporary;DataCatalog catalog;data::DataService sdk;QString project,model,error;
  document::WellRef well;document::SourceProjection projection;std::vector<data::DataEntry> entries;
  bool open(bool inferred=true,bool largeFirst=false,bool largeSource=false) {
    project=temporary.path()+"/project";if(!catalog.open(project,&error))return false;model=project+"/artifacts/metadata/area-model";std::string detail;
    if(!sdk.createProject(model.toUtf8().toStdString(),"Query metadata",detail)){error=QString::fromStdString(detail);return false;}
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";if(largeSource)parsed.message=std::string(32768,'x');
    data::WellDataset dataset;dataset.wellInfo.wellName="DISCOVERY-1";
    data::LogSet log;log.logSetName="Explicit shared MD";log.depthInfo={"MD","m","KB","","",""};
    for(int i=0;i<8192;++i)log.samplingAxis.depthValues.push_back(100+i*0.25);
    for(int i=0;i<2;++i){data::LogCurve curve;curve.curveName=i?"B":largeFirst?std::string(2000,'X'):"A";curve.curveUnit=i?"m/s":"API";
      curve.sampleValues.assign(8192,double(i+1));log.logCurves.push_back(std::move(curve));}
    dataset.wellbore.logSets.push_back(log);
    if(inferred){log.logSetName="Legacy inferred MD";log.depthInfo.depthType.clear();log.logCurves.resize(1);log.logCurves[0].curveName="C";dataset.wellbore.logSets.push_back(log);}
    parsed.wells.push_back(std::move(dataset));const auto path=temporary.path()+"/source.las";QFile file(path);const QByteArray bytes="Prepared real SDK query fixture\n";
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())return false;file.close();
    const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();long long source=0;
    if(!sdk.importPreparedFile(path.toUtf8().toStdString(),path.toUtf8().toStdString(),path.toUtf8().toStdString(),hash.toStdString(),parsed,source,detail)||
        !sdk.listData("curves",0,entries,detail)||entries.size()!=(inferred?3u:2u)){error=QString::fromStdString(detail);return false;}
    CatalogEntity entity;entity.id="well-catalog";entity.entityType="well";entity.name="DISCOVERY-1";entity.extra.insert("areaModelWellId",qlonglong(entries[0].wellId));
    if(!catalog.addEntity(entity,&error))return false;CatalogAsset asset;asset.id="source-asset";asset.type="well_log";asset.displayName="Source";
    if(!catalog.addAsset(asset,&error))return false;CatalogVersion version;version.id="source-version";version.assetId=asset.id;version.stage="RAW";
    version.managed=false;version.path=path;version.sourceUri=path;version.fileName="source.las";version.sha256=QString::fromLatin1(hash);version.extra.insert("areaModelSourceId",qlonglong(source));
    if(!catalog.addVersion(version,&error))return false;projection=document::projectDocumentSources(catalog,"doc");if(!projection.error.isEmpty()){error=projection.error;return false;}
    data::ModelChangeHead head;if(!sdk.readModelChangeHead(head,detail)||!head.available)return false;
    well.documentId="doc";well.catalogEntityId=entity.id;well.modelStoreId="area-model";well.modelStoreIdentity=QString::fromStdString(head.storeId);
    well.modelGeneration=QString::fromStdString(head.generation);well.modelWellId=entries[0].wellId;well.queryHead=head.sequence;well.modelAvailable=true;return well.valid();
  }
  query::ProjectQueryCapability::SourceResolver resolver() {return [this](const document::SourceRef &ref){return projection.records.value(ref.key());};}
  QJsonObject arguments(int page=1,int bytes=8192) const {return {{"topic","well_curves"},{"well_ref",document::wellRefJson(well)},{"page_size",page},{"max_bytes",bytes}};}
  QByteArray snapshot(const QString &name) {
    const auto path=temporary.path()+'/'+name;if(!catalog.exportCatalogJson(path,&error))return {};QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
  }
};
QJsonObject firstCurve(const QJsonObject &page) {return page.value("curves").toArray().first().toObject();}
}
class PythonAnalysisQueryTests:public QObject {
  Q_OBJECT
private slots:
  void genuineSourceProjectionProducesPagedReferencesWithoutPayloadOrPaths() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));const auto before=f.snapshot("before.json");QVERIFY(!before.isEmpty());
    QHash<QString,int> reads;f.catalog.setReadObserver([&](const char *name){++reads[QString::fromLatin1(name)];});
    query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",7,1,"caller:chat-a"});service.setSourceResolver(f.resolver());auto args=f.arguments();
    QSet<QString> ids;int pages=0,ready=0,inferred=0;
    for(;;){const auto page=service.query(args);QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));
      QCOMPARE(page.value("returnedCount").toInt(),1);QCOMPARE(page.value("numericPayloadBytes").toString(),QString("0"));QVERIFY(page.value("metadataOnly").toBool());
      const auto sourceBytes=page.value("sourceMetadataBytes").toString().toLongLong();QVERIFY(sourceBytes>0&&sourceBytes<=8192);
      const auto metadataBytes=page.value("metadataTextBytes").toString().toLongLong();QVERIFY(metadataBytes>sourceBytes);
      QVERIFY(metadataBytes<=page.value("metadataReadBudgetBytes").toString().toLongLong());
      qInfo()<<"QUERY_METADATA page"<<pages+1<<"textBytes"<<metadataBytes<<"sourceBytes"<<sourceBytes
        <<"budgetBytes"<<page.value("metadataReadBudgetBytes").toString()<<"numericPayloadBytes"<<page.value("numericPayloadBytes").toString();
      const auto bytes=QJsonDocument(page).toJson(QJsonDocument::Compact);QVERIFY(bytes.size()<=8192);QVERIFY(!bytes.contains(f.temporary.path().toUtf8()));QVERIFY(!bytes.contains("\"path\""));
      const auto row=firstCurve(page);const auto id=row.value("curve_id").toString();QVERIFY(!ids.contains(id));ids.insert(id);QCOMPARE(row.value("total_samples").toString(),QString("8192"));
      if(row.value("metadataReady").toBool()){++ready;const auto input=row.value("input_ref").toObject();
        QCOMPARE(input.keys(),QStringList({"curve_id","depth_reference","depth_type","depth_unit","log_set_id","source_ref","source_version_id","total_samples","unit","well_id"}));
        QCOMPARE(input.value("source_version_id").toString(),QString("source-version"));QCOMPARE(input.value("well_id").toString(),QString::number(f.well.modelWellId));
        document::SourceRef ref;QVERIFY(document::sourceRefFromJson(input.value("source_ref").toObject(),ref));QCOMPARE(ref.documentId,QString("doc"));QCOMPARE(ref.modelStoreId,QString("area-model"));
      } else {++inferred;QCOMPARE(row.value("code").toString(),QString("curve_semantics_not_declared"));QVERIFY(row.value("depth_type").isNull());QVERIFY(!row.contains("input_ref"));}
      ++pages;if(!page.value("hasMore").toBool())break;args.insert("cursor",page.value("nextCursor"));QVERIFY(pages<10);
    }
    QCOMPARE(pages,3);QCOMPARE(ids.size(),3);QCOMPARE(ready,2);QCOMPARE(inferred,1);
    QCOMPARE(reads.value("entities"),0);QCOMPARE(reads.value("versions"),0);QCOMPARE(reads.value("links"),0);
    QCOMPARE(f.snapshot("after.json"),before);
  }
  void cursorKeepsIdentityBudgetScopeAndFullSdkHead() {
    Fixture f;QVERIFY(f.open());query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat-a"});service.setSourceResolver(f.resolver());
    auto args=f.arguments();const auto first=service.query(args);QVERIFY(first.value("ok").toBool());args.insert("cursor",first.value("nextCursor"));
    const auto second=service.query(args);QVERIFY(second.value("ok").toBool());QCOMPARE(service.query(args),second);
    auto changed=args;changed.insert("max_bytes",4096);QCOMPARE(service.query(changed).value("code").toString(),QString("cursor_query_mismatch"));
    changed=args;changed.insert("page_size",2);QCOMPARE(service.query(changed).value("code").toString(),QString("cursor_query_mismatch"));
    changed=args;auto ref=changed.value("well_ref").toObject();ref.insert("model_head",QString::number(f.well.queryHead+1));changed.insert("well_ref",ref);
    QCOMPARE(service.query(changed).value("code").toString(),QString("cursor_query_mismatch"));
    query::ProjectQueryCapability caller;caller.bind(&f.catalog,{"doc",1,1,"other-caller"});caller.setSourceResolver(f.resolver());QVERIFY(!caller.query(args).value("ok").toBool());
    service.bind(&f.catalog,{"doc",1,1,"chat-b"});QCOMPARE(service.query(args).value("code").toString(),QString("cursor_scope_mismatch"));
    service.bind(&f.catalog,{"doc",2,1,"chat-a"});QVERIFY(!service.query(args).value("ok").toBool());
    service.bind(&f.catalog,{"other-document",1,1,"chat-a"});QCOMPARE(service.query(f.arguments()).value("code").toString(),QString("well_ref_scope_mismatch"));
    service.bind(&f.catalog,{"doc",1,1,"chat-a"});auto fresh=f.arguments();const auto page=service.query(fresh);QVERIFY(page.value("ok").toBool());fresh.insert("cursor",page.value("nextCursor"));
    std::string detail;QVERIFY(f.sdk.updateCurveValue(f.entries[0].id,0,99.0,detail));QCOMPARE(service.query(fresh).value("code").toString(),QString("model_revision_changed"));
    for(const auto field:{"model_store_identity","model_generation","model_head"}){changed=f.arguments();ref=changed.value("well_ref").toObject();
      ref.insert(QLatin1String(field),QStringLiteral("wrong"));if(QLatin1String(field)==QLatin1String("model_head"))ref.insert(QLatin1String(field),"999999");changed.insert("well_ref",ref);
      QVERIFY(!service.query(changed).value("ok").toBool());}
  }
  void wrongWellMappingAndMissingProviderCannotYieldInputAuthority() {
    Fixture f;QVERIFY(f.open(false));query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat"});
    auto page=service.query(f.arguments());QVERIFY(page.value("ok").toBool());auto row=firstCurve(page);QVERIFY(!row.value("metadataReady").toBool());
    QCOMPARE(row.value("code").toString(),QString("source_resolver_unavailable"));QVERIFY(!row.contains("input_ref"));
    service.setSourceResolver(f.resolver());auto entity=f.catalog.storedEntityById(f.well.catalogEntityId);entity.extra.insert("areaModelWellId",qlonglong(f.well.modelWellId+1));QVERIFY(f.catalog.updateEntity(entity,&f.error));
    QCOMPARE(service.query(f.arguments()).value("code").toString(),QString("well_model_mapping_mismatch"));
    entity.extra.remove("areaModelWellId");QVERIFY(f.catalog.updateEntity(entity,&f.error));QCOMPARE(service.query(f.arguments()).value("code").toString(),QString("well_model_mapping_mismatch"));
  }
  void sourceVersionScopeAndCanonicalModelMustMatch() {
    Fixture f;QVERIFY(f.open(false));query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat"});
    const auto original=f.projection.records;QVERIFY(original.size()==1);const auto key=original.constBegin().key();
    for(const auto change:{"version","document","root","fingerprint"}){f.projection.records=original;auto &record=f.projection.records[key];
      if(QLatin1String(change)==QLatin1String("version"))record.versionId="not-in-catalog";
      if(QLatin1String(change)==QLatin1String("document"))record.ref.documentId="other-doc";
      if(QLatin1String(change)==QLatin1String("root"))record.modelDirectory=f.project;
      if(QLatin1String(change)==QLatin1String("fingerprint"))record.fingerprint=document::SourceFingerprint::fromStored(QString(64,'b'));
      service.setSourceResolver(f.resolver());const auto page=service.query(f.arguments());QVERIFY(page.value("ok").toBool());const auto row=firstCurve(page);
      QCOMPARE(row.value("code").toString(),QString("curve_source_mapping_unavailable"));QVERIFY(!row.value("metadataReady").toBool());QVERIFY(!row.contains("input_ref"));}
  }
  void metadataQueriesNeverClaimNumericStorageIsReady() {
    Fixture f;QVERIFY(f.open(false));const auto connection=QUuid::createUuid().toString();bool removed=false;
    {auto database=QSqlDatabase::addDatabase("QSQLITE",connection);database.setDatabaseName(f.model+"/data.sqlite");QVERIFY(database.open());QSqlQuery query(database);
      removed=query.exec("DELETE FROM curve_window_curves_v1");database.close();}QSqlDatabase::removeDatabase(connection);QVERIFY(removed);
    query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat"});service.setSourceResolver(f.resolver());const auto page=service.query(f.arguments());
    QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));QVERIFY(firstCurve(page).value("metadataReady").toBool());
    QVERIFY(!firstCurve(page).contains("numericReady"));QCOMPARE(page.value("numericPayloadBytes").toString(),QString("0"));QVERIFY(page.value("metadataOnly").toBool());
  }
  void changedHeadDuringPageRejectsAllRows() {
    Fixture f;QVERIFY(f.open(false));query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat"});bool changed=false;
    service.setSourceResolver([&](const document::SourceRef &ref){if(!changed){std::string detail;changed=f.sdk.updateCurveValue(f.entries[0].id,0,87.0,detail);}return f.projection.records.value(ref.key());});
    const auto page=service.query(f.arguments(2));QVERIFY(changed);QCOMPARE(page.value("code").toString(),QString("model_revision_changed"));QVERIFY(!page.contains("curves"));
  }
  void providerReplacementAndReentrantRebindingRevokePages() {
    Fixture f;QVERIFY(f.open(false));query::ProjectQueryCapability service;const query::Scope scope{"doc",1,1,"chat"};service.bind(&f.catalog,scope);service.setSourceResolver(f.resolver());
    const auto first=service.query(f.arguments());QVERIFY(firstCurve(first).value("metadataReady").toBool());auto continuation=f.arguments();continuation.insert("cursor",first.value("nextCursor"));
    service.setSourceResolver([](const document::SourceRef &){return document::SourceRecord{};});QVERIFY(!service.query(continuation).value("ok").toBool());
    const auto next=service.query(f.arguments());QVERIFY(next.value("ok").toBool());QVERIFY(!firstCurve(next).value("metadataReady").toBool());
    for(bool replace:{true,false}){bool rebound=false;service.setSourceResolver([&](const document::SourceRef &ref){
        if(!rebound){rebound=true;if(replace)service.setSourceResolver({});else service.bind(&f.catalog,scope);}return f.projection.records.value(ref.key());});
      const auto page=service.query(f.arguments());QVERIFY(rebound);QCOMPARE(page.value("code").toString(),QString("cursor_scope_mismatch"));QVERIFY(!page.contains("curves"));QVERIFY(!page.contains("nextCursor"));}
  }
  void oversizedSourceMetadataOnlyMakesTheRowUnavailable() {
    Fixture f;QVERIFY(f.open(false,false,true));query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat"});service.setSourceResolver(f.resolver());
    const auto page=service.query(f.arguments());QVERIFY2(page.value("ok").toBool(),qPrintable(QJsonDocument(page).toJson()));const auto row=firstCurve(page);
    QVERIFY(!row.value("metadataReady").toBool());QCOMPARE(row.value("code").toString(),QString("curve_source_unavailable"));QVERIFY(!row.contains("input_ref"));
    QCOMPARE(page.value("sourceMetadataBytes").toString(),QString("0"));QVERIFY(!QJsonDocument(page).toJson().contains(QByteArray(256,'x')));
    QVERIFY(row.value("unit").isNull());QVERIFY(row.value("depth_type").isNull());
    QVERIFY(page.value("metadataTextBytes").toString().toLongLong()<=8192);
  }
  void byteAndRowBudgetsRejectOversizedFirstEntryWithoutPartialRows() {
    Fixture f;QVERIFY(f.open(false,true));query::ProjectQueryCapability service;service.bind(&f.catalog,{"doc",1,1,"chat"});service.setSourceResolver(f.resolver());
    auto page=service.query(f.arguments(1,2048));QVERIFY(!page.value("ok").toBool());QCOMPARE(page.value("code").toString(),QString("query_entry_too_large"));QVERIFY(!page.contains("curves"));
    page=service.query(f.arguments(17));QCOMPARE(page.value("code").toString(),QString("query_budget_invalid"));
    auto args=f.arguments();args.remove("well_ref");QCOMPARE(service.query(args).value("code").toString(),QString("invalid_well_ref"));
    args=f.arguments();args.insert("well_id","well-catalog");QCOMPARE(service.query(args).value("code").toString(),QString("unsupported_topic_argument"));
  }
};
QTEST_GUILESS_MAIN(PythonAnalysisQueryTests)
#include "tst_pythonanalysisquery.moc"
