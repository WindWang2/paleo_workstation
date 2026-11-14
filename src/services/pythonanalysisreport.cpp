// 层：数据
#include "pythonanalysisreport.h"
#include "pythonanalysis.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>
#include <cmath>

namespace paleo::python {
namespace {
constexpr qsizetype ReportBytes=256*1024,SummaryBytes=4096;
constexpr auto SourceKind="well_curve_numeric_attributes";
constexpr auto Scope="selected_window_only";
constexpr auto NullPolicy="sdk_quality_bits_1_missing_2_abnormal_4_edited_and_nonfinite";
QString kindId(AnalysisKind kind) {
  switch(kind){case AnalysisKind::WellQuality:return "well_quality";
    case AnalysisKind::AttributeCorrelation:return "attribute_correlation";}return {};
}
bool keys(const QJsonObject &object,std::initializer_list<const char*> names) {
  QStringList expected;for(const auto name:names)expected.append(QString::fromLatin1(name));expected.sort();return object.keys()==expected;
}
bool text(const QJsonValue &value,qsizetype maximum=256) {
  return value.isString()&&!value.toString().trimmed().isEmpty()&&!value.toString().contains(QChar(0))&&value.toString().toUtf8().size()<=maximum;
}
bool integer(const QJsonValue &value,int maximum,int &out) {
  if(!value.isDouble())return false;const auto number=value.toDouble();
  if(!std::isfinite(number)||number<0||number>maximum||number!=std::floor(number))return false;out=int(number);return true;
}
bool decimal(const QJsonValue &value,qint64 &out,bool positive=false) {
  if(!value.isString())return false;bool valid=false;out=value.toString().toLongLong(&valid);
  return valid&&out>=(positive?1:0)&&QString::number(out)==value.toString();
}
bool digest(const QJsonValue &value) {
  if(!value.isString()||value.toString().size()!=64)return false;
  for(const auto c:value.toString())if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;
}
bool finite(const QJsonValue &value,double &out) {out=value.toDouble();return value.isDouble()&&std::isfinite(out);}
bool near(double left,double right) {
  const auto scale=qMax(std::abs(left),std::abs(right));return scale==0||std::abs(left/scale-right/scale)<=1e-12;
}
bool coverage(const QJsonValue &value,int valid,int rows) {
  if(!rows)return value.isNull();double number=0;return finite(value,number)&&number>=0&&number<=1&&near(number,double(valid)/rows);
}
// QJsonDocument deliberately collapses duplicate keys. Check the already parsed
// JSON token structure as well, including escaped spellings of the same key.
class UniqueKeys {
  const QByteArray &data;qsizetype offset=0;int nodes=0;
  void space(){while(offset<data.size()&&(data[offset]==' '||data[offset]=='\t'||data[offset]=='\n'||data[offset]=='\r'))++offset;}
  QByteArray string(){const auto begin=offset++;while(offset<data.size()){
    if(data[offset]=='\\'){offset+=2;continue;}if(data[offset++]=='"')return data.mid(begin,offset-begin);}return {};}
  bool value(int depth){
    if(depth>32||++nodes>8192)return false;space();if(offset>=data.size())return false;const auto token=data[offset];
    if(token=='{') {++offset;QSet<QString> names;space();if(offset<data.size()&&data[offset]=='}'){++offset;return true;}
      for(;;){space();if(offset>=data.size()||data[offset]!='"')return false;const auto raw=string();
        const auto name=QJsonDocument::fromJson('['+raw+']').array().first().toString();
        if(name.toUtf8().size()>256||names.contains(name))return false;names.insert(name);space();
        if(offset>=data.size()||data[offset++]!=':'||!value(depth+1))return false;space();if(offset>=data.size())return false;
        if(data[offset]=='}'){++offset;return true;}if(data[offset++]!=',')return false;}
    }
    if(token=='['){++offset;space();if(offset<data.size()&&data[offset]==']'){++offset;return true;}
      for(;;){if(!value(depth+1))return false;space();if(offset>=data.size())return false;if(data[offset]==']'){++offset;return true;}if(data[offset++]!=',')return false;}}
    if(token=='"')return !string().isEmpty();
    const auto begin=offset;while(offset<data.size()&&data[offset]!=','&&data[offset]!=']'&&data[offset]!='}'&&data[offset]!=' '&&data[offset]!='\n'&&data[offset]!='\r'&&data[offset]!='\t')++offset;
    return offset>begin;
  }
public:
  explicit UniqueKeys(const QByteArray &bytes):data(bytes){}
  bool valid(){const bool ok=value(0);space();return ok&&offset==data.size();}
};
struct Expected {int rows=0,channels=0;QJsonArray columns;QJsonObject provenance;};
bool sources(const Expected &expected) {
  const auto provenance=expected.provenance;
  if(!keys(provenance,{"documentId","callerScope","model","sources","axis","sampling","readMetrics"})||
      !text(provenance.value("documentId"))||!text(provenance.value("callerScope"))||!provenance.value("readMetrics").isObject())return false;
  const auto model=provenance.value("model").toObject();qint64 sequence=0;
  if(!keys(model,{"storeId","generation","sequence","boundary"})||!text(model.value("storeId"))||!text(model.value("generation"))||
      !decimal(model.value("sequence"),sequence)||model.value("boundary")!="unchanged_before_and_after_all_windows")return false;
  const auto array=provenance.value("sources").toArray();if(array.size()!=expected.channels)return false;
  for(int i=0;i<array.size();++i){const auto value=array[i].toObject(),column=expected.columns[i].toObject();
    qint64 well=0,log=0,curve=0,first=0,total=0,sourceId=0;int count=0;
    const auto source=value.value("source").toObject();
    if(!keys(value,{"source","sourceVersionId","wellId","logSetId","curveId","firstSample","sampleCount","sourceTotalSamples","processed"})||
        !keys(source,{"documentId","modelStoreId","sourceId","fileRevisionId"})||source.value("documentId")!=provenance.value("documentId")||
        source.value("modelStoreId")!="area-model"||!digest(source.value("fileRevisionId"))||!decimal(source.value("sourceId"),sourceId,true)||
        !text(value.value("sourceVersionId"))||!decimal(value.value("wellId"),well,true)||!decimal(value.value("logSetId"),log,true)||
        !decimal(value.value("curveId"),curve,true)||value.value("curveId")!=column.value("curveId")||!decimal(value.value("firstSample"),first)||
        !decimal(value.value("sourceTotalSamples"),total)||first>total||!integer(value.value("sampleCount"),AnalysisMaxSamples,count)||
        count!=expected.rows||count>total-first||value.value("processed")!=column.value("processed"))return false;
  }
  return true;
}
bool expectedMetadata(const QJsonObject &meta,AnalysisKind kind,Expected &expected) {
  if(QJsonDocument(meta).toJson(QJsonDocument::Compact).size()>64*1024||meta.value("contract")!="paleo.python-analysis-input.v1"||
      meta.value("analysis")!=kindId(kind)||kindId(kind).isEmpty()||meta.value("inputProtocol")!="paleo.python-analysis-frame.v1"||
      meta.value("sourceKind")!=QLatin1String(SourceKind)||meta.value("dtype")!="<f8")return false;
  const auto shape=meta.value("shape").toArray();if(shape.size()!=2||!integer(shape[0],AnalysisMaxSamples,expected.rows)||
      !integer(shape[1],AnalysisMaxChannels,expected.channels)||expected.channels<(kind==AnalysisKind::WellQuality?1:2))return false;
  expected.columns=meta.value("columns").toArray();expected.provenance=meta.value("provenance").toObject();
  if(expected.columns.size()!=expected.channels)return false;QSet<QString> ids;
  for(const auto &entry:expected.columns){const auto column=entry.toObject();qint64 id=0;
    if(!keys(column,{"curveId","name","unit","processed","nullPolicy"})||!decimal(column.value("curveId"),id,true)||ids.contains(column.value("curveId").toString())||
        !text(column.value("name"))||!text(column.value("unit"))||!column.value("processed").isBool()||column.value("nullPolicy")!=QLatin1String(NullPolicy))return false;
    ids.insert(column.value("curveId").toString());}
  const auto axis=meta.value("axis").toObject();
  const QJsonObject origins{{"kind","declared"},{"unit","declared"},{"reference","declared"}};
  if(!keys(axis,{"kind","unit","reference","alignment","conversion","origins"})||axis.value("kind")!="MD"||
      !QStringList{"m","ft","M","FT"}.contains(axis.value("unit").toString())||!text(axis.value("reference"))||
      axis.value("alignment")!="exact_shared_axis_v1"||axis.value("conversion")!="none"||axis.value("origins")!=QJsonValue(origins)||
      expected.provenance.value("axis")!=QJsonValue(axis))return false;
  const QJsonObject sampling{{"method","all_selected_window_v1"},{"seed",0},{"count",expected.rows},{"scope",Scope},{"random",false},{"extrapolation",false}};
  return expected.provenance.value("sampling")==QJsonValue(sampling)&&sources(expected);
}
bool descriptions(const QJsonValue &value) {
  if(!value.isArray()||value.toArray().isEmpty()||value.toArray().size()>8)return false;
  for(const auto &entry:value.toArray())if(!text(entry,512))return false;return true;
}
bool common(const QJsonObject &report,const Expected &expected) {
  const auto software=report.value("software").toObject();
  return report.value("sourceKind")==QLatin1String(SourceKind)&&report.value("scope")==QLatin1String(Scope)&&
    report.value("provenance")==QJsonValue(expected.provenance)&&keys(software,{"python","numpy","determinism"})&&
    software.value("python")==QLatin1String(PythonVersion)&&software.value("numpy")==QLatin1String(NumpyVersion)&&
    software.value("determinism")=="single_thread_fixed_input_order_tolerance"&&
    descriptions(report.value("recommendations"))&&descriptions(report.value("limitations"));
}
bool metric(const QJsonObject &value,const QJsonObject &column,int rows) {
  if(!keys(value,{"curveId","name","unit","sampleCount","validCount","missingCount","nonFiniteCount","sdkAbnormalCount","editedCount",
      "coverage","minimum","maximum","mean","populationStdDev","zScoreOutlierCount","zScoreThreshold","status"}))return false;
  for(const auto key:{"curveId","name","unit"})if(value.value(QLatin1String(key))!=column.value(QLatin1String(key)))return false;
  int samples=0,valid=0,missing=0,nonfinite=0,abnormal=0,edited=0,outliers=0;
  if(!integer(value.value("sampleCount"),AnalysisMaxSamples,samples)||samples!=rows||!integer(value.value("validCount"),rows,valid)||
      !integer(value.value("missingCount"),rows,missing)||!integer(value.value("nonFiniteCount"),missing,nonfinite)||
      !integer(value.value("sdkAbnormalCount"),rows,abnormal)||!integer(value.value("editedCount"),rows,edited)||
      !integer(value.value("zScoreOutlierCount"),valid,outliers)||valid>rows-qMax(missing,abnormal)||valid<rows-qMin(rows,missing+abnormal)||
      !coverage(value.value("coverage"),valid,rows)||value.value("zScoreThreshold")!=QJsonValue(3.0))return false;
  if(!valid){for(const auto key:{"minimum","maximum","mean","populationStdDev"})if(!value.value(QLatin1String(key)).isNull())return false;
    return !outliers&&value.value("status")==(!rows?"empty_window":missing==rows?"all_missing":"no_valid_samples");}
  double minimum=0,maximum=0,mean=0,deviation=0;
  if(!finite(value.value("minimum"),minimum)||!finite(value.value("maximum"),maximum)||!finite(value.value("mean"),mean)||
      !finite(value.value("populationStdDev"),deviation)||minimum>maximum||deviation<0||
      (mean<minimum&&!near(mean,minimum))||(mean>maximum&&!near(mean,maximum)))return false;
  if(minimum==maximum)return value.value("status")=="constant"&&deviation==0&&outliers==0&&near(mean,minimum);
  const auto scale=qMax(std::abs(minimum),std::abs(maximum));
  return valid>=2&&value.value("status")=="ok"&&deviation/scale<=(maximum/scale-minimum/scale)/2+1e-12;
}
bool quality(const QJsonObject &report,const Expected &expected,QJsonObject &summary) {
  if(!keys(report,{"contract","sourceKind","scope","metrics","provenance","software","recommendations","limitations"})||
      report.value("contract")!="paleo.well-quality-report.v1"||!report.value("metrics").isArray())return false;
  const auto metrics=report.value("metrics").toArray();if(metrics.size()!=expected.channels)return false;
  int valid=0,missing=0,constant=0,unavailable=0;
  for(int i=0;i<metrics.size();++i){const auto value=metrics[i].toObject();if(!metric(value,expected.columns[i].toObject(),expected.rows))return false;
    valid+=value.value("validCount").toInt();missing+=value.value("missingCount").toInt();
    constant+=value.value("status")=="constant";unavailable+=value.value("validCount").toInt()==0;}
  summary={{"analysis","well_quality"},{"contract",report.value("contract")},{"scope",Scope},{"channels",expected.channels},{"sampleCount",expected.rows},
    {"validSamples",valid},{"missingSamples",missing},{"constantChannels",constant},{"noValidChannels",unavailable}};return true;
}
bool correlationMatrix(const QJsonObject &report,int channels,int count,QJsonArray &matrix,QSet<int> &constant) {
  if(!report.value("constantColumns").isArray()||!report.value("matrix").isArray())return false;
  int previous=-1;for(const auto &entry:report.value("constantColumns").toArray()){int index=0;
    if(!integer(entry,channels-1,index)||index<=previous)return false;constant.insert(index);previous=index;}
  matrix=report.value("matrix").toArray();if(matrix.size()!=channels||(!constant.isEmpty()&&count<2))return false;
  for(int left=0;left<channels;++left){if(!matrix[left].isArray()||matrix[left].toArray().size()!=channels)return false;
    for(int right=0;right<channels;++right){const QJsonValue value=matrix[left].toArray().at(right);
      if(count<2||constant.contains(left)||constant.contains(right)){if(!value.isNull())return false;continue;}
      double number=0;if(!finite(value,number)||number<-1||number>1||(left==right&&!near(number,1.0)))return false;}}
  for(int left=0;left<channels;++left)for(int right=left+1;right<channels;++right){const QJsonValue a=matrix[left].toArray().at(right),b=matrix[right].toArray().at(left);
    if(a.isNull()!=b.isNull()||(!a.isNull()&&!near(a.toDouble(),b.toDouble())))return false;}
  return true;
}
bool pairs(const QJsonValue &value,const QJsonArray &matrix,int channels,int count) {
  if(!value.isArray())return false;const auto array=value.toArray();int index=0;
  for(int left=0;left<channels;++left)for(int right=left+1;right<channels;++right){const QJsonValue coefficient=matrix[left].toArray().at(right);
    if(coefficient.isNull()||std::abs(coefficient.toDouble())<0.95)continue;if(index>=array.size())return false;
    const auto pair=array[index++].toObject();int l=0,r=0,samples=0;double number=0;
    if(!keys(pair,{"left","right","pearson","sampleCount"})||!integer(pair.value("left"),channels-1,l)||l!=left||
        !integer(pair.value("right"),channels-1,r)||r!=right||!integer(pair.value("sampleCount"),AnalysisMaxSamples,samples)||samples!=count||
        !finite(pair.value("pearson"),number)||number<-1||number>1||std::abs(number)<0.95||!near(number,coefficient.toDouble()))return false;}
  return index==array.size();
}
bool correlation(const QJsonObject &report,const Expected &expected,QJsonObject &summary) {
  if(!keys(report,{"contract","sourceKind","scope","method","columns","sampleCount","jointValidCount","rejectedRows","coverage","matrix",
      "constantColumns","redundancyThreshold","redundantPairs","status","provenance","software","recommendations","limitations"})||
      report.value("contract")!="paleo.attribute-correlation-report.v1"||report.value("method")!="pearson_joint_complete_rows_v1"||
      report.value("columns")!=QJsonValue(expected.columns)||report.value("redundancyThreshold")!=QJsonValue(0.95))return false;
  int samples=0,count=0,rejected=0;
  if(!integer(report.value("sampleCount"),AnalysisMaxSamples,samples)||samples!=expected.rows||!integer(report.value("jointValidCount"),samples,count)||
      !integer(report.value("rejectedRows"),samples,rejected)||rejected!=samples-count||!coverage(report.value("coverage"),count,samples)||
      report.value("status")!=(!samples?"empty_window":count<2?"insufficient_samples":"ok"))return false;
  QJsonArray matrix;QSet<int> constant;
  if(!correlationMatrix(report,expected.channels,count,matrix,constant)||!pairs(report.value("redundantPairs"),matrix,expected.channels,count))return false;
  summary={{"analysis","attribute_correlation"},{"contract",report.value("contract")},{"scope",Scope},{"channels",expected.channels},{"sampleCount",samples},
    {"jointValidCount",count},{"rejectedRows",rejected},{"coverage",report.value("coverage")},{"constantChannels",constant.size()},
    {"redundantPairs",report.value("redundantPairs").toArray().size()},{"status",report.value("status")}};return true;
}
} // namespace
AnalysisReportValidation validateAnalysisReport(const QByteArray &bytes,AnalysisKind kind,
    const QJsonObject &expectedInputMetadata,const ExecutionPolicy &policy) {
  AnalysisReportValidation out;const auto fail=[&](const QString &code){out.code=code;return out;};
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  if(bytes.isEmpty()||bytes.size()>qMin<qint64>(policy.outputBytes,ReportBytes))return fail("python_output_budget_exceeded");
  QStringDecoder decoder(QStringDecoder::Utf8);const QString decoded=decoder(bytes);Q_UNUSED(decoded);
  if(decoder.hasError())return fail("python_output_encoding_invalid");
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
  if(parse.error!=QJsonParseError::NoError||!document.isObject()||!UniqueKeys(bytes).valid())return fail("python_output_invalid");
  const auto envelope=document.object(),ports=envelope.value("ports").toObject();
  if(!keys(envelope,{"contract","ports"})||envelope.value("contract")!="paleo.python-output.v1"||ports.keys()!=QStringList{"REPORT"}||
      !ports.value("REPORT").isObject())return fail("python_output_contract_mismatch");
  Expected expected;if(!expectedMetadata(expectedInputMetadata,kind,expected))return fail("python_analysis_input_metadata_invalid");
  const auto report=ports.value("REPORT").toObject();
  if(report.value("provenance")!=QJsonValue(expected.provenance))return fail("python_output_provenance_mismatch");
  QJsonObject summary;if(!common(report,expected)||(kind==AnalysisKind::WellQuality?!quality(report,expected,summary):!correlation(report,expected,summary)))
    return fail("python_output_contract_mismatch");
  // Build a fresh host value from validated fields and reattach the host's source
  // identity; the script cannot supply file paths, digests or output-set authority.
  QJsonObject accepted;for(auto it=report.begin();it!=report.end();++it)accepted.insert(it.key(),it.value());
  accepted.insert("provenance",expected.provenance);if(kind==AnalysisKind::AttributeCorrelation)accepted.insert("columns",expected.columns);
  const auto validated=QJsonDocument(accepted).toJson(QJsonDocument::Compact);
  if(validated.size()>policy.outputBytes||QJsonDocument(summary).toJson(QJsonDocument::Compact).size()>SummaryBytes)return fail("python_output_budget_exceeded");
  out.code="ok";out.bytes=validated;out.summary=summary;return out;
}
} // namespace paleo::python
