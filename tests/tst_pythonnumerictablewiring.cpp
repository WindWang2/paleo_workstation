// 层：测试壳
#include "fixtures/pythonnumerictablehostfixture.h"
#include <QtTest>
using namespace paleo;
using namespace numeric_table_host_fixture;
class TestPythonNumericTableWiring:public QObject {
  Q_OBJECT
private slots:
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
