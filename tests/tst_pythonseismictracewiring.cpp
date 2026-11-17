// 层：测试壳
#include "fixtures/pythonseismictracehostfixture.h"
#include "workflow/pythonseismictraceoperation_internal.h"
#include <QtTest>
using namespace paleo;
using namespace seismic_trace_host_fixture;
class TestPythonSeismicTraceWiring:public QObject {
  Q_OBJECT
private slots:
  void descriptorQueryReadsOnlyBoundedMetadataAndMakesNoIntegrityClaim() {
    Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));query::ProjectQueryCapability query;
    QCOMPARE(f.descriptor.cut.sourceId,f.traceSelection.source.sourceId);
    QVERIFY(f.descriptorIo.metadataBytesRead>0);QVERIFY(f.descriptorIo.metadataBytesRead<=8192);
    QCOMPARE(f.descriptorIo.requestedSourceBytes,qint64(0));QCOMPARE(f.descriptorIo.sourceReadCalls,qint64(0));
    QCOMPARE(f.descriptorIo.outputBytes,qint64(0));
    query.bind(&f.catalog,{f.session.documentId(),1,0,"trace-query"});
    query.setSourceResolver([&](const auto &ref){return f.session.source(ref);});
    const QJsonObject args{{"topic","seismic_trace_descriptor"},{"source_ref",document::sourceRefJson(f.traceSelection.source)},
      {"volume_id",QString::number(f.traceSelection.volumeId)},{"max_bytes",65536}};
    const auto result=query.query(args);QVERIFY2(result.value("ok").toBool(),qPrintable(QJsonDocument(result).toJson()));
    QVERIFY(result.value("metadataOnly").toBool());QCOMPARE(result.value("numericPayloadBytes").toInt(),0);
    QCOMPARE(result.value("sourceIntegrity").toString(),QString("not_checked_metadata_only"));
    auto selected=f.traceSelection;selected.traceIndex=0;selected.firstSample=0;selected.sampleCount=6;
    QCOMPARE(result.value("input_ref").toObject(),python::seismicTraceSelectionJson(selected));
    QCOMPARE(result.value("readCut"),QJsonValue(python::seismicTraceReadCutJson(f.descriptor.cut)));
    QVERIFY(!result.contains("axis"));QVERIFY(!result.contains("amplitudes"));
    QCOMPARE(f.session.seismicSourceReader()->statistics().readBytes,quint64(0));
    auto invalid=args;invalid["path"]="source.sgy";QCOMPARE(query.query(invalid).value("code").toString(),QString("unsupported_topic_argument"));
    QVERIFY(QFile::remove(f.source.path));QVERIFY(query.query(args).value("ok").toBool());
    const auto selector=assemblePythonSeismicTraceProvider(&f.catalog,&f.session,f.host.get());
    python::SeismicTraceReadPlan plan;std::function<QString()> validate;
    QCOMPARE(selector(f.traceRequest(),plan,validate),QString("python_seismic_source_unavailable"));
    QCOMPARE(f.session.seismicSourceReader()->statistics().readBytes,quint64(0));
  }
  void realFormalSourceAndReaderAreStableAcrossRequestIds() {
    Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));const auto selector=assemblePythonSeismicTraceProvider(&f.catalog,&f.session,f.host.get());
    python::SeismicTraceReadPlan a,b;std::function<QString()> av,bv;
    QCOMPARE(selector(f.traceRequest("one"),a,av),QString());QCOMPARE(selector(f.traceRequest("two"),b,bv),QString());
    QVERIFY(a.reader);QCOMPARE(a.reader,b.reader);QCOMPARE(a.authenticatedSource.scopeKey,b.authenticatedSource.scopeKey);
    QCOMPARE(a.source.versionId,f.source.versionId);QCOMPARE(a.source.assetId,f.source.assetId);
    QCOMPARE(a.authenticatedSource.sha256,f.traceSelection.source.fileRevisionId);QCOMPARE(a.authenticatedSource.byteSize,qint64(f.raw.size()));
    QVERIFY(!a.authenticatedSource.scopeKey.contains("one"));QVERIFY(av);QCOMPARE(av(),QString());
    QCOMPARE(a.reader->statistics().readBytes,quint64(0));
  }
  void quickCatalogAnchorAndForeignSourceMappingCannotAuthorizeReads() {
    Fixture f;QVERIFY(f.openTrace());const auto selector=assemblePythonSeismicTraceProvider(&f.catalog,&f.session,f.host.get());
    auto request=f.traceRequest();auto selected=f.traceSelection;selected.source.fileRevisionId="quick:"+selected.source.fileRevisionId;
    request.inputs=python::seismicTraceSelectionJson(selected).toVariantMap();python::SeismicTraceReadPlan plan;std::function<QString()> validate;
    QCOMPARE(selector(request,plan,validate),QString("python_seismic_source_mismatch"));QVERIFY(!validate);
    QVERIFY(f.catalog.updateVersionExtra(f.source.versionId,"areaModelSourceId",f.source.ref.sourceId+100,&f.error));
    QCOMPARE(selector(f.traceRequest(),plan,validate),QString("python_seismic_source_mapping_unavailable"));QVERIFY(!validate);
    QCOMPARE(f.session.seismicSourceReader()->statistics().readBytes,quint64(0));
  }
  void documentCloseRebindInvalidatesInflightReaderAndRotatesScope() {
    Fixture f;QVERIFY(f.openTrace());const auto selector=assemblePythonSeismicTraceProvider(&f.catalog,&f.session,f.host.get());
    python::SeismicTraceReadPlan old,current;std::function<QString()> oldCheck,currentCheck;
    QCOMPARE(selector(f.traceRequest(),old,oldCheck),QString());const auto epoch=f.session.epoch();
    f.session.close();QCOMPARE(oldCheck(),QString("document_unavailable"));
    QCOMPARE(old.reader->read(old.authenticatedSource,0,16).code,QString("cancelled"));
    QVERIFY(f.bind());QVERIFY(f.session.epoch()>epoch);QCOMPARE(selector(f.traceRequest(),current,currentCheck),QString());
    QVERIFY(current.reader!=old.reader);QVERIFY(current.authenticatedSource.scopeKey!=old.authenticatedSource.scopeKey);
    QCOMPARE(oldCheck(),QString("source_revision_changed"));QCOMPARE(currentCheck(),QString());
  }
  void generatedPermissionAndHistoricalEnvelopeAreRequired() {
    python::ExecutionPolicy denied;denied.allowTrusted=true;
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{},denied);
    const auto adapter=python::seismicTraceScriptAdapter(runtime,std::make_shared<python::SandboxProvider>(runtime),{});
    capability::ExecuteRequest request;request.capabilityId=QLatin1String(python::SeismicTraceCapability);request.contractVersion=1;
    request.documentId="doc";request.callerScope="caller";request.clientRequestId="client";request.projectDir="project";QString error;
    const auto preparation=adapter.capture(request,{},&error);QCOMPARE(error,QString("python_execution_denied"));QVERIFY(!preparation.worker);
    auto registry=std::make_shared<python::PythonEnvironmentRegistry>(QString());
    const auto managed=python::environmentSeismicTraceAdapter(registry,{});capability::OperationLog::Record old;
    QCOMPARE(managed.validateFrozen(old),QString("invalid_python_environment_plan"));
  }
};
QTEST_GUILESS_MAIN(TestPythonSeismicTraceWiring)
#include "tst_pythonseismictracewiring.moc"
