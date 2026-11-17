// 层：测试壳
#include "services/pythongeneratedoutputspec.h"
#include "services/pythongeneratedspec.h"
#include "services/pythonnumerictable.h"
#include "services/pythonseismictrace.h"
#include "services/pythonseismictracespec.h"
#include "workflow/pythongeneratedoutputoperation.h"
#include "workflow/pythontableoutputoperation.h"
#include "ai/chat/domaintools.h"
#include <QJsonArray>
#include <QtTest>
using namespace paleo;
namespace {
QJsonObject table() {
  return {{"contract","paleo.python-table-declaration.v1"},{"maxRows",3},{"columns",QJsonArray{
    QJsonObject{{"id","time"},{"label","Time"},{"unit","ms"},{"dtype","<f8"},{"role","axis"}},
    QJsonObject{{"id","amplitude"},{"label","Amplitude"},{"unit","mV"},{"dtype","<f8"},{"role","value"}}}}};
}
QJsonObject image(){return {{"contract","paleo.python-image-declaration.v1"},{"width",3},{"height",1},{"format","rgba8"}};}
QJsonObject inputs(bool seismic) {
  const QJsonObject source{{"documentId","doc"},{"modelStoreId","area-model"},{"sourceId","1"},{"fileRevisionId",QString(64,'a')}};
  if(seismic)return {{"input_kind","seismic_trace"},{"source_ref",source},{"volume_id","7"},
    {"volume_revision","seismic-window-descriptor.v1:"+QString(64,'b')},{"trace_index","0"},{"first_sample",1},{"sample_count",3},
    {"amplitude_unit","mV"},{"time_unit","ms"},{"time_type","TWT"},{"time_reference","MSL"}};
  return {{"input_kind","numeric_table"},{"source_ref",source},{"table_id","7"},
    {"projection_revision","12345678-1234-1234-1234-123456789abc"},{"column_ids",QJsonArray{"amplitude"}},{"first_row","0"},{"row_count",3}};
}
QJsonObject arguments(bool seismic=false) {
  return {{"client_request_id","outputs-unit"},{"binding_mode","pinned-value"},{"publish_intent","compute-only"},{"inputs",inputs(seismic)},
    {"parameters",QJsonObject{{"code","pass\n"},{"output_table",table()},{"output_image",image()}}}};
}
}
class TestPythonGeneratedOutputs:public QObject {
  Q_OBJECT
private slots:
  void threePortSpecRetainsEveryOlderContract() {
    const auto spec=python::generatedOutputsAlgorithmSpec();QStringList ports;
    for(const auto &port:spec.outputs)if(port.required)ports.append(port.id);QCOMPARE(ports,QStringList({"REPORT","TABLE","IMAGE"}));
    QCOMPARE(spec.inputs.first().axis,QString("declared_input_axis"));
    const auto image=spec.outputs.last();QCOMPARE(image.kind,QString("file"));QVERIFY(image.constraints.contains("spatial"));QVERIFY(!image.constraints.value("spatial").toBool());
    QCOMPARE(python::generatedAlgorithmSpec().implementationVersion,QString("python-generated-numeric.v1"));
    QCOMPARE(python::numericTableAlgorithmSpec().implementationVersion,QString("python-numeric-table-report.v1"));
    QCOMPARE(python::seismicTraceAlgorithmSpec().implementationVersion,QString("python-seismic-trace-report.v1"));
    QCOMPARE(python::tableOutputsAlgorithmSpec().implementationVersion,QString("python-table-outputs.v1"));
    const auto tool=aiToolSpec(QLatin1String(python::GeneratedOutputsTool));QVERIFY(!tool.name.isEmpty());
    QCOMPARE(tool.effectsForIntent("compute-only"),host::Read|host::PrivateStage|host::LocalExecute);
    QVERIFY(tool.effectsForIntent("publish")&host::CatalogPublish);
  }
  void explicitInputsAndDeclarationsDoNotGrantHostAuthority() {
    for(const bool seismic:{false,true}) {
      const auto valid=arguments(seismic);QVERIFY2(python::validateGeneratedOutputsToolArguments(valid).isEmpty(),
        qPrintable(python::validateGeneratedOutputsToolArguments(valid)));
      for(const auto &field:QStringList{"actor","project_dir","runtime_root","sql","allow_generated"}) {
        auto changed=valid;changed[field]="untrusted";QVERIFY(!python::validateGeneratedOutputsToolArguments(changed).isEmpty());
      }
      auto changed=valid;auto selected=changed.value("inputs").toObject();selected["path"]="source.sgy";changed["inputs"]=selected;
      QVERIFY(!python::validateGeneratedOutputsToolArguments(changed).isEmpty());
      selected=valid.value("inputs").toObject();selected.remove("input_kind");changed=valid;changed["inputs"]=selected;
      QCOMPARE(python::validateGeneratedOutputsToolArguments(changed),QString("python_output_input_kind_invalid"));
      changed=valid;auto params=changed.value("parameters").toObject();auto output=image();output["mime"]="image/png";params["output_image"]=output;changed["parameters"]=params;
      QCOMPARE(python::validateGeneratedOutputsToolArguments(changed),QString("python_image_declaration_invalid"));
    }
    auto publication=arguments();publication["publish_intent"]="publish";publication["inputs"]=QJsonObject{{"source_operation_id","old-operation"}};
    publication["parameters"]=QJsonObject{};QVERIFY(python::validateGeneratedOutputsToolArguments(publication).isEmpty());
    publication["parameters"]=arguments().value("parameters");QVERIFY(!python::validateGeneratedOutputsToolArguments(publication).isEmpty());
  }
  void frozenEncoderIsCheckedForComputeAndStructurallyKeptForPublication() {
    auto profile=python::generatedOutputsProfile(table(),image());python::ExecutionPolicy policy;
    QCOMPARE(profile.value("pngEncoder").toObject().value("compression").toInt(),100);
    QVERIFY(python::validateGeneratedOutputsProfile(profile,policy,true).isEmpty());
    auto encoder=profile.value("pngEncoder").toObject();encoder["qtCompileVersion"]="6.0.0";encoder["qtRuntimeVersion"]="6.0.0";
    profile["pngEncoder"]=encoder;
    QCOMPARE(python::validateGeneratedOutputsProfile(profile,policy,true),QString("python_output_encoder_changed"));
    QVERIFY(python::validateGeneratedOutputsProfile(profile,policy,false).isEmpty()); // Pure contract check, no fake historical record/upgrade.
    encoder["compression"]=9;profile["pngEncoder"]=encoder;
    QVERIFY(python::validateGeneratedOutputsProfile(profile,policy,false).isEmpty());
    QCOMPARE(python::validateGeneratedOutputsProfile(profile,policy,true),QString("python_output_encoder_changed"));
    encoder["compression"]=0;profile["pngEncoder"]=encoder;
    QCOMPARE(python::validateGeneratedOutputsProfile(profile,policy,false),QString("invalid_python_outputs_profile"));
  }
  void knownImageSizeAndHostReservationRejectBeforeSdkSelection() {
    auto profile=python::generatedOutputsProfile(table(),image());python::ExecutionPolicy small;small.outputBytes=16;
    QCOMPARE(python::validateGeneratedOutputsProfile(profile,small,true),QString("python_output_budget_exceeded"));
    small={};small.memoryBytes=1024;
    QCOMPARE(python::validateGeneratedOutputsProfile(profile,small,true),QString("python_output_memory_budget"));
    bool selected=false;const auto selectTable=[&](const auto &,auto &,auto &){selected=true;return QString();};
    const auto selectTrace=[&](const auto &,auto &,auto &){selected=true;return QString();};
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{});
    const auto adapter=python::generatedOutputsScriptAdapter(runtime,{},selectTable,selectTrace);capability::ExecuteRequest request;
    request.capabilityId=QLatin1String(python::GeneratedOutputsCapability);request.contractVersion=1;
    request.inputs=inputs(true).toVariantMap();request.parameters=arguments(true).value("parameters").toObject().toVariantMap();
    auto bad=image();bad["width"]=1025;request.parameters["output_image"]=bad;QString code;
    QVERIFY(!adapter.capture(request,{},&code).worker);QCOMPARE(code,QString("python_image_declaration_invalid"));QVERIFY(!selected);
  }
  void explicitRestorersNeverUpgradeReportOrTableRecords() {
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{});
    const auto group=python::generatedOutputsScriptAdapter(runtime,{}, {},{}),old=python::tableOutputsScriptAdapter(runtime,{},{});
    capability::OperationLog::Record record;record.capabilityId=QLatin1String(python::TableOutputsCapability);record.contractVersion=1;
    record.implementationVersion=python::tableOutputsAlgorithmSpec().implementationVersion;
    QCOMPARE(group.validateFrozen(record),QString("invalid_python_outputs_plan"));
    record.capabilityId=QLatin1String(python::GeneratedOutputsCapability);record.implementationVersion=python::generatedOutputsAlgorithmSpec().implementationVersion;
    QCOMPARE(old.validateFrozen(record),QString("invalid_python_table_outputs_plan"));
  }
};
QTEST_GUILESS_MAIN(TestPythonGeneratedOutputs)
#include "tst_pythongeneratedoutputs.moc"
