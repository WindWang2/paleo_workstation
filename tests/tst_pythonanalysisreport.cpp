// 层：测试壳
#include "services/pythonanalysisreport.h"
#include "services/pythonanalysis.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <cmath>
#include <limits>

using namespace paleo::python;
namespace {
QJsonObject metadata(AnalysisKind kind=AnalysisKind::WellQuality,int rows=4,int channels=2) {
  QJsonArray columns,sources;
  for(int index=0;index<channels;++index){const auto id=QString::number(index+1);
    columns.append(QJsonObject{{"curveId",id},{"name",index?"B":"A"},{"unit",index?"m/s":"API"},{"processed",true},
      {"nullPolicy","sdk_quality_bits_1_missing_2_abnormal_4_edited_and_nonfinite"}});
    sources.append(QJsonObject{{"source",QJsonObject{{"documentId","doc"},{"modelStoreId","area-model"},{"sourceId","7"},{"fileRevisionId",QString(64,'a')}}},
      {"sourceVersionId","source-v1"},{"wellId","1"},{"logSetId","2"},{"curveId",id},{"firstSample","0"},
      {"sampleCount",rows},{"sourceTotalSamples","8192"},{"processed",true}});}
  const QJsonObject axis{{"kind","MD"},{"unit","m"},{"reference","KB"},{"alignment","exact_shared_axis_v1"},{"conversion","none"},
    {"origins",QJsonObject{{"kind","declared"},{"unit","declared"},{"reference","declared"}}}};
  const QJsonObject sampling{{"method","all_selected_window_v1"},{"seed",0},{"count",rows},{"scope","selected_window_only"},{"random",false},{"extrapolation",false}};
  const QJsonObject provenance{{"documentId","doc"},{"callerScope","agent:test"},
    {"model",QJsonObject{{"storeId","model"},{"generation","generation"},{"sequence","1"},{"boundary","unchanged_before_and_after_all_windows"}}},
    {"sources",sources},{"axis",axis},{"sampling",sampling},{"readMetrics",QJsonObject{{"payloadBytes","104"}}}};
  return {{"contract","paleo.python-analysis-input.v1"},{"analysis",kind==AnalysisKind::WellQuality?"well_quality":"attribute_correlation"},
    {"inputProtocol","paleo.python-analysis-frame.v1"},{"sourceKind","well_curve_numeric_attributes"},{"dtype","<f8"},
    {"shape",QJsonArray{rows,channels}},{"columns",columns},{"axis",axis},{"provenance",provenance}};
}
QJsonObject common(const QJsonObject &meta) {
  return {{"sourceKind","well_curve_numeric_attributes"},{"scope","selected_window_only"},{"provenance",meta.value("provenance")},
    {"software",QJsonObject{{"python",PythonVersion},{"numpy",NumpyVersion},{"determinism","single_thread_fixed_input_order_tolerance"}}},
    {"recommendations",QJsonArray{"Review the source curves."}},{"limitations",QJsonArray{"Selected windows only."}}};
}
QJsonObject quality(const QJsonObject &meta) {
  auto report=common(meta);report.insert("contract","paleo.well-quality-report.v1");const int rows=meta.value("shape").toArray()[0].toInt();QJsonArray metrics;
  for(const auto &entry:meta.value("columns").toArray()){const auto column=entry.toObject();
    QJsonObject metric{{"curveId",column.value("curveId")},{"name",column.value("name")},{"unit",column.value("unit")},{"sampleCount",rows},
      {"validCount",rows},{"missingCount",0},{"nonFiniteCount",0},{"sdkAbnormalCount",0},{"editedCount",0},
      {"coverage",rows?QJsonValue(1.0):QJsonValue(QJsonValue::Null)},{"minimum",rows?QJsonValue(5.0):QJsonValue(QJsonValue::Null)},
      {"maximum",rows?QJsonValue(5.0):QJsonValue(QJsonValue::Null)},{"mean",rows?QJsonValue(5.0):QJsonValue(QJsonValue::Null)},
      {"populationStdDev",rows?QJsonValue(0.0):QJsonValue(QJsonValue::Null)},{"zScoreOutlierCount",0},{"zScoreThreshold",3.0},
      {"status",rows?"constant":"empty_window"}};metrics.append(metric);}
  report.insert("metrics",metrics);return report;
}
QJsonObject correlation(const QJsonObject &meta,int count=4) {
  auto report=common(meta);const int rows=meta.value("shape").toArray()[0].toInt(),channels=meta.value("shape").toArray()[1].toInt();
  report.insert("contract","paleo.attribute-correlation-report.v1");report.insert("method","pearson_joint_complete_rows_v1");report.insert("columns",meta.value("columns"));
  report.insert("sampleCount",rows);report.insert("jointValidCount",count);report.insert("rejectedRows",rows-count);
  report.insert("coverage",rows?QJsonValue(double(count)/rows):QJsonValue(QJsonValue::Null));
  const auto cell=count>=2?QJsonValue(1.0):QJsonValue(QJsonValue::Null);
  QJsonArray matrix;for(int i=0;i<channels;++i){QJsonArray row;for(int j=0;j<channels;++j)row.append(cell);matrix.append(row);}
  report.insert("matrix",matrix);report.insert("constantColumns",QJsonArray{});
  report.insert("redundancyThreshold",0.95);QJsonArray pairs;
  if(count>=2)for(int i=0;i<channels;++i)for(int j=i+1;j<channels;++j)pairs.append(QJsonObject{{"left",i},{"right",j},{"pearson",1.0},{"sampleCount",count}});
  report.insert("redundantPairs",pairs);report.insert("status",!rows?"empty_window":count<2?"insufficient_samples":"ok");return report;
}
QByteArray envelope(const QJsonObject &report) {
  return QJsonDocument(QJsonObject{{"contract","paleo.python-output.v1"},{"ports",QJsonObject{{"REPORT",report}}}}).toJson(QJsonDocument::Compact);
}
void metricField(QJsonObject &report,const QString &key,const QJsonValue &value) {
  auto metrics=report.value("metrics").toArray();auto first=metrics[0].toObject();first.insert(key,value);metrics[0]=first;report.insert("metrics",metrics);
}
}
class PythonAnalysisReportTests:public QObject {
  Q_OBJECT
private slots:
  void rejectsTruncatedUtf8AtCompleteOutputBoundary() {
    const auto meta=metadata();const auto valid=envelope(quality(meta));
    for(const auto &tail:{QByteArray("\xc3",1),QByteArray("\xe2\x82",2),QByteArray("\xf0\x9f\x92",3)})
      QCOMPARE(validateAnalysisReport(valid+tail,AnalysisKind::WellQuality,meta,{}).code,QString("python_output_encoding_invalid"));
  }
  void parsedCorrelationCellsHaveValueOwnership_data() {
    QTest::addColumn<int>("channels");QTest::addColumn<int>("count");
    QTest::newRow("dense-redundancy")<<16<<4;QTest::newRow("null-insufficient")<<16<<1;
    QTest::newRow("mixed-coefficients")<<3<<4;
  }
  void parsedCorrelationCellsHaveValueOwnership() {
    QFETCH(int,channels);QFETCH(int,count);const auto meta=metadata(AnalysisKind::AttributeCorrelation,4,channels);
    auto value=correlation(meta,count);
    if(channels==3) {
      value["matrix"]=QJsonArray{QJsonArray{1.0,0.99,0.25},QJsonArray{0.99,1.0,0.3},QJsonArray{0.25,0.3,1.0}};
      value["redundantPairs"]=QJsonArray{QJsonObject{{"left",0},{"right",1},{"pearson",0.99},{"sampleCount",4}}};
    }
    const auto bytes=envelope(value);
    for(int iteration=0;iteration<128;++iteration) {
      // Parse fresh shared arrays just as the process-output path does. Array
      // indexing must produce owned QJsonValue copies, never temporary proxies.
      const auto parsed=QJsonDocument::fromJson(bytes).object();
      const auto roundTrip=QJsonDocument(parsed).toJson(QJsonDocument::Compact);
      const auto result=validateAnalysisReport(roundTrip,AnalysisKind::AttributeCorrelation,meta,{});
      QVERIFY2(result.ok(),qPrintable(result.code));QCOMPARE(QJsonDocument::fromJson(result.bytes).object(),value);
    }
  }
  void acceptsRepackagesAndSummarizesBothContracts() {
    for(const auto kind:{AnalysisKind::WellQuality,AnalysisKind::AttributeCorrelation}){const auto meta=metadata(kind);
      const auto report=kind==AnalysisKind::WellQuality?quality(meta):correlation(meta);
      const auto result=validateAnalysisReport(envelope(report),kind,meta,ExecutionPolicy{});QVERIFY2(result.ok(),qPrintable(result.code));
      QCOMPARE(QJsonDocument::fromJson(result.bytes).object(),report);QVERIFY(!QJsonDocument::fromJson(result.bytes).object().contains("ports"));
      QVERIFY(QJsonDocument(result.summary).toJson(QJsonDocument::Compact).size()<=4096);QVERIFY(!result.summary.contains("provenance"));
      QVERIFY(!result.summary.contains("matrix"));QVERIFY(!result.summary.contains("metrics"));}
  }
  void acceptsEmptyMissingAndInsufficientSemantics() {
    auto meta=metadata(AnalysisKind::WellQuality,0);QVERIFY(validateAnalysisReport(envelope(quality(meta)),AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
    meta=metadata();auto report=quality(meta);
    for(const auto key:{"minimum","maximum","mean","populationStdDev"})metricField(report,key,QJsonValue(QJsonValue::Null));
    metricField(report,"validCount",0);metricField(report,"missingCount",4);metricField(report,"nonFiniteCount",2);metricField(report,"coverage",0.0);metricField(report,"status","all_missing");
    QVERIFY(validateAnalysisReport(envelope(report),AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
    meta=metadata(AnalysisKind::AttributeCorrelation,0);QVERIFY(validateAnalysisReport(envelope(correlation(meta,0)),AnalysisKind::AttributeCorrelation,meta,ExecutionPolicy{}).ok());
    meta=metadata(AnalysisKind::AttributeCorrelation);QVERIFY(validateAnalysisReport(envelope(correlation(meta,1)),AnalysisKind::AttributeCorrelation,meta,ExecutionPolicy{}).ok());
    report=correlation(meta);report.insert("constantColumns",QJsonArray{0});report.insert("matrix",QJsonArray{QJsonArray{QJsonValue::Null,QJsonValue::Null},QJsonArray{QJsonValue::Null,1.0}});
    report.insert("redundantPairs",QJsonArray{});QVERIFY(validateAnalysisReport(envelope(report),AnalysisKind::AttributeCorrelation,meta,ExecutionPolicy{}).ok());
  }
  void acceptsVariableStatisticsAndCeilingShapes() {
    auto meta=metadata();auto report=quality(meta);metricField(report,"minimum",1.0);metricField(report,"maximum",4.0);
    metricField(report,"mean",2.5);metricField(report,"populationStdDev",std::sqrt(1.25));metricField(report,"status","ok");
    QVERIFY(validateAnalysisReport(envelope(report),AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
    for(const auto kind:{AnalysisKind::WellQuality,AnalysisKind::AttributeCorrelation}){meta=metadata(kind,8192,16);
      report=kind==AnalysisKind::WellQuality?quality(meta):correlation(meta,8192);
      const auto result=validateAnalysisReport(envelope(report),kind,meta,ExecutionPolicy{});QVERIFY2(result.ok(),qPrintable(result.code));
      QCOMPARE(result.summary.value("channels").toInt(),16);QVERIFY(QJsonDocument(result.summary).toJson().size()<4096);}
  }
  void rejectsQualityInconsistencies_data() {
    QTest::addColumn<QString>("field");QTest::addColumn<QJsonValue>("value");
    QTest::newRow("wrong-unit")<<QString("unit")<<QJsonValue("ohm");
    QTest::newRow("wrong-id")<<QString("curveId")<<QJsonValue("9");
    QTest::newRow("wrong-count")<<QString("sampleCount")<<QJsonValue(5);
    QTest::newRow("negative-count")<<QString("validCount")<<QJsonValue(-1);
    QTest::newRow("fractional-count")<<QString("validCount")<<QJsonValue(3.5);
    QTest::newRow("bool-count")<<QString("missingCount")<<QJsonValue(false);
    QTest::newRow("overlapping-counts")<<QString("missingCount")<<QJsonValue(1);
    QTest::newRow("wrong-coverage")<<QString("coverage")<<QJsonValue(0.5);
    QTest::newRow("wrong-threshold")<<QString("zScoreThreshold")<<QJsonValue(4);
    QTest::newRow("constant-outlier")<<QString("zScoreOutlierCount")<<QJsonValue(1);
    QTest::newRow("null-valid-statistic")<<QString("mean")<<QJsonValue(QJsonValue::Null);
    QTest::newRow("inconsistent-mean")<<QString("mean")<<QJsonValue(9.0);
    QTest::newRow("constant-positive-sd")<<QString("populationStdDev")<<QJsonValue(1);
    QTest::newRow("wrong-status")<<QString("status")<<QJsonValue("ok");
    QTest::newRow("script-path")<<QString("path")<<QJsonValue("/host/private");
  }
  void rejectsQualityInconsistencies() {
    QFETCH(QString,field);QFETCH(QJsonValue,value);const auto meta=metadata();auto report=quality(meta);metricField(report,field,value);
    const auto result=validateAnalysisReport(envelope(report),AnalysisKind::WellQuality,meta,ExecutionPolicy{});
    QVERIFY(!result.ok());QVERIFY(result.bytes.isEmpty());QVERIFY(result.summary.isEmpty());
  }
  void rejectsCorrelationShapeNullSymmetryAndRedundancy() {
    const auto meta=metadata(AnalysisKind::AttributeCorrelation);const auto original=correlation(meta);
    const auto reject=[&](QJsonObject report){const auto result=validateAnalysisReport(envelope(report),AnalysisKind::AttributeCorrelation,meta,ExecutionPolicy{});return !result.ok()&&result.bytes.isEmpty()&&result.summary.isEmpty();};
    auto report=original;report.insert("matrix",QJsonArray{QJsonArray{1.0}});QVERIFY(reject(report));
    report=original;report.insert("matrix",QJsonArray{QJsonArray{1.0,0.5},QJsonArray{1.0,1.0}});QVERIFY(reject(report));
    report=original;report.insert("matrix",QJsonArray{QJsonArray{1.0,1.1},QJsonArray{1.1,1.0}});QVERIFY(reject(report));
    report=original;report.insert("matrix",QJsonArray{QJsonArray{0.0,1.0},QJsonArray{1.0,1.0}});QVERIFY(reject(report));
    report=original;report.insert("constantColumns",QJsonArray{0});QVERIFY(reject(report));
    report=original;report.insert("constantColumns",QJsonArray{1,1});QVERIFY(reject(report));
    report=original;report.insert("redundantPairs",QJsonArray{});QVERIFY(reject(report));
    report=original;auto pairs=report.value("redundantPairs").toArray();pairs.append(pairs[0]);report.insert("redundantPairs",pairs);QVERIFY(reject(report));
    report=original;report.insert("rejectedRows",1);QVERIFY(reject(report));
    report=original;report.insert("coverage",0.25);QVERIFY(reject(report));
    report=original;report.insert("redundancyThreshold",0.9);QVERIFY(reject(report));
    report=correlation(meta,1);report.insert("matrix",original.value("matrix"));QVERIFY(reject(report));
  }
  void refusesProvenanceSamplingAxisAndColumnSubstitution() {
    const auto meta=metadata(AnalysisKind::AttributeCorrelation);const auto original=correlation(meta);
    for(const auto key:{"documentId","callerScope","model","sources","sampling","axis"}){auto report=original;auto provenance=report.value("provenance").toObject();
      provenance.insert(QLatin1String(key),"forged");report.insert("provenance",provenance);
      QCOMPARE(validateAnalysisReport(envelope(report),AnalysisKind::AttributeCorrelation,meta,ExecutionPolicy{}).code,QString("python_output_provenance_mismatch"));}
    auto report=original;auto columns=report.value("columns").toArray();auto first=columns[0].toObject();first.insert("unit","unknown");columns[0]=first;report.insert("columns",columns);
    QVERIFY(!validateAnalysisReport(envelope(report),AnalysisKind::AttributeCorrelation,meta,ExecutionPolicy{}).ok());
    auto expected=meta;expected.insert("shape",QJsonArray{8193,2});QCOMPARE(validateAnalysisReport(envelope(original),AnalysisKind::AttributeCorrelation,expected,ExecutionPolicy{}).code,QString("python_analysis_input_metadata_invalid"));
    expected=meta;expected.insert("columns",QJsonArray{});QVERIFY(!validateAnalysisReport(envelope(original),AnalysisKind::AttributeCorrelation,expected,ExecutionPolicy{}).ok());
  }
  void rejectsDuplicatePortsMalformedJsonInvalidUtf8AndBudgets() {
    const auto meta=metadata();const auto report=quality(meta);const auto good=envelope(report);
    auto result=validateAnalysisReport(good+good,AnalysisKind::WellQuality,meta,ExecutionPolicy{});QVERIFY(!result.ok());
    auto object=QJsonDocument::fromJson(good).object();auto ports=object.value("ports").toObject();ports.insert("EXTRA",report);object.insert("ports",ports);
    QVERIFY(!validateAnalysisReport(QJsonDocument(object).toJson(),AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
    auto duplicate=good;duplicate.replace("\"ports\":{","\"ports\":{\"REPORT\":{},");
    QCOMPARE(validateAnalysisReport(duplicate,AnalysisKind::WellQuality,meta,ExecutionPolicy{}).code,QString("python_output_invalid"));
    duplicate=good;duplicate.replace("\"ports\":{","\"ports\":{\"R\\u0045PORT\":{},");
    QCOMPARE(validateAnalysisReport(duplicate,AnalysisKind::WellQuality,meta,ExecutionPolicy{}).code,QString("python_output_invalid"));
    auto invalid=good;invalid.replace("Review",QByteArray::fromHex("ff"));
    QCOMPARE(validateAnalysisReport(invalid,AnalysisKind::WellQuality,meta,ExecutionPolicy{}).code,QString("python_output_encoding_invalid"));
    auto nan=good;nan.replace("\"mean\":5","\"mean\":NaN");QVERIFY(!validateAnalysisReport(nan,AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
    auto inf=good;inf.replace("\"mean\":5","\"mean\":1e999");QVERIFY(!validateAnalysisReport(inf,AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
    ExecutionPolicy policy;policy.outputBytes=good.size()-1;QCOMPARE(validateAnalysisReport(good,AnalysisKind::WellQuality,meta,policy).code,QString("python_output_budget_exceeded"));
    auto extra=report;extra.insert("sha256",QString(64,'a'));QVERIFY(!validateAnalysisReport(envelope(extra),AnalysisKind::WellQuality,meta,ExecutionPolicy{}).ok());
  }
  void actualLockedTemplatesPassIndependentHostValidation() {
    const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if(root.isEmpty()){QVERIFY(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_REPORT")!="1");
      qInfo()<<"Pinned runtime not configured; this run does not exercise template outputs";return;}
    const auto runtime=verifyRuntime({root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true});QVERIFY2(runtime.ready(),qPrintable(runtime.code));
    const auto templateRoot=QFINDTESTDATA("../vendor/agent-python/templates");QVERIFY(!templateRoot.isEmpty());
    const auto permit=claimExecutionPermit();ExecutionPolicy policy;policy.allowTrusted=true;
    for(const auto kind:{AnalysisKind::WellQuality,AnalysisKind::AttributeCorrelation})for(int scenario=0;scenario<3;++scenario){
      const int rows=scenario==1?0:4;auto meta=metadata(kind,rows);QByteArray payload;QDataStream data(&payload,QIODevice::WriteOnly);data.setByteOrder(QDataStream::LittleEndian);
      data.setFloatingPointPrecision(QDataStream::DoublePrecision);for(int i=0;i<rows;++i)data<<double(100+0.25*i);
      for(int i=0;i<rows;++i)for(int j=0;j<2;++j)data<<(scenario==2?std::numeric_limits<double>::quiet_NaN():j?5.0:double(i+1));
      payload.append(QByteArray(rows*2,scenario==2?char(1):char(0)));
      meta.insert("layout",QJsonObject{{"axisBytes",rows*8},{"valueBytes",rows*2*8},{"qualityBytes",rows*2},{"order","axis_then_row_major_values_then_row_major_quality"}});
      meta.insert("payload",QJsonObject{{"bytes",payload.size()},{"sha256",QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex())}});
      const auto control=QJsonDocument(meta).toJson(QJsonDocument::Compact);QByteArray frame(4,Qt::Uninitialized);qToLittleEndian<quint32>(quint32(control.size()),frame.data());frame+=control;frame+=payload;
      const auto name=kind==AnalysisKind::WellQuality?"well_quality.py":"attribute_correlation.py";QFile script(templateRoot+'/'+name);QVERIFY(script.open(QIODevice::ReadOnly));const auto source=script.readAll();
      TrustedProcessRequest request;request.runtime=runtime.runtime;request.scriptPath=QFileInfo(script).canonicalFilePath();request.scriptSha256=QString::fromLatin1(QCryptographicHash::hash(source,QCryptographicHash::Sha256).toHex());
      request.input=frame;request.policy=policy;request.permit=permit;const auto process=runTrustedProcess(request);QVERIFY2(process.ok(),qPrintable(process.code+':'+QString::fromUtf8(process.error)));
      const auto result=validateAnalysisReport(process.output,kind,meta,policy);QVERIFY2(result.ok(),qPrintable(QString::fromLatin1(name)+':'+result.code+':'+QString::fromUtf8(process.output)));
      const auto report=QJsonDocument::fromJson(result.bytes).object();
      if(kind==AnalysisKind::WellQuality){const auto first=report.value("metrics").toArray().first().toObject();QCOMPARE(first.value("validCount").toInt(),scenario?0:4);
        if(!scenario){QCOMPARE(first.value("mean").toDouble(),2.5);QVERIFY(qAbs(first.value("populationStdDev").toDouble()-std::sqrt(1.25))<1e-12);}}
      else if(!scenario){QCOMPARE(report.value("constantColumns").toArray(),QJsonArray{1});QVERIFY(report.value("matrix").toArray()[0].toArray()[1].isNull());}
      qInfo()<<"Real template accepted"<<name<<"scenario"<<scenario<<"reportBytes"<<result.bytes.size()<<"summaryBytes"<<QJsonDocument(result.summary).toJson(QJsonDocument::Compact).size();
    }
  }
};
QTEST_GUILESS_MAIN(PythonAnalysisReportTests)
#include "tst_pythonanalysisreport.moc"
