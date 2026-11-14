// 层：测试壳
#include "services/pythongenerated.h"
#include "services/pythonanalysisselection.h"
#include "services/pythonoutputjson.h"
#include "fixtures/pythonanalysisreportfixture.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>

using namespace paleo;
using namespace paleo::python;
namespace {
QJsonObject report() {
  return {{"contract","paleo.python-generated-report.v1"},
    {"metrics",QJsonArray{QJsonObject{{"id","mean"},{"unit","API"},{"value",1.5}}}},
    {"tables",QJsonArray{}},{"notes",QJsonArray{"Selected input window only."}}};
}
QByteArray envelope(const QJsonObject &value) {
  return QJsonDocument(QJsonObject{{"contract","paleo.python-output.v1"},{"ports",QJsonObject{{"REPORT",value}}}}).toJson(QJsonDocument::Compact);
}
QJsonObject selection() {
  document::WellRef well;well.documentId="doc";well.catalogEntityId="well";well.modelStoreId="area-model";
  well.modelStoreIdentity="model";well.modelGeneration="generation";well.modelWellId=1;well.queryHead=1;well.modelAvailable=true;
  return {{"well_ref",document::wellRefJson(well)},{"curves",QJsonArray{QJsonObject{
    {"source_ref",document::sourceRefJson({"doc","area-model",7,QString(64,'a')})},{"source_version_id","source-v1"},
    {"well_id","1"},{"log_set_id","2"},{"curve_id","1"},{"unit","API"},{"depth_type","MD"},{"depth_unit","m"},
    {"depth_reference","KB"},{"total_samples","8192"}}}},{"first_sample","0"},{"sample_count",4},
    {"processed",true},{"alignment","exact_shared_axis_v1"},{"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};
}
QJsonObject arguments() {
  return {{"client_request_id","generated-request"},{"binding_mode","pinned-value"},
    {"parameters",QJsonObject{{"code","print('report')\n"}}},{"inputs",selection()}};
}
}
class TestPythonGenerated:public QObject {
  Q_OBJECT
private slots:
  void generatedCodeHasItsOwnAdmissionAndTrustIdentity() {
    ExecutionPolicy policy;policy.allowTrusted=true;QCOMPARE(validateGeneratedCode("print(1)",policy),QString("python_execution_denied"));
    policy.allowTrusted=false;policy.allowGenerated=true;QVERIFY(validateGeneratedCode("print(1)\n",policy).isEmpty());
    QVERIFY(validateGeneratedCode(QByteArray(GeneratedCodeBytes,'#'),policy).isEmpty());
    QCOMPARE(validateGeneratedCode(QByteArray(GeneratedCodeBytes+1,'#'),policy),QString("python_code_budget_exceeded"));
    QCOMPARE(validateGeneratedCode(QByteArray("\xff",1),policy),QString("python_code_encoding_invalid"));
    QCOMPARE(validateGeneratedCode(QByteArray("x\0y",3),policy),QString("python_code_encoding_invalid"));
    for(const auto &tail:{QByteArray("\xc3",1),QByteArray("\xe2\x82",2),QByteArray("\xf0\x9f\x92",3)})
      QCOMPARE(validateGeneratedCode("#x"+tail,policy),QString("python_code_encoding_invalid"));
    QCOMPARE(validateGeneratedCode(QByteArray("\xef\xbb\xbfprint(1)"),policy),QString("python_code_encoding_invalid"));
    policy.codeBytes=1;QCOMPARE(validateGeneratedCode("##",policy),QString("python_code_budget_exceeded"));
    QCOMPARE(analysisTemplateSha256(AnalysisKind::Generated),QString());
  }
  void schemaAndValidatorRejectAuthorityAndPhaseOverrides() {
    auto args=arguments();QVERIFY2(validateGeneratedToolArguments(args).isEmpty(),qPrintable(validateGeneratedToolArguments(args)));
    const auto schema=generatedToolRequestSchema();QCOMPARE(schema.value("additionalProperties"),QJsonValue(false));
    for(const auto &field:{"argv","runtime_root","policy","effects","interpreter","code"}) {
      auto bad=args;bad[QLatin1String(field)]="override";QVERIFY(!validateGeneratedToolArguments(bad).isEmpty());
    }
    auto bad=args;bad["parameters"]=QJsonObject{{"code","print(1)"},{"argv",QJsonArray{}}};QVERIFY(!validateGeneratedToolArguments(bad).isEmpty());
    bad=args;bad["publish_intent"]="publish";QVERIFY(!validateGeneratedToolArguments(bad).isEmpty());
    bad["parameters"]=QJsonObject{};bad["inputs"]=QJsonObject{{"source_operation_id","original-operation"}};
    QVERIFY(validateGeneratedToolArguments(bad).isEmpty());
    bad=args;bad["parameters"]=QJsonObject{{"code",QString(QChar(0xd800))}};QVERIFY(!validateGeneratedToolArguments(bad).isEmpty());
    bad=args;auto inputs=selection();inputs["seed"]=7;bad["inputs"]=inputs;QVERIFY(!validateGeneratedToolArguments(bad).isEmpty());
  }
  void hostOwnsProvenanceAndReportsGeneratedClaims() {
    const auto metadata=report_fixture::metadata(AnalysisKind::Generated,4,1);
    QVERIFY(validateAnalysisInputMetadata(metadata,AnalysisKind::Generated));
    const auto result=validateGeneratedReport(envelope(report()),metadata,QString(64,'a'),QString(64,'b'),{});
    QVERIFY2(result.ok(),qPrintable(result.code));const auto accepted=QJsonDocument::fromJson(result.bytes).object();
    QCOMPARE(accepted.value("provenance"),metadata.value("provenance"));QCOMPARE(accepted.value("executionMode").toString(),QString("generated"));
    QCOMPARE(accepted.value("inputColumns"),metadata.value("columns"));QVERIFY(!result.summary.contains("provenance"));
    QVERIFY(!result.summary.contains("code"));QVERIFY(!result.summary.contains("notesText"));
    QVERIFY(!validateAnalysisReport(envelope(report()),AnalysisKind::Generated,metadata,{}).ok());
    auto forged=report();forged["provenance"]=QJsonObject{};
    QVERIFY(!validateGeneratedReport(envelope(forged),metadata,QString(64,'a'),QString(64,'b'),{}).ok());
  }
  void rejectsMalformedOrUnboundedReports_data() {
    QTest::addColumn<QJsonObject>("value");
    auto add=[](const char *name,const std::function<void(QJsonObject&)> &edit){auto v=report();edit(v);QTest::newRow(name)<<v;};
    add("trusted-contract",[](auto &v){v["contract"]="paleo.well-quality-report.v1";});
    add("code-identity",[](auto &v){v["codeSha256"]=QString(64,'a');});
    add("path",[](auto &v){v["path"]="/host/file";});
    add("metric-string",[](auto &v){v["metrics"]=QJsonArray{QJsonObject{{"id","mean"},{"unit","API"},{"value","1.5"}}};});
    add("duplicate-metric",[](auto &v){auto a=v["metrics"].toArray();a.append(a[0]);v["metrics"]=a;});
    add("missing-unit",[](auto &v){v["metrics"]=QJsonArray{QJsonObject{{"id","mean"},{"unit",""},{"value",1}}};});
    add("unknown-unit-type",[](auto &v){v["metrics"]=QJsonArray{QJsonObject{{"id","mean"},{"unit",true},{"value",1}}};});
    add("notes-too-long",[](auto &v){v["notes"]=QJsonArray{QString(2049,'x')};});
    add("too-many-notes",[](auto &v){QJsonArray a;for(int i=0;i<17;++i)a.append("note");v["notes"]=a;});
    add("ragged-table",[](auto &v){v["tables"]=QJsonArray{QJsonObject{{"id","table"},{"columns",QJsonArray{QJsonObject{{"id","a"},{"unit","API"},{"type","number"}}}},{"rows",QJsonArray{QJsonArray{1,2}}}}};});
    add("wrong-cell-type",[](auto &v){v["tables"]=QJsonArray{QJsonObject{{"id","table"},{"columns",QJsonArray{QJsonObject{{"id","a"},{"unit","API"},{"type","number"}}}},{"rows",QJsonArray{QJsonArray{"1"}}}}};});
  }
  void rejectsMalformedOrUnboundedReports() {
    QFETCH(QJsonObject,value);
    const auto result=validateGeneratedReport(envelope(value),report_fixture::metadata(AnalysisKind::Generated,4,1),QString(64,'a'),QString(64,'b'),{});
    QVERIFY(!result.ok());QVERIFY(result.bytes.isEmpty());
  }
  void strictJsonDoesNotCollapseEscapedDuplicateKeys() {
    QJsonObject object;
    QCOMPARE(parsePythonOutputObject(R"({"REPORT":{},"\u0052EPORT":{}})",1024,128,object),QString("python_output_invalid"));
    QCOMPARE(parsePythonOutputObject(QByteArray("{\"x\":\"\xff\"}",9),1024,128,object),QString("python_output_encoding_invalid"));
    QCOMPARE(parsePythonOutputObject("{\"x\":[1,2,3]}",1024,3,object),QString("python_output_invalid"));
    QVERIFY(parsePythonOutputObject("{\"x\":[1,2,3]}",1024,128,object).isEmpty());
    for(const auto &tail:{QByteArray("\xc3",1),QByteArray("\xe2\x82",2),QByteArray("\xf0\x9f\x92",3)})
      QCOMPARE(parsePythonOutputObject("{}"+tail,1024,128,object),QString("python_output_encoding_invalid"));
  }
};
QTEST_GUILESS_MAIN(TestPythonGenerated)
#include "tst_pythongenerated.moc"
