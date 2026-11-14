// 层：测试壳
#include "services/pythonanalysisselection.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "ai/chat/domaintools.h"
#include <QtTest>
#include <QJsonArray>
#include <limits>

using namespace paleo;
namespace {
QJsonObject selection(int channels=2) {
  document::WellRef well;well.documentId="doc";well.catalogEntityId="well-id";
  well.modelStoreId="area-model";well.modelStoreIdentity="store";well.modelGeneration="generation";
  well.modelWellId=9007199254740993LL;well.queryHead=17;well.modelAvailable=true;
  QJsonArray curves;
  for(int i=0;i<channels;++i)curves.append(QJsonObject{
    {"source_ref",document::sourceRefJson({"doc","area-model",42,QString(64,'a')})},
    {"source_version_id","ver-source"},{"well_id",QString::number(well.modelWellId)},
    {"log_set_id","5"},{"curve_id",QString::number(10+i)},{"unit",i?"m/s":"API"},
    {"depth_type","MD"},{"depth_unit","m"},{"depth_reference","KB"},{"total_samples","65536"}});
  return {{"well_ref",document::wellRefJson(well)},{"curves",curves},{"first_sample","49152"},
    {"sample_count",8192},{"processed",true},{"alignment","exact_shared_axis_v1"},
    {"sampling",QJsonObject{{"method","all_selected_window_v1"},{"seed",0}}}};
}
QJsonObject arguments(const QJsonObject &inputs) {
  return {{"client_request_id","stable-request"},{"binding_mode","pinned-value"},
    {"parameters",QJsonObject{}},{"inputs",inputs}};
}
void changeCurve(QJsonObject &inputs,int index,const QString &key,const QJsonValue &value) {
  auto curves=inputs.value("curves").toArray();auto curve=curves[index].toObject();
  curve[key]=value;curves[index]=curve;inputs["curves"]=curves;
}
}
class TestPythonAnalysisSelection:public QObject {
  Q_OBJECT
private slots:
  void preservesExactLargeIdsAndSelectedWindow() {
    python::AnalysisSelectionRequest parsed;QString code;
    QVERIFY2(python::analysisSelectionFromJson(selection(),python::AnalysisKind::AttributeCorrelation,parsed,&code),qPrintable(code));
    QCOMPARE(parsed.well.modelWellId,qlonglong(9007199254740993LL));QCOMPARE(parsed.curves.size(),2);
    QCOMPARE(parsed.curves[1].wellId,parsed.well.modelWellId);QCOMPARE(parsed.curves[1].firstSample,qlonglong(49152));
    QCOMPARE(parsed.curves[1].sampleCount,8192);QCOMPARE(parsed.curves[1].unit,QString("m/s"));
    QVERIFY(parsed.curves[0].source.path.isEmpty());QVERIFY(parsed.curves[0].source.modelDirectory.isEmpty());
  }
  void rejectsUntrustedSelections_data() {
    QTest::addColumn<QJsonObject>("inputs");
    auto add=[](const char *name,const std::function<void(QJsonObject&)> &edit){auto s=selection();edit(s);QTest::newRow(name)<<s;};
    add("host-path",[](auto &s){s["path"]="C:/host/secret";});
    add("hidden-budget",[](auto &s){s["read_byte_budget"]=1e9;});
    add("numeric-first",[](auto &s){s["first_sample"]=49152;});
    add("leading-zero",[](auto &s){s["first_sample"]="049152";});
    add("negative-first",[](auto &s){s["first_sample"]="-1";});
    add("overflow-first",[](auto &s){s["first_sample"]="9223372036854775808";});
    add("fractional-count",[](auto &s){s["sample_count"]=1.25;});
    add("window-budget",[](auto &s){s["sample_count"]=8193;});
    add("window-outside",[](auto &s){s["first_sample"]="65536";s["sample_count"]=1;});
    add("unbound-processing",[](auto &s){s["processed"]="true";});
    add("implicit-sampling",[](auto &s){s["sampling"]=QJsonObject{{"method","auto"},{"seed",0}};});
    add("seed-override",[](auto &s){s["sampling"]=QJsonObject{{"method","all_selected_window_v1"},{"seed",1}};});
    add("implicit-axis",[](auto &s){s["alignment"]="nearest";});
    add("missing-unit",[](auto &s){changeCurve(s,0,"unit","");});
    add("inferred-depth",[](auto &s){changeCurve(s,0,"depth_type","TVD");});
    add("unknown-depth-unit",[](auto &s){changeCurve(s,0,"depth_unit","unknown");});
    add("missing-datum",[](auto &s){changeCurve(s,0,"depth_reference","");});
    add("wrong-well",[](auto &s){changeCurve(s,0,"well_id","1");});
    add("numeric-curve",[](auto &s){changeCurve(s,0,"curve_id",10);});
    add("duplicate-curve",[](auto &s){changeCurve(s,1,"curve_id","10");});
    add("quick-fingerprint",[](auto &s){changeCurve(s,0,"source_ref",document::sourceRefJson({"doc","area-model",42,"quick:"+QString(64,'a')}));});
    add("other-document",[](auto &s){changeCurve(s,0,"source_ref",document::sourceRefJson({"other","area-model",42,QString(64,'a')}));});
    add("extra-well-field",[](auto &s){auto w=s["well_ref"].toObject();w["path"]="/untrusted";s["well_ref"]=w;});
  }
  void rejectsUntrustedSelections() {
    QFETCH(QJsonObject,inputs);python::AnalysisSelectionRequest parsed;QString code;
    QVERIFY(!python::analysisSelectionFromJson(inputs,python::AnalysisKind::AttributeCorrelation,parsed,&code));
    QVERIFY(!code.isEmpty());QVERIFY(parsed.curves.isEmpty());QVERIFY(parsed.well.documentId.isEmpty());
  }
  void channelRequirementsAndEmptySelectionAreExplicit() {
    python::AnalysisSelectionRequest out;QString code;auto inputs=selection(1);
    QVERIFY(python::analysisSelectionFromJson(inputs,python::AnalysisKind::WellQuality,out,&code));
    QVERIFY(!python::analysisSelectionFromJson(inputs,python::AnalysisKind::AttributeCorrelation,out,&code));
    inputs["first_sample"]="65536";inputs["sample_count"]=0;
    QVERIFY(python::analysisSelectionFromJson(inputs,python::AnalysisKind::WellQuality,out,&code));
    QCOMPARE(out.curves[0].sampleCount,0);
    QVERIFY(!python::analysisSelectionFromJson(selection(17),python::AnalysisKind::WellQuality,out,&code));
  }
  void phasesCannotSmuggleCodeOrGrants() {
    for(const auto kind:{python::AnalysisKind::WellQuality,python::AnalysisKind::AttributeCorrelation}) {
      const auto name=kind==python::AnalysisKind::WellQuality?python::WellQualityTool:python::CorrelationTool;
      const auto spec=aiToolSpec(QLatin1String(name));QVERIFY(!spec.name.isEmpty());
      auto args=arguments(selection());QVERIFY(spec.validateParameters(args).isEmpty());
      QCOMPARE(spec.toJsonSchema(),python::analysisToolRequestSchema(kind));
      for(const auto &phase:{QString("prepare"),QString("compute-only"),QString("publish")}) {
        args["publish_intent"]=phase;
        args["inputs"]=phase=="publish"?QJsonObject{{"source_operation_id","op-original"}}:selection();
        QVERIFY2(spec.validateParameters(args).isEmpty(),qPrintable(spec.validateParameters(args)));
        QCOMPARE(spec.effectsForArguments(args),host::computeEffects(phase)|host::LocalExecute);
        QCOMPARE(dispatchAiTool(spec.name,args).status,AiToolDispatchStatus::Routed);
      }
      args["publish_intent"]="invalid";QVERIFY(!spec.validateParameters(args).isEmpty());
      QCOMPARE(spec.effectsForArguments(args),0u);
      args=arguments(selection());args["publish_intent"]=true;QVERIFY(!spec.validateParameters(args).isEmpty());
      for(const auto &field:{"code","argv","interpreter","runtime_root","effects","policy"}) {
        args=arguments(selection());args[QLatin1String(field)]="override";QVERIFY(!spec.validateParameters(args).isEmpty());
      }
      args=arguments(selection());args["publish_intent"]="publish";QVERIFY(!spec.validateParameters(args).isEmpty());
      args=arguments(selection());args["binding_mode"]="follow-selection";QVERIFY(!spec.validateParameters(args).isEmpty());
    }
  }
};
QTEST_GUILESS_MAIN(TestPythonAnalysisSelection)
#include "tst_pythonanalysisselection.moc"
