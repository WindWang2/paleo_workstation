// 层：测试壳
#include "fixtures/pythonnumerictablehostfixture.h"
#include "services/projectquerycapability.h"
#include "services/pythonnumerictablequery.h"
#include <QtTest>
using namespace paleo;
using namespace numeric_table_host_fixture;
class TestPythonNumericTableWiring:public QObject {
  Q_OBJECT
private slots:
  void projectionQueryReturnsDeclaredMetadataAndExactInputWithoutValues() {
    Fixture f;QVERIFY(f.open());query::ProjectQueryCapability query;
    query.bind(&f.catalog,{f.context.documentId,1,0,"numeric-query"});
    query.setSourceResolver([&](const auto &ref){return f.session.source(ref);});
    const QJsonObject args{{"topic","numeric_table_projection"},{"source_ref",document::sourceRefJson(f.table.plan.selection.source)},
      {"table_id",QString::number(f.table.plan.selection.tableId)},{"max_bytes",65536}};
    const auto before=f.catalog.mutationSeq();const auto result=query.query(args);
    QVERIFY2(result.value("ok").toBool(),qPrintable(QJsonDocument(result).toJson()));
    QVERIFY(result.value("metadataOnly").toBool());QCOMPARE(result.value("numericPayloadBytes").toInt(),0);
    QVERIFY(!result.value("hasMore").toBool());QVERIFY(result.value("nextCursor").isNull());
    QCOMPARE(result.value("input_ref").toObject(),python::numericTableSelectionJson(f.table.plan.selection));
    QCOMPARE(result.value("source_version_id").toString(),f.table.plan.source.versionId);
    QCOMPARE(result.value("readCut").toObject().value("boundary").toString(),QString("observed_sqlite_read_snapshot_not_history_lease"));
    QVERIFY(!result.contains("values"));QVERIFY(!result.contains("payload"));QCOMPARE(f.catalog.mutationSeq(),before);
    auto small=args;small["max_bytes"]=2048;QCOMPARE(query.query(small).value("code").toString(),QString("query_response_budget_exceeded"));
    auto invalid=args;invalid["cursor"]="";QCOMPARE(query.query(invalid).value("code").toString(),QString("unsupported_topic_argument"));
    auto foreign=f.table.plan.selection.source;foreign.documentId="other-document";invalid=args;invalid["source_ref"]=document::sourceRefJson(foreign);
    QCOMPARE(query.query(invalid).value("code").toString(),QString("source_document_mismatch"));
  }
  void projectionQueryRejectsResolverReplacementAndBrokenFormalParent() {
    Fixture f;QVERIFY(f.open());query::ProjectQueryCapability query;query.bind(&f.catalog,{f.context.documentId,1,0,"numeric-query"});
    const QJsonObject args{{"topic","numeric_table_projection"},{"source_ref",document::sourceRefJson(f.table.plan.selection.source)},
      {"table_id",QString::number(f.table.plan.selection.tableId)},{"max_bytes",65536}};
    query.setSourceResolver([&](const auto &ref){query.setSourceResolver([&](const auto &r){return f.session.source(r);});return f.session.source(ref);});
    QCOMPARE(query.query(args).value("code").toString(),QString("cursor_scope_mismatch"));
    QVERIFY(f.catalog.updateVersionExtra(f.table.plan.source.versionId,"areaModelSourceId",f.table.plan.selection.source.sourceId+1,&f.error));
    QCOMPARE(query.query(args).value("code").toString(),QString("python_table_source_mapping_unavailable"));
  }
  void projectionQueryDoesNotInstallMissingProjectionSchema() {
    document_fixture::Fixture document;numeric_table_fixture::Model table;QVERIFY(document.init());
    QVERIFY(table.open(document.project,"fixture-document",0,"-2.5","1",false));
    QVERIFY(registerAreaSources(document.catalog,&document.error));QVERIFY(document.bind());std::string detail;
    data::ModelChangeHead before,after;QVERIFY(table.sdk.readModelChangeHead(before,detail));query::ProjectQueryCapability query;
    query.bind(&document.catalog,{document.session.documentId(),1,0,"numeric-query"});
    query.setSourceResolver([&](const auto &ref){return document.session.source(ref);});
    const auto result=query.query({{"topic","numeric_table_projection"},{"source_ref",document::sourceRefJson(table.plan.selection.source)},
      {"table_id",QString::number(table.plan.selection.tableId)},{"max_bytes",65536}});
    QCOMPARE(result.value("code").toString(),QString("python_table_schema_unavailable"));QVERIFY(!result.contains("input_ref"));
    QVERIFY(table.sdk.readModelChangeHead(after,detail));QCOMPARE(after.storeId,before.storeId);QCOMPARE(after.generation,before.generation);
    QCOMPARE(after.sequence,before.sequence);QCOMPARE(after.available,before.available);
    document::SourceRef source;qlonglong id=0;auto invalid=QJsonObject{{"topic","numeric_table_projection"},{"source_ref",document::sourceRefJson(table.plan.selection.source)},{"table_id","01"}};
    QCOMPARE(python::numericTableProjectionQueryArguments(invalid,source,id),QString("python_table_id_required"));
  }
  void sdkTableUsesItsActualCatalogFileSourceParent() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));const auto provider=assemblePythonNumericTableProvider(&f.catalog,&f.session,f.host.get());
    python::NumericTableReadPlan plan;std::function<QString()> validate;const auto revision=f.catalog.mutationSeq();
    QCOMPARE(provider(f.request(),plan,validate),QString());QVERIFY(validate);QCOMPARE(validate(),QString());
    const auto parent=f.catalog.versionById(plan.source.versionId);QCOMPARE(parent.assetId,plan.source.assetId);
    QCOMPARE(parent.extra.value("areaModelSourceId").toLongLong(),plan.selection.source.sourceId);
    QCOMPARE(document::SourceFingerprint::fromStored(parent.sha256).stored(),plan.selection.source.fileRevisionId);
    QCOMPARE(plan.modelDirectory,QFileInfo(f.table.model).canonicalFilePath());QCOMPARE(f.catalog.mutationSeq(),revision);
    f.context.allowedEffects=host::Read|host::PrivateStage;QCOMPARE(validate(),QString("effect_denied"));
  }
  void brokenFormalSourceMappingCannotInventAParentVersion() {
    Fixture f;QVERIFY(f.open());const auto source=f.table.plan.source;
    QVERIFY(f.catalog.updateVersionExtra(source.versionId,"areaModelSourceId",source.ref.sourceId+100,&f.error));
    const auto provider=assemblePythonNumericTableProvider(&f.catalog,&f.session,f.host.get());
    python::NumericTableReadPlan plan;std::function<QString()> validate;
    QCOMPARE(provider(f.request(),plan,validate),QString("python_table_source_mapping_unavailable"));QVERIFY(!validate);
    QVERIFY(plan.source.versionId.isEmpty());
  }
  void validationRejectsContextOrSourceRebind() {
    Fixture f;QVERIFY(f.open());const auto provider=assemblePythonNumericTableProvider(&f.catalog,&f.session,f.host.get());
    python::NumericTableReadPlan plan;std::function<QString()> validate;QCOMPARE(provider(f.request(),plan,validate),QString());
    f.context.epoch++;QVERIFY(!validate().isEmpty());f.context.epoch--;
    QVERIFY(f.catalog.updateVersionExtra(plan.source.versionId,"areaModelSourceId",plan.source.ref.sourceId+1,&f.error));
    QCOMPARE(validate(),QString("python_table_source_mapping_unavailable"));
  }
  void scopeCallbackMayDeleteCatalogBeforeAnySourceAccess() {
    Fixture f;QVERIFY(f.open());auto alternate=std::make_unique<DataCatalog>();QVERIFY(alternate->open(f.project,&f.error));int calls=0;
    f.host->setBindingProvider([&] {capability::HostBinding out;out.context=f.context;if(++calls==2)alternate.reset();return out;});
    const auto provider=assemblePythonNumericTableProvider(alternate.get(),&f.session,f.host.get());
    python::NumericTableReadPlan plan;std::function<QString()> validate;
    QCOMPARE(provider(f.request(),plan,validate),QString("document_unavailable"));QCOMPARE(calls,2);QVERIFY(!validate);
  }
};
QTEST_GUILESS_MAIN(TestPythonNumericTableWiring)
#include "tst_pythonnumerictablewiring.moc"
