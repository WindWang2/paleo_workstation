// 层：测试壳
#include "services/pythonanalysis.h"
#include <paleo_data/service.h>
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

using namespace paleo;
namespace {
QByteArray script(python::AnalysisKind kind) {
  const auto root=qEnvironmentVariable("PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES",QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES));
  QFile file(root+'/'+python::analysisKindId(kind)+".py");
  return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
struct Fixture {
  QTemporaryDir temporary;
  QString root;
  data::DataService service;
  python::AnalysisReadPlan plan;
  QString error;
  bool open(int samples=65536,bool declared=true,int sourceMessageBytes=0) {
    root=temporary.path()+"/project";std::string detail;
    if(!service.createProject(root.toUtf8().toStdString(),"Python bounded analysis",detail))return false;
    data::ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";
    parsed.message.assign(size_t(sourceMessageBytes),'x');
    data::WellDataset well;well.wellInfo.wellName="ANALYSIS-1";
    data::LogSet log;log.logSetName="Declared shared axis";log.depthInfo={"MD","m","KB","","",""};
    if(!declared)log.depthInfo.depthType.clear();
    data::LogCurve first,second;first.curveName="A";first.curveUnit="API";second.curveName="B";second.curveUnit="m/s";
    for(int i=0;i<samples;++i) {
      log.samplingAxis.depthValues.push_back(100+0.25*i);
      first.sampleValues.push_back(double(i));second.sampleValues.push_back(3.0*i+7.0);
    }
    log.logCurves={first,second};well.wellbore.logSets={log};parsed.wells={well};
    const QByteArray bytes="Typed SDK analysis fixture\n";QFile source(temporary.path()+(declared?"/source.data":"/source.las"));
    if(!source.open(QIODevice::WriteOnly) || source.write(bytes)!=bytes.size())return false;source.close();
    qlonglong sourceId=0;const auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    if(!service.importPreparedFile(source.fileName().toUtf8().toStdString(),source.fileName().toUtf8().toStdString(),
        source.fileName().toUtf8().toStdString(),digest.toStdString(),parsed,sourceId,detail)) {error=QString::fromStdString(detail);return false;}
    std::vector<data::DataEntry> entries;
    if(!service.listData("curves",0,entries,detail) || entries.size()!=2)return false;
    plan.projectDir=root;plan.modelDirectory=root;plan.documentId="doc";plan.callerScope="agent:test";
    plan.kind=python::AnalysisKind::AttributeCorrelation;
    if(!service.readModelChangeHead(plan.expectedHead,detail) || !plan.expectedHead.available){error=QString::fromStdString(detail);return false;}
    for(const auto &entry:entries) {
      python::AnalysisCurveSelection selection;
      selection.source.ref={plan.documentId,"area-model",sourceId,QString::fromLatin1(digest)};
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
using NumericRows=QVector<QVector<double>>;
using QualityRows=QVector<QVector<int>>;
void appendDouble(QByteArray &bytes,double value) {
  static_assert(sizeof(double)==sizeof(quint64));quint64 bits=0;std::memcpy(&bits,&value,sizeof(bits));
  const auto offset=bytes.size();bytes.resize(offset+8);qToLittleEndian(bits,bytes.data()+offset);
}
QByteArray referenceFrame(python::AnalysisKind kind,const NumericRows &values,const QualityRows &quality={},
    QVector<double> axis={},int columns=0,const std::function<void(QJsonObject &)> &change={}) {
  const int rows=int(values.size());if(!columns)columns=rows?int(values.first().size()):2;
  if(axis.isEmpty())for(int row=0;row<rows;++row)axis.append(100+row*0.5);
  QByteArray payload;
  for(const auto value:axis)appendDouble(payload,value);
  for(const auto &row:values)for(const auto value:row)appendDouble(payload,value);
  for(int row=0;row<rows;++row)for(int column=0;column<columns;++column)
    payload.append(char(quality.isEmpty()?0:quality.at(row).at(column)));
  QJsonArray channels;for(int column=0;column<columns;++column)
    channels.append(QJsonObject{{"curveId",QString::number(column+1)},{"name","curve-"+QString::number(column)},{"unit","API"}});
  const QJsonObject declaredAxis{{"kind","MD"},{"unit","m"},{"reference","KB"},{"alignment","exact_shared_axis_v1"}};
  const QJsonObject sampling{{"method","all_selected_window_v1"},{"seed",0},{"count",rows},
    {"scope","selected_window_only"},{"random",false},{"extrapolation",false}};
  QJsonObject metadata{{"contract","paleo.python-analysis-input.v1"},{"analysis",python::analysisKindId(kind)},
    {"inputProtocol","paleo.python-analysis-frame.v1"},{"sourceKind","well_curve_numeric_attributes"},{"dtype","<f8"},
    {"shape",QJsonArray{rows,columns}},{"axis",declaredAxis},{"columns",channels},
    {"layout",QJsonObject{{"axisBytes",rows*8},{"valueBytes",rows*columns*8},{"qualityBytes",rows*columns},
      {"order","axis_then_row_major_values_then_row_major_quality"}}},
    {"payload",QJsonObject{{"bytes",payload.size()},{"sha256",QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex())}}},
    {"provenance",QJsonObject{{"documentId","fixture"},{"callerScope","agent:test"},{"sampling",sampling},{"axis",declaredAxis}}}};
  if(change)change(metadata);
  auto provenance=metadata.value("provenance").toObject();provenance["axis"]=metadata.value("axis");metadata["provenance"]=provenance;
  const auto header=QJsonDocument(metadata).toJson(QJsonDocument::Compact);
  QByteArray frame(4,Qt::Uninitialized);qToLittleEndian(quint32(header.size()),frame.data());return frame+header+payload;
}
struct TemplateRunner {
  std::shared_ptr<python::RuntimeProvider> provider;
  std::shared_ptr<const python::VerifiedRuntime> runtime;
  QString error;
  bool open(const QString &root) {
    provider=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{
      root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true},policy());
    const auto identity=provider->inspectIntegrity();error=identity.code;runtime=identity.runtime;return identity.ready();
  }
  bool execute(python::AnalysisKind kind,const QByteArray &input,QJsonObject *report) {
    const auto templates=qEnvironmentVariable("PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES",QString::fromUtf8(PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES));
    python::TrustedProcessRequest request;request.runtime=runtime;request.policy=policy();
    request.policy.outputBytes=64*1024;request.policy.wallSeconds=15;
    request.scriptPath=templates+'/'+python::analysisKindId(kind)+".py";request.scriptSha256=python::analysisTemplateSha256(kind);
    request.permit=python::claimExecutionPermit();request.input=input;
    const auto result=python::runTrustedProcess(request);error=result.code+": "+QString::fromUtf8(result.error);
    if(!report)return result.code=="python_process_failed" && result.exitCode>0 && result.output.isEmpty() && result.outputLog.isEmpty();
    if(!result.ok() || result.output.size()>=64*1024)return false;
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(result.output,&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isObject())return false;
    *report=document.object().value("ports").toObject().value("REPORT").toObject();return !report->isEmpty();
  }
};
}
class PythonAnalysisTests:public QObject {
  Q_OBJECT
private slots:
  void lockedTemplatesMatchIndependentReferences_data() {
    QTest::addColumn<QString>("reference");
    for(const auto name:{"quality","pearson","complete-rows","empty-and-missing","large-magnitude","uppercase-unit","invalid-frames","maximum-window"})
      QTest::newRow(name)<<QString::fromLatin1(name);
  }
  void lockedTemplatesMatchIndependentReferences() {
    QFETCH(QString,reference);const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if(root.isEmpty()) {
      QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")!="1","Required real Python fixture missing");
      QVERIFY(!python::RuntimeProvider({}).inspectIntegrity().ready());return;
    }
    TemplateRunner runner;QVERIFY2(runner.open(root),qPrintable(runner.error));
    if(reference=="quality")qualityReference(runner);
    else if(reference=="pearson")pearsonReference(runner);
    else if(reference=="complete-rows")completeRowsReference(runner);
    else if(reference=="empty-and-missing")emptyAndMissingReference(runner);
    else if(reference=="large-magnitude")largeMagnitudeReference(runner);
    else if(reference=="uppercase-unit")uppercaseUnitReference(runner);
    else if(reference=="invalid-frames")invalidFramesReference(runner);
    else if(reference=="maximum-window")maximumWindowReference(runner);
    else QFAIL("Unknown independent template reference");
  }
  void boundedSdkWindowsCarryIdentityAndAuthenticateTransport() {
    Fixture fixture;QVERIFY2(fixture.open(),qPrintable(fixture.error));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QVERIFY2(result.ok(),qPrintable(result.code));QCOMPARE(result.io.legacyBytes,0);
    QCOMPARE(result.io.sourceRows,1);QVERIFY(result.io.metadataTextBytes>0);
    qInfo().noquote()<<"PY2_IO sourceSamplesPerCurve=65536 selectedSamplesPerCurve=8192 channels=2 payloadBytes="
      <<result.io.payloadBytes<<" axisBlocks="<<result.io.axisBlocks<<" valueBlocks="<<result.io.valueBlocks
      <<" legacyBytes="<<result.io.legacyBytes;
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
    QVERIFY(!result.metadata.value("provenance").toObject().value("readMetrics").toObject().contains("hostNumericPreparationReservedBytes"));
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
  void generatedNumericPreparationRejectsBeforeSdkIo() {
    Fixture fixture;QVERIFY(fixture.open(1024));fixture.plan.kind=python::AnalysisKind::Generated;
    auto limits=policy();limits.allowTrusted=false;limits.allowGenerated=true;limits.memoryBytes=1;
    const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,"print('generated')\n",limits,permit);
    QCOMPARE(result.code,QString("python_input_memory_budget"));QVERIFY(result.bundle.reference.isEmpty());QVERIFY(result.metadata.isEmpty());
    QCOMPARE(result.io.payloadBytes,0);QCOMPARE(result.io.legacyBytes,0);QCOMPARE(result.io.axisBlocks,0);
    QCOMPARE(result.io.valueBlocks,0);QCOMPARE(result.io.windows,0);QCOMPARE(result.io.metadataRows,0);
    QCOMPARE(result.io.metadataTextBytes,0);QCOMPARE(result.io.sourceRows,0);
    QVERIFY(!QFileInfo::exists(fixture.root+"/.paleo/capability-ops/inputs"));
    limits.allowTrusted=true;limits.allowGenerated=false;
    const auto denied=python::materializeAnalysis(fixture.plan,"print('generated')\n",limits,permit);
    QCOMPARE(denied.code,QString("python_execution_denied"));QCOMPARE(denied.io.windows,0);QCOMPARE(denied.io.metadataRows,0);
  }
  void generatedNumericReservationIsExplicitAndAdmitsItsExactBoundary() {
    Fixture fixture;QVERIFY(fixture.open(1024));fixture.plan.kind=python::AnalysisKind::Generated;
    auto limits=policy();limits.allowTrusted=false;limits.allowGenerated=true;
    const auto permit=python::claimExecutionPermit();const QByteArray source="print('generated')\n";
    const auto result=python::materializeAnalysis(fixture.plan,source,limits,permit);QVERIFY2(result.ok(),qPrintable(result.code));
    const auto metrics=result.metadata.value("provenance").toObject().value("readMetrics").toObject();
    const auto reserved=metrics.value("hostNumericPreparationReservedBytes").toString().toLongLong();
    QCOMPARE(metrics.value("hostNumericPreparationMeasurement").toString(),QString("reservation_not_process_rss"));
    QVERIFY(reserved>result.io.payloadBytes);QVERIFY(reserved<limits.memoryBytes);QCOMPARE(result.io.windows,2);
    QCOMPARE(result.bundle.manifest.value("metadata").toObject().value("provenance"),result.metadata.value("provenance"));
    limits.memoryBytes=reserved-1;const auto rejected=python::materializeAnalysis(fixture.plan,source,limits,permit);
    QCOMPARE(rejected.code,QString("python_input_memory_budget"));QCOMPARE(rejected.io.windows,0);QCOMPARE(rejected.io.metadataRows,0);
    limits.memoryBytes=reserved;const auto exact=python::materializeAnalysis(fixture.plan,source,limits,permit);
    QVERIFY2(exact.ok(),qPrintable(exact.code));QCOMPARE(exact.metadata,result.metadata);
  }
  void oversizedSourceTextIsRejectedBeforeArraysOrBundle() {
    Fixture fixture;QVERIFY2(fixture.open(1024,true,32768),qPrintable(fixture.error));QVERIFY(fixture.plan.expectedHead.available);
    const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QCOMPARE(result.code,QString("python_analysis_read_budget"));QCOMPARE(result.io.sourceRows,0);
    QCOMPARE(result.io.payloadBytes,0);QCOMPARE(result.io.legacyBytes,0);QVERIFY(result.bundle.reference.isEmpty());
  }
  void cachedSourceIdentityStillRejectsAnotherRevision() {
    Fixture fixture;QVERIFY(fixture.open());auto &source=fixture.plan.curves[1].source;
    source.ref.fileRevisionId=QString(64,'a');source.fingerprint=document::SourceFingerprint::fromStored(source.ref.fileRevisionId);
    const auto permit=python::claimExecutionPermit();const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QCOMPARE(result.code,QString("python_analysis_source_changed"));QCOMPARE(result.io.sourceRows,1);QVERIFY(result.bundle.reference.isEmpty());
  }
  void modelChangesDuringCaptureRejectTheEntireInput() {
    Fixture fixture;QVERIFY(fixture.open());const auto permit=python::claimExecutionPermit();
    int checks=0;bool changed=false;
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit,[&] {
      if(++checks==2) {
        std::string error;changed=fixture.service.updateCurveValue(fixture.plan.curves.first().curveId,101,999.0,error);
      }
      return false;
    });
    QVERIFY(changed);QCOMPARE(result.code,QString("python_analysis_model_changed"));QVERIFY(result.bundle.reference.isEmpty());
    QVERIFY(result.metadata.isEmpty());
  }
  void inferredLegacyDepthTypeCannotMasqueradeAsDeclaredMd() {
    Fixture fixture;QVERIFY(fixture.open(1024,false));const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QCOMPARE(result.code,QString("python_analysis_semantics_inferred"));QVERIFY(result.bundle.reference.isEmpty());
  }
  void missingChunkStorageNeverMaterializesLegacyArrays() {
    Fixture fixture;QVERIFY(fixture.open());const auto name=QUuid::createUuid().toString();bool removed=false;
    {
      auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(fixture.root+"/data.sqlite");
      QVERIFY(db.open());QSqlQuery query(db);
      removed=query.exec("DELETE FROM curve_window_curves_v1");db.close();
    }
    QSqlDatabase::removeDatabase(name);QVERIFY(removed);const auto permit=python::claimExecutionPermit();
    const auto result=python::materializeAnalysis(fixture.plan,script(fixture.plan.kind),policy(),permit);
    QCOMPARE(result.code,QString("python_analysis_chunk_storage_required"));QCOMPARE(result.io.legacyBytes,0);
    QCOMPARE(result.io.payloadBytes,0);QVERIFY(result.bundle.reference.isEmpty());
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
    qInfo().noquote()<<"PY2_PROCESS cpuMicros="<<run.cpuMicros<<" observedPeakMemoryBytes="<<run.peakMemoryBytes
      <<" memoryPeakComplete="<<run.memoryPeakComplete;
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
private:
  void qualityReference(TemplateRunner &runner) {
    const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();QJsonObject report;
    QVERIFY2(runner.execute(python::AnalysisKind::WellQuality,referenceFrame(python::AnalysisKind::WellQuality,
      {{1,5},{2,5},{100,5},{4,5},{nan,5},{inf,5},{7,5}},{{0,0},{4,0},{2,0},{1,0},{0,0},{0,0},{0,0}}),&report),qPrintable(runner.error));
    const auto metrics=report.value("metrics").toArray();QCOMPARE(metrics.size(),2);
    const auto first=metrics.at(0).toObject(),second=metrics.at(1).toObject();
    const QVector<double> accepted{1,2,7};const auto mean=std::accumulate(accepted.begin(),accepted.end(),0.0)/accepted.size();
    double squared=0;for(const auto value:accepted)squared+=(value-mean)*(value-mean);
    QCOMPARE(first.value("validCount").toInt(),3);QVERIFY(qAbs(first.value("mean").toDouble()-mean)<1e-13);
    QVERIFY(qAbs(first.value("populationStdDev").toDouble()-std::sqrt(squared/accepted.size()))<1e-13);
    QCOMPARE(first.value("missingCount").toInt(),3);QCOMPARE(first.value("sdkAbnormalCount").toInt(),1);
    QCOMPARE(first.value("editedCount").toInt(),1);QCOMPARE(first.value("nonFiniteCount").toInt(),2);
    QCOMPARE(second.value("status").toString(),QString("constant"));QCOMPARE(second.value("populationStdDev").toDouble(),0.0);
  }
  void pearsonReference(TemplateRunner &runner) {
    QJsonObject report;const NumericRows values{{1,5,7},{2,4,7},{3,2,7},{4,1,7}};
    QVERIFY2(runner.execute(python::AnalysisKind::AttributeCorrelation,referenceFrame(python::AnalysisKind::AttributeCorrelation,values),&report),qPrintable(runner.error));
    double covariance=0,squaredX=0,squaredY=0;
    const auto meanX=(1.0+2+3+4)/4,meanY=(5.0+4+2+1)/4;
    for(const auto &row:values) {
      const auto x=row.at(0)-meanX,y=row.at(1)-meanY;covariance+=x*y;squaredX+=x*x;squaredY+=y*y;
    }
    const auto matrix=report.value("matrix").toArray();QCOMPARE(matrix.size(),3);
    QVERIFY(qAbs(matrix.at(0).toArray().at(1).toDouble()-covariance/std::sqrt(squaredX*squaredY))<1e-13);
    QCOMPARE(report.value("constantColumns").toArray(),QJsonArray{2});QVERIFY(matrix.at(2).toArray().at(2).isNull());
    QCOMPARE(report.value("redundantPairs").toArray().size(),1);QCOMPARE(report.value("jointValidCount").toInt(),4);
  }
  void completeRowsReference(TemplateRunner &runner) {
    const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();QJsonObject report;
    QVERIFY2(runner.execute(python::AnalysisKind::AttributeCorrelation,referenceFrame(python::AnalysisKind::AttributeCorrelation,
      {{1,2},{2,4},{nan,3},{3,6},{4,inf}},{{0,0},{0,0},{0,0},{2,0},{0,0}}),&report),qPrintable(runner.error));
    QCOMPARE(report.value("jointValidCount").toInt(),2);QCOMPARE(report.value("rejectedRows").toInt(),3);
    QVERIFY(qAbs(report.value("matrix").toArray().at(0).toArray().at(1).toDouble()-1.0)<1e-14);
  }
  void emptyAndMissingReference(TemplateRunner &runner) {
    QJsonObject report;
    for(const auto kind:{python::AnalysisKind::WellQuality,python::AnalysisKind::AttributeCorrelation}) {
      QVERIFY2(runner.execute(kind,referenceFrame(kind,{}),&report),qPrintable(runner.error));
      auto status=report.value("status").toString();
      if(status.isEmpty()) {
        const auto metrics=report.value("metrics").toArray();QVERIFY(!metrics.isEmpty());status=metrics.at(0).toObject().value("status").toString();
      }
      QCOMPARE(status,QString("empty_window"));
    }
    QVERIFY2(runner.execute(python::AnalysisKind::WellQuality,referenceFrame(python::AnalysisKind::WellQuality,
      {{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}}),&report),qPrintable(runner.error));
    for(const auto value:report.value("metrics").toArray()) {
      const auto metric=value.toObject();QCOMPARE(metric.value("status").toString(),QString("all_missing"));QVERIFY(metric.value("mean").isNull());
    }
    QVERIFY2(runner.execute(python::AnalysisKind::WellQuality,referenceFrame(python::AnalysisKind::WellQuality,{{1,2}},{{2,2}}),&report),qPrintable(runner.error));
    for(const auto value:report.value("metrics").toArray()) {
      const auto metric=value.toObject();QCOMPARE(metric.value("status").toString(),QString("no_valid_samples"));QCOMPARE(metric.value("missingCount").toInt(),0);
    }
    QVERIFY2(runner.execute(python::AnalysisKind::AttributeCorrelation,referenceFrame(python::AnalysisKind::AttributeCorrelation,{{1,2}}),&report),qPrintable(runner.error));
    QCOMPARE(report.value("status").toString(),QString("insufficient_samples"));
    for(const auto row:report.value("matrix").toArray())for(const auto value:row.toArray())QVERIFY(value.isNull());
  }
  void largeMagnitudeReference(TemplateRunner &runner) {
    const NumericRows values{{1e308,-1e308},{-1e308,1e308},{0,0}};QJsonObject report;
    QVERIFY2(runner.execute(python::AnalysisKind::WellQuality,referenceFrame(python::AnalysisKind::WellQuality,values),&report),qPrintable(runner.error));
    for(const auto value:report.value("metrics").toArray()) {
      const auto deviation=value.toObject().value("populationStdDev");QVERIFY(deviation.isDouble());QVERIFY(std::isfinite(deviation.toDouble()));
    }
    QVERIFY2(runner.execute(python::AnalysisKind::AttributeCorrelation,referenceFrame(python::AnalysisKind::AttributeCorrelation,values),&report),qPrintable(runner.error));
    QVERIFY(qAbs(report.value("matrix").toArray().at(0).toArray().at(1).toDouble()+1.0)<1e-13);
  }
  void uppercaseUnitReference(TemplateRunner &runner) {
    for(const auto kind:{python::AnalysisKind::WellQuality,python::AnalysisKind::AttributeCorrelation}) {
      QJsonObject report;const auto frame=referenceFrame(kind,{{1,2},{2,3}},{},{},0,[](QJsonObject &metadata) {
        auto axis=metadata.value("axis").toObject();axis["unit"]="M";metadata["axis"]=axis;
      });
      QVERIFY2(runner.execute(kind,frame,&report),qPrintable(runner.error));
      QCOMPARE(report.value("provenance").toObject().value("axis").toObject().value("unit").toString(),QString("M"));
    }
  }
  void invalidFramesReference(TemplateRunner &runner) {
    const QVector<std::function<void(QJsonObject &)>> changes{
      [](QJsonObject &m){m["dtype"]="object";},[](QJsonObject &m){m["shape"]=QJsonArray{8193,2};},
      [](QJsonObject &m){auto columns=m.value("columns").toArray();auto first=columns.at(0).toObject();first["unit"]="";columns[0]=first;m["columns"]=columns;},
      [](QJsonObject &m){auto axis=m.value("axis").toObject();axis["reference"]="";m["axis"]=axis;},
      [](QJsonObject &m){auto p=m.value("provenance").toObject();auto s=p.value("sampling").toObject();s["method"]="implicit_latest";p["sampling"]=s;m["provenance"]=p;},
      [](QJsonObject &m){auto payload=m.value("payload").toObject();payload["sha256"]=QString(64,'0');m["payload"]=payload;}};
    for(const auto kind:{python::AnalysisKind::WellQuality,python::AnalysisKind::AttributeCorrelation}) {
      for(const auto &change:changes)
        QVERIFY2(runner.execute(kind,referenceFrame(kind,{{1,2}},{},{},0,change),nullptr),qPrintable(runner.error));
      const auto frame=referenceFrame(kind,{{1,2}});
      QVERIFY2(runner.execute(kind,frame.first(frame.size()-1),nullptr),qPrintable(runner.error));
      QVERIFY2(runner.execute(kind,frame+"extra",nullptr),qPrintable(runner.error));
      QVERIFY2(runner.execute(kind,referenceFrame(kind,{{1,2},{3,4}},{},{1,1}),nullptr),qPrintable(runner.error));
    }
  }
  void maximumWindowReference(TemplateRunner &runner) {
    NumericRows values(8192,QVector<double>(16));
    for(int row=0;row<8192;++row)for(int column=0;column<16;++column)values[row][column]=row+column;
    QJsonObject report;QVERIFY2(runner.execute(python::AnalysisKind::AttributeCorrelation,
      referenceFrame(python::AnalysisKind::AttributeCorrelation,values),&report),qPrintable(runner.error));
    QCOMPARE(report.value("sampleCount").toInt(),8192);QCOMPARE(report.value("matrix").toArray().size(),16);
    QCOMPARE(report.value("sourceKind").toString(),QString("well_curve_numeric_attributes"));
    QCOMPARE(report.value("scope").toString(),QString("selected_window_only"));
  }
};
QTEST_GUILESS_MAIN(PythonAnalysisTests)
#include "tst_pythonanalysis.moc"
