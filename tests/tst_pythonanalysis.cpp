// 层：测试壳
#include "services/pythonanalysis.h"
#include <paleo_data/service.h>
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>

using namespace paleo;
namespace {
QByteArray script(python::AnalysisKind kind) {
  QFile file(QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES)+'/'+python::analysisKindId(kind)+".py");
  return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
struct Fixture {
  QTemporaryDir temporary;
  QString root;
  data::DataService service;
  python::AnalysisReadPlan plan;
  QString error;
  bool open(int samples=65536) {
    root=temporary.path()+"/project";std::string detail;
    if(!service.createProject(root.toUtf8().toStdString(),"Python bounded analysis",detail))return false;
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";
    data::WellDataset well;well.wellInfo.wellName="ANALYSIS-1";
    data::LogSet log;log.logSetName="Declared shared axis";log.depthInfo={"MD","m","KB","","",""};
    data::LogCurve first,second;first.curveName="A";first.curveUnit="API";second.curveName="B";second.curveUnit="m/s";
    for(int i=0;i<samples;++i) {
      log.samplingAxis.depthValues.push_back(100+0.25*i);
      first.sampleValues.push_back(double(i));second.sampleValues.push_back(3.0*i+7.0);
    }
    log.logCurves={first,second};well.wellbore.logSets={log};parsed.wells={well};
    const QByteArray bytes="Typed SDK analysis fixture\n";QFile source(temporary.path()+"/source.data");
    if(!source.open(QIODevice::WriteOnly) || source.write(bytes)!=bytes.size())return false;source.close();
    qlonglong sourceId=0;const auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    if(!service.importPreparedFile(source.fileName().toUtf8().toStdString(),source.fileName().toUtf8().toStdString(),
        source.fileName().toUtf8().toStdString(),digest.toStdString(),parsed,sourceId,detail)) {error=QString::fromStdString(detail);return false;}
    std::vector<data::DataEntry> entries;data::SourceIndexEntry registered;
    if(!service.listData("curves",0,entries,detail) || entries.size()!=2 || !service.readSourceIndexEntry(sourceId,registered,detail))return false;
    plan.projectDir=root;plan.modelDirectory=root;plan.documentId="doc";plan.callerScope="agent:test";
    plan.kind=python::AnalysisKind::AttributeCorrelation;
    if(!service.readModelChangeHead(plan.expectedHead,detail) || !plan.expectedHead.available)return false;
    for(const auto &entry:entries) {
      python::AnalysisCurveSelection selection;
      selection.source.ref={plan.documentId,"area-model",sourceId,QString::fromStdString(registered.sha256)};
      selection.source.fingerprint=document::SourceFingerprint::fromStored(selection.source.ref.fileRevisionId);
      selection.source.modelDirectory=root;selection.source.versionId="source-v1";selection.sourceVersionId="source-v1";
      selection.wellId=entry.wellId;selection.logSetId=entry.parentId;selection.curveId=entry.id;
      selection.firstSample=100;selection.sampleCount=qMin(8192,qMax(0,samples-100));
      selection.unit=entry.name=="A"?"API":"m/s";selection.depthType="MD";selection.depthUnit="m";selection.depthReference="KB";
      plan.curves.append(selection);
    }
    return true;
  }
};
python::ExecutionPolicy policy() {python::ExecutionPolicy value;value.allowTrusted=true;return value;}
}
class PythonAnalysisTests:public QObject {
  Q_OBJECT
private slots:
  void boundedSdkWindowsCarryIdentityAndAuthenticateTransport() {
    Fixture fixture;QVERIFY2(fixture.open(),qPrintable(fixture.error));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QVERIFY2(result.ok(),qPrintable(result.code));QCOMPARE(result.io.legacyBytes,0);
    QCOMPARE(result.io.windows,2);QVERIFY(result.io.payloadBytes>0);QVERIFY(result.io.payloadBytes<1024*1024);
    QVERIFY(result.io.axisBlocks<=6);QVERIFY(result.io.valueBlocks<=6);
    QCOMPARE(result.metadata.value("shape").toArray(),QJsonArray({8192,2}));
    QCOMPARE(result.bundle.manifest.value("metadata").toObject().value("inputProtocol").toString(),QString("paleo.python-analysis-frame.v1"));
    const auto input=python::loadAnalysisProcessInput(fixture.root,result.bundle.reference,"doc","agent:test",policy(),permit);
    QVERIFY2(input.ok(),qPrintable(input.code));QVERIFY(input.bytes.size()<=policy().inputBytes);
    const auto length=qFromLittleEndian<quint32>(input.bytes.constData());
    QCOMPARE(QJsonDocument::fromJson(input.bytes.mid(4,length)).object(),result.metadata);
    const auto matrix=input.bytes.mid(4+length);QCOMPARE(matrix.size(),8192*(8+9*2));
    QCOMPARE(result.metadata.value("provenance").toObject().value("sources").toArray().first().toObject().value("sourceVersionId").toString(),QString("source-v1"));
    QCOMPARE(python::loadAnalysisProcessInput(fixture.root,result.bundle.reference,"doc","other",policy(),permit).code,QString("python_input_scope_mismatch"));
    QFile changed(result.bundle.root+"/data.bin");QVERIFY(changed.open(QIODevice::ReadWrite));QVERIFY(changed.seek(12));QCOMPARE(changed.write("x"),1);changed.close();
    QVERIFY(!python::loadAnalysisProcessInput(fixture.root,result.bundle.reference,"doc","agent:test",policy(),permit).ok());
  }
  void semanticAndSnapshotDenials_data() {
    QTest::addColumn<QString>("change");QTest::addColumn<QString>("code");
    QTest::newRow("wrong-unit")<<QString("unit")<<QString("python_analysis_semantics_changed");
    QTest::newRow("wrong-axis")<<QString("axis")<<QString("python_analysis_axis_mismatch");
    QTest::newRow("old-head")<<QString("head")<<QString("python_analysis_model_changed");
    QTest::newRow("wrong-document")<<QString("document")<<QString("python_analysis_source_mismatch");
    QTest::newRow("missing-unit")<<QString("missing-unit")<<QString("python_analysis_semantics_required");
    QTest::newRow("oversized-window")<<QString("oversized")<<QString("python_analysis_selection_invalid");
    QTest::newRow("read-budget")<<QString("budget")<<QString("python_analysis_read_budget");
  }
  void semanticAndSnapshotDenials() {
    QFETCH(QString,change);QFETCH(QString,code);Fixture fixture;QVERIFY(fixture.open());
    if(change=="unit")fixture.plan.curves[0].unit="ohm";
    if(change=="axis")++fixture.plan.curves[1].firstSample;
    if(change=="head")++fixture.plan.expectedHead.sequence;
    if(change=="document")fixture.plan.documentId="other";
    if(change=="missing-unit")fixture.plan.curves[0].unit.clear();
    if(change=="oversized")fixture.plan.curves[0].sampleCount=8193;
    if(change=="budget")fixture.plan.readByteBudget=1;
    const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QCOMPARE(result.code,code);QVERIFY(result.bundle.reference.isEmpty());QVERIFY(result.metadata.isEmpty());
  }
  void rejectsMissingPermitAndChangedTemplateBeforeSdkRead() {
    Fixture fixture;QVERIFY(fixture.open());
    QCOMPARE(python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),{}).code,QString("python_execution_permit_required"));
    const auto permit=python::claimExecutionPermit();
    const auto changed=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind)+"\n# changed",policy(),permit);
    QCOMPARE(changed.code,QString("python_template_changed"));QCOMPARE(changed.io.payloadBytes,0);
    const auto cancelled=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit,[]{return true;});
    QCOMPARE(cancelled.code,QString("cancelled"));QCOMPARE(cancelled.io.payloadBytes,0);
  }
  void actualLockedTemplateMatchesIndependentLinearReference() {
    const auto runtimeRoot=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if(runtimeRoot.isEmpty()){QVERIFY(!python::RuntimeProvider({}).inspect().ready());return;}
    Fixture fixture;QVERIFY(fixture.open(1024));for(auto &curve:fixture.plan.curves)curve.sampleCount=100;
    auto provider=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{runtimeRoot,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true},policy());
    const auto runtime=provider->inspect();QVERIFY2(runtime.ready(),qPrintable(runtime.code));
    const auto permit=python::claimExecutionPermit();
    const auto materialized=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);QVERIFY2(materialized.ok(),qPrintable(materialized.code));
    const auto input=python::loadAnalysisProcessInput(fixture.root,materialized.bundle.reference,"doc","agent:test",policy(),permit);QVERIFY(input.ok());
    python::TrustedProcessRequest request;request.runtime=runtime.runtime;request.scriptPath=materialized.bundle.root+"/code.py";
    request.scriptSha256=python::analysisTemplateSha256(fixture.plan.kind);request.input=input.bytes;request.policy=policy();request.permit=permit;
    const auto run=python::runTrustedProcess(request);QVERIFY2(run.ok(),qPrintable(run.code+QString::fromUtf8(run.error)));
    const auto report=QJsonDocument::fromJson(run.output).object().value("ports").toObject().value("REPORT").toObject();
    QCOMPARE(report.value("jointValidCount").toInt(),100);QVERIFY(qAbs(report.value("matrix").toArray()[0].toArray()[1].toDouble()-1.0)<1e-12);
    QCOMPARE(report.value("sourceKind").toString(),QString("well_curve_numeric_attributes"));
    fixture.plan.kind=python::AnalysisKind::WellQuality;
    const auto quality=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);QVERIFY(quality.ok());
    const auto qualityInput=python::loadAnalysisProcessInput(fixture.root,quality.bundle.reference,"doc","agent:test",policy(),permit);QVERIFY(qualityInput.ok());
    request.scriptPath=quality.bundle.root+"/code.py";request.scriptSha256=python::analysisTemplateSha256(fixture.plan.kind);request.input=qualityInput.bytes;
    const auto qualityRun=python::runTrustedProcess(request);QVERIFY2(qualityRun.ok(),qPrintable(qualityRun.code));
    const auto metric=QJsonDocument::fromJson(qualityRun.output).object().value("ports").toObject().value("REPORT").toObject().value("metrics").toArray().first().toObject();
    QCOMPARE(metric.value("validCount").toInt(),100);QCOMPARE(metric.value("mean").toDouble(),149.5);
    QCOMPARE(metric.value("minimum").toDouble(),100.0);QCOMPARE(metric.value("maximum").toDouble(),199.0);
  }
};
QTEST_GUILESS_MAIN(PythonAnalysisTests)
#include "tst_pythonanalysis.moc"
