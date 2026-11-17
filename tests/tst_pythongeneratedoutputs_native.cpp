// 层：测试壳
#include "fixtures/pythongeneratedoutputsfixture.h"
#include <QImage>
#include <QtTest>
#include <cstring>
using namespace paleo;
using namespace generated_outputs_fixture;
namespace {
void checkPng(const QByteArray &png,const QString &kind) {
  QVERIFY(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));QVERIFY(png.size()<=python::ImageOutputPngBytes);
  const auto decoded=QImage::fromData(png,"PNG").convertToFormat(QImage::Format_RGBA8888);
  const auto rgba=expectedRgba(kind);QCOMPARE(decoded.width(),kind=="numeric_table"?4:3);QCOMPARE(decoded.height(),1);
  QCOMPARE(std::memcmp(decoded.constScanLine(0),rgba.constData(),size_t(rgba.size())),0);
}
}
class TestPythonGeneratedOutputsNative:public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY2(!qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT").isEmpty(),"Explicit locked Python root required by this opt-in suite");
    const auto sha=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");QCOMPARE(sha.size(),64);
    QCOMPARE(QByteArray::fromHex(sha.toLatin1()).toHex(),sha.toLatin1());
  }
  void actualInputThreePortsPagesHistoricalRestartAndOnePublication_data() {
    QTest::addColumn<QString>("kind");QTest::newRow("authenticated-seismic-time-axis")<<QString("seismic_trace");
    QTest::newRow("declared-table-physical-row-axis")<<QString("numeric_table");
  }
  void actualInputThreePortsPagesHistoricalRestartAndOnePublication() {
    QFETCH(QString,kind);Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs();
    const auto identity=f.original.identity;const auto parent=kind=="numeric_table"?f.table.plan.source.versionId:f.source.versionId;
    const auto arguments=f.outputArguments("outputs-compute","valid",kind);
    const auto computed=f.call(QLatin1String(python::GeneratedOutputsTool),arguments);QVERIFY2(computed.result.ok,qPrintable(computed.result.code));QVERIFY(f.derived().isEmpty());
    const auto saved=f.record("outputs-compute");QCOMPARE(saved.computeCount,1);
    const auto envelope=saved.frozenRequest.value("plan").toObject();QCOMPARE(envelope.value("environmentIdentityId").toString(),identity.id);
    const auto plan=envelope.value("plan").toObject();QCOMPARE(plan.value("schema").toString(),QString("paleo.python-generated-outputs-operation.v1"));
    QCOMPARE(plan.value("inputKind").toString(),kind);QCOMPARE(saved.stagePlan.value("parents").toArray(),QJsonArray{parent});
    QCOMPARE(plan.value("codeSha256").toString(),QString::fromLatin1(QCryptographicHash::hash(sourceCode("valid",kind),QCryptographicHash::Sha256).toHex()));
    QCOMPARE(plan.value("runtimeManifestSha256").toString(),identity.runtimeSha256);
    const auto profile=plan.value("outputProfile").toObject();QCOMPARE(profile.value("pngEncoder").toObject(),python::pngEncoderIdentity());
    QCOMPARE(profile.value("table").toObject(),tableDeclaration(kind));QCOMPARE(profile.value("image").toObject(),imageDeclaration(kind));
    const auto reportBytes=f.bytesFor(saved.operationId,"REPORT");const auto csv=f.bytesFor(saved.operationId,"TABLE");const auto png=f.bytesFor(saved.operationId,"IMAGE");
    QVERIFY2(!reportBytes.isEmpty()&&!csv.isEmpty()&&!png.isEmpty(),qPrintable(f.error));QCOMPARE(csv,expectedCsv(kind));checkPng(png,kind);
    const auto report=QJsonDocument::fromJson(reportBytes).object();const auto metadata=plan.value("inputMetadata").toObject();
    QCOMPARE(report.value("provenance"),metadata.value("provenance"));QCOMPARE(report.value("outputProvenance"),QJsonValue(profile));
    const auto provenance=metadata.value("provenance").toObject(),io=provenance.value("readMetrics").toObject();
    if(kind=="seismic_trace") {
      QCOMPARE(provenance.value("readCut"),QJsonValue(python::seismicTraceReadCutJson(f.descriptor.cut)));
      QCOMPARE(io.value("sdkRequestedSourceBytes").toString(),QString("652"));QCOMPARE(io.value("authenticatedFullHashBytes").toString(),QString::number(f.raw.size()));
    } else {QCOMPARE(io.value("cellsRead").toString(),QString("7"));QCOMPARE(io.value("typedOutputBytes").toString(),QString("76"));}
    const auto mean=report.value("metrics").toArray().first().toObject().value("value").toDouble();
    QVERIFY(qAbs(mean-(kind=="numeric_table"?2.0/3:10.0))<1e-12);
    const auto image=f.port(saved,"IMAGE").value("extra").toObject();QCOMPARE(image.value("spatial"),QJsonValue(false));
    QCOMPARE(image.value("inputProvenance"),metadata.value("provenance"));QCOMPARE(image.value("codeSha256"),plan.value("codeSha256"));
    QCOMPARE(image.value("runtimeManifestSha256").toString(),identity.runtimeSha256);
    QCOMPARE(image.value("provenance").toObject().value("encoder"),profile.value("pngEncoder"));
    QCOMPARE(image.value("provenance").toObject().value("sourceRgbaSha256").toString(),
      QString::fromLatin1(QCryptographicHash::hash(expectedRgba(kind),QCryptographicHash::Sha256).toHex()));
    for(const auto &id:QStringList{"REPORT","TABLE","IMAGE"}) {
      const auto bytes=id=="REPORT"?reportBytes:id=="TABLE"?csv:png;
      QCOMPARE(f.port(saved,id).value("sha256").toString(),QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()));
    }
    QVERIFY(f.counts->ownerOnly.load());QVERIFY(f.counts->workerOffOwner.load());const auto preparations=f.counts->materializations.load(),workers=f.counts->workerInvocations.load();
    const auto replay=f.call(QLatin1String(python::GeneratedOutputsTool),arguments);QVERIFY(replay.result.ok);QCOMPARE(replay.result.operationId,saved.operationId);
    QCOMPARE(f.counts->materializations.load(),preparations);QCOMPARE(f.counts->workerInvocations.load(),workers);
    auto changed=arguments;auto parameters=changed.value("parameters").toObject();auto imageDeclaration=parameters.value("output_image").toObject();
    imageDeclaration["width"]=2;parameters["output_image"]=imageDeclaration;changed["parameters"]=parameters;
    QCOMPARE(f.call(QLatin1String(python::GeneratedOutputsTool),changed).result.code,QString("idempotency_conflict"));
    const auto originalPath=kind=="numeric_table"?f.table.plan.source.path:f.source.path;
    QVERIFY(QFile::remove(originalPath));
    QVERIFY2(f.restartWithAnotherActivePolicy(),qPrintable(f.error));f.configureOutputs();
    const auto historical=f.registry->resolve(identity.id,identity.runtimeSha256,identity.policy.json(),python::EnvironmentMode::Generated);
    QVERIFY2(historical.ok(),qPrintable(historical.code));QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    const auto publication=f.publication("outputs-publish",saved.operationId);const auto published=f.call(QLatin1String(python::GeneratedOutputsTool),publication);
    QVERIFY2(published.result.ok,qPrintable(published.result.code));QCOMPARE(published.result.commitState,QString("committed"));QCOMPARE(f.derived().size(),3);
    for(const auto &version:f.derived())QCOMPARE(version.parentVersionIds,QStringList{parent});
    QCOMPARE(f.record("outputs-publish").computeCount,0);QCOMPARE(f.record("outputs-compute").computeCount,1);
    QCOMPARE(f.counts->workerInvocations.load(),workers);QCOMPARE(f.counts->materializations.load(),preparations);
    QCOMPARE(f.bytesFor(saved.operationId,"REPORT"),reportBytes);QCOMPARE(f.bytesFor(saved.operationId,"TABLE"),csv);QCOMPARE(f.bytesFor(saved.operationId,"IMAGE"),png);
    QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    QVERIFY(f.call(QLatin1String(python::GeneratedOutputsTool),publication).result.ok);QCOMPARE(f.derived().size(),3);
  }
  void anyBadPortIncompleteFrameOrLatePngWriteHasNoPartialGroup_data() {
    QTest::addColumn<QByteArray>("mode");QTest::addColumn<bool>("failWrite");QTest::addColumn<QString>("expected");
    QTest::newRow("report-business-contract")<<QByteArray("bad-report")<<false<<QString();
    QTest::newRow("table-nonfinite")<<QByteArray("nonfinite-table")<<false<<QString();
    QTest::newRow("image-port-identity")<<QByteArray("wrong-image-port")<<false<<QString();
    QTest::newRow("image-byte-integrity")<<QByteArray("bad-image-sha")<<false<<QString("python_output_integrity_failed");
    QTest::newRow("incomplete-image-segment")<<QByteArray("truncated")<<false<<QString();
    QTest::newRow("png-four-MiB-limit")<<QByteArray("png-budget")<<false<<QString("python_png_budget_exceeded");
    QTest::newRow("third-private-write-failure")<<QByteArray("valid")<<true<<QString("python_output_write_failed");
  }
  void anyBadPortIncompleteFrameOrLatePngWriteHasNoPartialGroup() {
    QFETCH(QByteArray,mode);QFETCH(bool,failWrite);QFETCH(QString,expected);Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));
    QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs(failWrite);
    const auto failed=f.call(QLatin1String(python::GeneratedOutputsTool),f.outputArguments("invalid-group",mode));
    QVERIFY(!failed.result.ok);if(!expected.isEmpty())QCOMPARE(failed.result.code,expected);QVERIFY(f.derived().isEmpty());
    const auto record=f.record("invalid-group");QVERIFY(record.artifactManifest.value("diagnosticsOnly").toBool());
    for(const auto &entry:record.artifactManifest.value("ports").toArray()) {
      const auto id=entry.toObject().value("portId").toString();QVERIFY(id!="REPORT"&&id!="TABLE"&&id!="IMAGE");
    }
    for(const auto &id:QStringList{"REPORT","TABLE","IMAGE"})QVERIFY(!f.pageForScope(record.operationId,id,0).value("ok").toBool());
    const auto publication=f.call(QLatin1String(python::GeneratedOutputsTool),f.publication("invalid-publish",record.operationId));
    QVERIFY(!publication.result.ok);QVERIFY(f.derived().isEmpty());
  }
  void changedPngFailsWarmAuthenticationAndBlocksTheWholePublication() {
    Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureOutputs();
    const auto computed=f.call(QLatin1String(python::GeneratedOutputsTool),f.outputArguments("png-integrity"));
    QVERIFY2(computed.result.ok,qPrintable(computed.result.code));const auto record=f.record("png-integrity");
    QVERIFY(f.pageForScope(record.operationId,"IMAGE",0).value("ok").toBool());
    const auto entry=f.port(record,"IMAGE");QVERIFY(!entry.value("path").toString().isEmpty());
    QFile file(QDir(f.project).filePath(entry.value("path").toString()));QVERIFY(file.open(QIODevice::ReadWrite));
    const auto modified=file.fileTime(QFileDevice::FileModificationTime);QCOMPARE(file.write("!"),qint64(1));
    QVERIFY(file.setFileTime(modified,QFileDevice::FileModificationTime));file.close();
    const auto changed=f.pageForScope(record.operationId,"IMAGE",1);QVERIFY(!changed.value("ok").toBool());QVERIFY(changed.value("data").toString().isEmpty());
    const auto publish=f.call(QLatin1String(python::GeneratedOutputsTool),f.publication("changed-png-publication",record.operationId));
    QVERIFY(!publish.result.ok);QVERIFY(f.derived().isEmpty());QCOMPARE(f.record("png-integrity").computeCount,1);QCOMPARE(f.counts->workerInvocations.load(),1);
  }
};
QTEST_GUILESS_MAIN(TestPythonGeneratedOutputsNative)
#include "tst_pythongeneratedoutputs_native.moc"
